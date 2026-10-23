#include "datalogger.h"
#include <string.h>

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
  if (activeSize == 0) {
    if (header_.size()) { // brand-new (empty) segment -> write the CSV header first
      active.print(header_.c_str());
      active.print("\n");
      active.flush();
      activeSize = active.size();
    }
    // Re-enters writeLine, which cannot rotate a segment this empty and keeps activeSize itself.
    if (newSegmentHook) newSegmentHook();
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

// DUMP SINCE. The line-time rule is the host capture script's (SinceFilter in its capture2.py), which also
// filters what this sends, so the two agree:
//  - a CSV row (`millis,utc,...`) or an EVT line (`EVT,utc,millis,state,atrv,rail,text`) has a time; any
//    other line takes the verdict of the line before it;
//  - utc >= 1e9 is a real epoch. A smaller one, logged before the clock was set, takes its boot's first real
//    epoch; a boot with none takes the last epoch before it, and with none at all it is kept;
//  - a boot starts at a `WAKE reason=` event or where millis goes backwards;
//  - the boot's WAKE and `FW build=` lines, and each segment's `=====` line and CSV header, come ahead of
//    the first kept line.
// Only the first kept line (the cut) is found here; everything after it streams verbatim, as DUMP does, and
// the host filter trims what the rule would still drop after it (lines from a clock that stepped back).
// The scan starts at the newest segment whose last epoch is before `since`, announced at once so a host
// probe sees the reply in its window; older segments are not opened, which assumes the clock does not run
// backwards across segments. Lines before the log's first time (torn or pattern-less) are never a cut.
namespace {
const uint64_t SINCE_CLOCK_VALID = 1000000000ULL; // capture2's CLOCK_VALID, compared with >= as it does
const int SINCE_MAX_CONTEXT = 8;                  // a boot's WAKE and FW lines: one FW per segment it spans

struct LineTime {
  bool timed = false, wake = false, fw = false;
  uint64_t utc = 0, millis = 0;
};

// Saturates, so an overlong digit run still compares as Python's unbounded int does.
bool takeDigits(const char*& p, const char* end, uint64_t& value) {
  const char* first = p;
  for (value = 0; p < end && *p >= '0' && *p <= '9'; ++p)
    value = value > (UINT64_MAX - 9) / 10 ? UINT64_MAX : value * 10 + (uint64_t)(*p - '0');
  return p > first;
}

bool startsWith(const char* p, const char* end, const char* prefix) {
  const size_t n = strlen(prefix);
  return (size_t)(end - p) >= n && memcmp(p, prefix, n) == 0;
}

// The fields SinceFilter's ROW_RE and EVT_RE take from a line; its first bytes are enough.
LineTime lineTime(const char* line, size_t len) {
  LineTime t;
  const char* p = line;
  const char* end = line + len;
  if (startsWith(p, end, "EVT,")) {
    p += 4;
    if (!takeDigits(p, end, t.utc) || !(p < end && *p++ == ',') || !takeDigits(p, end, t.millis) || !(p < end && *p++ == ','))
      return LineTime{};
    for (int field = 0; field < 3; ++field) { // state, atrv, rail
      const char* comma = (const char*)memchr(p, ',', end - p);
      if (!comma) return LineTime{};
      p = comma + 1;
    }
    t.wake = startsWith(p, end, "WAKE reason=");
    t.fw = startsWith(p, end, "FW build=");
  } else if (!takeDigits(p, end, t.millis) || !(p < end && *p++ == ',') || !takeDigits(p, end, t.utc) || !(p < end && *p++ == ',')) {
    return LineTime{};
  }
  t.timed = true;
  return t;
}

// Reads a segment line by line through one buffer, keeping each line's first bytes.
struct LineScanner {
  File& file;
  uint8_t buf[512];
  size_t len = 0, pos = 0;
  size_t offset = 0;  // file offset of the next unread byte: the end of the current line
  size_t start = 0;   // file offset of the current line
  char head[192];     // an EVT line's fields up to its text prefix fit easily
  size_t headLen = 0;
  explicit LineScanner(File& f) : file(f) {}
  bool next() {
    start = offset;
    headLen = 0;
    for (;;) {
      if (pos == len) {
        const int n = file.read(buf, sizeof(buf));
        if (n <= 0) return offset > start; // a last line without its newline still counts
        len = (size_t)n;
        pos = 0;
      }
      const uint8_t c = buf[pos++];
      ++offset;
      if (c == '\n') return true;
      if (headLen < sizeof(head)) head[headLen++] = (char)c;
    }
  }
  bool csvHeader() const { return start == 0 && startsWith(head, head + headLen, "millis,utc,"); }
};

// The last real epoch among the whole lines in a segment's last 512 bytes; false when there is none.
bool tailEpoch(const char* path, uint64_t& epoch) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t buf[512];
  const size_t size = f.size();
  const size_t from = size > sizeof(buf) ? size - sizeof(buf) : 0;
  const int n = f.seek(from) ? (int)f.read(buf, sizeof(buf)) : 0;
  f.close();
  const char* p = (const char*)buf;
  const char* end = p + (n > 0 ? n : 0);
  if (from > 0) { // the first line began before the chunk
    const char* nl = (const char*)memchr(p, '\n', end - p);
    p = nl ? nl + 1 : end;
  }
  bool found = false;
  while (p < end) {
    const char* nl = (const char*)memchr(p, '\n', end - p);
    const LineTime t = lineTime(p, (nl ? nl : end) - p);
    if (t.timed && t.utc >= SINCE_CLOCK_VALID) { epoch = t.utc; found = true; }
    p = nl ? nl + 1 : end;
  }
  return found;
}

// Bytes [from, to) of a segment, to its end when `to` is past it.
size_t copyRange(const char* path, size_t from, size_t to, Stream& out) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  size_t sent = 0;
  if (f.seek(from)) {
    uint8_t buf[512];
    for (int n; from + sent < to && (n = f.read(buf, to - from - sent < sizeof(buf) ? to - from - sent : sizeof(buf))) > 0;) {
      out.write(buf, n);
      sent += (size_t)n;
    }
  }
  f.close();
  return sent;
}

// The `=====` line dump() prints, then the segment's CSV header when it has one, or the whole segment.
size_t segmentHeader(const char* path, Stream& out, bool whole = false) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  char banner[64];
  int n = snprintf(banner, sizeof(banner), "===== %s (%u bytes) =====\n", path, (unsigned)f.size());
  if (n < 0 || n >= (int)sizeof(banner)) n = 0; // cannot happen for pathFor() names; never read past banner
  LineScanner scan(f);
  const size_t header = whole ? (size_t)-1 : scan.next() && scan.csvHeader() ? scan.offset : 0;
  f.close();
  out.write((const uint8_t*)banner, n);
  return n + (header ? copyRange(path, 0, header, out) : 0);
}

struct LineMark {
  int seg = -1;
  size_t off = 0, len = 0;
  LineMark() {}
  LineMark(int s, size_t o, size_t l) : seg(s), off(o), len(l) {}
  bool before(const LineMark& other) const { return seg < other.seg || (seg == other.seg && off < other.off); }
};
} // namespace

DataLogger::SinceStats DataLogger::dumpSince(Stream& out, uint32_t since) {
  SinceStats stats;
  if (!mounted) { out.printf("[log] not mounted\n"); return stats; }
  invalidateReadCursor();
  bool wasOpen = (bool)active; // as dump(): no reads alongside the append handle
  int resumeIndex = activeIndex;
  if (active) active.close();

  const int lo = lowestIndex(), hi = highestIndex();
  for (int i = hi; lo >= 0 && i >= lo && stats.start < 0; --i) {
    uint64_t last;
    if (tailEpoch(pathFor(i).c_str(), last) && last < since) stats.start = i; // a gap fails the open
  }
  if (stats.start < 0) stats.start = lo;

  LineMark cut, pending, context[SINCE_MAX_CONTEXT];
  int contexts = 0;
  if (stats.start >= 0) {
    stats.sent += segmentHeader(pathFor(stats.start).c_str(), out);
    bool clocked = false, clockless = false; // the boot has had a real epoch / a small utc before one
    bool epochBefore = stats.start != lo;     // the skipped segments end before since
    bool haveMillis = false;
    uint64_t lastMillis = 0;
    for (int i = stats.start; i <= hi && cut.seg < 0; ++i) {
      File f = LittleFS.open(pathFor(i).c_str(), "r"); // index gaps are normal after budget deletions
      if (!f) continue;
      LineScanner scan(f);
      while (cut.seg < 0 && scan.next()) {
        const LineTime t = lineTime(scan.head, scan.headLen);
        if (!t.timed || scan.csvHeader()) continue;
        const LineMark here(i, scan.start, scan.offset - scan.start);
        if (t.wake || (haveMillis && t.millis < lastMillis)) { // a new boot
          if (clockless && !clocked && !epochBefore) { cut = pending; break; }
          clocked = clockless = false;
          contexts = 0;
        }
        haveMillis = true;
        lastMillis = t.millis;
        if (t.utc >= SINCE_CLOCK_VALID) {
          if (t.utc >= since) cut = clockless && !clocked ? pending : here; // the clockless lines take this epoch
          clocked = epochBefore = true;
        } else if (!clocked && !clockless) {
          clockless = true;
          pending = here;
        }
        if ((t.wake || t.fw) && contexts < SINCE_MAX_CONTEXT) context[contexts++] = here;
      }
      f.close();
    }
    if (cut.seg < 0 && clockless && !clocked && !epochBefore) cut = pending; // a clockless last boot
  }

  if (cut.seg >= 0) {
    stats.cut = cut.seg;
    int shown = stats.start;
    for (int c = 0; c < contexts && context[c].before(cut); ++c) {
      if (context[c].seg != shown) stats.sent += segmentHeader(pathFor(shown = context[c].seg).c_str(), out);
      stats.sent += copyRange(pathFor(context[c].seg).c_str(), context[c].off, context[c].off + context[c].len, out);
    }
    if (cut.seg != shown) stats.sent += segmentHeader(pathFor(cut.seg).c_str(), out);
    stats.sent += copyRange(pathFor(cut.seg).c_str(), cut.off, (size_t)-1, out);
    for (int i = cut.seg + 1; i <= hi; ++i) stats.sent += segmentHeader(pathFor(i).c_str(), out, true);
  }
  out.printf("===== end of dump =====\n");

  if (wasOpen) openSegment(resumeIndex);
  return stats;
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
