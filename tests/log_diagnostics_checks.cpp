#include <algorithm>
#include <cstdarg>
#include <climits>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <map>
#include "ble_write_summary.h"
#include "motion_stats.h"
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "json.hpp"

void check(bool condition, const std::string& message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Stream {
  std::string bytes;
  size_t write(const uint8_t* data, size_t size) {
    bytes.append(reinterpret_cast<const char*>(data), size);
    return size;
  }
  void printf(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    const int size = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    check(size >= 0 && static_cast<size_t>(size) < sizeof(text), "stream formatting fits");
    bytes.append(text, size);
  }
} Serial;

struct File {
  struct Handle {
    std::string path;
    std::string* bytes = nullptr;
    size_t position = 0;
    std::vector<std::string> children;
    size_t child = 0;
    bool directory = false;
  };
  std::shared_ptr<Handle> handle;
  File() = default;
  File(const std::string& path, std::string* bytes) : handle(std::make_shared<Handle>()) {
    handle->path = path;
    handle->bytes = bytes;
  }
  explicit operator bool() const { return handle && (handle->bytes || handle->directory); }
  const char* name() const { return handle ? handle->path.c_str() : ""; }
  size_t size() const { return handle && handle->bytes ? handle->bytes->size() : 0; }
  size_t print(const char* text) {
    if (!handle || !handle->bytes) return 0;
    handle->bytes->append(text);
    return std::strlen(text);
  }
  void flush() {}
  void close() { handle.reset(); }
  int read() {
    if (!handle || !handle->bytes || handle->position >= size()) return -1;
    return static_cast<unsigned char>((*handle->bytes)[handle->position++]);
  }
  int read(uint8_t* data, size_t requested) {
    if (!handle || !handle->bytes) return 0;
    const size_t count = std::min(requested, size() - handle->position);
    std::memcpy(data, handle->bytes->data() + handle->position, count);
    handle->position += count;
    return static_cast<int>(count);
  }
  bool seek(size_t offset) {
    if (!handle || !handle->bytes || offset > size()) return false;
    handle->position = offset;
    return true;
  }
  File openNextFile();
};

struct Filesystem {
  std::map<std::string, std::string> files;
  std::set<std::string> failed_reads, failed_appends;
  std::map<std::string, int> opens; // file opens per path, so a check can prove a segment was never read
  bool mount_ok = true;
  size_t capacity = 8 * 1024 * 1024;
  bool begin(bool) { return mount_ok; }
  size_t totalBytes() const { return capacity; }
  size_t usedBytes() const {
    size_t size = 0;
    for (const auto& item : files) size += item.second.size();
    return size;
  }
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool remove(const char* path) { return files.erase(path) != 0; }
  File open(const char* path, const char* mode = "r") {
    if (std::strcmp(path, "/") == 0) {
      File directory;
      directory.handle = std::make_shared<File::Handle>();
      directory.handle->directory = true;
      for (const auto& item : files) directory.handle->children.push_back(item.first);
      return directory;
    }
    const bool append = std::strcmp(mode, "a") == 0;
    ++opens[path];
    if ((append ? failed_appends : failed_reads).count(path)) return {};
    if (!append && !exists(path)) return {};
    return File(path, &files[path]);
  }
} LittleFS;

File File::openNextFile() {
  if (!handle || handle->child >= handle->children.size()) return {};
  const std::string path = handle->children[handle->child++];
  return File(path, &LittleFS.files.at(path));
}

#include "datalogger.inc"
#include "legacy_begin.inc"
#include "FreematicsBase.h"

#define ENABLE_LOGGING 1
#define ENABLE_SLEEP 0
#define ENABLE_MEMS 0
#define ENABLE_WIFI 0
#define PRINT 0
#define FW_BUILD_TAG "v1.4.0-rc.2+12.gabc1234.dirty"
#define OTA_FW_VERSION 1004000
#include "../baseLogger/config.h"

#define RTC_DATA_ATTR
using json = nlohmann::json;
#include "../libraries/OBDWrapper/obd_vector.h"
enum ActivityState { BLE, ACTIVE, CHARGING, STANDBY };
ActivityState current_state = ACTIVE;
float g_last_voltage = 12.5, g_last_io36 = 12.6;
const char* g_wake_reason = "motion";
int g_last_http = 200;
std::string g_last_status = "ok";
bool g_clean_sleep = true;
unsigned long fake_ms = 1000;
unsigned long millis() { return fake_ms; }
int esp_reset_reason() { return 1; }
struct RtcStub { unsigned long getEpoch() { return 1791348543; } } rtc;
struct EspStub {
  unsigned getFreeHeap() { return 100000; }
  unsigned getMinFreeHeap() { return 80000; }
  unsigned getMaxAllocHeap() { return 60000; }
  unsigned getFreePsram() { return 2000000; }
} ESP;
GPS_DATA gps_data{}, last_gd{};
GPS_DATA* gd = &gps_data;
bool g_gps_powered = true, is_currently_parked = true, frozen_valid = true;
double frozen_lat = 11, frozen_lon = 21;
struct SysStub {
  GPSReadDiagnostics diagnostics;
  const GPSReadDiagnostics& gpsReadDiagnostics() const { return diagnostics; }
  bool gpsGetData(GPS_DATA** data) {
    if (data) *data = &gps_data;
    return diagnostics.result == GPSReadResult::Accepted;
  }
} sys;
DataLogger datalogger;
enum ClockSource { CLK_GPS };
time_t utcToEpoch(int, int, int, int, int, int) { return 0; }
void setClock(ClockSource, time_t) {}
#include "logging.inc"
#include "fw_build_log.h"
#include "legacy_header.inc"
#include "legacy_logging.inc"
#include "telemetry_format.h"
#include "gps_location.h"

std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> fields;
  size_t begin = 0;
  for (;;) {
    const size_t end = text.find(',', begin);
    fields.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos) return fields;
    begin = end + 1;
  }
}

std::string column(const std::string& row, const std::string& name) {
  const auto header = split(LOG_HEADER), fields = split(row);
  check(header.size() == fields.size(), "CSV header and row widths agree");
  const auto position = std::find(header.begin(), header.end(), name);
  check(position != header.end(), "known CSV column " + name);
  return position == header.end() ? "" : fields[position - header.begin()];
}

// True when no field reads as the receiver fix, in degrees at any precision or in micro-degrees.
bool hasNoRawFix(const std::string& row, double lat, double lng) {
  for (const auto& field : split(row)) {
    char* end = nullptr;
    const double v = std::strtod(field.c_str(), &end);
    if (field.empty() || *end != '\0') continue;
    for (const double raw : {lat, lng})
      if (std::fabs(v - raw) < 0.05 || std::fabs(v - raw * 1e6) < 1e3) return false;
  }
  return true;
}

void loggingChecks() {
  check(LOG_HEADER.find(LEGACY_LOG_HEADER + ",") == 0, "diagnostic schema preserves entire real baseline column prefix for rollback");
  gps_data.date = 71026;
  gps_data.time = 4490300;
  gps_data.lat = 10.123456f;
  gps_data.lng = 20.654321f;
  gps_data.speed = 0;
  sys.diagnostics.result = GPSReadResult::Accepted;
  sys.diagnostics.reply_bytes = 65;
  OBDVector frame{{"soc", 50}};
  addGpsData(frame);
  const std::string row = buildLogLine(frame, 1);
  check(row.find(buildLegacyLogLine(frame, 1) + ",") == 0, "all real baseline field values retain exact prefix encoding");
  check(column(row, "gps_read") == "ok", "GPS read result is persisted separately from cached position");
  check(column(row, "gps_reply_bytes") == "65" && column(row, "gps_field") == "-1", "read metadata is distinct from quality");
  check(column(row, "gps_source") == "frozen" && column(row, "lat") == "11.000", "actual frozen selection is recorded");
  check(column(row, "gps_date") == "71026" && column(row, "gps_time") == "4490300", "receiver timestamp is separate from telemetry UTC");
  check(hasNoRawFix(row, gps_data.lat, gps_data.lng), "raw GNSS coordinates hidden by the frozen position are not logged");
  check(column(row, "gps_time_age_ms") == "0", "new receiver timestamp starts measured age");
  fake_ms += 10000;
  sys.diagnostics.result = GPSReadResult::RequiredFieldInvalid;
  sys.diagnostics.field = 5;
  const std::string failed = buildLogLine(frame, 1);
  check(column(failed, "gps_read") == "field" && column(failed, "gps_field") == "5", "rejection reason and field persist");
  sys.diagnostics.result = GPSReadResult::Unconfirmed;
  check(column(buildLogLine(frame, 1), "gps_read") == "unconfirmed", "a held first fix is named");
  sys.diagnostics.result = GPSReadResult::RequiredFieldInvalid;
  check(column(failed, "gps_date") == "71026" && column(failed, "gps_time_age_ms") == "10000", "cached timestamp ages despite a failed read");
  gps_data.speed = 20;
  gps_data.time += 100;
  OBDVector moving{{"soc", 50}};
  addGpsData(moving);
  const std::string live = buildLogLine(moving, 1);
  check(column(live, "gps_source") == "gnss" && column(live, "gps_speed_kmh") == "37.04", "GNSS speed has explicit km/h units");
  check(column(live, "lat") == "10.123", "first moving fix emission stays unchanged");
  check(is_currently_parked, "observing selection does not unlatch parked");
  is_currently_parked = false;
  OBDVector unchanged;
  addGpsData(unchanged);
  const std::string omitted = buildLogLine(unchanged, 0);
  check(column(omitted, "gps_source") == "omitted", "unchanged position omission is accurately recorded");
  check(hasNoRawFix(omitted, gps_data.lat, gps_data.lng), "an omitted position does not fall back to the raw fix");
  gd = nullptr;
  g_gps_powered = false;
  gps_fix_seq = 0;
  OBDVector missing;
  addGpsData(missing);
  const std::string none = buildLogLine(missing, 0);
  check(column(none, "gps_read") == "none" && column(none, "gps_time_age_ms").empty(), "unpowered GPS and unknown age are explicit");
  check(column(none, "gps_source") == "omitted" && column(none, "soc").empty(), "no coordinates or measurements are fabricated");
  gd = &gps_data;
  g_gps_powered = true;
  gps_data.date = UINT32_MAX;
  gps_data.time = UINT32_MAX;
  gps_data.speed = static_cast<float>(INT32_MAX) / 100;
  fake_ms = ULONG_MAX - 15;
  gpsNoteFix();
  fake_ms = 16;
  const std::string extreme = buildLogLine({}, 0);
  check(column(extreme, "gps_time_age_ms") == "32", "age remains correct across native millis wrap");
  check(column(extreme, "gps_source") == "omitted", "extreme valid integer conversions do not truncate CSV columns");
  size_t added = 0;
  for (const auto* name : {"gps_read", "gps_reply_bytes", "gps_field", "gps_date", "gps_time", "gps_time_age_ms",
                           "gps_speed_kmh", "gps_source"}) added += column(row, name).size() + 1;
  std::cout << "GPS columns " << split(LOG_HEADER).size() << ", representative row " << row.size()
            << " bytes (+" << added << " diagnostic bytes)\n";
}

void schemaChecks() {
  const std::string old = "soc,lat,lon\n50,1,2\n";
  const std::string header = "soc,lat,lon,gps_read";
  LittleFS = Filesystem{};
  LittleFS.files["/log0000.csv"] = old;
  DataLogger logger;
  check(logger.begin(header), "new logger mounts");
  check(logger.writeLine("51,1,2,accepted"), "new logger writes");
  check(LittleFS.files.size() == 2, "schema change creates a new segment");
  check(LittleFS.files.at("/log0000.csv") == old, "old header and rows stay intact");
  check(LittleFS.files.at("/log0001.csv") == header + "\n51,1,2,accepted\n", "new rows use new header");
  logger.end();
  DataLogger wake;
  check(wake.begin(header) && wake.writeLine("52,1,2,no_reply"), "next wake writes");
  check(LittleFS.files.size() == 2, "same schema does not rotate each wake");
  Stream dump;
  wake.dump(dump);
  check(dump.bytes.find(old) != std::string::npos && dump.bytes.find(header) != std::string::npos,
        "mixed schema dump preserves both segments");
  const size_t total = wake.totalSize();
  std::vector<uint8_t> read(total);
  check(wake.readAt(0, read.data(), read.size()) == total, "BLE byte-stream reader returns both schemas");
  const std::string combined(read.begin(), read.end());
  check(combined == LittleFS.files.at("/log0000.csv") + LittleFS.files.at("/log0001.csv"),
        "BLE byte stream and segment contents agree");
  LittleFS.files["/log0003.csv"] = header + "\n53,1,2,accepted\n"; // a missing index must not count
  auto segment = [](const std::string& p) {
    return "===== " + p + " (" + std::to_string(LittleFS.files.at(p).size()) + " bytes) =====\n" + LittleFS.files.at(p);
  };
  Stream tail, all, full;
  wake.dump(tail, 2);
  check(tail.bytes == segment("/log0001.csv") + segment("/log0003.csv") + "===== end of dump =====\n",
        "DUMP n streams exactly the newest n segments, oldest first, skipping index gaps");
  wake.dump(full);
  wake.dump(all, 99);
  check(all.bytes == full.bytes, "DUMP n larger than the segment count streams everything");
}

void schemaFailureChecks() {
  const std::string header = "millis,utc,soc,gps_read";
  for (const auto& old : {header.substr(0, 8), header + "extra\n1,2,3\n", std::string(256 * 1024, 'x')}) {
    LittleFS = Filesystem{};
    LittleFS.files["/log0000.csv"] = old;
    DataLogger logger;
    check(logger.begin(header) && logger.writeLine("1,1791348543,50,ok"), "partial/corrupt header recovers by rotation");
    check(LittleFS.files.at("/log0000.csv") == old && LittleFS.files.size() == 2, "recovery does not alter old bytes");
  }
  LittleFS = Filesystem{};
  LittleFS.files["/log0000.csv"] = "";
  DataLogger empty;
  check(empty.begin(header) && empty.writeLine("1,1791348543,50,ok"), "empty segment receives header");
  check(LittleFS.files.size() == 1 && LittleFS.files.at("/log0000.csv").find(header + "\n") == 0,
        "empty segment is reused, not abandoned");
  LittleFS = Filesystem{};
  const std::string old = "millis,utc,soc\n1,1791348543,50\n";
  LittleFS.files["/log0000.csv"] = old;
  LittleFS.failed_reads.insert("/log0000.csv");
  DataLogger unreadable;
  check(unreadable.begin(header) && unreadable.writeLine("2,1791348543,51,ok"), "unreadable header fails safe into new segment");
  check(LittleFS.files.at("/log0000.csv") == old, "unreadable old segment is not appended");
  LittleFS = Filesystem{};
  LittleFS.files["/log0000.csv"] = old;
  LittleFS.failed_appends.insert("/log0001.csv");
  DataLogger failed;
  failed.begin(header);
  check(!failed.writeLine("2,1791348543,51,ok"), "failed new-segment open refuses writes");
  check(LittleFS.files.at("/log0000.csv") == old && LittleFS.files.size() == 1, "open failure never falls back to old schema");
  LittleFS.failed_appends.clear();
  DataLogger retry;
  check(retry.begin(header) && retry.writeLine("2,1791348543,51,ok"), "next boot retries a failed schema rotation");
  LittleFS = Filesystem{};
  const std::string full = header + "\n" + std::string(256 * 1024 - header.size() - 1, 'x');
  LittleFS.files["/log0000.csv"] = full;
  DataLogger capacity;
  check(capacity.begin(header) && capacity.writeLine("3,1791348543,52,ok"), "full segment rotates with same schema");
  check(LittleFS.files.at("/log0000.csv") == full && LittleFS.files.size() == 2, "size rotation preserves previous segment");
  LittleFS = Filesystem{};
  LittleFS.files["/log0000.csv"] = "1,2,3\n";
  DataLogger no_header;
  check(no_header.begin("") && no_header.writeLine("4,5,6"), "headerless logger mode remains supported");
  check(LittleFS.files.size() == 1, "headerless mode does not rotate every boot");
  LittleFS = Filesystem{};
  LittleFS.mount_ok = false;
  DataLogger mount_failure;
  check(!mount_failure.begin(header) && !mount_failure.writeLine("1,2,3,ok"), "mount failure never writes");
}

void fwBuildChecks() {
  const std::string fw = "FW build=v1.4.0-rc.2+12.gabc1234.dirty v=1004000 elf=0123456789abcdef part=app1";
  const std::string line = "EVT,1791348543,1000,ACTIVE,12.50,12.60," + fw + "\n";
  const std::string head = LOG_HEADER + "\n";
  auto boot = [](const std::map<std::string, std::string>& files) {
    LittleFS = Filesystem{};
    LittleFS.files = files;
    datalogger = DataLogger{};
    datalogger.onNewSegment(logFwBuild);
    return datalogger.begin(LOG_HEADER);
  };
  fake_ms = 1000;
  current_state = ACTIVE;

  check(boot({}), "first boot mounts");
  check(LittleFS.files.at("/log0000.csv") == head + line, "a new segment starts with the build line: " +
        LittleFS.files.at("/log0000.csv").substr(head.size()));

  const std::string kept = head + "1,2\n";
  check(boot({{"/log0000.csv", kept}}) && LittleFS.files.size() == 1 && LittleFS.files.at("/log0000.csv") == kept,
        "continuing a segment (a deep-sleep wake) writes no build line");

  const std::string old = "soc,lat\n1,2\n";
  check(boot({{"/log0000.csv", old}}), "schema change mounts");
  check(LittleFS.files.at("/log0000.csv") == old && LittleFS.files.at("/log0001.csv") == head + line,
        "a schema rotation in begin() starts the new segment with the build line");
  DataLogger legacy;
  check(legacy.beginBaseline(LOG_HEADER) && LittleFS.files.size() == 2,
        "eacc642 firmware appends to a segment that starts with the build line instead of rotating");

  const std::string full = head + std::string(SEGMENT_BYTES - head.size() - 10, 'x') + "\n";
  check(boot({{"/log0000.csv", full}}) && LittleFS.files.size() == 1, "a nearly full segment is continued");
  logEvent("STATE x");
  const std::string state = "EVT,1791348543,1000,ACTIVE,12.50,12.60,STATE x\n";
  check(LittleFS.files.at("/log0000.csv") == full && LittleFS.files.at("/log0001.csv") == head + line + state,
        "rotation inside logEvent puts the build line between the header and the event");
  const size_t room = SEGMENT_BYTES - LittleFS.files.at("/log0001.csv").size();
  check(datalogger.writeLine(std::string(room - 1, 'y')) && LittleFS.files.size() == 2,
        "the build line is counted once: a row that exactly fills the segment stays in it");
  check(datalogger.writeLine("z") && LittleFS.files.size() == 3 && LittleFS.files.at("/log0002.csv") == head + line + "z\n",
        "the next row rotates, and the build line is not undercounted");

  datalogger.clearAll();
  check(LittleFS.files.size() == 1 && LittleFS.files.at("/log0000.csv") == head + line, "CLEAR restarts with the build line");

  host_running_partition = nullptr;
  logFwBuild();
  host_running_partition = &host_app_partition;
  check(LittleFS.files.at("/log0000.csv").find(" part=?\n") != std::string::npos, "an unknown slot is marked, not omitted");
  datalogger = DataLogger{};
}

void emitDump() {
  LittleFS = Filesystem{};
  LittleFS.files["/log0000.csv"] = "millis,utc,soc\n1,1791348543,50\n";
  check(datalogger.begin(LOG_HEADER), "fixture logger mounts");
  check(datalogger.writeLine(buildLogLine({{"soc", 51}}, 1)), "fixture logger writes real diagnostic CSV");
  datalogger.end();
  DataLogger rollback;
  check(rollback.beginBaseline(LEGACY_LOG_HEADER), "actual baseline firmware begin ignores new header");
  check(rollback.writeLine(buildLegacyLogLine({{"soc", 52}}, 1)), "actual baseline firmware appends legacy width row");
  rollback.end();
  check(datalogger.begin(LOG_HEADER) && datalogger.writeLine(buildLogLine({{"soc", 53}}, 1)),
        "diagnostic firmware resumes same segment without rewriting legacy row");
  Stream dump;
  datalogger.dump(dump);
  std::cout << dump.bytes;
}

void bleWriteChecks() {
  const std::string token = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
  const std::pair<std::string, std::string> cases[] = {
    {"{\"AUTH\":\"" + token + "\"}", "keys=AUTH"},
    {"{\"tlm_push_token\":\"" + token + "\"}", "keys=tlm_push_token"},
    {"{\"AUTH\":\"" + token, "non-object"},
    {"AUTH " + token, "non-object"},
    {"[\"" + token + "\"]", "non-object"},
    {"{\"" + token + "\":1}", "keys= +1 other"},
    {"{\"CLEAN\":1,\"a\\nEVT,0\":2,\"obd_alarm\":0}", "keys=CLEAN,obd_alarm +1 other"},
  };
  for (const auto& [write, summary] : cases)
    check(bleWriteSummary(write) == "len=" + std::to_string(write.size()) + " " + summary, "BLE write logs length and identifier keys only: " + bleWriteSummary(write));
  check(std::string(bleKeyForLog("obd_alarm")) == "obd_alarm" && std::string(bleKeyForLog(token)) == "?" &&
        std::string(bleKeyForLog("a\nEVT")) == "?", "a logged key is named only when identifier-shaped");
}


std::string motionLine(const MotionStats& m, bool parked = true, bool chg = false) {
  char buf[160];
  motionStatsLine(buf, sizeof(buf), m, parked, chg);
  return buf;
}

void motionChecks() {
  MotionStats m, out;
  unsigned long now = 1000;
  for (int i = 0; i < 120; ++i, now += 500)
    check(!motionStatsWindow(m, true, true, 500, 0.2f, 0.12f, now, out), "windows inside the minute emit nothing");
  check(motionStatsWindow(m, true, false, 500, 0.01f, 0.0f, now, out), "the first window past 60 s rolls the minute over");
  check(motionLine(out) == "MOTION act=100 n=120 s=59 peak=0.200 mean=0.200 axmax=0.120 obd=0 parked=1 chg=0",
        "all-motion minute: " + motionLine(out));
  check(m.n == 1 && m.active_ms == 0, "the rollover window starts the next minute");

  m = MotionStats{};
  now = 2000;
  for (int i = 0; i < 100; ++i) motionStatsWindow(m, true, true, 0, 0.3f, 0.2f, now += 150, out);
  check(motionStatsTake(m, out) && motionLine(out).rfind("MOTION act=100 ", 0) == 0,
        "an all-motion minute of 0 ms windows is act=100: " + motionLine(out));

  m = MotionStats{};
  now = 3000;
  motionStatsWindow(m, true, false, 500, 0.01f, 0.0f, now, out);
  motionStatsWindow(m, true, false, 500, 0.01f, 0.0f, now += 500, out);
  motionStatsWindow(m, true, true, 0, 0.3f, 0.2f, now += 20000, out);  // a 20 s blocking upload before it
  check(motionStatsTake(m, out) && out.total_ms == 3000 && out.active_ms == 2000,
        "a long gap counts only up to the cap: " + motionLine(out));
  motionStatsWindow(m, true, true, 0, 0.3f, 0.2f, now += 150, out);
  check(motionStatsTake(m, out) && motionLine(out).rfind("MOTION act=100 n=1 ", 0) == 0,
        "a lone 0 ms moving window is act=100: " + motionLine(out));

  m = MotionStats{};
  now = 5000;
  const float peaks[] = {0.05f, 0.30f, 0.15f}, axes[] = {-0.04f, 0.25f, 0.12f};
  const uint32_t lens[] = {500, 1, 0};  // a moving window ends on its first sample
  for (int i = 0; i < 3; ++i, now += 500)
    motionStatsWindow(m, true, axes[i] > 0.1f, lens[i], peaks[i], axes[i], now, out);
  m.obd = true;
  check(motionStatsTake(m, out), "a partial minute is flushed at sleep");
  check(motionLine(out, false, true) ==
        "MOTION act=67 n=3 s=1 peak=0.300 mean=0.167 axmax=0.250 obd=1 parked=0 chg=1",
        "mixed minute: " + motionLine(out, false, true));
  check(!motionStatsTake(m, out), "a flushed minute is emitted once");

  m = MotionStats{};
  now = 9000;
  // Field shape: ~150 ms loops; half the minute moving, each moving window 0-1 ms long.
  for (int i = 0; i < 400; ++i) motionStatsWindow(m, true, i >= 200, i >= 200 ? i % 2 : 500, 0.2f, 0.15f, now += 150, out);
  check(motionStatsTake(m, out) && motionLine(out).rfind("MOTION act=50 n=400 ", 0) == 0,
        "0-1 ms moving windows count by wall time: " + motionLine(out));

  m = MotionStats{};
  int lines = 0;
  now = 0;
  for (int i = 0; i < 3 * 120 + 1; ++i, now += 500) lines += motionStatsWindow(m, true, false, 500, 0.01f, 0.0f, now, out);
  check(lines == 3 && out.n == 120, "rollover emits exactly one line per minute");

  m = MotionStats{};
  now = 0;
  for (int i = 0; i < 240; ++i, now += 500)
    check(!motionStatsWindow(m, false, true, 500, 0.5f, 0.5f, now, out), "a bypassed window never emits");
  check(m.n == 0 && !motionStatsTake(m, out), "OTA/log-dump bypass windows are not counted");
}


const std::string END_OF_DUMP = "===== end of dump =====\n";
// DUMP SINCE fixtures: synthetic rows and events in the firmware's line formats (no real log content).
const std::string SINCE_HEADER = "millis,utc,soc";
const unsigned long T0 = 1800000000UL;
std::string sinceRow(unsigned long ms, unsigned long utc) { return std::to_string(ms) + "," + std::to_string(utc) + ",50\n"; }
std::string sinceEvt(unsigned long utc, unsigned long ms, const std::string& msg) {
  return "EVT," + std::to_string(utc) + "," + std::to_string(ms) + ",ACTIVE,12.00,12.10," + msg + "\n";
}
std::string sinceWake(unsigned long utc, unsigned long ms) { return sinceEvt(utc, ms, "WAKE reason=timer reset=8"); }
std::string sinceFw(unsigned long utc, unsigned long ms) { return sinceEvt(utc, ms, "FW build=v0.0.0 v=0 elf=0 part=app0"); }
std::string sincePath(int index) { char path[24]; snprintf(path, sizeof(path), "/log%04d.csv", index); return path; }
void sinceSegments(const std::vector<std::string>& bodies) {
  LittleFS = Filesystem{};
  for (size_t i = 0; i < bodies.size(); ++i) LittleFS.files[sincePath(i)] = SINCE_HEADER + "\n" + bodies[i];
}
std::string sinceHead(int index) {
  const std::string& file = LittleFS.files.at(sincePath(index));
  return "===== " + sincePath(index) + " (" + std::to_string(file.size()) + " bytes) =====\n" + SINCE_HEADER + "\n";
}
std::string sinceWhole(int index) { return sinceHead(index) + LittleFS.files.at(sincePath(index)).substr(SINCE_HEADER.size() + 1); }
std::string runSince(uint32_t since, DataLogger::SinceStats* stats = nullptr) {
  DataLogger logger;
  check(logger.begin(SINCE_HEADER), "since fixture mounts");
  LittleFS.opens.clear();
  Stream out;
  const DataLogger::SinceStats result = logger.dumpSince(out, since);
  if (stats) *stats = result;
  check(result.sent + END_OF_DUMP.size() == out.bytes.size(), "sent counts every streamed byte but the end marker");
  return out.bytes;
}

void dumpSinceChecks() {
  // Two clocked boots over four segments; every new segment starts with its FW line.
  const std::vector<std::string> clocked = {
    sinceWake(T0, 10) + sinceRow(20, T0 + 10) + sinceRow(30, T0 + 20),
    sinceFw(T0 + 30, 40) + sinceRow(50, T0 + 100),
    sinceWake(T0 + 1000, 10) + sinceFw(T0 + 1000, 15) + sinceRow(20, T0 + 1010) + sinceRow(30, T0 + 1020),
    sinceFw(T0 + 1030, 40) + sinceRow(50, T0 + 1100)};
  sinceSegments(clocked);
  std::string full;
  { DataLogger logger; check(logger.begin(SINCE_HEADER), "since fixture mounts"); Stream out; logger.dump(out); full = out.bytes; }
  check(full == sinceWhole(0) + sinceWhole(1) + sinceWhole(2) + sinceWhole(3) + END_OF_DUMP,
        "DUMP is every segment, header then bytes, then the end marker");
  check(runSince(0) == full && runSince(T0) == full, "a since older than everything streams exactly what DUMP does");

  DataLogger::SinceStats stats;
  check(runSince(T0 + 1015, &stats) == sinceHead(1) + sinceHead(2) + sinceWake(T0 + 1000, 10) + sinceFw(T0 + 1000, 15) +
            sinceRow(30, T0 + 1020) + sinceWhole(3) + END_OF_DUMP,
        "mid-boot since: the start segment is announced, the boot's WAKE/FW lines come ahead of its first kept line");
  check(stats.start == 1 && stats.cut == 2, "the start is the newest segment ending before since");
  check(LittleFS.opens["/log0000.csv"] == 0, "a segment older than the start segment is never opened");

  check(runSince(T0 + 5000, &stats) == sinceHead(3) + END_OF_DUMP && stats.cut == -1 && stats.start == 3,
        "a since newer than everything prints the newest header and the end marker only");
  check(LittleFS.opens["/log0000.csv"] + LittleFS.opens["/log0001.csv"] + LittleFS.opens["/log0002.csv"] == 0,
        "a since newer than everything reads only the newest segment");

  // A boot spanning a segment boundary: its context lines come from the earlier segment, under its header.
  check(runSince(T0 + 50) == sinceHead(0) + sinceWake(T0, 10) + sinceHead(1) + sinceFw(T0 + 30, 40) +
            sinceRow(50, T0 + 100) + sinceWhole(2) + sinceWhole(3) + END_OF_DUMP,
        "segment rollover: context from both segments of the boot, each under its own header");

  // A cold boot: clockless lines (utc 0) take the boot's first real epoch, even one in the next segment.
  sinceSegments({
    sinceWake(T0, 10) + sinceRow(20, T0 + 10),
    sinceFw(0, 5) + sinceWake(0, 60) + sinceRow(70, 0),
    sinceFw(0, 80) + sinceRow(90, T0 + 2000) + sinceRow(100, T0 + 3500)});
  check(runSince(T0 + 1000) == sinceHead(0) + sinceHead(1) + sinceWake(0, 60) + sinceRow(70, 0) + sinceWhole(2) + END_OF_DUMP,
        "clockless lines before the boot's first epoch are kept with it, across a segment boundary");
  check(runSince(T0 + 3000) == sinceHead(0) + sinceHead(1) + sinceWake(0, 60) + sinceHead(2) + sinceFw(0, 80) +
            sinceRow(100, T0 + 3500) + END_OF_DUMP,
        "clockless lines take the boot's first epoch, so a later since drops them but keeps them as context");

  // A boot that never gets a clock takes the epoch before it; with none before it, it is kept.
  sinceSegments({sinceWake(T0, 10) + sinceRow(20, T0 + 10) + sinceWake(0, 5) + sinceRow(15, 0) +
                 sinceWake(T0 + 500, 5) + sinceRow(15, T0 + 510)});
  check(runSince(T0 + 505) == sinceHead(0) + sinceWake(T0 + 500, 5) + sinceRow(15, T0 + 510) + END_OF_DUMP,
        "a clockless boot after an older epoch is dropped");
  check(runSince(T0 + 5) == sinceHead(0) + sinceWake(T0, 10) + sinceRow(20, T0 + 10) + sinceWake(0, 5) + sinceRow(15, 0) +
            sinceWake(T0 + 500, 5) + sinceRow(15, T0 + 510) + END_OF_DUMP,
        "a clockless boot after a kept epoch is kept");
  sinceSegments({sinceWake(0, 5) + sinceRow(15, 0), sinceWake(T0, 5) + sinceRow(15, T0 + 10)});
  check(runSince(T0 + 5) == sinceWhole(0) + sinceWhole(1) + END_OF_DUMP, "a clockless boot with no epoch before it is kept");

  sinceSegments({sinceWake(0, 5) + sinceRow(15, 0)});
  check(runSince(T0) == sinceWhole(0) + END_OF_DUMP, "a log that never had a clock is kept whole");
  // A tail longer than the 512 bytes read: its torn first line is skipped, its last epoch still found.
  std::string longTail = sinceWake(T0, 10);
  for (unsigned long ms = 20; longTail.size() < 1500; ms += 10) longTail += sinceRow(ms, T0 + ms);
  sinceSegments({longTail, sinceWake(T0 + 5000, 5) + sinceRow(15, T0 + 5010)});
  check(runSince(T0 + 4000, &stats) == sinceHead(0) + sinceWhole(1) + END_OF_DUMP && stats.start == 0,
        "a long segment's tail epoch is read from its last 512 bytes");

  // A boot boundary is also a millis drop with no WAKE line, here at a segment edge.
  sinceSegments({sinceWake(T0, 10) + sinceRow(1000, T0 + 10), sinceRow(5, 0) + sinceRow(15, T0 + 100)});
  check(runSince(T0 + 50) == sinceHead(0) + sinceHead(1) + sinceRow(5, 0) + sinceRow(15, T0 + 100) + END_OF_DUMP,
        "a millis drop starts a boot, so its clockless lines take the new boot's epoch");

  // Past skipped segments there is an epoch before since, so a clockless boot opening the start segment is dropped.
  sinceSegments({sinceWake(T0, 10) + sinceRow(20, T0 + 10),
                 sinceWake(0, 5) + sinceRow(15, 0) + sinceWake(T0 + 100, 5) + sinceRow(15, T0 + 110),
                 sinceWake(T0 + 500, 5) + sinceRow(15, T0 + 510)});
  check(runSince(T0 + 200, &stats) == sinceHead(1) + sinceWhole(2) + END_OF_DUMP && stats.start == 1 &&
            LittleFS.opens["/log0000.csv"] == 0,
        "a clockless boot after skipped segments takes their older epoch");

  // A segment whose tail has no epoch is not a start candidate: the walk goes on to an older one.
  sinceSegments({sinceWake(T0, 10) + sinceRow(20, T0 + 10), sinceWake(0, 5) + sinceRow(15, 0)});
  check(runSince(T0 + 150, &stats) == sinceHead(0) + END_OF_DUMP && stats.start == 0 && stats.cut == -1,
        "a clockless tail keeps the walk going, and that clockless boot takes the older epoch");
  // Lines are times only in the two formats SinceFilter matches; an overlong utc is still a real epoch.
  sinceSegments({sinceWake(T0, 10) + sinceRow(20, T0 + 10) + "EVT,1800000200,30,ACTIVE\n"});
  check(runSince(T0 + 150) == sinceHead(0) + END_OF_DUMP, "an EVT line with too few fields is not a time");
  sinceSegments({sinceWake(T0, 10) + sinceRow(20, T0 + 10) + "30,99999999999999999999999,x\n"});
  check(runSince(T0 + 150) == sinceHead(0) + sinceWake(T0, 10) + "30,99999999999999999999999,x\n" + END_OF_DUMP,
        "a utc too long for 64 bits saturates and counts as a real epoch, as Python's int does");

  // Streaming closes and reopens the append handle: logging continues in the newest segment afterwards.
  sinceSegments(clocked);
  {
    DataLogger logger;
    check(logger.begin(SINCE_HEADER), "since fixture mounts");
    Stream out;
    logger.dumpSince(out, T0 + 1015);
    check(logger.writeLine("tail") && LittleFS.files.at("/log0003.csv").size() >= 5 &&
              LittleFS.files.at("/log0003.csv").substr(LittleFS.files.at("/log0003.csv").size() - 5) == "tail\n",
          "logging resumes in the active segment after DUMP SINCE");
    DataLogger unmounted;
    Stream none;
    unmounted.dumpSince(none, 0);
    check(none.bytes == "[log] not mounted\n", "an unmounted log says so, as DUMP does");
  }

  int newest = -1;
  bool by_time = true;
  uint32_t since = 7;
  check(parseDumpArgs("", newest, by_time, since) && newest == 0 && !by_time, "DUMP is everything");
  check(parseDumpArgs(" 3", newest, by_time, since) && newest == 3 && !by_time, "DUMP 3 is the newest three");
  check(parseDumpArgs(" since 5", newest, by_time, since) && by_time && since == 5, "dump since 5, any case");
  for (const char* bad : {" 0", " x", " SINCEX 5", " SINCE", " SINCE 5x"})
    check(!parseDumpArgs(bad, newest, by_time, since), std::string("DUMP refuses '") + bad + "'");
  check(parseSinceEpoch(" 0", since) && since == 0, "SINCE 0 parses");
  check(parseSinceEpoch("  1800000000", since) && since == 1800000000UL, "SINCE <epoch> parses");
  check(parseSinceEpoch(" 4294967295", since) && since == 4294967295UL, "the largest 32-bit epoch parses");
  for (const char* bad : {"", " ", "1800000000", " 4294967296", " 18000000000", " 12x", " -1", " 1 2", "X 5"})
    check(!parseSinceEpoch(bad, since), std::string("SINCE refuses '") + bad + "'");
  LittleFS = Filesystem{};
}

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "all";
  if (group == "dump") { emitDump(); return 0; }
  if (group == "since-dir" && argc == 4) { // DUMP SINCE over the segments in a directory, for --since-helper
    LittleFS = Filesystem{};
    for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
      std::ifstream file(entry.path(), std::ios::binary);
      LittleFS.files["/" + entry.path().filename().string()] = std::string(std::istreambuf_iterator<char>(file), {});
    }
    DataLogger logger;
    check(logger.begin(SINCE_HEADER), "since-dir mounts");
    Serial.bytes.clear();
    Stream out;
    const DataLogger::SinceStats stats = logger.dumpSince(out, (uint32_t)std::stoul(argv[3]));
    std::cout << out.bytes;
    std::cerr << "start=" << stats.start << "\n";
    return 0;
  }
  check(group == "all" || group == "schema" || group == "logging" || group == "schema-failures" || group == "ble-write" ||
        group == "motion" || group == "fw-build" || group == "dump-since", "known group");
  if (group == "all" || group == "schema") { schemaChecks(); std::cout << "PASS schema\n"; }
  if (group == "all" || group == "logging") { loggingChecks(); std::cout << "PASS logging\n"; }
  if (group == "all" || group == "schema-failures") { schemaFailureChecks(); std::cout << "PASS schema-failures\n"; }
  if (group == "all" || group == "ble-write") { bleWriteChecks(); std::cout << "PASS ble-write\n"; }
  if (group == "all" || group == "motion") { motionChecks(); std::cout << "PASS motion\n"; }
  if (group == "all" || group == "fw-build") { fwBuildChecks(); std::cout << "PASS fw-build\n"; }
  if (group == "all" || group == "dump-since") { dumpSinceChecks(); std::cout << "PASS dump-since\n"; }
}
