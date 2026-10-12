#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"
#include "parser.h"
#include "FreematicsBase.h"

using OBDVector = std::vector<std::pair<std::string, double>>;
#include "charge_config.inc"

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct ProgressState {
  double prev_parked_soc = -1, prev_parked_kwh = -1, prev_parked_soe = -1;
  bool poll(const OBDVector& current_frame, bool is_currently_parked = true) {
#include "charge_progress.inc"
    return charge_progress;
  }
};

void progressChecks() {
  ProgressState state;
  check(!state.poll({{"soe", 24407}}), "first remaining-energy sample anchors only");
  check(!state.poll({{"soe", 24468}}), "61 Wh BMS estimate adjustment is not charge progress");
  check(!state.poll({{"soe", 24550}}), "sub-threshold estimates accumulate without counting as charge");
  check(state.poll({{"soe", 24626}}), "219 Wh rise preserves charge progress through a bad current sample");
  check(!state.poll({{"soe", 24626}}), "unchanged remaining energy is not repeated progress");
  check(!state.poll({{"soe", 24000}}), "discharge reanchors remaining energy");
  check(state.poll({{"soe", 24219}}), "new charge is detected after discharge");
  check(!state.poll({{"soe", 25000}}, false), "driving regeneration is not parked charging");
  check(!state.poll({{"soe", 25000}}), "arrival cannot reuse driving regeneration as charge progress");
  check(!state.poll({{"soe", 25100}}, false), "sub-threshold driving regeneration reanchors");
  check(!state.poll({{"soe", 25219}}), "arrival excludes sub-threshold regeneration from the parked delta");
  check(!state.poll({}), "missing energy does not manufacture progress");
  ProgressState legacy;
  check(!legacy.poll({{"kwh_charged", 1000}}), "legacy counter anchors only");
  check(legacy.poll({{"kwh_charged", 1000.01}}), "cumulative kWh profiles retain their existing threshold");
  ProgressState soc;
  check(!soc.poll({{"soc", 40}}), "SoC-only profiles anchor");
  check(soc.poll({{"soc", 40.3}}), "SoC-only progress remains supported");
}

#define ENABLE_LOGGING 0

struct CorroborationState {
  double charge_corr_soc_hw = -1, charge_corr_kwh_hw = -1, charge_corr_soe_hw = -1;
  uint32_t charge_uncorroborated_s = 600;
  uint64_t charge_corr_last_us = 1;
  void reset() {
#include "reset_corroboration.inc"
  }
  std::pair<bool, bool> poll(const OBDVector& current_frame, bool cadence_charging = true) {
#include "charge_corroboration.inc"
    return {has_charge_counter, corroborated};
  }
};

void corroborationChecks() {
  CorroborationState state;
  check(state.poll({{"soe", 24407}}) == std::make_pair(true, false), "soe is available charge corroboration");
  check(!state.poll({{"soe", 24468}}).second, "small estimate adjustment cannot corroborate charging");
  check(state.poll({{"soe", 24626}}).second, "meaningful soe progress corroborates charging");
  check(state.charge_uncorroborated_s == 0, "remaining-energy progress clears the stale-charge budget");
  check(!state.poll({{"soe", 24626}}).second, "same energy cannot repeatedly corroborate");
  check(!state.poll({{"soe", 24000}}).second, "falling energy cannot corroborate");
  check(!state.poll({{"soe", 24626}}).second, "dithering below the high-water mark cannot corroborate");
  check(state.poll({{"soe", 24850}}).second, "further genuine progress advances the high-water mark");
  check(state.poll({}) == std::make_pair(false, false), "missing counters remain distinguishable");
  state.reset();
  check(state.charge_corr_soe_hw == -1 && state.charge_corr_last_us == 0,
        "ending a charge clears the remaining-energy high-water mark and timing");
  check(state.poll({{"soe", 1000}}) == std::make_pair(true, false), "a new session anchors below the old session");
  check(state.poll({{"soe", 1219}}).second, "a new session can corroborate below the previous high-water mark");
  CorroborationState legacy;
  legacy.poll({{"kwh_charged", 1000}});
  check(legacy.poll({{"kwh_charged", 1000.01}}).second, "legacy cumulative counters still corroborate");
}

#define PRINT 0
using json = nlohmann::json;
#include "telemetry_format.h"

enum ActivityState { BLE, ACTIVE, CHARGING, STANDBY, INIT };
ActivityState current_state = ACTIVE;
unsigned long millis() { return 1000; }
int esp_reset_reason() { return 1; }
struct RTCStub { unsigned long getEpoch() { return 1791192686; } } rtc;
struct ESPStub {
  unsigned int getFreeHeap() { return 65536; }
  unsigned int getMinFreeHeap() { return 64000; }
  unsigned int getMaxAllocHeap() { return 49152; }
  unsigned int getFreePsram() { return 4000000; }
} ESP;
GPS_DATA* gd = nullptr;
bool g_gps_powered = false;
struct SysStub {
  GPSReadDiagnostics gpsReadDiagnostics() const { return {}; }
} sys;
static unsigned long gpsFixAgeMs() { return ULONG_MAX; }
static const char* gpsLocationSource() { return "omitted"; }
float g_last_voltage = 13.4, g_last_io36 = 13.4;
const char* g_wake_reason = "timer";
int g_last_http = 200;
std::string g_last_status = "ok";
bool g_clean_sleep = true;
#include "logging.inc"

std::vector<std::string> csv(const std::string& line) {
  std::vector<std::string> cells;
  size_t start = 0;
  while (true) {
    const size_t end = line.find(',', start);
    cells.push_back(line.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return cells;
}

void loggingChecks() {
  const auto header = csv(LOG_HEADER);
  const auto row = csv(buildLogLine({{"soe", 47077}, {"batt_temp", 17},
                                    {"batt_temp_min", -1.25}, {"batt_temp_max", 21}}, 4));
  check(header.size() == row.size(), "CSV header and telemetry row have identical widths");
  const auto energy_column = std::find(header.begin(), header.end(), "soe");
  check(energy_column != header.end() && energy_column > header.begin() && *(energy_column - 1) == "gps_source",
        "energy columns append after GPS diagnostics without shifting existing fields");
  for (const auto& expected : std::vector<std::pair<std::string, std::string>>{
       {"soe", "47077.000"}, {"batt_temp", "17.000"},
       {"batt_temp_min", "-1.250"}, {"batt_temp_max", "21.000"}}) {
    const auto position = std::find(header.begin(), header.end(), expected.first);
    check(position != header.end(), "CSV contains " + expected.first);
    check(row[position - header.begin()] == expected.second, "CSV preserves units for " + expected.first);
  }
  check(csv(buildLogLine({}, 0)).size() == header.size(), "missing PIDs still produce a complete CSV row");
}

void transportChecks() {
  Parser parser;
  for (int value = 0; value < 256; ++value) {
    check(parser.parse("Signed(A)", {value})->evaluate() == (value < 128 ? value : value - 256),
          "signed temperature bytes decode without undefined floating-to-int8 conversion");
  }
  const OBDVector frame{{"utc", 1791192686}, {"soc", 49.11}, {"soh", 98.05},
    {"voltage", 359.34}, {"current", -51.23}, {"soe", 42111}, {"batt_temp_min", 15.12},
    {"batt_temp_max", 21.34}, {"speed", 0}, {"ext_temp", 17}, {"is_parked", 1},
    {"is_charging", 1}, {"is_dcfc", 0}};
  const auto point = json::parse(bulkStringV2({frame}, 1791192686))[0];
  check(point["soe"]["wh"] == 42111, "cellular preserves remaining energy in Wh");
  check(!point.contains("chargingEnergyAdded"), "remaining energy is not uploaded as charging energy");
  check(point["batteryTemperatureMin"]["c"] == 15.12, "cellular carries minimum temperature");
  check(point["batteryTemperatureMax"]["c"] == 21.34, "cellular carries maximum temperature");
  const auto ble = bleTlmString(frame);
  check(ble.size() <= 244, "promoted Scenic fields fit the Scenic test car's negotiated MTU 247");
  const auto tlm = json::parse(ble)["tlm"];
  check(tlm["soe"] == 42111 && !tlm.contains("kwh_charged"), "BLE preserves remaining-energy meaning");
  check(tlm["batt_temp_min"] == 15.12 && tlm["batt_temp_max"] == 21.34, "BLE carries temperature bounds");
  const auto legacy = json::parse(bulkStringV2({{{"utc", 1791192686}, {"kwh_charged", 123.456}}}))[0];
  check(legacy["chargingEnergyAdded"]["wh"] == 123456, "legacy profiles retain kWh-to-Wh conversion");
}

struct File {
  std::string* data = nullptr;
  size_t position = 0;
  explicit operator bool() const { return data != nullptr; }
  size_t size() const { return data ? data->size() : 0; }
  int read() { return data && position < data->size() ? static_cast<unsigned char>((*data)[position++]) : -1; }
  void close() { data = nullptr; }
};
struct FilesystemStub {
  std::map<std::string, std::string> files;
  bool begin(bool) { return true; }
  File open(const char* path, const char* mode) {
    if (*mode == 'r' && !files.count(path)) return {};
    return {&files[path]};
  }
} LittleFS;
struct SerialStub {
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
const size_t SEGMENT_BYTES = 256 * 1024;
struct DataLogger {
  bool mounted = false;
  std::string header_;
  File active;
  int activeIndex = -1;
  size_t activeSize = 0;
  std::string pathFor(int index) {
    char path[24];
    std::snprintf(path, sizeof(path), "/log%04d.csv", index);
    return path;
  }
  int highestIndex() {
    int highest = -1;
    for (const auto& entry : LittleFS.files) highest = std::max(highest, std::stoi(entry.first.substr(4, 4)));
    return highest;
  }
  bool openSegment(int index) {
    active = LittleFS.open(pathFor(index).c_str(), "a");
    activeIndex = index;
    if (active.size() == 0) *active.data = header_ + "\n";
    activeSize = active.size();
    return true;
  }
  void enforceBudget() {}
  void printStats(SerialStub&) {}
  bool begin(const std::string& header);
};
#include "logger_begin.inc"

void schemaChecks() {
  const std::string old = "soc,kwh_charged\n49.24,42.230\n";
  LittleFS.files = {{"/log0000.csv", old}};
  DataLogger updated;
  check(updated.begin(LOG_HEADER), "updated logger mounts");
  check(LittleFS.files.size() == 2, "CSV schema change rotates rather than appending beneath an old header");
  check(LittleFS.files.at("/log0000.csv") == old, "schema migration preserves existing log rows");
  check(LittleFS.files.at("/log0001.csv") == LOG_HEADER + "\n", "new segment starts with the new schema");
  DataLogger wake;
  check(wake.begin(LOG_HEADER), "next wake mounts");
  check(LittleFS.files.size() == 2, "unchanged schema reuses the existing segment");
  LittleFS.files = {{"/log0000.csv", ""}};
  DataLogger empty;
  check(empty.begin(LOG_HEADER) && LittleFS.files.size() == 1, "empty segment needs a header, not another segment");
}

void profileChecks(const std::string& directory) {
  Parser parser;
  for (const auto& name : {"scenic", "scenic_devel", "ix3", "ix3_devel"}) {
    std::ifstream file(directory + "/" + name + ".json");
    check(file.good(), "profile exists: " + std::string(name));
    json profile;
    file >> profile;
    const bool developer = std::string(name).find("_devel") != std::string::npos;
    const bool scenic = std::string(name).find("scenic") == 0;
    const bool usesWh = scenic || developer;
    check(profile.contains("soe") == usesWh && profile.contains("kwh_charged") == !usesWh,
          "Scenic remaining energy is promoted while BMW remaining energy stays developer-only");
    const std::vector<int> bytes = scenic ? std::vector<int>{0, 183, 229} : std::vector<int>{31, 97, 50, 11};
    const double energy = parser.parse(profile[usesWh ? "soe" : "kwh_charged"]["equation"], bytes)->evaluate();
    check(std::abs(energy * (usesWh ? 1 : 1000) - (scenic ? 47077 : 30330)) < 0.001,
          "firmware supports Scenic Wh and unchanged BMW production kWh units");
    check(profile.contains("batt_temp_min") && profile.contains("batt_temp_max"),
          "validated temperature bounds are available in production and developer profiles");
    for (const auto& key : {"batt_temp_min", "batt_temp_max"}) {
      const std::vector<int> temperatures = scenic ? std::vector<int>{3, 112} : std::vector<int>{5, 220, 8, 52, 6, 164};
      const double value = parser.parse(profile[key]["equation"], temperatures)->evaluate();
      check(value == (scenic || std::string(key) == "batt_temp_min" ? 15 : 21), "firmware decodes temperature bounds");
    }
    const std::vector<int> cold = scenic ? std::vector<int>{2, 80} : std::vector<int>{255, 131, 0, 0, 0, 50};
    const double minimum = parser.parse(profile["batt_temp_min"]["equation"], cold)->evaluate();
    check(minimum == (scenic ? -3 : -1.25), "firmware preserves negative temperatures");
  }
}

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "all";
  check(group == "all" || group == "progress" || group == "corroboration" ||
        group == "logging" || group == "transport" || group == "profiles" || group == "schema", "known test group");
  if (group == "all" || group == "progress") progressChecks();
  if (group == "all" || group == "corroboration") corroborationChecks();
  if (group == "all" || group == "logging") loggingChecks();
  if (group == "all" || group == "schema") schemaChecks();
  if (group == "all" || group == "transport") transportChecks();
  if (argc > 2 && (group == "all" || group == "profiles")) profileChecks(argv[2]);
  std::cout << "Energy " << group << " checks PASS\n";
}
