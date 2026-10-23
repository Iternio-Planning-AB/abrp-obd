#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  // The rail the gate saw before a sample poke. 13.1 V is the taper band, where silence still releases.
  float g_rail_before_poke = 13.1f;
  uint8_t charge_followup = 0, pdw_open = 0, pdw_polls = 0;
  uint64_t pdw_open_us = 0, charge_last_evidence_us = 0;
  const char* g_cadence_reason = nullptr;
  bool reason(const char* r) const { return g_cadence_reason && std::strcmp(g_cadence_reason, r) == 0; }
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
#include "charge_wake_state.inc"
  struct Result { bool evaluated, charging, progress; };
  Result poll(OBDVector current_frame, bool is_currently_parked = true) {
    const int obd_count = (int)current_frame.size();
#include "charge_signal.inc"
#include "charge_progress.inc"
#include "charge_dcfc.inc"
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


// A car that stops answering mid-charge while its DC-DC holds the rail high. The silence is the car, not the
// end of the charge, so it never releases; past the hold it waits at the parked cadence with the latch up.
void highRailSilenceChecks() {
  ChargePoll car;
  car.g_sample_poke = true;
  car.g_rail_before_poke = 14.9f;
  car.poll({{"current", -12.0}, {"soc", 50}});
  check(car.recently_charging && car.g_next_wake_minutes == WAKE_CHARGING_MINUTES, "an answered charge latches");
  uint32_t fast = 0, uncorroborated = 0;
  for (int i = 0; i < 30; ++i) {
    car.now += car.g_next_wake_minutes * 60000UL;
    car.poll({});
    check(car.recently_charging && car.charge_release_polls == 0, "high-rail silence never counts toward release");
    if (i < CHARGE_SILENT_HOLD_POLLS) {
      check(car.g_next_wake_minutes == WAKE_CHARGING_MINUTES, "the first silent polls keep the charging cadence");
      fast = car.charging_fast_minutes;
      uncorroborated = car.charge_uncorroborated_s;
    } else {
      check(car.g_next_wake_minutes == WAKE_PARKED_MINUTES, "past the hold, high-rail silence waits at parked cadence");
      check(!car.charging_cap_latched, "a held poll sets no cap");
      if (car.now < CHARGE_STALE_RESET_MINUTES * 60000UL)
        check(car.charging_fast_minutes == fast && car.charge_uncorroborated_s == uncorroborated,
              "a held poll neither accrues nor refills the charge's budget");
      else
        check(car.charging_fast_minutes == 0 && car.charge_uncorroborated_s == 0, "a long-stale hold drops the old budget");
    }
  }

  ChargePoll counted;
  counted.g_sample_poke = true;
  counted.g_rail_before_poke = 14.9f;
  counted.recently_charging = true;
  counted.poll({{"current", 0.4}});
  check(counted.charge_release_polls == 1, "an answered poll with no evidence starts the release");
  for (int i = 0; i < 2 * CHARGE_SILENT_HOLD_POLLS; ++i) counted.poll({});
  check(counted.recently_charging && counted.charge_release_polls == 1, "held polls neither advance nor reset the release");
  counted.poll({{"current", 0.4}});
  check(!counted.recently_charging, "the next answered poll with no evidence completes the release");

  // A car whose rail never drops (a 12 V maintainer) must not hand the old charge's budget to the next one.
  ChargePoll stale;
  stale.g_sample_poke = true;
  stale.g_rail_before_poke = 14.9f;
  stale.recently_charging = stale.charging_cap_latched = true;
  stale.charging_fast_minutes = WAKE_CHARGING_MAX_MINUTES;
  stale.charge_corr_soc_hw = stale.prev_charge_soc = 90;
  for (int i = 0; i < 50; ++i) {
    stale.now += WAKE_PARKED_MINUTES * 60000UL;
    stale.poll({});
  }
  stale.g_taper_confirm = true;
  for (double soc = 30; soc < 33; soc += 0.5) {
    stale.now += stale.g_next_wake_minutes * 60000UL;
    stale.poll({{"current", -12.0}, {"soc", soc}});
    check(stale.g_next_wake_minutes == WAKE_CHARGING_MINUTES, "a new lower-SoC charge after a long hold gets the charging cadence");
    check(stale.taper_confirm_wakes == 0, "and its own SoC baseline for the taper stall budget");
  }

  ChargePoll motion;
  motion.g_rail_before_poke = 14.9f;
  motion.recently_charging = true;
  for (int i = 0; i < CHARGE_SILENT_HOLD_POLLS + CHARGE_RELEASE_POLLS; ++i) motion.poll({});
  check(!motion.recently_charging, "a silent streaming wake still releases on the existing bound");
}

// A rail-high car with a stuck charging current and a frozen SoC that answers one poll in six or seven. The
// hold between answers must not refill its budget: it still reaches the uncorroborated or capped cadence.
void stuckCurrentChecks() {
  for (const int period : {6, 7}) {
    ChargePoll car;
    car.g_sample_poke = true;
    car.g_rail_before_poke = 14.9f;
    car.g_next_wake_minutes = WAKE_CHARGING_MINUTES;
    int poll = 0, fast = 0;
    bool relaxed = false;
    while (car.now < 7UL * 24 * 60 * 60 * 1000) {
      car.now += car.g_next_wake_minutes * 60000UL;
      car.poll(poll++ % period ? OBDVector{} : OBDVector{{"current", -12.0}, {"soc", 55}});
      if (car.g_next_wake_minutes < WAKE_PARKED_MINUTES) fast++;
      relaxed |= car.reason("parked-uncorroborated") || car.reason("parked-cap");
    }
    check(relaxed, "a stuck current held through rail-high silence still relaxes");
    check(fast * WAKE_CHARGING_MINUTES <= CHARGE_CORROBORATE_MINUTES + period * WAKE_CHARGING_MINUTES,
          "a week of a stuck current costs about one corroboration budget of fast polls");
  }

  // The silent-corroboration scenario at a high rail: the 90-minute fallback must hold through the holds.
  ChargePoll wake;
  wake.g_sample_poke = true;
  wake.g_rail_before_poke = 14.9f;
  int poll = 0;
  unsigned long relaxed_at = 0;
  while (wake.now < 4UL * 60 * 60 * 1000 && !relaxed_at) {
    wake.now += WAKE_CHARGING_MINUTES * 60000UL;
    wake.poll(poll++ % 6 ? OBDVector{} : OBDVector{{"current", -12.0}, {"soe", 40000}});
    if (wake.reason("parked-uncorroborated")) relaxed_at = wake.now;
  }
  check(relaxed_at >= CHARGE_CORROBORATE_MINUTES * 60000UL && relaxed_at <= (CHARGE_CORROBORATE_MINUTES + 30) * 60000UL,
        "an uncorroborated charge relaxes on wall-clock time at a high rail too");

  // A pause inside one session (released at a rail-high poke) keeps the spent budget for the resume.
  ChargePoll paused;
  paused.g_sample_poke = true;
  paused.g_rail_before_poke = 14.9f;
  paused.recently_charging = paused.charging_cap_latched = true;
  paused.charging_fast_minutes = WAKE_CHARGING_MAX_MINUTES;
  paused.poll({{"current", -12.0}, {"soc", 55}});
  paused.now += WAKE_PARKED_MINUTES * 60000UL;
  paused.poll({{"current", 0.4}, {"soc", 55}});
  paused.now += WAKE_PARKED_MINUTES * 60000UL;
  paused.poll({{"current", 0.4}, {"soc", 55}});
  check(!paused.recently_charging, "two answered polls with no evidence release");
  paused.now += WAKE_PARKED_MINUTES * 60000UL;
  paused.poll({{"current", -12.0}, {"soc", 55}});
  check(paused.reason("parked-cap"), "a resume after a rail-high release keeps the cap");

  ChargePoll later;
  later.g_sample_poke = true;
  later.g_rail_before_poke = 14.9f;
  later.charging_cap_latched = true;
  later.charging_fast_minutes = WAKE_CHARGING_MAX_MINUTES;
  later.now = (CHARGE_STALE_RESET_MINUTES + 30) * 60000UL;
  later.poll({{"current", 0.4}, {"soc", 55}});
  check(!later.charging_cap_latched && later.charging_fast_minutes == 0,
        "a rail-high poke long after the last evidence drops the old budget");
}
// The real SLEEP enter line and the two escalation sleeps that name their own reason.
#define ENABLE_LOGGING 1
namespace sleep_reason {
#define WOM_THRESHOLD_G 0.1582f
std::string last_log;
void logEvent(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  last_log = line;
}
int g_next_wake_minutes = WAKE_PARKED_MINUTES, g_recovers_since_ok = 0;
unsigned long net_reboot_count = 0, wdt_reset_count = 0;
float g_last_io36 = 12.6f;
const char* g_cadence_reason = nullptr;
bool window_opened = false;
bool pdwOpenWindow() { return window_opened = true; }
// A window opened at the backstop names its 3-minute cadence; the explicit reason must still win.
void pdwArmAtLadderEnd() {
  g_next_wake_minutes = WAKE_CHARGING_MINUTES;
  g_cadence_reason = "pdw";
}
std::vector<OBDVector> obd_result_vector;
std::string slept;
void logSleep(bool modem_on, const char* reason) {
  bool modem_down = modem_on;
#include "sleep_log.inc"
}
void enterDeepSleep(bool modem_on, const char* reason = nullptr) {
  logSleep(modem_on, reason);
  slept = last_log;
}
void recoverSleep() {
#include "net_recover_sleep.inc"
}
bool wdtStreakModemMayBeOn() { return true; }
void wdtSleep() {
#include "wdt_streak_sleep.inc"
}
bool maintenance_mode = false, g_ota_active = false, g_logdump_active = false, is_currently_parked = true;
enum { UNINITIALIZED, STANDBY } current_state = STANDBY;
unsigned long millis() { return (MAX_AWAKE_MINUTES + 1) * 60UL * 1000UL; }
struct { void printf(const char*, ...) {} } Serial;
void backstopSleep() {
#include "backstop_sleep.inc"
}
bool has(const std::string& text) { return slept.find(text) != std::string::npos; }

void checks() {
  g_recovers_since_ok = 2;
  recoverSleep();
  check(has("timer=1min reason=net-recover "), "the modem-recovery reboot logs its own reason, not dc-charge");
  check(window_opened, "the modem-recovery reboot opens the post-drive window");
  wdt_reset_count = 2;
  wdtSleep();
  check(has("reason=wdt-streak "), "the WDT-streak escalation logs its own reason");
  slept.clear();
  backstopSleep();
  check(has("reason=backstop "), "the parked MAX_AWAKE backstop logs its own reason");
  check(g_next_wake_minutes == WAKE_CHARGING_MINUTES, "the backstop still ends the ladder with the post-drive window");
  g_cadence_reason = "parked-held";
  g_next_wake_minutes = WAKE_PARKED_MINUTES;
  enterDeepSleep(true);
  check(has("reason=parked-held "), "a named cadence decision beats the reason derived from the timer");
  g_cadence_reason = nullptr;
  g_next_wake_minutes = WAKE_DC_CHARGING_MINUTES;
  enterDeepSleep(true);
  check(has("reason=dc-charge "), "a cadence sleep still names its tier from the armed timer");
  g_next_wake_minutes = WAKE_CHARGING_MINUTES;
  enterDeepSleep(true);
  check(has("reason=charging "), "an AC-charge cadence sleep still logs charging");
}
}  // namespace sleep_reason

// The crash-loop guard: the real store and advance (Helpers/wdt_streak.h), the setup() accounting and the
// escalation. One global stands in for RTC_NOINIT memory, so it keeps whatever a boot left in it; the runner
// checks the firmware declares it RTC_NOINIT_ATTR, which is what the stand-in assumes.
namespace wdt_streak {
enum esp_reset_reason_t {  // IDF 4.4 values
  ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
};
#define RTC_NOINIT_ATTR
#include "wdt_streak.h"
#undef RTC_NOINIT_ATTR
esp_reset_reason_t reset_reason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason() { return reset_reason; }
uint32_t wdt_reset_count = 0;
bool g_wdt_streak_retained = false, g_weak_rail_cold_boot = false, g_ota_active = false;
float g_last_io36 = 12.6f;
int g_next_wake_minutes = 0;
std::string last_log, sleep_reason;
bool slept_modem_on = false;
void logEvent(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  last_log = line;
}
// CellWrapper and the SIM7070 as far as power goes: init() powers it on through the power-on hook; CPOWD
// powers off an answering modem (ack, power-off hook), and otherwise the PWRKEY fallback toggles it (the
// hook calls are checked in cellwrapper.cpp by the runner). A hung modem is on but answers nothing.
struct {
  bool on = false, hung = false, dead = false;
  int inits = 0, power_offs = 0;
  bool connected() { return false; }
  void reconnect() { init(); }  // an unanswering modem gets a full init()
  void init() { inits++; cellModemPowerOnHook(); on = !dead; }
  std::string sendRawAt(const char*, unsigned) { return on && !hung ? "AT\r\r\nOK\r\n" : ""; }
  void powerOff() {
    power_offs++;
    if (on && !hung) { cellModemPowerOffHook(); on = false; }
    else if (!dead) { on = !on; hung = false; }  // 2.5 s PWRKEY
  }
} network;
struct { void printf(const char*, ...) {} } Serial;
bool bleRelaying() { return false; }
unsigned long millis() { return 600000UL; }
const unsigned long WIFI_RETRY_MS = 20000;
unsigned long last_reconnect_try = 0;
void offlineRetry() {
#include "reconnect.inc"
}
// The modem part of enterDeepSleep, then the sleep itself.
void enterDeepSleep(bool modem_on, const char* reason = nullptr) {
#define ENABLE_CELL 1
#include "sleep_probe.inc"
#undef ENABLE_CELL
#include "sleep_modem.inc"
  sleep_reason = reason ? reason : "?";
  slept_modem_on = modem_on;
}
void wake() {
#include "wdt_streak_wake.inc"
}
void escalation() {
#include "wdt_streak_sleep.inc"
}
// One boot: the reset the bootloader reports, then setup()'s accounting and escalation.
bool boot(esp_reset_reason_t reason) {
  reset_reason = reason;
  sleep_reason.clear();
  wake();
  escalation();
  return sleep_reason == "wdt-streak";
}

void checks() {
  const esp_reset_reason_t crashes[] = {ESP_RST_PANIC, ESP_RST_TASK_WDT, ESP_RST_INT_WDT, ESP_RST_WDT};
  const esp_reset_reason_t others[] = {ESP_RST_POWERON, ESP_RST_BROWNOUT, ESP_RST_DEEPSLEEP, ESP_RST_SW,
                                       ESP_RST_EXT, ESP_RST_UNKNOWN, ESP_RST_SDIO};
  const std::string parked_log =
    "WDT streak=2 -> skip init, sleep " + std::to_string(WAKE_PARKED_MINUTES) + "min (retry fresh next wake)";
  std::memset(&wdt_streak_store, 0xA5, sizeof(wdt_streak_store));
  check(!boot(ESP_RST_POWERON) && wdt_reset_count == 0, "a power-on over garbage starts at 0");
  for (auto crash : crashes) {
    check(!boot(ESP_RST_DEEPSLEEP) && wdt_reset_count == 0, "a deep-sleep wake ends the streak");
    check(!boot(crash) && wdt_reset_count == 1 && g_wdt_streak_retained,
          "the first crash reset counts 1 from a retained store");
    check(boot(crash) && wdt_reset_count == 2, "the second consecutive crash reset escalates");
    check(g_next_wake_minutes == WAKE_PARKED_MINUTES, "a parked rail sleeps the parked cadence");
    check(last_log == parked_log, "the escalation logs it");
  }
  for (auto other : others) {
    boot(ESP_RST_PANIC);
    check(boot(ESP_RST_PANIC) && wdt_reset_count >= 2, "consecutive crash boots extend the streak");
    check(!boot(other) && wdt_reset_count == 0, "any reset but a crash ends the streak");
    check(!boot(ESP_RST_PANIC) && wdt_reset_count == 1, "a crash after it starts again at 1");
  }
  check(boot(ESP_RST_PANIC) && wdt_reset_count == 2 && boot(ESP_RST_PANIC) && wdt_reset_count == 3,
        "a crash loop the sleep did not break keeps counting and keeps escalating");
  // An awake rail (driving, charging) comes back on the charging cadence instead of a parked half hour.
  boot(ESP_RST_DEEPSLEEP);
  boot(ESP_RST_PANIC);
  g_last_io36 = RAIL_AWAKE_VOLTS;
  check(boot(ESP_RST_PANIC) && g_next_wake_minutes == WAKE_CHARGING_MINUTES, "an awake rail sleeps the charging cadence");
  g_last_io36 = 12.6f;
  // Bytes that are not this store - garbage, another layout, a torn write - never extend a streak.
  const WdtStreakStore foreign[] = {{0xA5A5A5A5u, 1, 0, ~1u}, {WDT_STREAK_MAGIC ^ 1u, 1, 0, ~1u},
    {WDT_STREAK_MAGIC, 1, 0, 1u}, {WDT_STREAK_MAGIC, 1, 0, ~2u}, {WDT_STREAK_MAGIC, 1, 1, ~1u},
    {WDT_STREAK_MAGIC, 1, 2, ~1u}};
  for (const auto& bytes : foreign) {
    boot(ESP_RST_DEEPSLEEP);
    boot(ESP_RST_PANIC);
    wdt_streak_store = bytes;
    check(!boot(ESP_RST_PANIC) && wdt_reset_count == 1 && !g_wdt_streak_retained,
          "a crash over an unrecognised store counts 1 and says nothing was retained");
  }
  // The OTA commit ends the streak in this image's store: a rollback boots this image on the trial image's
  // crash reset, and must not add that crash to one from before the update.
  boot(ESP_RST_DEEPSLEEP);
  boot(ESP_RST_PANIC);
  wdtStreakEnd();
  check(!boot(ESP_RST_PANIC) && wdt_reset_count == 1, "a rollback after a crash-recovery update does not escalate");

  // The modem flag. The escalation powers the modem down only if it may be on: the 2.5 s PWRKEY fallback
  // for an unanswered AT+CPOWD would switch an off modem ON for the whole sleep.
  std::memset(&wdt_streak_store, 0xA5, sizeof(wdt_streak_store));
  boot(ESP_RST_POWERON);
  check(!wdtStreakModemMayBeOn(), "a power loss cut the modem's power as well");
  boot(ESP_RST_PANIC);
  check(boot(ESP_RST_PANIC) && !slept_modem_on, "crashes that never reached the modem leave it off");
  boot(ESP_RST_DEEPSLEEP);
  network.init();
  boot(ESP_RST_PANIC);
  check(boot(ESP_RST_PANIC) && slept_modem_on && !wdtStreakModemMayBeOn(),
        "a crash after network init powers the modem down, and the ack clears the flag");
  boot(ESP_RST_DEEPSLEEP);
  boot(ESP_RST_PANIC);
  network.init();
  check(boot(ESP_RST_PANIC) && slept_modem_on, "network init in the crash-recovery wake counts too");
  network.init();
  boot(ESP_RST_POWERON);
  check(wdtStreakModemMayBeOn(), "an EN-pin reset (POWERON over an intact store) keeps the flag");
  boot(ESP_RST_BROWNOUT);
  check(wdtStreakModemMayBeOn(), "a brownout leaves the modem as it was");
  // A lost store after any other reset: the wake before may have powered the modem.
  boot(ESP_RST_DEEPSLEEP);
  enterDeepSleep(false);
  boot(ESP_RST_PANIC);
  std::memset(&wdt_streak_store, 0xA5, sizeof(wdt_streak_store));
  check(!boot(ESP_RST_PANIC) && wdtStreakModemMayBeOn(), "a lost store assumes the modem may be on");
  check(boot(ESP_RST_PANIC) && slept_modem_on, "and the escalation after it powers the modem down");
  network.on = false;
  // Every sleep acts on the flag, not only the escalation: a skip sleep after a crash powers down a modem
  // the crashed wake left on.
  boot(ESP_RST_DEEPSLEEP);
  network.init();
  check(!boot(ESP_RST_PANIC), "one crash after network init does not escalate");
  enterDeepSleep(false);
  check(!network.on && !wdtStreakModemMayBeOn(), "a skip sleep powers down a modem a crash left on");
  // A wedge in modem init: the escalation's CPOWD goes unanswered and its PWRKEY switches the hung modem
  // off, leaving the flag "on". The next skip sleep must not PWRKEY the now-off modem back on.
  boot(ESP_RST_DEEPSLEEP);
  network.init();
  network.hung = true;
  boot(ESP_RST_TASK_WDT);
  check(boot(ESP_RST_TASK_WDT) && !network.on && wdtStreakModemMayBeOn(),
        "the escalation's PWRKEY switches a hung modem off and leaves the flag unknown");
  boot(ESP_RST_DEEPSLEEP);
  enterDeepSleep(false);
  check(!network.on && !wdtStreakModemMayBeOn(), "a silent modem is off: no PWRKEY, flag cleared");
  enterDeepSleep(false);
  check(!network.on, "and the sleeps after it leave it off");
  // A dead or absent modem never acknowledges: one probe clears the flag, then skip sleeps cost nothing.
  network.dead = true;
  network.init();
  enterDeepSleep(true);
  check(wdtStreakModemMayBeOn(), "an unacknowledged power-down leaves the flag set");
  enterDeepSleep(false);
  network.power_offs = 0;
  enterDeepSleep(false);
  check(!wdtStreakModemMayBeOn() && network.power_offs == 0, "a dead modem stops costing a power-down per sleep");
  network.dead = false;
  // A wake that powered the modem powers it down as before, flag or not.
  network.init();
  wdtStreakModem(false);
  enterDeepSleep(true);
  check(!network.on && network.power_offs == 1, "modem_on=true still sends the power-down without a probe");
  // A weak-rail cold boot is BLE-only: the offline retry must not reconnect, which on an unanswering modem
  // runs a full init() and powers it on.
  boot(ESP_RST_POWERON);
  network.inits = 0;
  g_weak_rail_cold_boot = true;
  offlineRetry();
  check(network.inits == 0 && !wdtStreakModemMayBeOn(), "a weak-rail cold boot never powers the modem");
  g_weak_rail_cold_boot = false;
  offlineRetry();
  check(network.inits == 1 && wdtStreakModemMayBeOn(), "any other offline wake retries, and flags the modem");
}
}  // namespace wdt_streak
#undef WOM_THRESHOLD_G
#undef ENABLE_LOGGING

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

// Today's wire text for one number, the 2 dp contract the compact frame must reproduce exactly.
json referenceTlm(const OBDVector& frame) {
  json tlm = json::object();
  for (const auto& field : frame) {
    if (field.first == "utc" || field.first == "lat" || field.first == "lon" || field.first == "heading") continue;
    if (field.first == "is_parked" || field.first == "is_charging" || field.first == "is_dcfc") {
      tlm[field.first] = field.second != 0.0;
    } else {
      char text[24];
      snprintf(text, sizeof(text), "%.2f", field.second);
      tlm[field.first] = json::parse(text);
    }
  }
  return tlm;
}

void bleFrameChecks() {
  // Synthetic driving frame the size of the field overflow (201-222 B under %.2f).
  const OBDVector driving{{"utc", 1791192686}, {"soc", 63.5}, {"soh", 100}, {"voltage", 712.3},
    {"current", -152.7}, {"power", 108.8}, {"speed", 112}, {"batt_temp", 31.5}, {"batt_temp_min", 29},
    {"batt_temp_max", 33}, {"kwh_charged", 1234.5}, {"is_parked", 0}, {"is_charging", 0}, {"is_dcfc", 0},
    {"lat", 1.5}, {"lon", 2.5}};
  const json reference = referenceTlm(driving);
  const size_t reference_bytes = std::string("{\"tlm\":}").size() + reference.dump().size();
  const auto android = bleTlmFrame(driving, bleNotifyBudget(247));
  check(android.dropped == 0 && android.notify == android.full &&
        android.full.size() <= 244 && android.full.size() < reference_bytes,
        "compact driving frame fits MTU 247 untrimmed and is shorter than %.2f");
  check(json::parse(android.notify)["tlm"] == reference, "compact frame keeps every key and its 2 dp value");
  const auto ios = bleTlmFrame(driving, bleNotifyBudget(185));
  const auto ios_kept = json::parse(ios.notify)["tlm"];
  check(ios.notify.size() <= 182 && json::parse(ios.full)["tlm"] == reference, "MTU 185 notify fits, full frame intact");
  for (const auto& field : reference.items()) {
    const std::string key = field.key();
    const bool low = key == "batt_temp_min" || key == "batt_temp_max" || key == "kwh_charged";
    check(low || (ios_kept.contains(key) && ios_kept[key] == field.value()),
          "MTU 185 drops only the lowest-priority fields, kept " + key);
  }
  // The smaller field frame (~200 B under %.2f) needs only the compact numbers to fit iOS.
  const OBDVector smaller{{"soc", 64}, {"soh", 100}, {"voltage", 712.3}, {"current", -152.7}, {"power", 108.8},
    {"speed", 112}, {"batt_temp_min", 29}, {"batt_temp_max", 33}, {"is_parked", 0}, {"is_charging", 0},
    {"is_dcfc", 0}};
  const json smaller_reference = referenceTlm(smaller);
  const auto smaller_ios = bleTlmFrame(smaller, bleNotifyBudget(185));
  check(smaller_ios.dropped == 0 && smaller_ios.notify.size() <= 182 && json::parse(smaller_ios.notify)["tlm"] == smaller_reference,
        "the smaller field frame fits MTU 185 untrimmed once compact");
  check(bleTlmString(driving) == bleTlmFrame(driving, 244).full, "bleTlmString is the full compact frame");

  auto text = [](double value) {
    const auto frame = bleTlmFrame({{"soc", value}}, 244);
    return frame.full.substr(14, frame.full.size() - 16);
  };
  check(text(100) == "100" && text(0) == "0" && text(0.5) == "0.5" && text(49.11) == "49.11" &&
        text(-51.2) == "-51.2" && text(42111) == "42111" && text(10.10) == "10.1",
        "trailing zeros and a bare decimal point are dropped");
  check(text(1.23456) == "1.23" && text(0.001) == "0" && text(-0.004) == "0" && text(0.005001) == "0.01",
        "never more precision than %.2f, and negative zero is plain zero");
  const auto non_finite = bleTlmFrame({{"soc", NAN}, {"power", INFINITY}, {"current", -INFINITY}, {"speed", 3}}, 244);
  check(json::parse(non_finite.full)["tlm"] == json{{"speed", 3}}, "non-finite values are omitted, frame stays valid JSON");

  // Small budget: priority decides what survives, never a cut mid-token.
  const auto small = bleTlmFrame(driving, 140);
  const auto kept = json::parse(small.notify)["tlm"];
  check(small.notify.size() <= 140 && small.dropped > 0 &&
        small.dropped == (int)(reference.size() - kept.size()), "small MTU trims and counts dropped fields");
  for (const auto& key : {"soc", "power", "current", "voltage", "speed", "is_charging", "is_parked", "is_dcfc"})
    check(kept.contains(key) && kept[key] == reference[key], std::string("small MTU keeps priority field ") + key);
  for (const auto& key : {"batt_temp_min", "batt_temp_max", "kwh_charged"})
    check(!kept.contains(key), std::string("small MTU drops low-priority field ") + key);
  check(json::parse(small.full)["tlm"] == reference, "readable value keeps the full frame when the notify is trimmed");

  // MTU 23 carries 20 B: whatever fits is still a valid frame (it triggers the app's read).
  const auto tiny = bleTlmFrame(driving, bleNotifyBudget(23));
  check(bleNotifyBudget(23) == 20 && tiny.dropped > 0 && tiny.notify.size() <= 20 &&
        json::parse(tiny.notify)["tlm"].is_object(), "MTU 23 notifies a valid frame within 20 bytes");
  const auto first = bleTlmFrame({{"power", 5}, {"is_parked", 1}, {"soc", 63.5}}, bleNotifyBudget(23));
  check(json::parse(first.notify)["tlm"] == json{{"soc", 63.5}}, "MTU 23 spends its 20 bytes on soc first");
  const auto greedy = bleTlmFrame({{"soc", 63.5}, {"voltage", 712.34}, {"speed", 0}}, 30);
  check(json::parse(greedy.notify)["tlm"] == json({{"soc", 63.5}, {"speed", 0}}),
        "a field too long to fit does not block a shorter lower-priority one");
  const OBDVector detail{{"soc", 63.5}, {"batt_temp_min", 29}, {"_bms_soc", 64.1}, {"kwh_charged", 1234.5},
    {"batt_temp_max", 33}};
  const auto profile_first = bleTlmFrame(detail, 39);
  check(json::parse(profile_first.notify)["tlm"] == json({{"soc", 63.5}, {"_bms_soc", 64.1}}),
        "profile-private keys outrank the temperature bounds and kWh counter");
  const auto listed_first = bleTlmFrame({{"_bms_soc", 64.1}, {"soc", 63.5}, {"power", 108.8}}, 36);
  check(json::parse(listed_first.notify)["tlm"] == json({{"soc", 63.5}, {"power", 108.8}}),
        "listed fields outrank profile-private keys");
  const auto exact = bleTlmFrame(detail, bleTlmString(detail).size());
  const auto over = bleTlmFrame(detail, bleTlmString(detail).size() - 1);
  check(exact.dropped == 0 && over.dropped > 0 && over.notify.size() < bleTlmString(detail).size(),
        "a frame exactly the budget fits, one byte over is trimmed");
  check(bleNotifyBudget(185) == 182 && bleNotifyBudget(247) == 244 && bleNotifyBudget(517) == 244 &&
        bleNotifyBudget(0) == 20, "budget is MTU-3 inside the 23..247 range the stack can carry");

  // Every listed key plus a private one. Each adjacent pair, in reverse frame order, at a budget that holds
  // either field alone but not both: the higher one must win. That pins the whole order, which a prefix
  // budget cannot do once a longer field can be skipped for a shorter one.
  const std::vector<std::string> order{"soc", "power", "current", "voltage", "speed", "is_charging", "is_parked",
    "is_dcfc", "soh", "est_battery_range", "batt_temp", "ext_temp", "odometer", "soe", "capacity", "hvac_power",
    "cabin_temp", "elevation", "_private", "kwh_charged", "batt_temp_min", "batt_temp_max"};
  for (size_t i = 0; i + 1 < order.size(); ++i) {
    const OBDVector pair{{order[i + 1], 1}, {order[i], 1}};
    const size_t alone = std::max(bleTlmFrame({pair[0]}, 244).full.size(), bleTlmFrame({pair[1]}, 244).full.size());
    const auto kept = json::parse(bleTlmFrame(pair, alone).notify)["tlm"];
    check(kept.size() == 1 && kept.contains(order[i]), order[i] + " outranks " + order[i + 1]);
  }

  // A key that is not a plain identifier could break out of its string, so it never reaches the frame.
  const auto hostile = bleTlmFrame({{"x\":1,\"soc", 5}, {"q\"", 6}, {"a\\b", 1}, {"", 2}, {"s p", 3}, {"ok_9", 4}}, 244);
  check(json::parse(hostile.full)["tlm"] == json{{"ok_9", 4}}, "keys outside [A-Za-z0-9_] are skipped");
}

// The frame log line: first frame of a wake, then only a higher drop count or a new MTU.
#undef ENABLE_LOGGING
#define ENABLE_LOGGING 1
namespace ble_frame_log {
std::vector<std::string> lines;
void logEvent(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  lines.push_back(line);
}
struct { void printf(const char*, ...) {} } Serial;
int g_logged_ble_drop = -1;
uint16_t g_logged_ble_mtu = 0;
void poll(uint16_t ble_mtu, const OBDVector& frame) {
  const BleTlmFrame ble_frame = bleTlmFrame(frame, bleNotifyBudget(ble_mtu));
#include "ble_frame_log.inc"
}

void checks() {
  const OBDVector parked{{"soc", 80}, {"is_parked", 1}};
  const OBDVector driving{{"soc", 63.5}, {"power", 108.8}, {"voltage", 712.3}, {"current", -152.7},
    {"speed", 112}, {"batt_temp_min", 29}, {"batt_temp_max", 33}, {"kwh_charged", 1234.5}};
  OBDVector wide = driving;
  for (const auto& extra : {"soh", "est_battery_range", "batt_temp", "ext_temp", "odometer"}) wide.emplace_back(extra, 1234.5);
  poll(23, parked);
  poll(23, parked);
  poll(23, driving);
  poll(23, parked);
  poll(185, parked);
  poll(185, parked);
  poll(185, wide);
  poll(185, driving);
  poll(185, driving);
  poll(185, parked);
  poll(100, driving);
  poll(100, driving);
  poll(23, driving);
  poll(23, driving);
  const std::vector<std::string> expected{
    "BLE tlm mtu=23 bytes=35 sent=18 trimmed drop=1",
    "BLE tlm mtu=23 bytes=138 sent=20 trimmed drop=7",
    "BLE tlm mtu=185 bytes=35 sent=35 fit drop=0",
    "BLE tlm mtu=185 bytes=233 sent=174 trimmed drop=3",
    "BLE tlm mtu=100 bytes=138 sent=79 trimmed drop=3",
    "BLE tlm mtu=23 bytes=138 sent=20 trimmed drop=7",
  };
  check(lines == expected, "frame log: first frame, then only a higher drop count or a new MTU");
}
}  // namespace ble_frame_log
#undef ENABLE_LOGGING
#define ENABLE_LOGGING 0

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
  bool g_sample_poke = false;
  float g_rail_before_poke = 0;
  uint8_t charge_followup = 0;
  int g_alarm_flag = 0;
  bool alarmEnforced() const { return false; }
  void resetChargeCorroboration() { corroboration_resets++; }
  // Long after any charge evidence.
  uint64_t charge_last_evidence_us = 0;
  uint64_t esp_rtc_get_time_us() const { return 1ULL << 40; }
#include "charge_stale.inc"
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

  ChargePoll held;
  held.g_sample_poke = true;
  held.g_rail_before_poke = 14.9f;
  held.poll({{"current", -12.0}, {"soc", 60}});
  for (int i = 0; i < 10; ++i) held.poll({});
  WakeGate fall;
  fall.recently_charging = held.recently_charging;
  check(fall.poll(12.7f) && fall.charge_end, "the rail drop after a long high-rail silence gets the charge-end read");
}

// A dongle across deep sleeps: the setup() gate, the loop poll and the awake-ladder end, with RTC state as
// members. Time is one clock: millis() and the RTC both read now, and a sleep adds its timer.
struct Dongle : ChargePoll {
  bool charge_end_polled = false, alarm = false, charge_end = false, polled = false;
  int g_alarm_flag = 0, next = WAKE_PARKED_MINUTES;
  float ladder_rail = 0;
  unsigned long last_active = 0;
  const char* g_wake_reason = "TIMER";
  bool g_pdw_ladder_done = false;
  int fast_pokes = 0, low_rail_polls = 0;
  bool alarmEnforced() const { return alarm; }
  float readIO36Rail() const { return ladder_rail; }
#define ENABLE_SLEEP 1  // the production sleep build, where the ladder reads the rail itself
#define ENABLE_MEMS 1
#include "pdw_ladder.inc"
#undef ENABLE_SLEEP
#undef ENABLE_MEMS
  bool gate(float rail, bool is_cold, bool is_motion, bool awake = true, bool heartbeat_due = false) {
#include "wake_gate.inc"
    charge_end = charge_end_poll;
    return should_poll;
  }
  // Fresh RAM, as every wake is a reboot.
  void boot() {
    g_sample_poke = g_taper_confirm = g_pdw_ladder_done = false;
    g_rail_before_poke = 0;
    g_next_wake_minutes = WAKE_PARKED_MINUTES;
    g_cadence_reason = nullptr;
  }
  // A timer wake: the gate, then the skip path in setup() or one sample-poke poll.
  int wake(float rail, const OBDVector& frame = {}) {
    now += (unsigned long)next * 60000UL;
    boot();
    chargeWakeStateSanitize(true);
    polled = gate(rail, false, false);
    if (!polled) {
#include "gate_skip.inc"
    } else {
      poll(frame);
      if (rail < RAIL_AWAKE_VOLTS) low_rail_polls++;
      else if (g_next_wake_minutes < WAKE_PARKED_MINUTES && !recently_charging) fast_pokes++;
    }
    return next = g_next_wake_minutes;
  }
  // A motion wake or power-on: streams polls while awake, then the ladder (about five minutes) ends in sleep.
  int awake(float rail, bool cold, const OBDVector& frame = {}, float rail_at_end = -1) {
    now += (unsigned long)next * 60000UL;
    boot();
    chargeWakeStateSanitize(!cold);
    g_wake_reason = cold ? "POWERON" : "MOTION";
    polled = gate(rail, cold, !cold);
    for (int i = 0; i < 12; ++i) {
      now += 5000;
      poll(frame);
    }
    last_active = now;
    now += 5UL * 60000UL;
    ladder_rail = rail_at_end < 0 ? rail : rail_at_end;
    pdwArmAtLadderEnd();
    return next = g_next_wake_minutes;
  }
};

const OBDVector SILENT = {};
const OBDVector TOP_UP = {{"current", 0.3}, {"soc", 60}};

// One follow-up after a charge, never a low-rail read without evidence, and never a second follow-up.
void followupChecks() {
  Dongle car;
  car.wake(14.9f, {{"current", -12.0}, {"soc", 60}});
  check(car.recently_charging && car.next == WAKE_CHARGING_MINUTES, "an answered charge latches");
  car.wake(12.7f, TOP_UP);
  check(car.charge_end && !car.recently_charging, "the rail drop gets its charge-end read");
  check(car.charge_followup == 1, "a charge that ended arms one follow-up");
  check(car.wake(12.7f) == WAKE_PARKED_MINUTES && !car.polled, "a low rail after the charge is not read");
  check(car.wake(14.9f) == WAKE_CHARGING_MINUTES, "the first silent high-rail poke after a charge follows up in 3 min");
  check(car.charge_followup == 0, "the follow-up is spent");
  check(car.wake(14.9f) == WAKE_PARKED_MINUTES, "a silent follow-up goes back to the parked cadence");
  check(car.wake(14.9f) == WAKE_PARKED_MINUTES, "and there is no second follow-up");

  Dongle quiet;
  quiet.wake(14.9f, {{"current", -12.0}, {"soc", 60}});
  quiet.wake(12.7f);
  check(quiet.charge_end && quiet.next == WAKE_PARKED_MINUTES && quiet.charge_followup == 1,
        "a silent charge-end read at a low rail does not spend the follow-up");

  Dongle answers;
  answers.wake(14.9f, {{"current", -12.0}, {"soc", 60}});
  answers.wake(14.9f, TOP_UP);
  answers.wake(14.9f, TOP_UP);
  check(!answers.recently_charging && answers.charge_followup == 1, "a release in the loop arms the follow-up too");
  check(answers.wake(14.9f) == WAKE_CHARGING_MINUTES, "its silent high-rail poke follows up");
  answers.wake(14.9f, {{"current", -11.0}, {"soc", 64}});
  check(answers.recently_charging && answers.next == WAKE_CHARGING_MINUTES, "a follow-up that answers with evidence latches");

  // A week of 12 V top-ups after one charge: the rail rises about four times a day, silent or answering 0 A.
  Dongle week;
  week.wake(14.9f, {{"current", -12.0}, {"soc", 60}});
  week.wake(12.7f, TOP_UP);
  week.fast_pokes = week.low_rail_polls = 0;
  for (int i = 0; i < 7 * 48; ++i) {
    const bool high = i % 12 == 5;
    week.wake(high ? 14.4f : (i % 3 ? 12.7f : 13.1f), high && i % 24 == 5 ? SILENT : TOP_UP);
  }
  check(week.fast_pokes == 1, "a week of top-ups after a charge costs exactly one follow-up");
  check(week.low_rail_polls == 0, "and no low-rail read without charge evidence");

  Dongle drive;
  drive.charge_followup = 1;
  drive.awake(13.5f, false);
  check(drive.charge_followup == 0, "motion clears an armed follow-up");
  drive.charge_followup = 1;
  drive.awake(13.5f, true);
  check(drive.charge_followup == 0, "a power-on clears it");

  ChargePoll rtc;
  rtc.charge_followup = 1;
  rtc.pdw_open = 1;
  rtc.pdw_open_us = 1000;
  rtc.pdw_polls = 1;
  rtc.charge_last_evidence_us = 1000;
  rtc.chargeWakeStateSanitize(true);
  check(rtc.charge_followup == 1 && rtc.pdw_open == 1 && rtc.pdw_polls == 1, "a deep-sleep wake keeps the state");
  rtc.chargeWakeStateSanitize(false);
  check(!rtc.charge_followup && !rtc.pdw_open && !rtc.pdw_open_us && !rtc.pdw_polls && !rtc.charge_last_evidence_us,
        "any other boot zeroes the follow-up and the window");
}

// The post-drive window: keyed on the awake ladder and a fresh rail read, never on OBD.
void postDriveChecks() {
  // No speed key and no answers during the drive: the window still opens on the rail.
  Dongle fast;
  check(fast.awake(14.2f, false) == WAKE_CHARGING_MINUTES && fast.pdw_open, "a drive ending at a high rail opens the window");
  const unsigned long last_motion = fast.last_active;
  int pokes = 0;
  while (fast.next == WAKE_CHARGING_MINUTES && pokes < 20) {
    fast.wake(14.9f);
    if (fast.next == WAKE_CHARGING_MINUTES) {
      pokes++;
      check(fast.now - last_motion < POST_DRIVE_WINDOW_MINUTES * 60000UL, "window polls stay inside the window");
    }
  }
  check(pokes >= 1 && pokes <= POST_DRIVE_MAX_POLLS, "a silent car gets a bounded number of 3-minute polls");
  check(fast.next == WAKE_PARKED_MINUTES && !fast.pdw_open, "the window expires across sleeps");

  Dongle capped;
  capped.awake(14.2f, false);
  capped.pdw_polls = POST_DRIVE_MAX_POLLS;
  check(capped.wake(14.9f) == WAKE_PARKED_MINUTES && !capped.pdw_open, "a spent poll budget ends the window early");
  check(capped.awake(14.2f, false) == WAKE_CHARGING_MINUTES && capped.pdw_open && capped.pdw_polls == 0,
        "the next drive opens a fresh window");

  Dongle reboot;
  reboot.boot();
  reboot.ladder_rail = 14.5f;
  reboot.last_active = reboot.now;
  check(reboot.pdwOpenWindow() && reboot.g_next_wake_minutes == WAKE_PARKED_MINUTES,
        "opening the window leaves the cadence to its caller (the NET-recovery reboot keeps its 1 min)");

  Dongle low;
  check(low.awake(13.5f, false, SILENT, 12.6f) == WAKE_PARKED_MINUTES && !low.pdw_open, "a low rail at the ladder end opens nothing");
  low.ladder_rail = 14.5f;
  low.pdwArmAtLadderEnd();
  check(!low.pdw_open && low.g_next_wake_minutes == WAKE_PARKED_MINUTES, "the ladder end decides once per wake");
  check(low.wake(12.6f) == WAKE_PARKED_MINUTES && !low.polled, "and polls nothing");

  Dongle dropped;
  dropped.awake(14.2f, false);
  check(dropped.wake(12.6f) == WAKE_PARKED_MINUTES && !dropped.polled, "a window never reads a low rail");

  Dongle plugged;
  plugged.pdw_open = 1;
  plugged.pdw_polls = POST_DRIVE_MAX_POLLS;
  check(plugged.awake(14.6f, true) == WAKE_CHARGING_MINUTES && plugged.pdw_open && plugged.pdw_polls == 0,
        "a power-on at a high rail opens a fresh window");

  Dongle dc;
  dc.awake(14.2f, false);
  dc.wake(14.9f, {{"current", -150.0}, {"voltage", 400.0}, {"soc", 30}});
  check(dc.recently_charging && dc.next == WAKE_DC_CHARGING_MINUTES && !dc.pdw_open,
        "a DC answer in the window takes the DC cadence and closes the window");

  Dongle charging;
  charging.wake(14.9f, {{"current", -12.0}, {"soc", 60}});
  check(charging.awake(14.9f, false, {{"current", -12.0}, {"soc", 60}}) == WAKE_CHARGING_MINUTES && !charging.pdw_open,
        "a ladder ending on the charging cadence leaves it alone");

  Dongle stale;
  stale.awake(14.2f, false);
  stale.pdw_open = 0;
  stale.g_pdw_ladder_done = false;
  stale.g_next_wake_minutes = WAKE_PARKED_MINUTES;
  stale.last_active = stale.now - (POST_DRIVE_WINDOW_MINUTES + 5) * 60000UL;
  stale.pdwArmAtLadderEnd();
  check(!stale.pdw_open && stale.g_next_wake_minutes == WAKE_PARKED_MINUTES, "a window already past opens nothing");

  // The ladder never opens over a latched, capped or sampled wake.
  for (int c = 0; c < 3; ++c) {
    Dongle busy;
    busy.boot();
    busy.recently_charging = c == 0;
    busy.charging_cap_latched = c == 1;
    busy.g_sample_poke = c == 2;
    busy.ladder_rail = 14.5f;
    busy.last_active = busy.now;
    busy.pdwArmAtLadderEnd();
    check(!busy.pdw_open && busy.g_next_wake_minutes == WAKE_PARKED_MINUTES,
          "a latched, capped or sample-poke wake opens no window");
  }

  Dongle both;
  both.awake(14.2f, false);
  both.charge_followup = 1;
  both.wake(14.9f);
  check(both.next == WAKE_CHARGING_MINUTES && both.pdw_polls == 1 && both.charge_followup == 1,
        "inside the window the follow-up is not spent");
}

// A stuck charging current on a car whose 12 V is topped up every three hours: two hours at a high rail,
// answering is_charging, then a low rail where it is silent. Each top-up is inside the stale window of the
// last, so it must continue the old budget rather than start a fresh one.
void topUpChecks() {
  Dongle car;
  const OBDVector stuck = {{"current", -12.0}, {"soc", 55}};
  const unsigned long window = CHARGE_STALE_RESET_MINUTES * 60000UL;
  std::map<unsigned long, int> fast;  // fast polls per stale window
  while (car.now < 7UL * 24 * 60 * 60 * 1000) {
    const bool high = car.now % (3UL * 60 * 60 * 1000) < 2UL * 60 * 60 * 1000;
    car.wake(high ? 14.4f : 12.7f, high ? stuck : SILENT);
    if (car.polled && car.next < WAKE_PARKED_MINUTES) fast[car.now / window]++;
  }
  for (const auto& w : fast)
    check(w.second <= CHARGE_CORROBORATE_MINUTES / WAKE_CHARGING_MINUTES + CHARGE_SILENT_HOLD_POLLS + 2,
          "top-ups inside the stale window share one corroboration budget");

  // A drive is not a pause: it still drops the old charge's state.
  car.recently_charging = false;
  car.awake(13.8f, false, {{"current", 20.0}, {"soc", 55}});
  check(car.charging_fast_minutes == 0 && !car.charging_cap_latched && car.charge_uncorroborated_s == 0,
        "a drive resets the charge budget, cap and corroboration");
}

// The alarm gate (§7): a flagged car gets none of the extra polls.
void alarmChecks() {
  Dongle car;
  car.alarm = true;
  check(car.awake(14.2f, false) == WAKE_PARKED_MINUTES && !car.pdw_open, "a flagged car opens no post-drive window");
  car.recently_charging = true;
  car.charge_followup = 1;
  check(car.wake(14.9f) == WAKE_PARKED_MINUTES && !car.polled, "a flagged car is not sampled, latched or not");
  check(car.wake(12.7f) == WAKE_PARKED_MINUTES && !car.polled, "nor read at the rail drop");
}

// A week with no charge and no drive: 12 V top-ups about four times a day cost nothing extra.
void costChecks() {
  Dongle idle;
  for (int i = 0; i < 7 * 48; ++i) idle.wake(i % 12 == 5 ? 14.4f : 12.7f, i % 24 == 5 ? SILENT : TOP_UP);
  check(idle.fast_pokes == 0 && idle.low_rail_polls == 0, "top-ups with no charge and no drive cost no extra poll");
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
        group == "logging" || group == "transport" || group == "sleep-reason" || group == "wdt-streak" || group == "profiles" || group == "schema" || group == "ble-frame", "known test group");
  if (group == "all" || group == "progress") progressChecks();
  if (group == "all" || group == "signal") signalChecks();
  if (group == "all" || group == "release") {
    releaseChecks();
    highRailSilenceChecks();
    stuckCurrentChecks();
    attendedChargeChecks();
    taperStallChecks();
    silentCorroborationChecks();
  }
  if (group == "all" || group == "corroboration") corroborationChecks();
  if (group == "all" || group == "wake-gate") {
    wakeGateChecks();
    chargeEndChecks();
    followupChecks();
    postDriveChecks();
    alarmChecks();
    costChecks();
    topUpChecks();
  }
  if (group == "all" || group == "logging") loggingChecks();
  if (group == "all" || group == "schema") schemaChecks();
  if (group == "all" || group == "transport") transportChecks();
  if (group == "all" || group == "ble-frame") { bleFrameChecks(); ble_frame_log::checks(); }
  if (group == "all" || group == "sleep-reason") sleep_reason::checks();
  if (group == "all" || group == "wdt-streak") wdt_streak::checks();
  if (argc > 2 && (group == "all" || group == "profiles")) profileChecks(argv[2]);
  std::cout << "Energy " << group << " checks PASS\n";
}
