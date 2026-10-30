#include <cerrno>
#include <functional>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "FreematicsBase.h"
#include "FreematicsGPS.h"
#include "json.hpp"

#define RTC_DATA_ATTR
#define PRINT 0
#define ENABLE_LOGGING 0
#define ENABLE_NETWORK 1
#define ENABLE_CELL 0
#include "../baseLogger/config.h"
#define FLAG_GNSS_USE_LINK 1

using json = nlohmann::json;
#include "../libraries/OBDWrapper/obd_vector.h"
using HeaderVector = std::vector<std::pair<std::string, std::string>>;
#include "header_budget.inc"
extern std::vector<OBDVector> obd_result_vector;
extern std::vector<OBDVector> g_upload_held;

struct ESPStub {
  unsigned long getFreeHeap() const;
  unsigned long getMaxAllocHeap() const;
  unsigned long getMaxAllocPsram() const;
  unsigned long getPsramSize() const;
  unsigned long getFreePsram() const;
} ESP;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

unsigned long fake_ms = 1000, last_active = 0, g_last_speed_ms = 0;
unsigned long millis() { return fake_ms; }
struct SerialStub {
  void printf(const char* text) { std::fputs(text, stdout); }
  template<typename... Args> void printf(const char* format, Args... args) { std::printf(format, args...); }
} Serial;

struct HostGPS : TinyGPS {
  void get_datetime(void* date, void* time, unsigned long* age) {
    unsigned long parsedDate, parsedTime;
    TinyGPS::get_datetime(&parsedDate, &parsedTime, age);
    const uint32_t date32 = parsedDate, time32 = parsedTime;
    if (date) std::memcpy(date, &date32, sizeof(date32));
    if (time) std::memcpy(time, &time32, sizeof(time32));
  }
} gps;
GPS_DATA gpsData{}, last_gd{};
bool gpsHasDecodedData = false;
class FreematicsESP32 {
public:
  byte m_flags = FLAG_GNSS_USE_LINK;
  CLink* link = nullptr;
  bool gpsGetData(GPS_DATA** pgd);
#include "gps_read_diagnostics_public.inc"
private:
#include "gps_read_diagnostics_private.inc"
};
#include "gps_get_data.inc"

struct FakeLink : CLink {
  std::string response;
  unsigned commandCalls = 0;
  int sendCommand(const char* command, char* buffer, int size, unsigned int) override {
    ++commandCalls;
    check(std::string(command) == "ATGPS\r", "GPS command");
    check(response.size() < static_cast<size_t>(size), "fake reply fits production buffer");
    std::memcpy(buffer, response.c_str(), response.size() + 1);
    return static_cast<int>(response.size());
  }
} link;
FreematicsESP32 sys;
GPS_DATA* gd = &gpsData;
bool g_gps_powered = false, is_currently_parked = false, frozen_valid = false;
// The drive rules' inputs (drive_state.h): a car that is on, with motion at every fix, unless a check says otherwise.
bool drive_unconfirmed = false, g_drive_gnss_refused = false, recently_charging = false;
float g_last_io36 = 13.5f;
unsigned long g_last_motion_ms = 0, g_last_obd_answer_ms = 0;
double frozen_lat = 0, frozen_lon = 0;
enum ClockSource { CLK_GPS, CLK_HTTP };
#include "utc_converter.inc"
time_t clock_epoch = 0;
struct RTCStub { uint32_t getEpoch() { return clock_epoch; } } rtc;
#include "clock_quality.inc"
void setClock(ClockSource, time_t epoch) { clock_epoch = (uint32_t)epoch; last_clock_sync_epoch = epoch; }
time_t parseHttpDate(const char*) { return 0; }
void esp_task_wdt_reset() {}
#include "telemetry_format.h"
#include "gps_location.h"
#include "motion_stats.h"
DriveMotion g_drive_motion;
#include "drive_state.h"
// The poll's frame, which logger.ino's block hands to driveGnss (#95); empty here, so discharge is unknown.
void gpsUnlatch(const OBDVector& current_frame = OBDVector{}) {
  g_last_motion_ms = fake_ms;
#include "gps_unlatch.inc"
}

struct HttpResponse { int responseCode; std::string payload; };
struct FakeNetwork {
  std::vector<json> received;
  int responseCode = 200;
  bool set_headers(const HeaderVector&) { return true; }
  std::string getDateHeader() { return {}; }
  HttpResponse sendBulk(const std::string& payload) {
    auto points = json::parse(payload);
    for (const auto& point : points) {
      if (point.contains("heading")) {
        const double heading = point["heading"]["degrees"].get<double>();
        check(heading > -360 && heading < 360, "uploaded heading meets backend range");
      }
    }
    if (responseCode == 200) for (const auto& point : points) received.push_back(point);
    return {responseCode, "{}"};
  }
} network;
#include "gzip_fake.h"
GzipOutcome gzipBody(const std::string&, std::string&) { return GZIP_UNAVAILABLE; }
int logging_errors = 0, g_last_http = 0, g_http0_streak = 0;
int g_upload_posts = 0, g_upload_accepted = 0, g_upload_frames_ok = 0; // logger.ino's FLUSHSTAT counters (#110)
unsigned long g_last_post_ms = 0;
std::string g_last_status, tlm_push_token = "test-token", api_key = "test-key";
std::vector<OBDVector> obd_result_vector;
unsigned long upload_capped = 0;
unsigned long upload_heap_dropped = 0;
#define MAX_UPLOAD_BUFFER 120
#include "tlm_signature.h"
#include "network_upload.h"
#include "backlog_fs.h"
#include "geo_backlog.h"
#include "backlog.inc"
namespace historical {
std::vector<OBDVector> obd_result_vector;
#include "historical_format.inc"
#include "historical_backlog.inc"
}

#include "upload_heap_fixture.h"

size_t backlog_read_count = 0;
bool backlog_read_bounds_ok = true;
bool backlog_queue_grew_during_read = false;
void observeBacklogRead() {
  if (!heap_fixture_enabled) return;
  ++backlog_read_count;
  if (obd_result_vector.size() > 40 || ESP.getFreeHeap() < 32768 ||
      ESP.getMaxAllocHeap() < 16384) backlog_read_bounds_ok = false;
  if (backlog_read_count > 2 && obd_result_vector.size() >= 2)
    backlog_queue_grew_during_read = true;
}

enum ResetReason { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_WDT, ESP_RST_DEEPSLEEP };
ResetReason reset_reason = ESP_RST_POWERON;

void resetFixture() {
  fake_ms = 1000;
  reset_reason = ESP_RST_POWERON;
  gps_location_source = "omitted";
  LittleFS.files.clear();
  LittleFS.fail_open = false;
  LittleFS.read_observer = nullptr;
  backlog_read_count = 0;
  backlog_read_bounds_ok = true;
  backlog_queue_grew_during_read = false;
  historical::obd_result_vector.clear();
  clock_epoch = 0;
  last_clock_sync_epoch = 0;
  geo_accepted = GPS_DATA{};
  geo_accepted_utc = park_acc_utc = frozen_geo_utc = 0;
  park_acc_unknown_utc = false;
  frozen_geo_lat = frozen_geo_lon = 0;
  g_geo_accepted = g_geo_queued = {};
  g_geo_queued_seeded = true;
  g_backlog_unread = false;
  last_active = g_last_speed_ms = 0;
  gps = HostGPS{};
  gpsData = last_gd = GPS_DATA{};
  gd = &gpsData;
  gpsHasDecodedData = false;
  sys = FreematicsESP32{};
  sys.link = &link;
  link.response.clear();
  link.commandCalls = 0;
  g_gps_powered = is_currently_parked = frozen_valid = false;
  frozen_lat = frozen_lon = 0;
  gps_anchor_date = gps_anchor_time = 0;
  gps_anchor_ms = gps_run_ms = gps_alt_ref_ms = 0;
  gps_alt_ref = 0;
  gps_run_last = GPS_DATA{};
  gps_run_n = gps_unconfirmed_n = 0;
  gps_fix_date = gps_fix_time = gps_fix_seq = 0;
  gps_fix_changed_ms = park_last_ts = 0;
  park_acc_lat = park_acc_lon = 0;
  park_acc_n = gps_moving_streak = 0;
  network.received.clear();
  network.responseCode = 200;
  std::vector<OBDVector>().swap(obd_result_vector);
  std::vector<OBDVector>().swap(g_upload_held);
  upload_capped = upload_heap_dropped = 0;
  resetHeapFixture();
  logging_errors = g_last_http = g_http0_streak = 0;
  g_last_post_ms = 0;
  g_last_status.clear();
}

bool readLink(const std::string& reply) {
  sys.link = &link;
  link.response = reply;
  return pollGpsData();
}

// The wake's first fix is held until a second fresh fix agrees, so a check about something else first sends
// the same fix one receiver unit earlier. The first-fix rule itself is checked in gps-jump with readLink().
void confirmFirstFix(const std::string reply, const std::string& label) {
  if (gpsJumpAnchor(gpsData.lat, gpsData.lng) || reply.rfind("$GNIFO,", 0) != 0) return;
  std::vector<std::string> fields;
  std::stringstream stream(reply.substr(7));
  for (std::string field; std::getline(stream, field, ',');) fields.push_back(field);
  if (fields.size() < 4 || !gpsJumpAnchor(std::atol(fields[2].c_str()) / 1e6f, std::atol(fields[3].c_str()) / 1e6f))
    return;
  std::string echo = "$GNIFO," + fields[0] + "," + std::to_string(std::atol(fields[1].c_str()) - 1);
  for (size_t i = 2; i < fields.size(); ++i) echo += "," + fields[i];
  check(!readLink(echo) && sys.gpsReadDiagnostics().result == GPSReadResult::Unconfirmed, label + ": first fix held");
}

GPS_DATA acceptLink(const std::string& reply, const std::string& label) {
  confirmFirstFix(reply, label);
  const unsigned commandCalls = link.commandCalls;
  check(readLink(reply), label + " accepted");
  check(link.commandCalls == commandCalls + 1, label + " sends only one ATGPS request");
  return *gd;
}

void checkPreserved(const GPS_DATA& lastGood, const std::string& label) {
  check(gd->heading == GPS_HEADING_INVALID, label + " clears previous heading");
  check(gd->lat == lastGood.lat && gd->lng == lastGood.lng && gd->alt == lastGood.alt,
        label + " preserves last-good coordinates and altitude");
  check(gd->speed == lastGood.speed && gd->sat == lastGood.sat && gd->hdop == lastGood.hdop,
        label + " preserves last-good speed and quality");
  check(gd->ts == lastGood.ts && gd->sentences == lastGood.sentences && gd->errors == lastGood.errors,
        label + " preserves last-good timestamp and counters");
  check(gd->date == lastGood.date && gd->time == lastGood.time,
        label + " preserves last-good date/time: got " + std::to_string(gd->date) + "/" +
        std::to_string(gd->time) + ", expected " + std::to_string(lastGood.date) + "/" +
        std::to_string(lastGood.time));
}

void rejectLink(const std::string& reply, const GPS_DATA& lastGood, const std::string& label) {
  link.response = reply;
  const unsigned commandCalls = link.commandCalls;
  check(!pollGpsData(), label + " rejected");
  check(link.commandCalls == commandCalls + 1, label + " sends only one ATGPS request");
  checkPreserved(lastGood, label);
}

std::string requiredReply(const std::vector<std::string>& fields) {
  std::string reply = "$GNIFO,";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) reply += ',';
    reply += fields[i];
  }
  return reply;
}

uint16_t linkHeading(const std::string& raw) {
  gpsData = GPS_DATA{};
  gpsData.heading = 123;
  sys.link = &link;
  const std::string reply = "$GNIFO,41026,110757,59329000,18068000,2500,2000," + raw + ",12,7\r";
  confirmFirstFix(reply, "heading " + raw);
  check(readLink(reply), "numeric reply retains position");
  check(gpsData.sat == 12 && gpsData.hdop == 7, "fields after course retained");
  return gpsData.heading;
}

void checkDiagnostics(GPSReadResult result, uint16_t bytes, int8_t field, const std::string& label) {
  const auto& diagnostics = sys.gpsReadDiagnostics();
  check(diagnostics.result == result, label + " diagnostic result");
  check(diagnostics.reply_bytes == bytes, label + " diagnostic reply bytes: got " +
        std::to_string(diagnostics.reply_bytes) + ", expected " + std::to_string(bytes));
  check(diagnostics.field == field, label + " diagnostic field: got " +
        std::to_string(diagnostics.field) + ", expected " + std::to_string(field));
}

void gpsDiagnostics() {
  checkDiagnostics(GPSReadResult::NotAttempted, 0, -1, "initial");
  check(linkHeading("9000") == 90, "diagnostic seed parses valid course");
  checkDiagnostics(GPSReadResult::Accepted, link.response.size(), -1, "accepted link read");
  const GPS_DATA lastGood = *gd;
  const unsigned commandCalls = link.commandCalls;
  sys.link = nullptr;
  check(!sys.gpsGetData(&gd), "diagnostic missing link rejected");
  check(link.commandCalls == commandCalls, "missing link sends no request");
  checkPreserved(lastGood, "diagnostic missing link");
  checkDiagnostics(GPSReadResult::LinkUnavailable, 0, -1, "missing link after accepted read");
  sys.link = &link;
  rejectLink("", lastGood, "diagnostic empty reply");
  checkDiagnostics(GPSReadResult::NoReply, 0, -1, "empty reply");
  for (const std::string reply : {"OK\r", "$GNIFO", "$GNGGA,71026,4490300\r"}) {
    rejectLink(reply, lastGood, "diagnostic missing marker");
    checkDiagnostics(GPSReadResult::MarkerMissing, reply.size(), -1, "missing marker");
  }
  const std::vector<std::string> valid{"41026", "110757", "59329000", "18068000", "2500", "2000"};
  for (size_t i = 0; i < valid.size(); ++i) {
    for (const std::string token : {"", "abc", "123x", "1.5", "+", "-", "2147483648", "-2147483649"}) {
      auto fields = valid;
      fields[i] = token;
      const std::string reply = requiredReply(fields) + ",18000,13,8\r";
      rejectLink(reply, lastGood, "diagnostic invalid required field");
      checkDiagnostics(GPSReadResult::RequiredFieldInvalid, reply.size(), i,
                       "required field " + std::to_string(i) + " token '" + token + "'");
    }
  }
  for (const std::string reply : {"$GNIFO,71026,4490300,55738990,18069000,2600,2100,18000,13,8\r",
                                 "$GNIFO,71026,4490400,0,0,2600,2200,27000,14,9\r"}) {
    rejectLink(reply, lastGood, "diagnostic coordinate rejection");
    checkDiagnostics(GPSReadResult::CoordinateRejected, reply.size(), -1, "rejected coordinates");
  }
  for (size_t i = 0; i < valid.size(); ++i) {
    auto fields = valid;
    fields.resize(i);
    const std::string reply = requiredReply(fields) + (i ? "," : "");
    rejectLink(reply, lastGood, "diagnostic truncated required field");
    checkDiagnostics(GPSReadResult::RequiredFieldInvalid, reply.size(), i,
                     "truncated field " + std::to_string(i));
  }
  rejectLink("$GNIFO,71026,4490300,55738990", lastGood, "diagnostic truncated latitude delimiter");
  checkDiagnostics(GPSReadResult::RequiredFieldInvalid, link.response.size(), 2, "latitude delimiter");
  rejectLink("OK\r", lastGood, "marker failure after required failure");
  checkDiagnostics(GPSReadResult::MarkerMissing, link.response.size(), -1, "reset invalid field");
  rejectLink("$GNIFO,", lastGood, "required failure before missing link");
  sys.link = nullptr;
  check(!sys.gpsGetData(nullptr), "null output missing link read rejected");
  checkPreserved(lastGood, "null output missing link");
  checkDiagnostics(GPSReadResult::LinkUnavailable, 0, -1, "reset bytes and field");
  const auto recovered = acceptLink("OK\r\n$GNIFO,71026,4490500,59330000,18069000,2600,2100,18000,13,8\r",
                                    "diagnostic accepted recovery");
  checkDiagnostics(GPSReadResult::Accepted, link.response.size(), -1, "recovery with reply prefix");
  check(recovered.date == 71026 && recovered.time == 4490500 && recovered.alt == 26 &&
        recovered.speed == 21 && recovered.heading == 180 && recovered.sat == 13 && recovered.hdop == 8,
        "diagnostic recovery publishes coherent required fields and quality");
  rejectLink("", recovered, "no reply after recovery");
  checkDiagnostics(GPSReadResult::NoReply, 0, -1, "reset previous reply byte count");
  gpsData = GPS_DATA{};
  const auto zero = acceptLink("$GNIFO,41026,110757,0,0,2500,0,,12,7\r", "diagnostic cold zero position");
  check(zero.lat == 0 && zero.lng == 0 && zero.speed == 0 && zero.heading == GPS_HEADING_INVALID &&
        zero.sat == 12 && zero.hdop == 7, "accepted zero-position reply retains existing optional behavior");
  checkDiagnostics(GPSReadResult::Accepted, link.response.size(), -1, "accepted zero position");
}

void linkRange() {
  const std::pair<std::string, uint16_t> cases[] = {
    {"0", 0}, {"9000", 90}, {"35999", 359}, {"36000", 0},
    {"-9000", 270}, {"-1", 359}, {"-35999", 0},
    {"", GPS_HEADING_INVALID}, {"36001", GPS_HEADING_INVALID},
    {"36099", GPS_HEADING_INVALID}, {"36100", GPS_HEADING_INVALID},
    {"-36000", GPS_HEADING_INVALID}, {"-36001", GPS_HEADING_INVALID},
    {"65535", GPS_HEADING_INVALID}, {"2147483648", GPS_HEADING_INVALID},
    {"-2147483649", GPS_HEADING_INVALID}, {"999999999999999999999999999999", GPS_HEADING_INVALID},
  };
  for (const auto& entry : cases) {
    const uint16_t actual = linkHeading(entry.first);
    check(actual == entry.second, "raw heading " + entry.first + ": got " + std::to_string(actual) +
          ", expected " + std::to_string(entry.second));
  }
}

void linkFailures() {
  for (const std::string reply : {"", "OK\r", "$GNIFO", "$GNGGA,71026,4490300\r"}) {
    resetFixture();
    check(linkHeading("9000") == 90, "seed valid moving course");
    const GPS_DATA lastGood = *gd;
    fake_ms += 5000;
    rejectLink(reply, lastGood, "transport/no-GNIFO reply");
    OBDVector frame{{"utc", 1791500000}, {"soc", 50}};
    addGpsData(frame);
    const auto point = json::parse(bulkStringV2({frame}, 1791500000))[0];
    check(!point.contains("heading") && point.contains("location") && point["soc"]["frac"] == 0.5,
          "failed reply preserves position and battery, never uploads stale heading");
  }
  linkHeading("9000");
  const GPS_DATA lastGood = *gd;
  sys.link = nullptr;
  check(!pollGpsData(), "missing link rejected");
  checkPreserved(lastGood, "missing link");
}

void linkTruncated() {
  check(linkHeading("9000") == 90, "seed valid moving fix");
  const GPS_DATA lastGood = *gd;
  link.response = "$GNIFO,71026,4490300,55738990";
  check(!pollGpsData(), "truncated timestamp-bearing reply rejected");
  checkPreserved(lastGood, "truncated reply");
  for (const std::string reply : {"$GNIFO,", "$GNIFO,71026", "$GNIFO,71026,4490300",
                                 "$GNIFO,71026,4490300,59330000,",
                                 "$GNIFO,71026,4490300,59330000,18069000,"}) {
    linkHeading("9000");
    const GPS_DATA accepted = *gd;
    fake_ms += 5000;
    rejectLink(reply, accepted, "truncated required prefix");
  }
}

void linkRequired() {
  check(linkHeading("9000") == 90, "seed valid moving fix");
  const GPS_DATA lastGood = *gd;
  link.response = "$GNIFO,71026,4490300,59330000,18069000,2600\r";
  check(!pollGpsData(), "altitude-only reply without speed rejected");
  checkPreserved(lastGood, "altitude-only reply");
  const std::vector<std::string> valid{"71026", "4490300", "59330000", "18069000", "2600", "2100"};
  const std::string names[] = {"date", "time", "latitude", "longitude", "altitude", "speed"};
  const std::vector<std::string> malformed{"", "abc", "123x", "1.5", "+", "-", "2147483648",
                                           "-2147483649", "999999999999999999999999999999"};
  for (size_t i = 2; i <= 3; ++i) {
    linkHeading("9000");
    const GPS_DATA before = *gd;
    auto fields = valid;
    fields[i] += 'x';
    rejectLink(requiredReply(fields) + ",9000,13,8\r", before, "nearby prefix-junk " + names[i]);
    for (const auto& token : malformed) {
      resetFixture();
      const GPS_DATA empty = *gd;
      fields = valid;
      fields[i] = token;
      rejectLink(requiredReply(fields) + ",9000,13,8\r", empty, "cold-cache " + names[i] + " token '" + token + "'");
    }
  }
  for (size_t i = 0; i < valid.size(); ++i) {
    linkHeading("9000");
    const GPS_DATA accepted = *gd;
    fake_ms += 5000;
    auto missing = valid;
    missing.resize(i);
    rejectLink(requiredReply(missing), accepted, "missing " + names[i]);
    for (const auto& token : malformed) {
      linkHeading("9000");
      const GPS_DATA before = *gd;
      fake_ms += 5000;
      auto fields = valid;
      fields[i] = token;
      rejectLink(requiredReply(fields) + ",9000,13,8\r", before, names[i] + " token '" + token + "'");
    }
  }
}

void linkAtomic() {
  check(linkHeading("9000") == 90, "seed cached home fix");
  const GPS_DATA lastGood = *gd;
  fake_ms += 5000;
  rejectLink("$GNIFO,71026,4490300,55738990,18069000,2600,2100,18000,13,8\r",
             lastGood, "complete coordinate outlier");
  fake_ms += 5000;
  rejectLink("$GNIFO,71026,4490400,0,0,2600,2200,27000,14,9\r", lastGood, "cached-home zero coordinates");
  fake_ms += 5000;
  const auto recovered = acceptLink("$GNIFO,71026,4490500,59338000,18078000,2600,2100,18000,13,8\r",
                                    "nearby one-kilometre recovery");
  check(recovered.date == 71026 && recovered.time == 4490500 && recovered.ts == fake_ms,
        "recovery publishes new date/time and receive timestamp");
  check(std::abs(recovered.lat - 59.338) < 0.00001 && std::abs(recovered.lng - 18.078) < 0.00001 &&
        recovered.alt == 26 && recovered.speed == 21 && recovered.heading == 180 &&
        recovered.sat == 13 && recovered.hdop == 8, "recovery publishes coherent complete fix");
}

void linkValid() {
  const auto stationary = acceptLink("$GNIFO,41026,110757,59329000,18068000,2500,0,9000,12,7\r",
                                    "stationary zero-speed fix");
  check(stationary.speed == 0 && stationary.heading == 90 && stationary.alt == 25,
        "numeric zero speed is accepted, not missing");
  gpsData = GPS_DATA{};
  const auto signedFix = acceptLink("$GNIFO,+41026,+110757,-59329000,-18068000,-2500,+2000,-9000,12,7\r",
                                   "signed coordinates and altitude");
  check(std::abs(signedFix.lat + 59.329) < 0.00001 && std::abs(signedFix.lng + 18.068) < 0.00001 &&
        signedFix.alt == -25 && signedFix.speed == 20 && signedFix.heading == 270,
        "signed integer fix retains coordinates, altitude, speed and normalized course");
  const struct { std::string raw; uint16_t heading; uint8_t sat, hdop; } tails[] = {
    {"", GPS_HEADING_INVALID, 12, 7}, {"\r", GPS_HEADING_INVALID, 12, 7}, {"\n", GPS_HEADING_INVALID, 12, 7},
    {",", GPS_HEADING_INVALID, 12, 7}, {",9000", GPS_HEADING_INVALID, 12, 7},
    {",,13,8\r", GPS_HEADING_INVALID, 13, 8}, {",bad,13,8\r", GPS_HEADING_INVALID, 13, 8},
    {",36001,13,8\r", GPS_HEADING_INVALID, 13, 8}, {",9000,13", 90, 13, 7}, {",9000,13,8\r", 90, 13, 8},
    {",9000,bad,8\r", 90, 0, 8}, {",9000,13,bad\r", 90, 13, 0},
  };
  for (const auto& tail : tails) {
    resetFixture();
    linkHeading("9000");
    fake_ms += 5000;
    const auto accepted = acceptLink("$GNIFO,71026,4490300,59330000,18069000,2600,2100" + tail.raw,
                                     "complete required fix with optional tail");
    check(accepted.date == 71026 && accepted.time == 4490300 && accepted.ts == fake_ms &&
          std::abs(accepted.lat - 59.330) < 0.00001 && std::abs(accepted.lng - 18.069) < 0.00001 &&
          accepted.alt == 26 && accepted.speed == 21 && accepted.heading == tail.heading,
          "optional tail never discards valid required position/speed: " + tail.raw);
    check(accepted.sat == tail.sat && accepted.hdop == tail.hdop,
          "optional quality retains legacy conversion/defaults: " + tail.raw);
    OBDVector frame{{"utc", 1791500000}, {"soc", 50}};
    addGpsData(frame);
    const auto point = json::parse(bulkStringV2({frame}, 1791500000))[0];
    check(point.contains("location") && point["soc"]["frac"] == 0.5 &&
          point.contains("heading") == (tail.heading != GPS_HEADING_INVALID),
          "optional heading validity only controls heading emission");
  }
}

void parkedFixture() {
  g_gps_powered = is_currently_parked = frozen_valid = true;
  acceptLink("$GNIFO,31026,11070000,59320000,18060000,2500,0,9000,12,7\r", "accepted parking fix");
  freezeParkedLocation();
}

OBDVector gpsFrame() {
  parkAccumSample();
  OBDVector frame{{"utc", 1791500000}, {"soc", 50}};
  addGpsData(frame);
  gpsUnlatch();
  frame.emplace_back("is_parked", is_currently_parked ? 1.0 : 0.0);
  return frame;
}

json checkState(const OBDVector& frame, bool parked, const std::string& label) {
  const auto point = json::parse(bulkStringV2({frame}, 1791500000))[0];
  check(is_currently_parked == parked && point["drivingState"]["state"] == (parked ? "PARK" : "DRIVE") &&
        json::parse(bleTlmString(frame))["tlm"]["is_parked"] == parked,
        label + " preserves observable latch and serialized PARK/DRIVE state");
  check(point["soc"]["frac"] == 0.5, label + " preserves battery telemetry");
  return point;
}

void checkLocation(const json& point, double lat, double lng, const std::string& label) {
  check(point.contains("location") && std::abs(point["location"]["lat"].get<double>() - lat) < 0.00001 &&
        std::abs(point["location"]["long"].get<double>() - lng) < 0.00001, label + " emitted location");
}

void gpsReplay() {
  parkedFixture();
  check(linkHeading("9000") == 90, "seed first moving fix");
  const std::string firstReply = link.response;
  const auto first = checkState(gpsFrame(), true, "first moving fix");
  checkLocation(first, gd->lat, gd->lng, "first moving fix uses live coordinates while PARK");
  check(!first.contains("heading") && last_active == 0 && g_last_speed_ms == 0,
        "first moving fix does not unlatch or extend leash");
  fake_ms += 5000;
  acceptLink(firstReply, "same-record replay");
  const auto replay = checkState(gpsFrame(), true, "same-record replay");
  checkLocation(replay, gd->lat, gd->lng, "same-record replay remains live PARK");
  check(gd->ts == fake_ms && gpsFixAgeMs() == 5000 && !gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) &&
        last_active == 0 && g_last_speed_ms == 0, "replay is not a second fresh movement sample");
  fake_ms += 5000;
  acceptLink("$GNIFO,41026,110758,59330000,18069000,2600,2000,9000,12,7\r", "next distinct moving fix");
  const auto second = checkState(gpsFrame(), false, "next distinct moving fix");
  checkLocation(second, gd->lat, gd->lng, "next distinct moving fix uses live coordinates and DRIVE");
  check(gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) && last_active == fake_ms && g_last_speed_ms == fake_ms,
        "second distinct moving fix refreshes real production GNSS leash");
  const unsigned long lastMovement = fake_ms;
  const std::string secondReply = link.response;
  fake_ms += GPS_MOVING_MAX_AGE_MS + 1;
  acceptLink(secondReply, "aged moving replay");
  checkState(gpsFrame(), false, "aged replay leaves existing latch unchanged");
  check(!gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) && gpsFixAgeMs() == GPS_MOVING_MAX_AGE_MS + 1 &&
        last_active == lastMovement && g_last_speed_ms == lastMovement,
        "replayed receiver timestamp cannot keep the GNSS leash fresh");
}

void gpsRejected() {
  parkedFixture();
  linkHeading("9000");
  checkState(gpsFrame(), true, "first moving fix before rejection");
  const GPS_DATA lastGood = *gd;
  const std::string invalid[] = {
    "$GNIFO,71026,4490300,55738990",
    "$GNIFO,71026,4490400,59330000,18069000,2600\r",
    "$GNIFO,71026,4490500,55738990,18069000,2600,2200,18000,13,8\r",
    "$GNIFO,71026,4490600,0,0,2600,2200,18000,13,8\r",
  };
  for (const auto& reply : invalid) {
    fake_ms += 5000;
    rejectLink(reply, lastGood, "invalid timestamp-bearing movement reply");
    const auto point = checkState(gpsFrame(), true, "invalid reply after one moving fix");
    checkLocation(point, lastGood.lat, lastGood.lng, "invalid reply retains accepted live location");
    check(!point.contains("heading") && !gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) &&
          gpsFixAgeMs() == fake_ms - lastGood.ts && last_active == 0 && g_last_speed_ms == 0,
          "invalid reply is not a second fresh fix and cannot extend leash");
  }
  fake_ms += 5000;
  acceptLink("$GNIFO,41026,110758,59330000,18069000,2600,2000,9000,12,7\r", "fresh recovery");
  checkState(gpsFrame(), false, "fresh recovery confirms driving");
  const GPS_DATA recovered = *gd;
  const unsigned long lastMovement = fake_ms;
  fake_ms += GPS_MOVING_MAX_AGE_MS + 1;
  rejectLink(invalid[0], recovered, "invalid reply after movement expires");
  const auto expired = checkState(gpsFrame(), false, "expired movement leaves latch unchanged");
  check(!expired.contains("heading") && !gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) &&
        gpsFixAgeMs() == GPS_MOVING_MAX_AGE_MS + 1 && last_active == lastMovement &&
        g_last_speed_ms == lastMovement, "invalid new timestamp cannot revive stale confirmed movement");
}

void gpsStationary() {
  parkedFixture();
  acceptLink("$GNIFO,41026,110757,59329000,18068000,2500,0,9000,12,7\r", "first stationary fix");
  const auto first = checkState(gpsFrame(), true, "first stationary fix");
  checkLocation(first, frozen_lat, frozen_lon, "stationary fix uses frozen parking location");
  fake_ms += 5000;
  acceptLink("$GNIFO,41026,110758,59330000,18069000,2600,0,18000,13,8\r", "fresh stationary coordinates");
  const auto fresh = checkState(gpsFrame(), true, "fresh stationary fix");
  checkLocation(fresh, frozen_lat, frozen_lon, "fresh zero-speed coordinates still use frozen location");
  check(std::abs(gd->lat - 59.330) < 0.00001 && std::abs(gd->lng - 18.069) < 0.00001 &&
        gd->time == 110758 && gd->speed == 0 && gpsFixAgeMs() == 0,
        "fresh stationary receiver record is accepted and ages independently of published frozen location");
  check(!first.contains("heading") && !fresh.contains("heading") && !gpsMovingFresh(GPS_MOVING_MAX_AGE_MS) &&
        last_active == 0 && g_last_speed_ms == 0, "stationary fixes neither emit heading nor unlatch parked");

  // UNCONFIRMED (#6): still parked for every guard, but the live fix is published and the frame says nothing of PARK/DRIVE.
  drive_unconfirmed = true;
  OBDVector frame{{"utc", 1791500000}, {"soc", 50}};
  addGpsData(frame);
  const auto unconfirmed = json::parse(bulkStringV2({frame}, 1791500000))[0];
  drive_unconfirmed = false;
  checkLocation(unconfirmed, gd->lat, gd->lng, "UNCONFIRMED publishes the live fix, not the frozen spot");
}

// Fresh zero-speed fixes in GNIFO 1e6 units, one receiver second apart from 11:08:00; lat_step walks them.
int stationary_second = 0;
void stationaryFixes(int count, long lat, long lng, const std::string& label, int hdop = 7,
                     const std::string& date = "41026", long lat_step = 0) {
  for (int i = 0; i < count; ++i) {
    fake_ms += 5000;
    const int second = stationary_second++;
    acceptLink("$GNIFO," + date + "," + std::to_string(11080000 + second / 60 * 10000 + second % 60 * 100) + "," +
               std::to_string(lat + i * lat_step) + "," + std::to_string(lng) + ",2500,0,9000,12," +
               std::to_string(hdop) + "\r", label);
    gpsFrame();
  }
}

void gpsRelocate() {
  const long far = 59338000, lng = 18060000;  // ~2 km north of parkedFixture's 59.32, 18.06
  auto keepsFrozen = [](const std::string& label) {
    checkLocation(checkState(gpsFrame(), true, label), 59.32, 18.06, label + ": frozen spot kept");
  };
  auto fresh = [](const std::function<void()>& setup) { resetFixture(); stationary_second = 0; setup(); };

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MIN - 1, far, lng, "short far average");
  keepsFrozen("fewer than PARK_ACC_MIN far fixes");
  stationaryFixes(1, far, lng, "full far average");
  auto point = checkState(gpsFrame(), true, "relocated parked spot");
  checkLocation(point, 59.338, 18.06, "full stationary average 2 km away relocates the frozen spot");
  check(point["location"]["time"] == "2026-10-04T11:08:11Z" && !point.contains("heading") &&
        last_active == 0 && g_last_speed_ms == 0, "relocation keeps newest contributor UTC and does not unlatch");

  fresh(parkedFixture);
  stationaryFixes(5, far, lng, "far before bad date");
  stationaryFixes(1, far, lng, "far bad date", 7, "310426");
  stationaryFixes(6, far, lng, "far after bad date");
  point = checkState(gpsFrame(), true, "relocated with unknown contributor time");
  check(!point.contains("location") && point["soc"]["frac"] == 0.5,
        "relocation with an unknown contributor time omits geo but keeps battery");

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MIN * 2, 59320270, lng, "near average");
  keepsFrozen("stationary average 30 m away");

  fresh(parkedFixture);
  stationaryFixes(1, far, lng, "replay seed");
  const std::string replay = link.response;
  for (int i = 0; i < PARK_ACC_MIN * 2; ++i) {
    fake_ms += 5000;
    acceptLink(replay, "replayed far fix");
    gpsFrame();
  }
  keepsFrozen("one replayed far fix");

  fresh(parkedFixture);
  is_currently_parked = false;
  stationaryFixes(PARK_ACC_MIN, far, lng, "unlatched far average");
  is_currently_parked = true;
  keepsFrozen("stops while unlatched are left to the park transition");

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MIN * 2, far, lng, "poor-HDOP far average", PARK_RELOCATE_MAX_HDOP + 1);
  keepsFrozen("far fixes with poor HDOP");

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MIN * 2, far, lng, "HDOP 3.00 overflowing uint8_t", 300);
  keepsFrozen("far fixes whose HDOP overflows the byte");

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MIN * 2, far, lng, "4 km/h crawl", 7, "41026", 50);
  keepsFrozen("zero-speed fixes crawling 5.6 m per 5 s poll");

  fresh(parkedFixture);
  stationaryFixes(PARK_ACC_MAX, 59324300, lng, "saturating average ~480 m away");
  stationaryFixes(PARK_ACC_MIN, 59324700, lng, "next average ~520 m away");
  checkLocation(checkState(gpsFrame(), true, "after saturation"), 59.3247, 18.06,
                "a saturated parked average restarts and can still relocate");

  fresh([] { g_gps_powered = is_currently_parked = true; });
  stationaryFixes(PARK_ACC_MIN, far, lng, "cold-boot average");
  point = checkState(gpsFrame(), true, "cold-boot average");
  checkLocation(point, 59.338, 18.06, "cold boot freezes a full stopped average");
  check(frozen_valid && std::string(gpsLocationSource()) == "frozen", "cold-boot average becomes the frozen spot");
}

// A GNIFO record `second` receiver seconds after 11:10:00: coordinates in 1e6 units, altitude in cm, then
// speed, course, satellites and HDOP. The default is a good moving fix.
std::string jumpFix(int second, long lat, long lng, long alt = 2500, const std::string& rest = "2000,9000,12,7") {
  const int t = 11100000 + second / 60 * 10000 + second % 60 * 100;
  return "$GNIFO,41026," + std::to_string(t) + "," + std::to_string(lat) + "," + std::to_string(lng) + "," +
         std::to_string(alt) + "," + rest + "\r";
}

int jumpReanchors = 0;
GPS_DATA jumpAccept(const std::string& reply, const std::string& label) {
  const auto fix = acceptLink(reply, label);
  checkDiagnostics(GPSReadResult::Accepted, reply.size(), -1, label);
  jumpReanchors += sys.gpsReadDiagnostics().reanchored;
  return fix;
}

void jumpReject(const std::string& reply, const GPS_DATA& lastGood, const std::string& label, int8_t field = -1) {
  rejectLink(reply, lastGood, label);
  checkDiagnostics(GPSReadResult::CoordinateRejected, reply.size(), field, label);
  check(!sys.gpsReadDiagnostics().reanchored, label + " does not re-anchor");
}

void gpsJump() {
  const long lng = 18069000, step = 1350;   // ~150 m north per 5 s poll
  auto fresh = [] { resetFixture(); jumpReanchors = 0; };

  fresh();
  const auto bad = jumpAccept(jumpFix(0, 55738990, lng), "cold-start anchor ~400 km off");
  for (int k = 1; k <= 2; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(5 * k, 59330000 + step * k, lng), bad, "good fix behind a bad anchor");
  }
  fake_ms += 5000;
  jumpAccept(jumpFix(15, 59330000 + step * 3, lng), "third consistent fresh fix");
  check(sys.gpsReadDiagnostics().reanchored && sys.gpsReadDiagnostics().reanchor_rejects == 2 &&
        sys.gpsReadDiagnostics().reanchor_dist_m > 390000, "third consistent fix re-anchors and reports why");
  for (int k = 4; k <= 8; ++k) {
    fake_ms += 5000;
    jumpAccept(jumpFix(5 * k, 59330000 + step * k, lng), "stream after re-anchor");
  }
  check(jumpReanchors == 1, "a bad cold-start anchor re-anchors exactly once");

  fresh();
  GPS_DATA last = jumpAccept(jumpFix(0, 59330000, lng), "good anchor");
  for (int k = 1; k <= 3; ++k) {
    fake_ms += 5000;
    last = jumpAccept(jumpFix(5 * k, 59330000 + step * k, lng), "good stream");
  }
  fake_ms += 5000;
  jumpReject(jumpFix(20, 59780000, lng), last, "single fresh spike 50 km off");
  fake_ms += 5000;
  jumpAccept(jumpFix(25, 59330000 + step * 5, lng), "on-track fix after a spike");
  check(jumpReanchors == 0, "a single spike never re-anchors");

  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "good anchor");
  for (int k = 1; k <= 5; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(5, 55738990, lng), last, "replayed far reply");
  }
  check(jumpReanchors == 0, "a replayed far reply never re-anchors");

  fresh();
  const std::string tunnel = jumpFix(0, 59330000, lng);
  jumpAccept(tunnel, "anchor before a tunnel");
  for (int k = 0; k < 120; ++k) {
    fake_ms += 5000;
    jumpAccept(tunnel, "co-processor re-echo in a tunnel");
  }
  fake_ms += 5000;
  jumpAccept(jumpFix(605, 59330000, 18209500), "fresh fix 8 km east after 10 min of echoes");
  check(jumpReanchors == 0, "the tunnel exit is accepted on distance, not by re-anchoring");

  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "good anchor");
  for (int k = 1; k <= 3; ++k) {
    fake_ms += 61000;
    jumpReject(jumpFix(61 * k, 55738990 + 900 * k, lng), last, "sparse self-consistent far fix");
  }
  check(jumpReanchors == 0, "fresh far fixes more than 60 s apart never re-anchor");

  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "good anchor");
  for (int k = 1; k <= 3; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(5 * k, 55738990 + 450000 * k, lng), last, "scattered fresh far fix");
  }
  check(jumpReanchors == 0, "fresh far fixes 50 km apart from each other never re-anchor");

  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "good anchor");
  fake_ms += 5000;
  jumpReject(jumpFix(5, 55738990, lng), last, "far spike");
  fake_ms += 5000;
  last = jumpAccept(jumpFix(10, 59330000 + step, lng), "on-track fix between spikes");
  for (int k = 3; k <= 4; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(5 * k, 55738990, lng), last, "spike consistent with the first");
  }
  check(jumpReanchors == 0, "an accepted fix restarts the re-anchor run");

  // #59: after a fix loss, two fresh fixes at ~181 km/h, HDOP 255, horizontally plausible from the anchor and
  // from each other, but far off vertically. Altitude is the only thing that gives them away.
  const std::string bogus = "9773,9000,5,255";
  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "crawl anchor");
  fake_ms += 90000;
  jumpReject(jumpFix(90, 59348000, lng, 2500 + 240000, bogus), last, "bogus fix after a loss", 4);
  fake_ms += 7000;
  jumpReject(jumpFix(97, 59351400, lng, 2500 + 300000, "9759,9000,6,255"), last, "second bogus fix", 4);
  fake_ms += 5000;
  jumpReject(jumpFix(102, 59354800, lng, 2500 + 368000, "9760,9000,6,255"), last, "third bogus fix", 4);
  fake_ms += 5000;
  jumpAccept(jumpFix(107, 59330000 + step, lng), "good fix after the bogus run");
  check(jumpReanchors == 0, "a run that is consistent only horizontally never re-anchors");

  fresh();
  last = jumpAccept(jumpFix(0, 59330000, lng), "anchor before a long loss");
  fake_ms += 600000;
  jumpReject(jumpFix(600, 59348000, lng, 2500 + 240000, bogus), last, "bogus fix after 10 min", 4);

  fresh();
  jumpAccept(jumpFix(0, 59330000, lng), "garage anchor");
  fake_ms += 300000;
  jumpAccept(jumpFix(300, 59340000, lng, 2500 + 130000), "fresh fix 1300 m higher after a 5-minute climb");
  fake_ms += 5000;
  last = jumpAccept(jumpFix(305, 59340000 + step, lng, 0), "2D fix reporting altitude 0");
  fake_ms += 5000;
  jumpReject(jumpFix(310, 59340000 + 2 * step, lng, 2500 + 230000), last, "far off vertically after a 2D fix", 4);
  fake_ms += 5000;
  jumpReject(jumpFix(315, 59340000 + 2 * step, 18209500, 2500 + 400000), last, "far off both ways", -1);
  fake_ms += 5000;
  jumpAccept(jumpFix(320, 59340000 + 2 * step, lng, 2500 + 130000), "3D fix after them");

  fresh();
  const auto high = jumpAccept(jumpFix(0, 59330000, lng, 1100000), "cold-start anchor far up");
  for (int k = 1; k <= 2; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(5 * k, 59330000 + step * k, lng), high, "good fix below a bad-altitude anchor", 4);
  }
  fake_ms += 5000;
  jumpAccept(jumpFix(15, 59330000 + step * 3, lng), "third consistent fix");
  check(sys.gpsReadDiagnostics().reanchored && sys.gpsReadDiagnostics().reanchor_rejects == 2,
        "a bad-altitude anchor re-anchors on the third consistent fix");

  fresh();
  const auto sticky = jumpAccept(jumpFix(0, 59330000, lng, 150000), "cold-start anchor 1.5 km up");
  for (int k = 1; k <= 3; ++k) {
    fake_ms += 5000;
    jumpReject(jumpFix(10 * k - 5, 0, 0, 0, "0,0,0,0"), sticky, "no-fix reply between real fixes");
    fake_ms += 5000;
    if (k < 3) jumpReject(jumpFix(10 * k, 59330000 + step * k, lng), sticky, "real fix in poor sky", 4);
  }
  jumpAccept(jumpFix(30, 59330000 + step * 3, lng), "third real fix");
  check(jumpReanchors == 1, "no-fix replies between real fixes do not pin a bad-altitude anchor");

  // The wake's first fix: held until the next fresh fix agrees with it on the same bounds.
  auto held = [](const std::string& reply, const std::string& label) {
    check(!readLink(reply) && sys.gpsReadDiagnostics().result == GPSReadResult::Unconfirmed && gd->lat == 0,
          label + " is held, not accepted");
  };
  auto confirmed = [](const std::string& reply, unsigned held, const std::string& label) {
    check(readLink(reply) && sys.gpsReadDiagnostics().confirmed && sys.gpsReadDiagnostics().confirm_held == held &&
          !sys.gpsReadDiagnostics().reanchored && gd->time == std::stoul(reply.substr(13, 8)),
          label + " confirms the first fix and is the one used");
  };
  fresh();
  held(jumpFix(0, 59348000, lng, 2500 + 240000, bogus), "bogus fix at power-on");
  fake_ms += 7000;
  held(jumpFix(7, 59351400, lng, 2500 + 300000, "9759,9000,6,255"), "second bogus fix at power-on");
  fake_ms += 5000;
  held(jumpFix(12, 59330000, lng), "first real fix");
  held(jumpFix(12, 59330000, lng), "its replay");
  fake_ms += 5000;
  check(readLink("$GNIFO,41026,11101500,0,0,200000,0,0,0,0\r") && gd->lat == 0, "no-fix reply still accepted");
  fake_ms += 5000;
  confirmed(jumpFix(22, 59330000 + step, lng, 0), 3, "an agreeing 2D fix");
  fake_ms += 5000;
  jumpAccept(jumpFix(27, 59330000 + 2 * step, lng), "3D fix after it, not judged against the no-fix altitude");

  fresh();
  held(jumpFix(0, 59430000, lng), "cold-start fix 11 km off, good HDOP");
  fake_ms += 5000;
  held(jumpFix(5, 59330000, lng), "real fix that disagrees only horizontally");
  check(sys.gpsReadDiagnostics().confirm_restarted, "the dropped candidate is reported");
  fake_ms += 60500;
  confirmed(jumpFix(65, 59330000 + step, lng), 2, "the next real fix at the charging cadence");

  fresh();
  held(jumpFix(0, 59330000, lng), "first fix");
  fake_ms += GPS_JUMP_CONFIRM_GAP_MS + 1000;
  held(jumpFix(121, 59330000 + step, lng), "agreeing fix after the window");
  fake_ms += 5000;
  confirmed(jumpFix(126, 59330000 + 2 * step, lng), 2, "the next agreeing fix");
}

void feedRmc(const std::string& course, const std::string& latitude = "5920.000") {
  const std::string body = "GPRMC,110757,A," + latitude + ",N,01804.000,E,20.00," + course + ",041026,,,A";
  unsigned char checksum = 0;
  for (char c : body) checksum ^= c;
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  gpsHasDecodedData = false;
  for (char c : "$" + body + suffix) if (gps.encode(c)) gpsHasDecodedData = true;
  check(gpsHasDecodedData, "checksum-valid RMC decoded");
}

void directRmc(const std::string& course) {
  feedRmc(course);
  check(pollGpsData(), "direct UART fix read");
}

void directMissing() {
  gps = HostGPS{};
  gpsData = GPS_DATA{};
  sys.m_flags = 0;
  directRmc("90.00");
  check(gpsData.heading == 90, "direct UART course 90");
  directRmc("");
  check(gpsData.heading == GPS_HEADING_INVALID, "empty RMC course must not retain previous heading");
  sys.m_flags = FLAG_GNSS_USE_LINK;
}

void directRange() {
  gps = HostGPS{};
  gpsData = GPS_DATA{};
  sys.m_flags = 0;
  const std::pair<std::string, uint16_t> cases[] = {
    {"0.00", 0}, {"90.00", 90}, {"359.99", 359}, {"360.00", 0},
    {"360.01", GPS_HEADING_INVALID}, {"361.00", GPS_HEADING_INVALID},
  };
  for (const auto& entry : cases) {
    directRmc(entry.first);
    check(gpsData.heading == entry.second, "direct UART heading " + entry.first);
  }
  sys.m_flags = FLAG_GNSS_USE_LINK;
}

void checkDirectPreserved(const GPS_DATA& lastGood, const std::string& label) {
  check(gd->lat == lastGood.lat && gd->lng == lastGood.lng && gd->alt == lastGood.alt &&
        gd->date == lastGood.date && gd->time == lastGood.time && gd->ts == lastGood.ts,
        label + " preserves cached coordinates and required fields");
  check(gd->speed == lastGood.speed && gd->heading == lastGood.heading &&
        gd->sat == lastGood.sat && gd->hdop == lastGood.hdop,
        label + " preserves direct-UART heading and quality");
  uint16_t sentences, errors;
  gps.stats(&sentences, &errors);
  check(gd->sentences == sentences && gd->errors == errors, label + " still updates TinyGPS counters");
}

void directGpsDiagnostics() {
  const auto lastGood = acceptLink("$GNIFO,41026,110757,59329000,18068000,2500,2000,9000,12,7\r",
                                   "direct diagnostic link seed");
  const unsigned commandCalls = link.commandCalls;
  sys.m_flags = 0;
  check(!sys.gpsGetData(&gd), "direct diagnostic no decoded data rejected");
  checkDirectPreserved(lastGood, "direct no data");
  checkDiagnostics(GPSReadResult::DirectNoData, 0, -1, "direct no data resets co-processor bytes");
  directRmc("90.00");
  checkDiagnostics(GPSReadResult::Accepted, 0, -1, "direct accepted with no co-processor measurement");
  const GPS_DATA directGood = *gd;
  fake_ms += 5000;
  feedRmc("180.00", "5820.000");
  check(!sys.gpsGetData(&gd), "direct diagnostic coordinate outlier rejected");
  checkDirectPreserved(directGood, "direct coordinate rejection");
  check(gpsHasDecodedData, "direct rejection keeps legacy decoded-data latch");
  checkDiagnostics(GPSReadResult::CoordinateRejected, 0, -1, "direct rejected coordinates");
  check(!sys.gpsGetData(&gd), "direct outlier remains rejected on successive call");
  checkDirectPreserved(directGood, "direct repeated rejection");
  checkDiagnostics(GPSReadResult::CoordinateRejected, 0, -1, "direct repeated rejection");
  directRmc("90.00");
  checkDiagnostics(GPSReadResult::Accepted, 0, -1, "direct recovery");
  check(!gpsHasDecodedData, "direct acceptance still consumes decoded-data latch");
  const GPS_DATA recovered = *gd;
  check(!sys.gpsGetData(nullptr), "direct no data after accepted recovery");
  checkDirectPreserved(recovered, "direct no data after recovery");
  checkDiagnostics(GPSReadResult::DirectNoData, 0, -1, "direct no data after recovery");
  check(link.commandCalls == commandCalls, "direct diagnostics never request co-processor data");
  sys.m_flags = FLAG_GNSS_USE_LINK;
  rejectLink("$GNIFO,", recovered, "link invalid field before direct no data");
  checkDiagnostics(GPSReadResult::RequiredFieldInvalid, link.response.size(), 0, "link invalid field");
  const GPS_DATA headingCleared = *gd;
  sys.m_flags = 0;
  check(!sys.gpsGetData(&gd), "direct no data after invalid link field");
  checkDirectPreserved(headingCleared, "direct no data after link failure");
  checkDiagnostics(GPSReadResult::DirectNoData, 0, -1, "direct resets prior invalid field");
}

void serializerAndQueue() {
  for (double heading : {0.0, 90.0, 359.0, 360.0, 360.01, 65446.0, 65535.0, -90.0,
                         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    const auto points = json::parse(bulkStringV2({{{"utc", 1791500000}, {"soc", 50}, {"heading", heading}, {"__geo_head_utc", 1791500000}}}, 1791500000));
    check(points.size() == 1 && points[0]["soc"]["frac"] == 0.5, "bad heading preserves battery point");
    if (heading >= 0 && heading <= 360)
      check(points[0]["heading"]["degrees"] == (heading == 360 ? 0 : heading), "serialized heading normalized");
    else check(!points[0].contains("heading"), "invalid saved heading omitted");
  }
  gpsData = GPS_DATA{};
  acceptLink("$GNIFO,41026,11070000,1000000,2000000,2500,1000,,12,7\r", "accepted position without heading");
  OBDVector position{{"utc", 1791500000}, {"soc", 50}};
  addGpsData(position);
  const auto point = json::parse(bulkStringV2({position}, 1791500000))[0];
  check(!point.contains("heading") && point.contains("location"), "invalid heading preserves live position");

  obd_result_vector.clear();
  network.received.clear();
  for (int i = 0; i < 120; ++i)
    obd_result_vector.push_back({{"utc", 1791500000.0 + i}, {"soc", 50}, {"batt_temp_min", 20},
                                {"batt_temp_max", 25}, {"heading", i == 2 ? 360.0 : 90.0},
                                {"__geo_head_utc", 1791500000.0 + i}});
  setClock(CLK_HTTP, 1791500000);
  network.responseCode = 503;
  check(!uploadBuffered(false) && obd_result_vector.size() == 120 && network.received.empty(),
        "failed POST retains every queued point");
  network.responseCode = 200;
  check(uploadBuffered(true) && obd_result_vector.empty(), "synthetic backlog drained");
  check(network.received.size() == 120, "exactly 120 points delivered");
  for (size_t i = 0; i < network.received.size(); ++i) {
    const auto& received = network.received[i];
    check(received["soc"]["time"] == v2Time(1791500000 + i), "all timestamps delivered once in order");
    check(received["soc"]["frac"] == 0.5 && received["batteryTemperatureMin"]["c"] == 20 &&
          received["batteryTemperatureMax"]["c"] == 25, "battery data delivered intact");
    check(received["heading"]["degrees"] == (i == 2 ? 0 : 90), "bad third heading repaired");
  }
}

ResetReason esp_reset_reason() { return reset_reason; }
void gpsResetFromSetup() {
#include "gps_reset.inc"
}

void geoRtc() {
  parkedFixture();
  gd = nullptr;
  g_gps_powered = false;
  auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  const auto original_time = point["location"]["time"];
  reset_reason = ESP_RST_DEEPSLEEP;
  gpsResetFromSetup();
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point["location"]["time"] == original_time, "same-image deep sleep fake preserves frozen UTC");
  const auto old_lat = frozen_lat;
  frozen_lat += 0.001;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location"), "RTC coordinate snapshot mismatch drops wire geo");
  frozen_lat = old_lat;
  for (auto reason : {ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_WDT}) {
    parkedFixture();
    gd = nullptr;
    g_gps_powered = false;
    reset_reason = reason;
    gpsResetFromSetup();
    const auto frame = gpsFrame();
    point = json::parse(bulkStringV2({frame}, 1791500000))[0];
    bool selected = false;
    for (const auto& item : frame) if (item.first == "lat") selected = item.second == frozen_lat;
    check(selected && frozen_valid && !point.contains("location") && point.contains("soc"),
          "all non-deep resets retain old selection but invalidate provenance");
  }
}

void geoRecordHistory() {
  g_gps_powered = true;
  acceptLink("$GNIFO,41026,11075700,59329000,18068000,2500,0,9000,12,7\r", "stationary accepted record");
  parkAccumSample();
  check(setClockFromGps(), "accepted GPS seeds clock");
  const auto before = *gd;
  const auto epoch = clock_epoch;
  const auto seq = gps_fix_seq;
  fake_ms += 5000;
  rejectLink("$GNIFO,51026,12000000,59330000", before, "partial valid calendar rejected");
  auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point["location"]["time"] == v2Time(epoch) && !point.contains("heading") &&
        clock_epoch == epoch && gps_fix_seq == seq && park_acc_n == 1,
        "rejected calendar leaves clock sequence mean and location UTC unchanged");
  is_currently_parked = true;
  const std::pair<std::string, std::string> records[] = {
    {"41026,11075700", "2026-10-04T11:07:57Z"}, {"41026,11075600", "2026-10-04T11:07:56Z"},
    {"31026,23595900", "2026-10-03T23:59:59Z"}, {"41026,0", "2026-10-04T00:00:00Z"},
  };
  for (const auto& record : records) {
    is_currently_parked = true;
    fake_ms += 5000;
    acceptLink("$GNIFO," + record.first + ",59329000,18068000,2500,2000,9000,12,7\r", "replay/backwards record");
    point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
    check(point["location"]["time"] == record.second, "replayed/backwards record retains original UTC");
  }
  is_currently_parked = false;
  acceptLink("$GNIFO,310426,11075700,59330000,18069000,2500,2000,9000,12,7\r", "accepted invalid calendar");
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading"), "accepted invalid calendar clears provenance");
  acceptLink("$GNIFO,41026,11075800,59331000,18070000,2500,2000,9000,12,7\r", "accepted recovery");
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point.contains("location") && point.contains("heading"), "valid recovery restores provenance");
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location") && point.contains("heading"), "unchanged unparked coords selection gate preserved");
  gd->lat += 0.001f;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading"),
        "unaccepted mutable coordinates cannot produce geo provenance");
  check(setClockFromGps() && v2Time(clock_epoch) == "2026-10-04T11:07:58Z",
        "matching accepted UTC remains clock eligible despite mutable coordinates");
  gd->lat = 0;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading"), "both nonzero GPS eligibility unchanged");
  for (const auto& coords : {std::string("91000000,18068000"), std::string("59329000,181000000"),
                             std::string("0,18068000"), std::string("59329000,0")}) {
    gpsData = GPS_DATA{};
    acceptLink("$GNIFO,41026,12000000," + coords + ",2500,2000,9000,12,7\r", "accepted unusable cold coordinates");
    point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
    check(!point.contains("location") && !point.contains("heading") && point["soc"]["frac"] == 0.5,
          "accepted invalid/zero coordinates reset provenance without changing parser acceptance");
  }
}

void geoFreezeBounds() {
  g_gps_powered = true;
  for (int i = 0; i < PARK_ACC_MAX; ++i) {
    acceptLink("$GNIFO,41026," + std::to_string((i / 60) * 10000 + (i % 60) * 100) + ",59329000,18068000,2500,0,9000,12,7\r", "capped contributor");
    parkAccumSample();
  }
  acceptLink("$GNIFO,41026,12000000,59330000,18069000,2500,0,9000,12,7\r", "ignored later receiver fix");
  parkAccumSample();
  check(park_acc_n == PARK_ACC_MAX, "stationary cap unchanged");
  freezeParkedLocation();
  is_currently_parked = true;
  auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  checkLocation(point, 59.329, 18.068, "mean ignores capped later fix");
  check(point["location"]["time"] == "2026-10-04T00:03:59Z", "cap time ignores later noncontributor");
  gd = nullptr;
  park_acc_n = 0;
  freezeParkedLocation();
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point["location"]["time"] == "2026-10-04T00:03:59Z", "keep previous frozen coords keeps old time");
  resetFixture();
  g_gps_powered = true;
  for (int i = 0; i < PARK_ACC_MIN; ++i) {
    acceptLink("$GNIFO,41026," + std::to_string(11073000 - i * 100) +
               ",59329000,18068000,2500,0,9000,12,7\r", "backwards contributor");
    parkAccumSample();
  }
  freezeParkedLocation();
  is_currently_parked = true;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point["location"]["time"] == "2026-10-04T11:07:30Z", "mean uses latest chronological contributor");
  resetFixture();
  g_gps_powered = true;
  for (int i = 0; i < PARK_ACC_MIN; ++i) {
    acceptLink("$GNIFO," + std::string(i == 3 ? "310426" : "41026") + "," + std::to_string(11070000 + i * 100) +
               ",59329000,18068000,2500,0,9000,12,7\r", "unknown time contributor");
    parkAccumSample();
  }
  freezeParkedLocation();
  is_currently_parked = true;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(frozen_valid && !point.contains("location") && point.contains("soc"), "unknown contributor makes mean time unknown");
  is_currently_parked = false;
  acceptLink("$GNIFO,41026,12000000,59329000,18068000,2500,2000,9000,12,7\r", "movement resets mean");
  parkAccumSample();
  acceptLink("$GNIFO,41026,12000100,59329000,18068000,2500,0,9000,12,7\r", "single fix after movement");
  freezeParkedLocation();
  is_currently_parked = true;
  point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point["location"]["time"] == "2026-10-04T12:00:01Z", "movement and freeze reset unknown mean bookkeeping");
}

void gpsClockWithoutPosition() {
  link.response = "$GNIFO,41026,11075700,0,0,2500,0,9000,12,7\r";
  check(pollGpsData(), "canonical valid UTC without position remains accepted");
  check(gpsLocationUtc() == 0, "zero coordinates have unknown geo provenance");
  const auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading") && point["soc"]["frac"] == 0.5,
        "accepted UTC without position omits geo but retains battery");
  check(setClockFromGps() && v2Time(clock_epoch) == "2026-10-04T11:07:57Z" &&
        last_clock_sync_epoch == clock_epoch && !clockNeedsSync(),
        "accepted valid GPS UTC seeds fresh trusted clock before position fix");
  const auto epoch = clock_epoch;
  gd->date = 51026;
  check(!setClockFromGps() && clock_epoch == epoch, "unaccepted mutable date cannot seed clock");
  gd->date = 41026;
  gd->time = 11075800;
  check(!setClockFromGps() && clock_epoch == epoch, "unaccepted mutable time cannot seed clock");
  gd->time = 11075700;
  check(setClockFromGps() && clock_epoch == epoch, "matching accepted date/time restores clock eligibility without position");
  acceptLink("$GNIFO,310426,11075700,0,0,2500,0,9000,12,7\r", "accepted invalid calendar without position");
  check(!setClockFromGps() && clock_epoch == epoch && gpsLocationUtc() == 0,
        "invalid calendar cannot seed clock even in an accepted no-position record");
}

void geoCalendar() {
  const std::pair<std::string, std::string> cases[] = {
    {"290224,23595999", "2024-02-29T23:59:59Z"}, {"10324,0", "2024-03-01T00:00:00Z"},
    {"10101,0", "2001-01-01T00:00:00Z"}, {"311299,23595900", "2099-12-31T23:59:59Z"},
    {"290223,0", ""}, {"310426,0", ""}, {"320126,0", ""}, {"10126,24000000", ""},
    {"10126,12600000", ""}, {"10126,12006000", ""}, {"10100,0", ""}, {"10026,0", ""},
    {"11326,0", ""}, {"126,0", ""}, {"10126,429496729", ""},
  };
  setenv("TZ", "Pacific/Honolulu", 1);
  tzset();
  for (const auto& entry : cases) {
    resetFixture();
    acceptLink("$GNIFO," + entry.first + ",59329000,18068000,2500,2000,9000,12,7\r", "calendar record");
    OBDVector frame{{"utc", 4294967295.0}, {"soc", 50}};
    addGpsData(frame);
    const auto point = json::parse(bulkStringV2({frame}, 1709251200))[0];
    const bool wire_eligible = !entry.second.empty() && entry.first != "10101,0";
    check(point.contains("location") == wire_eligible && point.contains("heading") == wire_eligible,
          "strict calendar geo eligibility under fixed trusted formatter time " + entry.first);
    check((gpsLocationUtc() != 0) == !entry.second.empty(), "strict calendar observation provenance " + entry.first);
    check(setClockFromGps() == !entry.second.empty(), "strict calendar clock eligibility " + entry.first);
    if (!entry.second.empty()) {
      check(v2Time(gpsLocationUtc()) == entry.second && v2Time(clock_epoch) == entry.second,
            "UTC conversion independent of host timezone " + entry.first);
      if (wire_eligible) check(point["location"]["time"] == entry.second, "eligible calendar geo preserves observation UTC");
    }
    else check(clock_epoch == 0, "invalid calendar never seeds clock");
  }
  unsetenv("TZ");
  tzset();
}

void geoFreeze() {
  g_gps_powered = true;
  for (int i = 0; i < PARK_ACC_MIN; ++i) {
    acceptLink("$GNIFO,41026," + std::to_string(11070000 + i * 100) +
               ",59329000,18068000,2500,0,9000,12,7\r", "stationary contributor");
    parkAccumSample();
  }
  freezeParkedLocation();
  is_currently_parked = true;
  const auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point.contains("location") && point["location"]["time"] == "2026-10-04T11:07:11Z",
        "frozen mean uses last actual contributor UTC");
}

void geoAccepted() {
  acceptLink("$GNIFO,41026,11075700,59329000,18068000,2500,2000,9000,12,7\r", "coherent UTC fix");
  const auto point = json::parse(bulkStringV2({gpsFrame()}, 1791500000))[0];
  check(point.contains("location") && point.contains("heading"), "accepted GPS record publishes geo");
  check(point["location"]["time"] == "2026-10-04T11:07:57Z" &&
        point["heading"]["time"] == "2026-10-04T11:07:57Z", "accepted record UTC not frame UTC");
}

#define ENABLE_GPS 1
void finalizeGpsFrame(OBDVector& current_frame) {
#include "gps_frame.inc"
}
#undef ENABLE_GPS
#define ENABLE_GPS 0
void finalizeNoGpsFrame(OBDVector& current_frame) {
#include "gps_frame.inc"
}
#undef ENABLE_GPS

void geoDowngrade() {
  std::vector<OBDVector> original;
  for (int i = 0; i < MAX_UPLOAD_BUFFER; ++i)
    original.push_back({{"utc", 1791500000.0 + i}, {"soc", 50.0 + i / 100.0}, {"is_parked", 1},
                        {"current", -32}, {"is_charging", 1}, {"batt_temp_min", 20}, {"batt_temp_max", 25},
                        {"lat", 59.329}, {"lon", 18.068}, {"heading", i == 2 ? 360.0 : 90.0},
                        {"__geo_loc_utc", 1791000000.0 + i}, {"__geo_head_utc", 1791000000.0 + i}});
  obd_result_vector = original;
  saveUploadBacklog();
  const auto saved = LittleFS.contents();
  historical::obd_result_vector.clear();
  historical::loadUploadBacklog();
  check(historical::obd_result_vector.size() == MAX_UPLOAD_BUFFER, "actual eacc loader accepts every numeric shadow frame");
  auto old_points = json::parse(historical::bulkStringV2(historical::obd_result_vector));
  check(old_points.size() == MAX_UPLOAD_BUFFER, "eacc serializer keeps all battery points");
  for (size_t i = 0; i < old_points.size(); ++i) {
    const auto& point = old_points[i];
    check(!point.contains("location") && !point.contains("heading") &&
          point["soc"]["time"] == v2Time(1791500000 + i) &&
          point["soc"]["frac"] == (50.0 + i / 100.0) / 100.0 &&
          point["batteryTemperatureMin"]["c"] == 20 && point["batteryTemperatureMax"]["c"] == 25 &&
          point["chargingState"]["state"] == "CHARGING_AC", "actual eacc wire preserves battery FIFO without geo restamping");
  }
  historical::saveUploadBacklog();
  std::istringstream before_save(saved), after_save(LittleFS.contents());
  std::string before_line, after_line;
  while (std::getline(before_save, before_line)) {
    check(bool(std::getline(after_save, after_line)) && json::parse(before_line) == json::parse(after_line),
          "actual eacc unsuccessful retry/resave preserves every numeric shadow and core value");
  }
  check(!std::getline(after_save, after_line), "historical resave never adds queued rows");
  obd_result_vector.clear();
  loadUploadBacklog();
  auto points = json::parse(bulkStringV2(obd_result_vector, 1791500000));
  check(points.size() == MAX_UPLOAD_BUFFER, "new loader restores every historical resave frame");
  for (size_t i = 0; i < points.size(); ++i)
    check(points[i]["location"]["time"] == v2Time(1791000000 + i) &&
          points[i]["heading"]["time"] == v2Time(1791000000 + i) &&
          points[i]["heading"]["degrees"] == (i == 2 ? 0 : 90) && points[i]["soc"] == old_points[i]["soc"],
          "new recovery preserves geo observation UTC and exact battery FIFO");
  historical::obd_result_vector.clear();
  saveUploadBacklog();
  historical::loadUploadBacklog();
  historical::obd_result_vector.clear();
  historical::saveUploadBacklog();
  check(!LittleFS.exists(UPLOAD_BACKLOG_PATH), "successful old battery upload conservatively loses protected geo");
  obd_result_vector.clear();
  loadUploadBacklog();
  check(obd_result_vector.empty(), "old successful upload cannot restore already drained geo");
  // An appended backlog (#92) is still one frame per line to the old loader.
  obd_result_vector = {original[0], original[1]};
  saveUploadBacklog();
  obd_result_vector = {original[2]};
  g_backlog_unread = true;
  saveUploadBacklog();
  g_backlog_unread = false;
  historical::obd_result_vector.clear();
  historical::loadUploadBacklog();
  check(historical::obd_result_vector.size() == 3 &&
        json::parse(historical::bulkStringV2(historical::obd_result_vector))[2]["soc"]["time"] == v2Time(1791500002),
        "the eacc loader restores an appended backlog in order");
  historical::obd_result_vector.clear();
  LittleFS.files.clear();
  json legacy = json::object();
  for (const auto& res : original[0]) if (!isGeoMetaKey(res.first)) legacy[res.first] = res.second;
  const auto encoded = encodeUploadFrame(original[0]).dump();
  const auto overhead = encoded.size() - legacy.dump().size();
  check(overhead <= 100, "representative numeric shadow overhead bounded to 100 bytes per geo frame");
  std::cout << "Backlog representative old=" << legacy.dump().size() << "B new=" << encoded.size()
            << "B overhead=" << overhead << "B/frame <= " << MAX_UPLOAD_BUFFER * 100 << "B/120 frames\n";
}

void geoBacklogNonfinite() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  for (const auto& values : {std::vector<double>{nan, 18.068, nan, 1791000000, 1791000000},
                            std::vector<double>{59.329, infinity, infinity, 1791000000, 1791000000},
                            std::vector<double>{59.329, 18.068, 90, nan, infinity}}) {
    obd_result_vector = {{{"utc", 1791500000}, {"soc", 50}, {"lat", values[0]}, {"lon", values[1]},
                          {"heading", values[2]}, {"__geo_loc_utc", values[3]}, {"__geo_head_utc", values[4]},
                          {"__geo_unknown", nan}, {"__geo_v", 9}}};
    saveUploadBacklog();
    auto saved = json::parse(LittleFS.contents());
    check(saved == json({{"utc", 1791500000}, {"soc", 50}}), "nonfinite geo cannot poison old numeric loader with JSON null");
    historical::obd_result_vector.clear();
    historical::loadUploadBacklog();
    check(historical::obd_result_vector.size() == 1 &&
          json::parse(historical::bulkStringV2(historical::obd_result_vector))[0]["soc"]["frac"] == 0.5,
          "actual old loader retains battery for nonfinite geo frame");
  }
  LittleFS.seed("previous interrupted queue\n");
  obd_result_vector = {{{"utc", 1791500000}, {"soc", 60}}};
  LittleFS.fail_open = true;
  saveUploadBacklog();
  check(LittleFS.contents() == "previous interrupted queue\n" && obd_result_vector.size() == 1,
        "failed save open retains RAM and existing file");
  LittleFS.fail_open = false;
  saveUploadBacklog();
  check(json::parse(LittleFS.contents())["soc"] == 60 && LittleFS.files.size() == 1,
        "save overwrites single existing backlog path without sidecars");
  obd_result_vector.clear();
  LittleFS.fail_open = true;
  loadUploadBacklog();
  check(obd_result_vector.empty() && LittleFS.exists(UPLOAD_BACKLOG_PATH), "failed load open retains file");
  LittleFS.fail_open = false;
  loadUploadBacklog();
  check(obd_result_vector.size() == 1 && !LittleFS.exists(UPLOAD_BACKLOG_PATH), "successful load owns RAM and deletes file");
}

void geoBacklogRows() {
  const json core{{"utc", 1791500000}, {"soc", 50}, {"is_parked", 1}, {"_soh", 98}};
  json canonical = core;
  canonical["__geo_v"] = 1;
  canonical["__geo_lat"] = 59.329;
  canonical["__geo_lon"] = 18.068;
  canonical["__geo_heading"] = 90;
  canonical["__geo_loc_utc"] = 1791000000;
  canonical["__geo_head_utc"] = 1791000060;
  std::vector<std::pair<json, bool>> rows{{canonical, true}, {core, false}};
  auto legacy = core;
  legacy["lat"] = 59.329;
  legacy["lon"] = 18.068;
  legacy["heading"] = 90;
  rows.push_back({legacy, false});
  for (const json& version : {json(0), json(2), json(1.5), json("1"), json(nullptr), json::object()}) {
    auto row = canonical;
    row["__geo_v"] = version;
    rows.push_back({row, false});
  }
  for (const auto& key : {"lat", "lon", "heading"}) {
    auto row = canonical;
    row[key] = legacy[key];
    rows.push_back({row, false});
  }
  for (const json& bad : {json(0), json(978307199), json(1791500001), json(4294967296.0), json(1791000000.5),
                          json("bad"), json(nullptr), json::array({1}), json::object({{"bad", 1}})}) {
    auto row = canonical;
    row["__geo_loc_utc"] = bad;
    row["__geo_head_utc"] = bad;
    rows.push_back({row, false});
  }
  for (const json& bad : {json(nullptr), json("NaN"), json(999), json::object()}) {
    auto row = canonical;
    row["__geo_lat"] = bad;
    row["__geo_heading"] = bad;
    rows.push_back({row, false});
  }
  auto row = core;
  row["__geo_loc_utc"] = "forged";
  row["__geo_extra"] = json::array({1, 2});
  rows.push_back({row, false});
  std::string data = "\nnot-json\n{\"utc\":\n";
  for (const auto& entry : rows) data += entry.first.dump() + '\n';
  LittleFS.seed(data);
  obd_result_vector.clear();
  loadUploadBacklog();
  check(obd_result_vector.size() == rows.size(), "mixed legacy/malformed private rows retain all valid core frames");
  const auto points = json::parse(bulkStringV2(obd_result_vector, 1791500000));
  check(points.size() == rows.size(), "every eligible battery frame serialized");
  for (size_t i = 0; i < points.size(); ++i) {
    check(points[i]["soc"]["frac"] == 0.5 && points[i]["soc"]["time"] == v2Time(1791500000) &&
          points[i]["drivingState"]["state"] == "PARK" && points[i].contains("location") == rows[i].second &&
          points[i].contains("heading") == rows[i].second, "row validation drops only untrusted geo not core state");
    for (const auto& item : obd_result_vector[i])
      check(!isGeoMetaKey(item.first) || item.first == "__geo_loc_utc" || item.first == "__geo_head_utc",
            "only trusted supported observation keys enter RAM");
  }
  std::string bounded;
  for (int i = 0; i < MAX_UPLOAD_BUFFER + 1; ++i) {
    row = core;
    row["utc"] = 1791500000 + i;
    bounded += row.dump() + '\n';
  }
  LittleFS.seed(bounded);
  obd_result_vector.clear();
  upload_capped = 0;
  loadUploadBacklog();
  check(obd_result_vector.size() == MAX_UPLOAD_BUFFER && !LittleFS.exists(UPLOAD_BACKLOG_PATH) && upload_capped == 1,
        "MAX120 load cap counts the drop and keeps load/delete behavior");
  const auto bounded_points = json::parse(bulkStringV2(obd_result_vector, 1791500000));
  for (size_t i = 0; i < bounded_points.size(); ++i)
    check(bounded_points[i]["soc"]["time"] == v2Time(1791500001 + i), "mixed queue load cap drops the oldest and preserves FIFO");
}

void geoBacklog() {
  acceptLink("$GNIFO,41026,11075700,59329000,18068000,2500,2000,9000,12,7\r", "queue accepted fix");
  const auto frame = gpsFrame();
  obd_result_vector = {frame};
  saveUploadBacklog();
  const auto saved = json::parse(LittleFS.contents());
  check(!saved.contains("lat") && !saved.contains("lon") && !saved.contains("heading"),
        "public geo must never be persisted for old firmware to restamp");
  check(saved["__geo_v"] == 1 && saved["__geo_lat"] == gd->lat && saved["__geo_lon"] == gd->lng &&
        saved["__geo_heading"] == 90, "canonical numeric geo shadow persisted");
  for (const auto& item : saved.items()) check(item.value().is_number(), "backlog remains entirely flat numeric");
  obd_result_vector.clear();
  loadUploadBacklog();
  check(obd_result_vector.size() == 1 && !LittleFS.exists(UPLOAD_BACKLOG_PATH), "new backlog restores and deletes existing path");
  const auto point = json::parse(bulkStringV2(obd_result_vector, 1791500000))[0];
  check(point["location"]["time"] == "2026-10-04T11:07:57Z" && point["heading"]["degrees"] == 90 &&
        point["soc"]["frac"] == 0.5, "backlog round trip retains original geo and battery UTC");
}

void geoProfile() {
  gd = nullptr;
  OBDVector frame{{"utc", 1791500000}, {"soc", 50}, {"lat", 59.329}, {"lon", 18.068}, {"heading", 90},
                  {"__geo_loc_utc", 1791000000}, {"__geo_loc_utc", 1791000001},
                  {"__geo_head_utc", 1791000000}, {"__geo_future", 123}, {"_soh", 99}};
  const auto hostile = frame;
  finalizeNoGpsFrame(frame);
  auto point = json::parse(bulkStringV2({frame}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading"), "GPS disabled logger scrubs metadata");
  frame = hostile;
  finalizeGpsFrame(frame);
  point = json::parse(bulkStringV2({frame}, 1791500000))[0];
  check(!point.contains("location") && !point.contains("heading") && point["soc"]["frac"] == 0.5,
        "hostile duplicate profile metadata cannot spoof no-GPS frame provenance");
  for (const auto& res : frame) check(!isGeoMetaKey(res.first), "profile reserved namespace removed completely");
  check(json::parse(logString(frame))["tlm"]["_soh"] == 99, "other private profile fields untouched");
  frame = hostile;
  acceptLink("$GNIFO,41026,11075700,59330000,18069000,2500,2000,9000,12,7\r", "trusted fix after hostile profile");
  finalizeGpsFrame(frame);
  point = json::parse(bulkStringV2({frame}, 1791500000))[0];
  checkLocation(point, gd->lat, gd->lng, "trusted metadata follows actual coordinates not spoofed profile");
  check(point["location"]["time"] == "2026-10-04T11:07:57Z" &&
        point["heading"]["time"] == "2026-10-04T11:07:57Z", "spoofed duplicate observation times removed before trusted append");
}

void geoPrivacy() {
  OBDVector frame{{"utc", 1791500000}, {"soc", 50}, {"power", -12.2}, {"voltage", 380.4},
                  {"current", -32.1}, {"is_parked", 1}, {"is_charging", 1}, {"is_dcfc", 0},
                  {"lat", 59.329}, {"lon", 18.068}, {"heading", 90}, {"_soh", 98},
                  {"__geo_loc_utc", 1791000000}, {"__geo_head_utc", 1791000060},
                  {"__geo_future_key", 42}, {"__geo_v", 1}, {"__geo_lat", 59}};
  for (const auto& wire : {logString(frame), bleTlmString(frame), bulkString({frame}, "test"), bulkStringV2({frame}, 1791500000)})
    check(wire.find("__geo_") == std::string::npos, "entire reserved namespace stays private");
  const auto legacy = json::parse(logString(frame))["tlm"];
  const auto bulk = json::parse(bulkString({frame}, "test"))["data"][0]["tlm_list"][0];
  for (const auto& value : {legacy, bulk})
    check(!value.contains("lat") && !value.contains("lon") && !value.contains("heading") && value["_soh"] == 98,
          "legacy formats omit geo without altering other private fields");
  const auto ble = json::parse(bleTlmString(frame))["tlm"];
  check(ble["is_parked"] == true && ble["is_charging"] == true &&
        ble["is_dcfc"] == false && !ble.contains("lat") && !ble.contains("heading") && !ble.contains("utc"),
        "BLE phone geo and booleans unchanged");
}

void geoTimes() {
  OBDVector frame{{"utc", 1791500000}, {"soc", 50}, {"is_parked", 1},
                  {"lat", 59.329}, {"lon", 18.068}, {"heading", 360},
                  {"__geo_loc_utc", 1791000000}, {"__geo_head_utc", 1791000060}};
  auto point = json::parse(bulkStringV2({frame}, 1791500000))[0];
  check(point["location"]["time"] == v2Time(1791000000) &&
        point["heading"]["time"] == v2Time(1791000060) && point["heading"]["degrees"] == 0 &&
        point["soc"]["time"] == v2Time(1791500000), "geo retains independent observation UTC");
  const double bad[] = {0, -1, 978307199, 1791000000.5, 1791500001, 4294967296.0,
                       std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
  for (double value : bad) {
    frame[6].second = value;
    point = json::parse(bulkStringV2({frame}, 1791500000))[0];
    check(!point.contains("location") && point.contains("heading") && point["soc"]["frac"] == 0.5,
          "invalid location UTC only drops location");
    frame[6].second = 1791000000;
    frame[7].second = value;
    point = json::parse(bulkStringV2({frame}, 1791500000))[0];
    check(point.contains("location") && !point.contains("heading") && point.contains("drivingState"),
          "invalid heading UTC only drops heading");
    frame[7].second = 1791000060;
  }
  for (double value : {91.0, -91.0, std::numeric_limits<double>::quiet_NaN()}) {
    frame[3].second = value;
    check(!json::parse(bulkStringV2({frame}, 1791500000))[0].contains("location"), "invalid latitude drops location");
  }
  frame[3].second = 90;
  for (double value : {181.0, -181.0, std::numeric_limits<double>::infinity()}) {
    frame[4].second = value;
    check(!json::parse(bulkStringV2({frame}, 1791500000))[0].contains("location"), "invalid longitude drops location");
  }
  frame[4].second = -180;
  check(json::parse(bulkStringV2({frame}, 1791500000))[0].contains("location"), "coordinate limits inclusive");
}

void geoLegacy() {
  const OBDVector frame{{"utc", 1791500000}, {"soc", 50}, {"is_parked", 1},
                        {"lat", 59.329}, {"lon", 18.068}, {"heading", 90}};
  const auto points = json::parse(bulkStringV2({frame}, 1791500000));
  check(points.size() == 1 && points[0]["soc"]["frac"] == 0.5 &&
        points[0]["drivingState"]["state"] == "PARK", "legacy frame retains battery and state");
  check(!points[0].contains("location") && !points[0].contains("heading"),
        "legacy geo without observation UTC must not be restamped");
}

// The real poll-upload gate, buffer push and pre-sleep trip-end marker (logger.ino, sleep_power.h).
bool park_marker_owed = false;
void bufferPoll(OBDVector current_frame, bool ble_relayed = false) {
  const uint32_t relay_seq = 0; // an app without relay acks; those are tests/run_relay_checks.py's (#90)
#include "upload_gate.inc"
#include "upload_buffer.inc"
}
void queueParkMarker() {
#include "park_marker.inc"
}

OBDVector richUploadFrame(double utc, unsigned index) {
  OBDVector frame{{"utc", utc}, {"soc", 45 + index / 100.0}, {"voltage", 350},
                  {"current", 12}, {"speed", 50}};
  for (unsigned i = 0; i < 20; ++i)
    frame.emplace_back("p" + std::to_string(i), index + i);
  return frame;
}

size_t expectedQueueCopyBytes(const OBDVector& frame, size_t destination_size,
                              size_t destination_capacity) {
  // No PSRAM in these fixtures: the pairs are internal; keys past 15 characters are counted as on the ESP32.
  size_t bytes = frame.size() * sizeof(OBDVector::value_type) + 16;
  for (const auto& value : frame)
    if (value.first.size() > 15) bytes += value.first.size() + 1 + 16;
  if (destination_size == destination_capacity)
    bytes += (destination_size + 1) * sizeof(OBDVector) + 16;
  return bytes;
}

void uploadGate() {
  const double T = 1791500000;
  resetFixture();
  setClock(CLK_HTTP, T);
  obd_result_vector.clear();
  park_marker_owed = false;
  check(!v2HasTelemetry({{"utc", T}}, T), "utc-only frame is not uploaded");
  check(!v2HasTelemetry({{"utc", T}, {"_bms_soc", 50}}, T), "unmapped private key alone is not uploaded");
  check(!v2HasTelemetry({{"soc", 50}}, T), "frame without utc is not uploaded");

  parkedFixture();
  OBDVector frozen{{"utc", T}};
  addGpsData(frozen);
  check(v2HasTelemetry(frozen, T), "eligible frozen location serializes with a send clock");
  network.responseCode = 503;
  for (int poll = 0; poll < 5; ++poll) {
    bufferPoll(frozen);
    check(!uploadBuffered(true), "uploads keep failing");
  }
  check(obd_result_vector.size() == 1, "a frozen fix the server has not accepted is buffered once, not once per poll");
  OBDVector moved{{"utc", T}, {"lat", 59.33}, {"lon", 18.06}, {"__geo_loc_utc", T}};
  bufferPoll(moved);
  check(obd_result_vector.size() == 2, "a new location behind an unsent one is still buffered");
  bufferPoll(moved);
  check(obd_result_vector.size() == 2, "and only once");
  obd_result_vector.pop_back(); // the rest of this check runs against the frozen fix alone
  network.responseCode = 200;
  check(uploadBuffered(true) && obd_result_vector.empty() && network.received.back().contains("location"), "and goes up");
  bufferPoll(frozen);
  check(obd_result_vector.empty(), "once accepted, a parked poll carrying only the frozen fix is not buffered");
  OBDVector parked{{"utc", T}, {"soc", 50}};
  addGpsData(parked);
  bufferPoll(parked);
  check(obd_result_vector.size() == 1 && !park_marker_owed, "parked car value is buffered and owes no marker");
  queueParkMarker();
  check(obd_result_vector.size() == 1, "a parked wake that ended no trip queues no marker");
  check(linkHeading("9000") == 90, "seed first moving fix");
  OBDVector moving{{"utc", T + 30}};
  addGpsData(moving);
  bufferPoll(moving);
  check(obd_result_vector.size() == 2 && json::parse(bulkStringV2({obd_result_vector.back()}, T + 30))[0]
        .contains("location"), "a live fix still latched PARK is buffered with its location");

  is_currently_parked = false;
  OBDVector driving{{"utc", T + 60}, {"soc", 49}};
  addGpsData(driving);
  bufferPoll(driving);
  check(obd_result_vector.size() == 3 && park_marker_owed, "driving poll is buffered and owes a marker");
  queueParkMarker();
  check(obd_result_vector.size() == 3 && park_marker_owed, "a mid-drive sleep queues no marker and keeps it owed");
  is_currently_parked = true;
  queueParkMarker();
  queueParkMarker();
  check(obd_result_vector.size() == 4 && !park_marker_owed, "the trip end queues exactly one marker");
  const auto marker = json::parse(bulkStringV2({obd_result_vector.back()}, T));
  check(marker.size() == 1 && marker[0].size() == 2 && marker[0]["drivingState"]["state"] == "PARK",
        "trip-end marker serializes to a PARK point");
  // A trip end after a full buffer: the marker is frame MAX + 1 in the backlog, and the restore keeps the
  // newest frames, as the loop's cap does (#49).
  obd_result_vector.assign(MAX_UPLOAD_BUFFER, {{"utc", T}, {"soc", 49}, {"is_parked", 0}});
  park_marker_owed = true;
  queueParkMarker();
  saveUploadBacklog();
  obd_result_vector.clear();
  loadUploadBacklog();
  check(obd_result_vector.size() == MAX_UPLOAD_BUFFER && obd_result_vector.back().size() == 2,
        "a backlog over the cap loses its oldest frame, never the trip-end marker");
  obd_result_vector = {parked};
  g_geo_accepted = g_geo_queued = {};
  saveUploadBacklog();
  obd_result_vector.clear();
  loadUploadBacklog();
  bufferPoll(frozen);
  check(obd_result_vector.size() == 1, "a fix already in the restored backlog is not buffered again");
  // A backlog exists because coverage was missing, so the restore usually runs before any clock is trusted.
  g_geo_accepted = g_geo_queued = {};
  saveUploadBacklog();
  obd_result_vector.clear();
  last_clock_sync_epoch = 0;
  loadUploadBacklog();
  bufferPoll(frozen);
  check(obd_result_vector.size() == 1, "an untrusted poll after the restore buffers nothing new");
  setClock(CLK_HTTP, T);
  bufferPoll(frozen);
  check(obd_result_vector.size() == 1, "a fix restored under an untrusted clock is not buffered again once it is trusted");

  // An unread backlog (#92): the frozen fix it holds is not buffered again; a frame with data and a live fix are.
  resetFixture();
  setClock(CLK_HTTP, T);
  park_marker_owed = false;
  parkedFixture();
  OBDVector filed{{"utc", T}, {"soc", 50}};
  addGpsData(filed);
  obd_result_vector = {filed};
  saveUploadBacklog();
  obd_result_vector.clear();
  g_backlog_unread = uploadBacklogPending();
  OBDVector frozen_only{{"utc", T}};
  addGpsData(frozen_only);
  bufferPoll(frozen_only);
  check(obd_result_vector.empty(), "a frozen fix behind an unread backlog is not buffered again");
  OBDVector parked_soc{{"utc", T}, {"soc", 51}};
  addGpsData(parked_soc);
  bufferPoll(parked_soc);
  check(obd_result_vector.size() == 1 && json::parse(bulkStringV2({obd_result_vector.back()}, T))[0].contains("location"),
        "a parked poll with data behind an unread backlog is buffered with its location");
  check(linkHeading("9000") == 90, "seed a moving fix");
  OBDVector live{{"utc", T + 30}};
  addGpsData(live);
  bufferPoll(live);
  check(obd_result_vector.size() == 2 && std::string(gpsLocationSource()) == "gnss",
        "a live fix behind an unread backlog is buffered");

  // A phone taking the transport (#58): what is queued for cellular stays queued, and what it relays is not.
  resetFixture();
  setClock(CLK_HTTP, T);
  park_marker_owed = false;
  parkedFixture();
  OBDVector parked_fix{{"utc", T}};
  addGpsData(parked_fix);
  is_currently_parked = false;
  bufferPoll({{"utc", T - 3}, {"soc", 60}});
  bufferPoll({{"utc", T - 2}, {"soc", 59}});
  park_marker_owed = false;
  bufferPoll({{"utc", T - 1}, {"soc", 58}}, true);
  check(obd_result_vector.size() == 2 && park_marker_owed, "a relayed driving poll is not queued and still owes the marker");
  is_currently_parked = true;
  bufferPoll(parked_fix, true);
  check(obd_result_vector.size() == 2 && obd_result_vector[0][0].second == T - 3 && obd_result_vector[1][0].second == T - 2,
        "frames queued before the phone relayed stay queued, in order");
  bufferPoll(parked_fix);
  check(obd_result_vector.size() == 3, "after the session the fix the phone never carried is queued");
  network.received.clear();
  check(uploadBuffered(false) && obd_result_vector.empty() && network.received.size() == 3, "the handoff drains them");

  upload_capped = 0;
  for (int i = 0; i <= MAX_UPLOAD_BUFFER; ++i) bufferPoll({{"utc", T - MAX_UPLOAD_BUFFER + i}, {"soc", 50}});
  check(obd_result_vector.size() == MAX_UPLOAD_BUFFER && obd_result_vector[0][0].second == T - MAX_UPLOAD_BUFFER + 1 &&
        upload_capped == 1, "the cap drops the oldest frame and counts it for the log");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 42000;
  heap_fixture_maxblock = 42000;
  park_marker_owed = false;
  size_t max_queued = 0;
  for (unsigned i = 0; i < 160; ++i) {
    bufferPoll(richUploadFrame(T + i, i));
    max_queued = std::max(max_queued, obd_result_vector.size());
    check(ESP.getFreeHeap() >= 32768, "offline queue leaves the configured internal-heap reserve");
    check(ESP.getMaxAllocHeap() >= 16384, "offline queue leaves the configured largest-block reserve");
  }
  check(max_queued < 40 && obd_result_vector.size() < 40,
        "sustained rich offline polls stay below the heap-driven frame budget");
  check(upload_heap_dropped > 0 && obd_result_vector.back()[0].second == T + 159,
        "heap evictions preserve the newest frame and count memory drops");
  for (size_t i = 1; i < obd_result_vector.size(); ++i)
    check(obd_result_vector[i - 1][0].second < obd_result_vector[i][0].second,
          "adaptive queue remains FIFO after evicting older frames");
  const unsigned first_retained = 160 - obd_result_vector.size();
  for (size_t i = 0; i < obd_result_vector.size(); ++i) {
    double utc = 0, soc = 0;
    for (const auto& value : obd_result_vector[i]) {
      if (value.first == "utc") utc = value.second;
      if (value.first == "soc") soc = value.second;
    }
    const unsigned expected = first_retained + i;
    check(utc == T + expected && soc == 45 + expected / 100.0,
          "adaptive queue retains the exact newest UTC/SOC suffix");
  }
  check(obd_result_vector.back()[1].first == "soc" &&
        obd_result_vector.back()[1].second == 45 + 159 / 100.0,
        "adaptive queue retains battery telemetry on the newest frame");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 80000;
  heap_fixture_maxblock = 16384;
  const GeoMarks prior_marks = g_geo_queued;
  OBDVector geo_poll{{"utc", T}, {"soc", 50}, {"lat", 59.33}, {"lon", 18.06},
                     {"__geo_loc_utc", T}};
  GeoMarks candidate_marks{};
  bulkStringV2({geo_poll}, T, &candidate_marks);
  check(candidate_marks.location.utc == T, "heap-refusal fixture carries a new location mark");
  bufferPoll(geo_poll);
  check(obd_result_vector.empty(), "fragmented heap refuses a frame below the largest-block reserve");
  check(g_geo_queued.location.utc == prior_marks.location.utc &&
        g_geo_queued.location.lat == prior_marks.location.lat &&
        g_geo_queued.location.lon == prior_marks.location.lon,
        "a refused frame does not advance the queued geo mark");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 32768;
  heap_fixture_maxblock = 40000;
  bufferPoll({{"utc", T}, {"soc", 50}});
  check(obd_result_vector.empty(), "an already-low heap refuses even a small incoming frame");

  OBDVector boundary{{"utc", T}, {"soc", 50}, {"is_parked", 0}};
  const size_t boundary_cost = expectedQueueCopyBytes(boundary, 0, 0);
  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 32768 + boundary_cost;
  heap_fixture_maxblock = 16384 + boundary_cost;
  bufferPoll({{"utc", T}, {"soc", 50}});
  check(obd_result_vector.size() == 1, "exact reserve plus estimated copy admits the frame");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 32768 + boundary_cost - 1;
  heap_fixture_maxblock = 16384 + boundary_cost;
  bufferPoll({{"utc", T}, {"soc", 50}});
  check(obd_result_vector.empty(), "one byte under the heap reserve refuses the frame: cost=" +
        std::to_string(boundary_cost) + " free=" + std::to_string(ESP.getFreeHeap()) +
        " queued=" + std::to_string(obd_result_vector.size()));

  resetFixture();
  setClock(CLK_HTTP, T);
  std::vector<OBDVector>().swap(obd_result_vector);
  is_currently_parked = true;
  park_marker_owed = true;
  obd_result_vector.assign(MAX_UPLOAD_BUFFER, {{"utc", T}, {"soc", 49}, {"is_parked", 0}});
  queueParkMarker();
  check(obd_result_vector.size() == MAX_UPLOAD_BUFFER && obd_result_vector.back().size() == 2 &&
        obd_result_vector.back()[0].first == "is_parked" && upload_capped == 1 && !park_marker_owed,
        "the PARK marker is last and evicts the oldest frame at the count limit");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 32768;
  heap_fixture_maxblock = 40000;
  is_currently_parked = true;
  park_marker_owed = true;
  obd_result_vector.push_back({{"utc", T}, {"soc", 49}});
  queueParkMarker();
  check(obd_result_vector.empty() && park_marker_owed,
        "an unqueueable PARK marker leaves its debt owed");

  resetFixture();
  setClock(CLK_HTTP, T);
  heap_fixture_enabled = true;
  heap_fixture_free = 42000;
  heap_fixture_maxblock = 42000;
  std::string rows;
  for (unsigned i = 0; i < MAX_UPLOAD_BUFFER; ++i) {
    OBDVector frame = richUploadFrame(T + i, i);
    frame.emplace_back("lat", 59.33);
    frame.emplace_back("lon", 18.06);
    frame.emplace_back("__geo_loc_utc", T + i);
    rows += encodeUploadFrame(frame).dump() + '\n';
  }
  LittleFS.seed(rows);
  LittleFS.read_observer = observeBacklogRead;
  OBDVector newer_ram = richUploadFrame(T + MAX_UPLOAD_BUFFER, MAX_UPLOAD_BUFFER);
  newer_ram.emplace_back("lat", 59.33);
  newer_ram.emplace_back("lon", 18.06);
  newer_ram.emplace_back("__geo_loc_utc", T + MAX_UPLOAD_BUFFER);
  obd_result_vector = {newer_ram};
  loadUploadBacklog();
  LittleFS.read_observer = nullptr;
  check(backlog_read_count == MAX_UPLOAD_BUFFER && backlog_read_bounds_ok &&
        backlog_queue_grew_during_read,
        "the global queue stays bounded and grows during the file-row restore pass");
  check(obd_result_vector.size() < 40 && ESP.getFreeHeap() >= 32768 &&
        ESP.getMaxAllocHeap() >= 16384,
        "backlog restore admits frames incrementally under the same heap reserves");
  check(obd_result_vector.back()[0].second == T + MAX_UPLOAD_BUFFER &&
        obd_result_vector.back()[1].first == "soc",
        "backlog restore keeps the newest RAM frame and battery data");
  bool retained_geo_provenance = false;
  for (const auto& value : obd_result_vector.back())
    if (value.first == "__geo_loc_utc") retained_geo_provenance = value.second == T + MAX_UPLOAD_BUFFER;
  check(retained_geo_provenance, "backlog restore preserves numeric geo provenance");
  const auto retained = obd_result_vector;
  std::vector<json> retained_private_rows;
  for (const auto& frame : retained) retained_private_rows.push_back(encodeUploadFrame(frame));
  heap_fixture_enabled = false;
  check(saveUploadBacklog() && obd_result_vector == retained,
        "saving a restored queue keeps RAM unchanged");
  obd_result_vector.clear();
  loadUploadBacklog();
  check(bulkStringV2(obd_result_vector) == bulkStringV2(retained),
        "save and restore preserve retained frames and provenance");
  std::vector<json> restored_private_rows;
  for (const auto& frame : obd_result_vector) restored_private_rows.push_back(encodeUploadFrame(frame));
  check(restored_private_rows == retained_private_rows,
        "save and restore preserve each frame's private geo shadow values");

  for (bool pending_ram : {false, true}) {
    resetFixture();
    setClock(CLK_HTTP, T);
    is_currently_parked = true;
    park_marker_owed = false;
    heap_fixture_enabled = true;
    heap_fixture_free = 32768;
    heap_fixture_maxblock = 40000;
    OBDVector marker{{"utc", T}, {"is_parked", 1}};
    if (pending_ram) {
      LittleFS.seed("\n");
      obd_result_vector = {marker};
    }
    else {
      LittleFS.seed(encodeUploadFrame(marker).dump() + '\n');
    }
    loadUploadBacklog();
    check(obd_result_vector.empty() && park_marker_owed,
          std::string(pending_ram ? "pending RAM" : "file") +
          " PARK marker refusal restores its owed debt");
    heap_fixture_enabled = false;
    queueParkMarker();
    check(obd_result_vector.size() == 1 && !park_marker_owed &&
          obd_result_vector.back().size() == 2 && obd_result_vector.back()[0].first == "is_parked",
          std::string(pending_ram ? "pending RAM" : "file") +
          " PARK marker can be queued after heap recovers");
  }
}

int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  const std::pair<std::string, void(*)()> tests[] = {
    {"link-range", linkRange}, {"link-failures", linkFailures}, {"link-truncated", linkTruncated},
    {"link-required", linkRequired}, {"link-atomic", linkAtomic}, {"link-valid", linkValid},
    {"gps-replay", gpsReplay}, {"gps-rejected", gpsRejected}, {"gps-stationary", gpsStationary},
    {"gps-relocate", gpsRelocate}, {"gps-jump", gpsJump},
    {"direct-missing", directMissing}, {"direct-range", directRange}, {"queue", serializerAndQueue},
    {"gps-diagnostics", gpsDiagnostics}, {"gps-direct-diagnostics", directGpsDiagnostics},
    {"geo-legacy", geoLegacy}, {"geo-times", geoTimes}, {"geo-privacy", geoPrivacy}, {"geo-accepted", geoAccepted},
    {"geo-calendar", geoCalendar}, {"geo-freeze", geoFreeze}, {"gps-clock-no-position", gpsClockWithoutPosition},
    {"geo-history", geoRecordHistory}, {"geo-freeze-bounds", geoFreezeBounds}, {"geo-rtc", geoRtc},
    {"geo-profile", geoProfile}, {"geo-backlog", geoBacklog}, {"geo-downgrade", geoDowngrade},
    {"geo-backlog-rows", geoBacklogRows}, {"geo-backlog-nonfinite", geoBacklogNonfinite}, {"upload-gate", uploadGate},
  };
  bool matched = false;
  for (const auto& test : tests) {
    if (selected != "all" && selected != test.first) continue;
    matched = true;
    resetFixture();
    test.second();
    std::cout << "PASS: " << test.first << '\n';
  }
  check(matched, "known test selection");
}
