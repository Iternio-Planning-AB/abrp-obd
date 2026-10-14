#include "datalogger.h"

// Tunables. SEGMENT_BYTES is the unit of rotation AND of deletion, so it trades granularity (how much
// old data is dropped at once) against file count. HEADROOM keeps the FS from ever being driven 100%
// full, which LittleFS dislikes and which leaves room for the next segment + metadata overhead.
static const size_t SEGMENT_BYTES = 256 * 1024;   // ~256 KB per segment file
static const size_t HEADROOM_BYTES = 384 * 1024;  // keep at least this much FS free

static int parseIndex(const char* name) {
  const char* p = strstr(name, "log");
  if (!p) return -1;
  p += 3;
  if (*p < '0' || *p > '9') return -1;
  return atoi(p);
}

std::string DataLogger::pathFor(int index) const {
  char buf[24];
  snprintf(buf, sizeof(buf), "/log%04d.csv", index);
  return std::string(buf);
}

int DataLogger::highestIndex() {
  int hi = -1;
  File dir = LittleFS.open("/");
  if (!dir) return -1;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int idx = parseIndex(f.name());
    if (idx > hi) hi = idx;
    f.close();
  }
  dir.close();
  return hi;
}

int DataLogger::lowestIndex() {
  int lo = -1;
  File dir = LittleFS.open("/");
  if (!dir) return -1;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    int idx = parseIndex(f.name());
    if (idx >= 0 && (lo < 0 || idx < lo)) lo = idx;
    f.close();
  }
  dir.close();
  return lo;
}

bool DataLogger::openSegment(int index) {
  std::string path = pathFor(index);
  // Open for append; "a" creates the file if it doesn't exist. Deliberately avoid LittleFS.exists()
  // here: its arduino-esp32 implementation opens the path in read mode to test existence, which logs a
  // scary-but-harmless "does not exist, no permits for creation" [E] line for every brand-new segment.
  active = LittleFS.open(path.c_str(), "a");
  if (!active) {
    Serial.printf("[log] failed to open %s\n", path.c_str());
    return false;
  }
  activeIndex = index;
  activeSize = active.size();
  if (activeSize == 0 && header_.size()) { // brand-new (empty) segment -> write the CSV header first
    active.print(header_.c_str());
    active.print("\n");
    active.flush();
    activeSize = active.size();
  }
  return true;
}

void DataLogger::enforceBudget() {
  // Drop oldest segments until the next segment will fit under (total - headroom). Never delete the
  // segment we are about to/continuing to write.
  size_t total = LittleFS.totalBytes();
  size_t budget = (total > HEADROOM_BYTES) ? (total - HEADROOM_BYTES) : (total / 2);
  while (LittleFS.usedBytes() + SEGMENT_BYTES > budget) {
    int lo = lowestIndex();
    if (lo < 0 || lo >= activeIndex) break; // only older-than-active segments are removable
    LittleFS.remove(pathFor(lo).c_str());
    Serial.printf("[log] budget reached, removed oldest segment %d\n", lo);
  }
}

bool DataLogger::begin(const std::string& header) {
  header_ = header;
  if (!LittleFS.begin(true)) { // format on first use / on corruption
    Serial.printf("[log] LittleFS mount failed - logging disabled\n");
    mounted = false;
    return false;
  }
  mounted = true;

  int hi = highestIndex();
  if (hi < 0) {
    openSegment(0); // first ever boot: fresh segment
  } else {
    bool schema_changed = false;
    if (!header_.empty()) {
      File previous = LittleFS.open(pathFor(hi).c_str(), "r");
      schema_changed = !previous;
      if (previous && previous.size()) {
        for (size_t i = 0; i < header_.size(); ++i) {
          if (previous.read() != (unsigned char)header_[i]) { schema_changed = true; break; }
        }
        if (!schema_changed && previous.read() != '\n') schema_changed = true;
      }
      previous.close();
    }
    if (openSegment(hi) && (schema_changed || activeSize >= SEGMENT_BYTES)) {
      active.close();           // newest segment full or old schema -> start the next one
      enforceBudget();
      openSegment(hi + 1);
    }
  }
  printStats(Serial);
  return mounted;
}

bool DataLogger::writeLine(const std::string& line) {
  if (!mounted || !active) return false;
  invalidateReadCursor(); // any write changes segment sizes/offsets - a cached cursor would be stale
  size_t need = line.size() + 1;
  if (activeSize + need > SEGMENT_BYTES) {
    int next = activeIndex + 1;
    active.close();
    enforceBudget();          // free space (delete oldest) before growing
    if (!openSegment(next)) return false;
  }
  active.print(line.c_str());
  active.print("\n");
  active.flush();             // commit each row: a black-box recorder must survive an abrupt power cut
  activeSize += need;
  return true;
}

void DataLogger::flush() {
  if (active) active.flush();
}

void DataLogger::end() {
  invalidateReadCursor();
  if (active) {
    active.flush();
    active.close();
  }
}

void DataLogger::dump(Stream& out, int newest) {
  if (!mounted) { out.printf("[log] not mounted\n"); return; }
  invalidateReadCursor();
  // Reading the active append handle alongside writing is fragile, so close it, stream all segments
  // in order, then reopen for append.
  bool wasOpen = (bool)active;
  int resumeIndex = activeIndex;
  if (active) active.close();

  int lo = lowestIndex(), hi = highestIndex();
  // A full log is ~2.7 MB, about 4 minutes at 115200 baud; a drive usually fits in the newest two or three.
  if (newest > 0 && lo >= 0) {
    int start = hi + 1;
    for (int kept = 0; start > lo && kept < newest;)
      if (LittleFS.exists(pathFor(--start).c_str())) ++kept; // index gaps are normal after budget deletions
    lo = start;
  }
  for (int i = lo; i >= 0 && i <= hi; ++i) {
    std::string p = pathFor(i);
    if (!LittleFS.exists(p.c_str())) continue;
    File f = LittleFS.open(p.c_str(), "r");
    if (!f) continue;
    out.printf("===== %s (%u bytes) =====\n", p.c_str(), (unsigned)f.size());
    uint8_t buf[512]; // chunked read - one byte per call would crawl on a multi-hundred-KB log
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0) out.write(buf, n);
    f.close();
  }
  out.printf("===== end of dump =====\n");

  if (wasOpen) openSegment(resumeIndex);
}

size_t DataLogger::totalSize() {
  if (!mounted) return 0;
  flush(); // buffered rows must be on flash so the size matches what readAt() will serve
  size_t total = 0;
  int lo = lowestIndex(), hi = highestIndex();
  for (int i = lo; i >= 0 && i <= hi; ++i) {
    std::string p = pathFor(i);
    if (!LittleFS.exists(p.c_str())) continue; // index gaps are normal after budget deletions
    File f = LittleFS.open(p.c_str(), "r");
    if (!f) continue;
    total += f.size();
    f.close();
  }
  return total;
}

void DataLogger::invalidateReadCursor() {
  if (readFile) readFile.close();
  readIndex = -1;
  readSegStart = 0;
  readSegSize = 0;
}

size_t DataLogger::readAt(size_t offset, uint8_t* buf, size_t len) {
  if (!mounted || !buf || !len) return 0;
  size_t done = 0;
  while (done < len) {
    if (readIndex < 0 || offset < readSegStart || offset >= readSegStart + readSegSize) {
      // (Re)locate the segment containing `offset`: one directory walk + opening every earlier
      // segment. This is the expensive part the cursor exists to avoid - sequential reads hit it
      // only when crossing a 256KB segment boundary, not per call.
      invalidateReadCursor();
      size_t segStart = 0;
      int lo = lowestIndex(), hi = highestIndex();
      for (int i = lo; i >= 0 && i <= hi; ++i) {
        std::string p = pathFor(i);
        if (!LittleFS.exists(p.c_str())) continue; // index gaps are normal after budget deletions
        File f = LittleFS.open(p.c_str(), "r");
        if (!f) continue;
        size_t fsize = f.size();
        if (offset < segStart + fsize) {
          readFile = f;
          readIndex = i;
          readSegStart = segStart;
          readSegSize = fsize;
          break;
        }
        segStart += fsize;
        f.close();
      }
      if (readIndex < 0) break; // offset at/past the end of the last segment
    }
    readFile.seek(offset - readSegStart);
    int n = readFile.read(buf + done, len - done);
    if (n <= 0) break; // shouldn't happen inside a located segment - bail rather than spin
    done += (size_t)n;
    offset += (size_t)n;
    if (offset >= readSegStart + readSegSize) {
      invalidateReadCursor(); // crossed the segment end - the next iteration relocates
    }
  }
  return done;
}

void DataLogger::clearAll() {
  if (!mounted) return;
  invalidateReadCursor();
  if (active) active.close();
  int lo = lowestIndex(), hi = highestIndex();
  for (int i = lo; i >= 0 && i <= hi; ++i) {
    std::string p = pathFor(i);
    if (LittleFS.exists(p.c_str())) LittleFS.remove(p.c_str());
  }
  activeIndex = -1;
  openSegment(0);
  Serial.printf("[log] cleared all segments\n");
}

void DataLogger::printStats(Stream& out) {
  if (!mounted) { out.printf("[log] not mounted\n"); return; }
  int lo = lowestIndex(), hi = highestIndex();
  int count = 0;
  for (int i = lo; i >= 0 && i <= hi; ++i) {
    if (LittleFS.exists(pathFor(i).c_str())) ++count;
  }
  out.printf("[log] FS used %u / %u bytes, %d segment(s), active=log%04d.csv (%u bytes)\n",
    (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(), count, activeIndex, (unsigned)activeSize);
}
