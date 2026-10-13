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
  bool charge_signal_trusted = true;
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

// One loop() poll from is_charging through the next-wake cadence, extracted whole. RTC latches are members
// and millis() is the poll's own clock, so a check replays a field wake sample by sample.
struct ChargePoll {
  bool recently_charging = false, g_sample_poke = false, g_taper_confirm = false, charging_cap_latched = false;
  uint8_t charge_release_polls = 0, charge_silent_polls = 0;
  uint32_t taper_confirm_wakes = 0, charging_fast_minutes = 0, charge_uncorroborated_s = 0;
  double prev_parked_soc = -1, prev_parked_kwh = -1, prev_parked_soe = -1, prev_charge_soc = 0;
  double charge_corr_soc_hw = -1, charge_corr_kwh_hw = -1, charge_corr_soe_hw = -1;
  uint64_t charge_corr_last_us = 0;
  int g_next_wake_minutes = 0;
  unsigned long g_last_discharge_ms = 0, now = 0;
  unsigned long millis() const { return now; }
  uint64_t esp_rtc_get_time_us() const { return (uint64_t)now * 1000ULL; }
  void resetChargeCorroboration() {
#include "reset_corroboration.inc"
  }
  struct Result { bool evaluated, charging, progress; };
  Result poll(OBDVector current_frame, bool is_currently_parked = true) {
    const int obd_count = (int)current_frame.size();
    const bool is_dcfc = false;
#include "charge_signal.inc"
#include "charge_progress.inc"
#include "charge_release.inc"
    bool evaluated = false, published = false;
    for (const auto& res : current_frame)
      if (res.first == "is_charging") { evaluated = true; published = res.second != 0; }
    check(published == is_charging, "the published flag is the verdict the cadence ran on");
    return {evaluated, published, charge_progress};
  }
};

struct Sample { unsigned long ms; double current; };

// FIELD 2026-09-30 04:46 motion wake, latched PARK until the GNSS unlatched at 367 s; every sample is
// regen or traction. The old firmware published CHARGING_AC on 12 of them.
const std::vector<Sample> DRIVE_0930 = {{19095,NAN},{25215,0.8},{31216,1.2},{37206,1.9},{43122,3.1},
  {49098,2.8},{56126,3.0},{62421,4.4},{73005,8.0},{78995,10.2},{85410,4.2},{91374,13.2},{97831,-10.4},
  {103762,24.9},{109807,30.9},{115742,1.2},{121853,35.5},{127847,-1.1},{143553,13.8},{149563,29.5},
  {155628,4.1},{161683,14.6},{167689,4.9},{173699,12.5},{179678,13.3},{185650,11.8},{191641,-2.3},
  {197640,-15.6},{216435,-7.6},{222435,2.3},{228465,-4.2},{234418,76.0},{240379,60.5},{246421,-24.5},
  {252374,18.4},{258333,28.1},{264401,-18.2},{270549,-37.1},{280826,-13.7},{286889,100.6},{292834,3.5},
  {298786,26.9},{305200,38.3},{311145,-1.3},{317184,-46.8},{323126,64.9},{329134,-26.9},{335088,22.0},
  {343467,-12.7},{349422,9.6},{355377,33.4},{361338,21.8},{367382,2.2}};
// FIELD 2026-10-12 14:25 motion wake: no GNSS fix for four minutes, unlatched at 459 s.
const std::vector<Sample> DRIVE_1012 = {{20468,NAN},{25997,NAN},{31597,NAN},{37192,NAN},{43190,NAN},
  {49891,1.2},{56041,2.1},{62441,3.2},{73418,4.1},{79428,3.3},{85368,9.7},{91337,4.0},{97409,30.6},
  {103412,-3.8},{109399,41.1},{115349,31.8},{121372,-7.3},{127499,12.0},{136334,1.4},{142775,7.0},
  {148791,63.4},{154750,2.8},{160695,12.0},{166680,12.5},{173157,-8.6},{179281,0.5},{185231,3.9},
  {191496,9.3},{199667,15.9},{205653,18.3},{211623,21.3},{217748,18.9},{223713,13.7},{229758,-9.3},
  {235847,-8.7},{242018,2.5},{247965,2.5},{253914,3.4},{262045,3.1},{268045,21.5},{274785,0.8},
  {281098,1.4},{287135,3.4},{293101,2.0},{299111,10.1},{305076,50.0},{311046,57.1},{317012,28.1},
  {326660,-15.2},{332602,14.3},{338612,-22.9},{344629,-18.0},{350666,-41.9},{356874,1.9},{363025,4.6},
  {368984,2.8},{374944,1.8},{380915,2.8},{396461,0.1},{402896,1.2},{408871,2.1},{414886,1.7},
  {420837,2.6},{426788,2.0},{432742,1.6},{438692,2.3},{444647,1.7},{450606,2.5},{459264,23.4}};
// FIELD 2026-10-04 same wake as a drive: arrive, park, plug in at 701 s, SoC 40 -> 51.5 over the hour.
const std::vector<Sample> PLUG_IN_1004 = {{583731,11.7},{589697,-3.2},{595667,-8.3},{601783,-6.5},{607738,18.8},
  {626710,0.6},{632893,0.8},{638930,-3.3},{645005,1.2},{650985,2.7},{656788,1.7},{663181,0.0},
  {669496,0.0},{676006,0.0},{681978,0.0},{696025,0.0},{701838,-12.8},{707346,NAN},{713158,-13.0},
  {718994,-13.0},{724558,NAN},{730118,NAN},{735944,-13.0},{741810,-13.0},{747304,NAN},{752880,NAN},
  {765010,NAN},{770585,NAN},{776168,NAN},{781652,NAN},{787497,-13.0},{793573,-13.0},{799453,-13.0},
  {805528,-13.0},{811148,NAN},{817634,-13.0},{823721,-13.0},{835849,NAN},{841916,-13.0},{847866,-13.0},
  {853355,NAN},{858833,NAN},{864406,NAN},{870437,-13.0},{876503,-13.0}};

OBDVector sampleFrame(const Sample& s, double kwh) {
  OBDVector frame{{"soc", 54}, {"kwh_charged", kwh}};
  if (!std::isnan(s.current)) frame.emplace_back("current", s.current);
  return frame;
}

void signalChecks() {
  for (const auto* drive : {&DRIVE_0930, &DRIVE_1012}) {
    ChargePoll wake;
    double kwh = 9550.5;
    for (const auto& s : *drive) {
      wake.now = s.ms;
      if (s.current < 0) kwh += 0.1; // regen moves the cumulative counter well past its 0.005 kWh floor
      const auto r = wake.poll(sampleFrame(s, kwh));
      check(!r.charging, "a latched-PARK drive never publishes charging from regen");
      check(!r.progress, "regen on a latched-PARK drive is not charge progress");
      check(!(s.current < CHARGING_CURRENT_THRESHOLD && r.evaluated),
            "an unconfirmed charging sample is not evaluated, rather than claimed NOT_CHARGING");
    }
    check(!wake.recently_charging, "a drive leaves no charge latch for the next wake");
  }

  ChargePoll plug;
  bool confirmed = false;
  for (const auto& s : PLUG_IN_1004) {
    plug.now = s.ms;
    const auto r = plug.poll(sampleFrame(s, 9600));
    if (s.ms < 650985UL + CHARGE_STREAM_CONFIRM_SECONDS * 1000UL)
      check(!r.charging, "charging waits out the window after the last discharge sample");
    else if (s.current < CHARGING_CURRENT_THRESHOLD)
      check(r.charging, "a plugged-in car in a streaming wake is reported charging once confirmed");
    confirmed |= r.charging;
  }
  check(confirmed && plug.recently_charging, "the plug-in sequence ends latched charging");

  ChargePoll poke;
  poke.g_sample_poke = true;
  poke.now = 21000;
  check(poke.poll({{"current", -13.0}}).charging, "a stationary sample poke trusts its single sample");
  check(poke.poll({{"current", -13.0}}, false).evaluated, "an unparked frame still states its verdict");
  check(!poke.poll({{"current", -13.0}}, false).charging, "an unparked frame never reports charging");
}

// A person-present wake at a charger: the latch was set by sample pokes, the owner walks up, and the cabin
// draws a few amps every two minutes, so the streaming window never confirms the charge.
void attendedChargeChecks() {
  ChargePoll wake;
  wake.recently_charging = true;
  wake.charging_fast_minutes = 600;
  for (unsigned long ms = 19000; ms < 600000; ms += 6000) {
    wake.now = ms;
    wake.poll({{"current", ms % 120000 < 6000 ? 3.0 : -18.0}, {"soc", 60}});
    check(wake.recently_charging, "an unconfirmed charging sample holds the latch it did not set");
  }
  check(wake.charging_fast_minutes >= 600, "an attended charge keeps its fast-cadence budget");
  wake.now += 6000;
  wake.poll({{"current", 30.0}, {"soc", 60}});
  wake.now += 6000;
  wake.poll({{"current", 25.0}, {"soc", 60}});
  check(!wake.recently_charging, "pulling away from the charger releases on discharge samples");
}

// A stuck charging reading at 100% on a car that answers every other poll: the SoC-stall budget must still run
// out, or taper confirm-polls keep waking the car until the rail drops.
void taperStallChecks() {
  ChargePoll wake;
  wake.g_sample_poke = wake.g_taper_confirm = true;
  wake.recently_charging = true;
  for (int i = 0; i < 2 * (TAPER_CONFIRM_MAX_WAKES + 1); ++i) {
    wake.now += 200000;
    wake.poll(i % 2 ? OBDVector{} : OBDVector{{"current", -3.0}, {"soc", 100}});
    check(wake.recently_charging, "the stuck reading keeps the latch");
  }
  check(wake.taper_confirm_wakes >= (uint32_t)TAPER_CONFIRM_MAX_WAKES,
        "held silent polls do not reset the SoC-stall budget");
}

// A charging current the car's counters never back, on a car that answers one poll in six: silence is not
// corroboration, so the slow-cadence fallback must arrive in about CHARGE_CORROBORATE_MINUTES of wall clock.
void silentCorroborationChecks() {
  ChargePoll wake;
  wake.g_sample_poke = true;
  int poll = 0;
  unsigned long relaxed_at = 0;
  while (wake.now < 4UL * 60 * 60 * 1000 && !relaxed_at) {
    wake.now += WAKE_CHARGING_MINUTES * 60000UL;
    wake.poll(poll++ % 6 ? OBDVector{} : OBDVector{{"current", -12.0}, {"soe", 40000}});
    if (wake.recently_charging && wake.g_next_wake_minutes == WAKE_PARKED_MINUTES) relaxed_at = wake.now;
  }
  check(relaxed_at >= CHARGE_CORROBORATE_MINUTES * 60000UL && relaxed_at <= (CHARGE_CORROBORATE_MINUTES + 30) * 60000UL,
        "an uncorroborated charge relaxes on wall-clock time even when most polls are silent");

  // A profile with no counter PID can never corroborate, so its silent polls must not count against it.
  ChargePoll current_only;
  current_only.g_sample_poke = true;
  poll = 0;
  while (current_only.now < 4UL * 60 * 60 * 1000) {
    current_only.now += WAKE_CHARGING_MINUTES * 60000UL;
    current_only.poll(poll++ % 3 ? OBDVector{} : OBDVector{{"current", -12.0}});
    check(current_only.g_next_wake_minutes == WAKE_CHARGING_MINUTES,
          "a charge on a car without a counter keeps the charging cadence through silent polls");
  }
}

void releaseChecks() {
  ChargePoll latch;
  latch.g_sample_poke = true;
  latch.recently_charging = true;
  for (int i = 0; i < CHARGE_SILENT_HOLD_POLLS; ++i) {
    latch.poll({});
    check(latch.recently_charging && latch.charge_release_polls == 0, "a silent poll does not release the latch");
  }
  for (int i = 0; i < CHARGE_RELEASE_POLLS; ++i) latch.poll({});
  check(!latch.recently_charging, "silence past the hold releases the latch, bounding a car that never answers");

  ChargePoll answered;
  answered.g_sample_poke = true;
  answered.recently_charging = true;
  answered.poll({});
  answered.poll({{"current", -11.8}});
  check(answered.charge_silent_polls == 0, "an answered poll re-arms the silent hold");
  for (int i = 0; i < CHARGE_RELEASE_POLLS; ++i) answered.poll({{"current", 0.4}});
  check(!answered.recently_charging, "answered polls with no evidence still release after CHARGE_RELEASE_POLLS");
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

static_assert(IO36_LOCKOUT_HIGH < CHARGE_END_POLL_FLOOR && CHARGE_END_POLL_FLOOR < RAIL_TAPER_FLOOR &&
              RAIL_TAPER_FLOOR < RAIL_AWAKE_VOLTS, "wake-gate rail bands must stay ordered");

// The setup() wake gate, extracted whole: its RTC latches are members so each check controls the wake.
struct WakeGate {
  bool recently_charging = false, charge_end_polled = false, g_taper_confirm = false;
  bool taper = false, charge_end = false;
  uint32_t taper_confirm_wakes = 0;
  int corroboration_resets = 0;
  void resetChargeCorroboration() { corroboration_resets++; }
  bool poll(float rail, bool is_cold = false, bool is_motion = false, bool awake = false,
            bool heartbeat_due = false) {
#include "wake_gate.inc"
    taper = taper_bridge;
    charge_end = charge_end_poll;
    return should_poll;
  }
};

void wakeGateChecks() {
  WakeGate gate;
  check(!gate.poll(12.7f), "a parked rail with no charge latch is not polled");
  gate.recently_charging = true;
  check(gate.poll(13.4f) && !gate.charge_end && gate.recently_charging, "a high rail is a charging sample");
  check(gate.poll(13.1f) && gate.taper && gate.recently_charging, "a taper-band rail spends a confirm poll");
  check(gate.poll(12.7f) && gate.charge_end && !gate.recently_charging && gate.corroboration_resets == 1,
        "a rail below the taper band gets one charge-end read and drops the latch");
  gate.recently_charging = true;  // the final read still saw charging current
  check(!gate.poll(12.7f) && !gate.charge_end && !gate.recently_charging,
        "a re-latch after the charge-end read is not polled on a low rail");
  check(gate.poll(13.4f) && !gate.charge_end_polled, "a high-rail sample re-arms the charge-end read");
  gate.recently_charging = true;
  check(gate.poll(12.7f) && gate.charge_end, "the next charge gets its own charge-end read");
  gate.recently_charging = true;
  gate.charge_end_polled = false;
  gate.taper_confirm_wakes = TAPER_CONFIRM_MAX_WAKES;
  check(!gate.poll(13.1f) && !gate.charge_end && !gate.recently_charging,
        "a spent stall budget gets no extra read");
  gate.recently_charging = true;
  check(!gate.poll(12.1f) && !gate.charge_end && !gate.recently_charging,
        "a rail under the charge-end floor is not polled");
  gate.recently_charging = true;
  check(!gate.poll(10.5f, true) && gate.recently_charging, "a weak rail is never polled, even on a cold boot");
}

// FIELD 2026-10-05 01:40: the last read saw -11.8 A at a 14.8 V rail, the next two polls got no answer,
// then the rail fell to 13.06 V and 12.51 V. The latch must survive the silence for the rail drop to be read.
void chargeEndChecks() {
  ChargePoll loop;
  loop.g_sample_poke = true;
  loop.poll({{"current", -11.8}, {"soc", 77}});
  loop.poll({});
  loop.poll({});
  WakeGate gate;
  gate.recently_charging = loop.recently_charging;
  check(gate.poll(13.06f) && gate.taper, "a silent end of charge still gets its taper confirm");
  loop.recently_charging = gate.recently_charging;
  loop.poll({});
  gate.recently_charging = loop.recently_charging;
  check(gate.poll(12.51f) && gate.charge_end, "the rail drop after a silent end of charge gets the charge-end read");
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
  check(group == "all" || group == "progress" || group == "signal" || group == "release" ||
        group == "corroboration" || group == "wake-gate" ||
        group == "logging" || group == "transport" || group == "profiles" || group == "schema", "known test group");
  if (group == "all" || group == "progress") progressChecks();
  if (group == "all" || group == "signal") signalChecks();
  if (group == "all" || group == "release") {
    releaseChecks();
    attendedChargeChecks();
    taperStallChecks();
    silentCorroborationChecks();
  }
  if (group == "all" || group == "corroboration") corroborationChecks();
  if (group == "all" || group == "wake-gate") { wakeGateChecks(); chargeEndChecks(); }
  if (group == "all" || group == "logging") loggingChecks();
  if (group == "all" || group == "schema") schemaChecks();
  if (group == "all" || group == "transport") transportChecks();
  if (argc > 2 && (group == "all" || group == "profiles")) profileChecks(argv[2]);
  std::cout << "Energy " << group << " checks PASS\n";
}
