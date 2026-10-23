#ifndef DATALOGGER_H
#define DATALOGGER_H

/*
  Circular data logger on the onboard flash filesystem (LittleFS).

  Storage survives deep sleep / reboot (unlike PSRAM, which is wiped on sleep). Logging is append-only
  and split into fixed-size segment files (logNNNN.csv). When the filesystem approaches full, the oldest
  segment is deleted before a new one is opened, so the device keeps the most-recent data and never stops
  recording ("circular" / wrap-around). Append + whole-file delete is much gentler on flash wear than
  rewriting records in place.

  Each segment starts with the CSV header passed to begin(), so every file is self-describing.
*/

#include <Arduino.h>
#include <LittleFS.h>
#include <string>

class DataLogger {
public:
  // Mount LittleFS (formats on first use), find existing segments, and open the newest for append.
  // `header` is written at the top of every new segment file. Returns false if the FS won't mount.
  bool begin(const std::string& header);

  // Append one row (a newline is added). Rotates to a new segment when the active one is full, first
  // deleting the oldest segment(s) so total usage stays under budget. Returns false if not mounted.
  bool writeLine(const std::string& line);

  // Called right after a brand-new segment is opened (and its header written), so each segment can start
  // with its own context line. The hook may call writeLine(); nullptr (the default) writes nothing extra.
  void onNewSegment(void (*hook)()) { newSegmentHook = hook; }

  void flush();            // commit buffered data (cheap to call often; called per row already)
  void end();              // flush + close the active segment (call before deep sleep)

  // Serial maintenance/retrieval helpers (flash logs need an explicit way off the device).
  void dump(Stream& out, int newest = 0);  // stream segments oldest -> newest; newest > 0 keeps only the last N
  // DUMP SINCE: stream from the first line the host capture script keeps for `since` (the rule is above
  // dumpSince in datalogger.cpp). Segments that end before `since` are not read.
  struct SinceStats { int start = -1; int cut = -1; size_t sent = 0; }; // segment indexes; -1 = none
  SinceStats dumpSince(Stream& out, uint32_t since);
  void clearAll();         // delete all segments and start fresh
  void printStats(Stream& out);

  // Random-access read view over the same oldest->newest byte stream dump() produces, for the BLE log
  // retrieval (Helpers/log_dump.h): the app pulls the log in chunks it can pace, which a Stream can't do.
  // Both use their own read handles, so they are safe alongside the open append handle ONLY while no
  // writes happen (the caller pauses logging for the whole dump, same as the OTA polling pause).
  // Sequential readAt() calls are served by a cached cursor (open segment handle + its position in the
  // byte stream): locating a segment costs a directory walk plus opening every earlier file, which is
  // tens of ms each on a near-full LittleFS and dominated the BLE dump (~900ms of a ~1.1s window). The
  // cursor is invalidated by EVERY write path (writeLine/clearAll/end/dump), so it can never go stale.
  size_t totalSize();                                     // sum of all segment sizes (0 if not mounted)
  size_t readAt(size_t offset, uint8_t* buf, size_t len); // bytes copied; 0 = offset at/past the end

  bool ready() const { return mounted; }

private:
  bool mounted = false;
  std::string header_;
  void (*newSegmentHook)() = nullptr;
  int activeIndex = -1;
  File active;
  size_t activeSize = 0;

  bool openSegment(int index);
  void enforceBudget();    // delete oldest segments until a new one fits
  int lowestIndex();       // oldest segment on disk (-1 if none)
  int highestIndex();      // newest segment on disk (-1 if none)
  std::string pathFor(int index) const;

  // Read cursor (see readAt above). readIndex < 0 = no cursor.
  File readFile;
  int readIndex = -1;
  size_t readSegStart = 0; // global byte-stream offset where the cached segment begins
  size_t readSegSize = 0;
  void invalidateReadCursor();
};

#endif
