#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
using OBDVector = std::vector<std::pair<std::string, double>>;
using HeaderVector = std::vector<std::pair<std::string, std::string>>;

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
void gpsUnlatch() {
#include "gps_unlatch.inc"
}

struct HttpResponse { int responseCode; std::string payload; };
struct FakeNetwork {
  std::vector<json> received;
  int responseCode = 200;
  void set_headers(const HeaderVector&) {}
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
bool gzipBody(const std::string&, std::string&) { return false; }
int logging_errors = 0, g_last_http = 0, g_http0_streak = 0;
unsigned long g_last_post_ms = 0;
std::string g_last_status, tlm_push_token = "test-token", api_key = "test-key";
std::vector<OBDVector> obd_result_vector;
#include "network_upload.h"
#include "backlog_fs.h"
#define MAX_UPLOAD_BUFFER 120
#include "geo_backlog.h"
#include "backlog.inc"
namespace historical {
std::vector<OBDVector> obd_result_vector;
#include "historical_format.inc"
#include "historical_backlog.inc"
}

enum ResetReason { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_WDT, ESP_RST_DEEPSLEEP };
ResetReason reset_reason = ESP_RST_POWERON;

void resetFixture() {
  fake_ms = 1000;
  reset_reason = ESP_RST_POWERON;
  gps_location_source = "omitted";
  LittleFS.files.clear();
  LittleFS.fail_open = false;
  historical::obd_result_vector.clear();
  clock_epoch = 0;
  last_clock_sync_epoch = 0;
  geo_accepted = GPS_DATA{};
  geo_accepted_utc = park_acc_utc = frozen_geo_utc = 0;
  park_acc_unknown_utc = false;
  frozen_geo_lat = frozen_geo_lon = 0;
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
  gps_fix_date = gps_fix_time = gps_fix_seq = 0;
  gps_fix_changed_ms = park_last_ts = 0;
  park_acc_lat = park_acc_lon = 0;
  park_acc_n = gps_moving_streak = 0;
  network.received.clear();
  network.responseCode = 200;
  obd_result_vector.clear();
  logging_errors = g_last_http = g_http0_streak = 0;
  g_last_post_ms = 0;
  g_last_status.clear();
}

GPS_DATA acceptLink(const std::string& reply, const std::string& label) {
  sys.link = &link;
  link.response = reply;
  const unsigned commandCalls = link.commandCalls;
  check(pollGpsData(), label + " accepted");
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
  link.response = "$GNIFO,41026,110757,59329000,18068000,2500,2000," + raw + ",12,7\r";
  check(pollGpsData(), "numeric reply retains position");
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
  loadUploadBacklog();
  check(obd_result_vector.size() == MAX_UPLOAD_BUFFER && !LittleFS.exists(UPLOAD_BACKLOG_PATH),
        "MAX120 load cap and existing load/delete behavior unchanged");
  const auto bounded_points = json::parse(bulkStringV2(obd_result_vector, 1791500000));
  for (size_t i = 0; i < bounded_points.size(); ++i)
    check(bounded_points[i]["soc"]["time"] == v2Time(1791500000 + i), "mixed queue load cap preserves FIFO");
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
  check(bleTlmString(frame).size() <= 185 && ble["is_parked"] == true && ble["is_charging"] == true &&
        ble["is_dcfc"] == false && !ble.contains("lat") && !ble.contains("heading") && !ble.contains("utc"),
        "BLE phone geo, booleans and 185 byte contract unchanged");
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

int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  const std::pair<std::string, void(*)()> tests[] = {
    {"link-range", linkRange}, {"link-failures", linkFailures}, {"link-truncated", linkTruncated},
    {"link-required", linkRequired}, {"link-atomic", linkAtomic}, {"link-valid", linkValid},
    {"gps-replay", gpsReplay}, {"gps-rejected", gpsRejected}, {"gps-stationary", gpsStationary},
    {"direct-missing", directMissing}, {"direct-range", directRange}, {"queue", serializerAndQueue},
    {"gps-diagnostics", gpsDiagnostics}, {"gps-direct-diagnostics", directGpsDiagnostics},
    {"geo-legacy", geoLegacy}, {"geo-times", geoTimes}, {"geo-privacy", geoPrivacy}, {"geo-accepted", geoAccepted},
    {"geo-calendar", geoCalendar}, {"geo-freeze", geoFreeze}, {"gps-clock-no-position", gpsClockWithoutPosition},
    {"geo-history", geoRecordHistory}, {"geo-freeze-bounds", geoFreezeBounds}, {"geo-rtc", geoRtc},
    {"geo-profile", geoProfile}, {"geo-backlog", geoBacklog}, {"geo-downgrade", geoDowngrade},
    {"geo-backlog-rows", geoBacklogRows}, {"geo-backlog-nonfinite", geoBacklogNonfinite},
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
