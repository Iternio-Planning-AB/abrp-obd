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

unsigned long millis() { return 1000; }
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
  const std::string replies[] = {
    "", "OK\r", "$GNIFO,", "$GNIFO,41026", "$GNIFO,41026,110758,59329000",
    "$GNIFO,41026,110758,59329000,18068000,", "$GNIFO,41026,110758,59329000,18068000,2500\r",
    "$GNIFO,41026,110758,59329000,18068000,2500,2000\r",
    "$GNIFO,41026,110758,59329000,18068000,2500,2000,9000",
  };
  for (const auto& reply : replies) {
    check(linkHeading("9000") == 90, "seed valid moving course");
    link.response = reply;
    sys.gpsGetData(&gd);
    check(gpsData.heading == GPS_HEADING_INVALID, "failed/incomplete reply clears previous heading: " + reply);
    OBDVector frame{{"utc", 1791112000}, {"soc", 50}};
    addGpsData(frame);
    check(!json::parse(bulkStringV2({frame}))[0].contains("heading"), "stale heading not uploaded");
  }
  linkHeading("9000");
  sys.link = nullptr;
  check(!sys.gpsGetData(&gd) && gpsData.heading == GPS_HEADING_INVALID, "missing link clears heading");
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
    {"link-range", linkRange}, {"link-failures", linkFailures},
    {"direct-missing", directMissing}, {"direct-range", directRange}, {"queue", serializerAndQueue},
  };
  bool matched = false;
  for (const auto& test : tests) {
    if (selected != "all" && selected != test.first) continue;
    matched = true;
    test.second();
    std::cout << "PASS: " << test.first << '\n';
  }
  check(matched, "known test selection");
}
