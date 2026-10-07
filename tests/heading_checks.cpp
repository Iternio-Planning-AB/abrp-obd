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

#define PRINT 0
#define ENABLE_LOGGING 0
#define ENABLE_NETWORK 1
#define ENABLE_CELL 0
#define HEADING_MIN_SPEED_KMH 3
#define GPS_MOVING_MIN_KMH 5
#define PARK_ACC_MIN 12
#define MAX_FRAMES_PER_POST 12
#define UPLOAD_MAX_BATCHES_PER_CYCLE 3
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
};
#include "gps_get_data.inc"

struct FakeLink : CLink {
  std::string response;
  int sendCommand(const char* command, char* buffer, int size, unsigned int) override {
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
time_t utcToEpoch(int, int, int, int, int, int) { return 0; }
void setClock(ClockSource, time_t) {}
time_t parseHttpDate(const char*) { return 0; }
void esp_task_wdt_reset() {}
#include "gps_location.h"
#include "telemetry_format.h"
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

void resetFixture() {
  fake_ms = 1000;
  last_active = g_last_speed_ms = 0;
  gps = HostGPS{};
  gpsData = last_gd = GPS_DATA{};
  gd = &gpsData;
  gpsHasDecodedData = false;
  sys.m_flags = FLAG_GNSS_USE_LINK;
  sys.link = &link;
  link.response.clear();
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
  check(sys.gpsGetData(&gd), label + " accepted");
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
  check(!sys.gpsGetData(&gd), label + " rejected");
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
  check(sys.gpsGetData(&gd), "numeric reply retains position");
  check(gpsData.sat == 12 && gpsData.hdop == 7, "fields after course retained");
  return gpsData.heading;
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
    OBDVector frame{{"utc", 1791112000}, {"soc", 50}};
    addGpsData(frame);
    const auto point = json::parse(bulkStringV2({frame}))[0];
    check(!point.contains("heading") && point.contains("location") && point["soc"]["frac"] == 0.5,
          "failed reply preserves position and battery, never uploads stale heading");
  }
  linkHeading("9000");
  const GPS_DATA lastGood = *gd;
  sys.link = nullptr;
  check(!sys.gpsGetData(&gd), "missing link rejected");
  checkPreserved(lastGood, "missing link");
}

void linkTruncated() {
  check(linkHeading("9000") == 90, "seed valid moving fix");
  const GPS_DATA lastGood = *gd;
  link.response = "$GNIFO,71026,4490300,55738990";
  check(!sys.gpsGetData(&gd), "truncated timestamp-bearing reply rejected");
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
  check(!sys.gpsGetData(&gd), "altitude-only reply without speed rejected");
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
    OBDVector frame{{"utc", 1791112000}, {"soc", 50}};
    addGpsData(frame);
    const auto point = json::parse(bulkStringV2({frame}))[0];
    check(point.contains("location") && point["soc"]["frac"] == 0.5 &&
          point.contains("heading") == (tail.heading != GPS_HEADING_INVALID),
          "optional heading validity only controls heading emission");
  }
}

void parkedFixture() {
  g_gps_powered = is_currently_parked = frozen_valid = true;
  frozen_lat = 59.320;
  frozen_lon = 18.060;
}

OBDVector gpsFrame() {
  parkAccumSample();
  OBDVector frame{{"utc", 1791112000}, {"soc", 50}};
  addGpsData(frame);
  gpsUnlatch();
  frame.emplace_back("is_parked", is_currently_parked ? 1.0 : 0.0);
  return frame;
}

json checkState(const OBDVector& frame, bool parked, const std::string& label) {
  const auto point = json::parse(bulkStringV2({frame}))[0];
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

void directRmc(const std::string& course) {
  const std::string body = "GPRMC,110757,A,5920.000,N,01804.000,E,20.00," + course + ",041026,,,A";
  unsigned char checksum = 0;
  for (char c : body) checksum ^= c;
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  gpsHasDecodedData = false;
  for (char c : "$" + body + suffix) if (gps.encode(c)) gpsHasDecodedData = true;
  check(gpsHasDecodedData, "checksum-valid RMC decoded");
  check(sys.gpsGetData(&gd), "direct UART fix read");
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

void serializerAndQueue() {
  for (double heading : {0.0, 90.0, 359.0, 360.0, 360.01, 65446.0, 65535.0, -90.0,
                         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    const auto points = json::parse(bulkStringV2({{{"utc", 1791112000}, {"soc", 50}, {"heading", heading}}}));
    check(points.size() == 1 && points[0]["soc"]["frac"] == 0.5, "bad heading preserves battery point");
    if (heading >= 0 && heading <= 360)
      check(points[0]["heading"]["degrees"] == (heading == 360 ? 0 : heading), "serialized heading normalized");
    else check(!points[0].contains("heading"), "invalid saved heading omitted");
  }
  gpsData = GPS_DATA{};
  gpsData.lat = 1;
  gpsData.lng = 2;
  gpsData.speed = 10;
  gpsData.heading = GPS_HEADING_INVALID;
  OBDVector position{{"utc", 1791112000}, {"soc", 50}};
  addGpsData(position);
  const auto point = json::parse(bulkStringV2({position}))[0];
  check(!point.contains("heading") && point.contains("location"), "invalid heading preserves live position");

  obd_result_vector.clear();
  network.received.clear();
  for (int i = 0; i < 120; ++i)
    obd_result_vector.push_back({{"utc", 1791112000.0 + i}, {"soc", 50}, {"batt_temp_min", 20},
                                {"batt_temp_max", 25}, {"heading", i == 2 ? 360.0 : 90.0}});
  network.responseCode = 503;
  check(!uploadBuffered(false) && obd_result_vector.size() == 120 && network.received.empty(),
        "failed POST retains every queued point");
  network.responseCode = 200;
  check(uploadBuffered(true) && obd_result_vector.empty(), "synthetic backlog drained");
  check(network.received.size() == 120, "exactly 120 points delivered");
  for (size_t i = 0; i < network.received.size(); ++i) {
    const auto& received = network.received[i];
    check(received["soc"]["time"] == v2Time(1791112000 + i), "all timestamps delivered once in order");
    check(received["soc"]["frac"] == 0.5 && received["batteryTemperatureMin"]["c"] == 20 &&
          received["batteryTemperatureMax"]["c"] == 25, "battery data delivered intact");
    check(received["heading"]["degrees"] == (i == 2 ? 0 : 90), "bad third heading repaired");
  }
}

int main(int argc, char** argv) {
  const std::string selected = argc > 1 ? argv[1] : "all";
  const std::pair<std::string, void(*)()> tests[] = {
    {"link-range", linkRange}, {"link-failures", linkFailures}, {"link-truncated", linkTruncated},
    {"link-required", linkRequired}, {"link-atomic", linkAtomic}, {"link-valid", linkValid},
    {"gps-replay", gpsReplay}, {"gps-rejected", gpsRejected}, {"gps-stationary", gpsStationary},
    {"direct-missing", directMissing}, {"direct-range", directRange}, {"queue", serializerAndQueue},
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
