// Differential checks for the park/drive decision and the poll-side energy block; built and run by
// run_sm_checks.py, which writes the old_side.inc / cur_side.inc / field.inc files from the pinned commit and the
// working tree.
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../baseLogger/config.h"

#include "../libraries/OBDWrapper/obd_vector.h"

enum class SmKind { POLL, ADVANCE, LADDER, HOLD, SLEEP, WAKE, MAINT, CRASH, SETUP };
enum class SmTo { ACTIVE, BLE, CHARGING, STANDBY };
enum { SM_NONE, SM_SPEED, SM_VRS, SM_BOTH };  // which speed keys the poll carries (BOTH: speed, then vrs)
enum { SM_TIMER, SM_MOTION, SM_COLD };   // wake cause
enum { SM_CAR_RAIL, SM_CAR_OBD, SM_CAR_OFF, SM_CAR_KEEP };  // car on by the 12 V rail or an OBD answer; KEEP: as read
enum { SM_MOVE_NOW, SM_MOVE_NONE, SM_MOVE_KEEP };  // the last detectMovement hit: this poll, never, as it was

struct SmEvent {
  SmKind k;
  std::string name;
  int key = SM_NONE;
  double speed = 0;
  double speed2 = 0;  // vehicle_reported_speed when key is SM_BOTH
  bool gnss = false;
  double gnss_kmh = 0;
  bool has_data = true;
  bool clock_ok = true;
  unsigned long ms = 0;
  SmTo to = SmTo::ACTIVE;
  bool modem_on = true;
  const char* reason = nullptr;
  bool sent = false;   // SLEEP: the pre-sleep flush sent every buffered frame
  bool crash = false;  // SLEEP: a panic in the pre-sleep flush, after the marker block
  int cause = SM_TIMER;
  float rail = 12.6f;
  bool awake = true;
  bool charge_latched = false;
  double atrv = NAN;   // WAKE: the ATRV read; NaN: 13.5 awake, 5.3 asleep
  bool alarm = false;  // WAKE: the OBD-alarm gate is enforced for this car
  bool dump = false;  // HOLD: the log dump rather than an OTA update
  bool on = false;    // MAINT: the console's maintenance_mode
  // POLL inputs to the driving rules (#6). The defaults make CUR agree with OLD: car on, motion this poll, no
  // sustained motion, no power, no charge latch. Charge and Silent polls keep the wake's rail and motion instead.
  int car = SM_CAR_RAIL;
  int motion = SM_MOVE_NOW;
  bool sustained = false;  // the accumulator holds 20 s of motion
  bool charge_block = false;
  // POLL, the car's side (carFrame): silent answers nothing; NaN leaves a PID out. Counter steps add to the last value.
  bool silent = false;
  double soc_step = 0, kwh_step = NAN, soe_step = NAN;
  double power = NAN, current = NAN, voltage = NAN, dcfc = NAN;
  double poll_rail = NAN;  // the rail this cycle read; NaN keeps the last
  unsigned long fix_age_ms = 0;  // POLL: how old the GNSS fix is
};

SmEvent Poll(int key, double speed, bool gnss = false, double kmh = 0, bool has_data = true, double speed2 = 0) {
  SmEvent e{SmKind::POLL, ""};
  std::ostringstream name;
  name << "POLL(" << (key == SM_SPEED || key == SM_BOTH ? "speed=" : key == SM_VRS ? "vrs=" : "none") ;
  if (key != SM_NONE) name << speed;
  if (key == SM_BOTH) name << ",vrs=" << speed2;
  if (gnss) name << ",gnss=" << kmh;
  if (!has_data) name << ",nodata";
  name << ")";
  e.name = name.str();
  e.key = key, e.speed = speed, e.speed2 = speed2, e.gnss = gnss, e.gnss_kmh = kmh, e.has_data = has_data;
  return e;
}
// A poll with the driving rules' inputs set; the name lists what differs from the defaults.
SmEvent Rules(SmEvent e, int car, int motion, bool sustained, double power = NAN, bool charge_block = false) {
  e.car = car, e.motion = motion, e.sustained = sustained, e.power = power, e.charge_block = charge_block;
  if (car == SM_CAR_OFF) e.silent = true, e.has_data = false;
  static const char* cars[] = {"", ",obd-only", ",car-off"};
  static const char* moves[] = {"", ",no-motion", ",motion-kept"};
  e.name.pop_back();
  e.name += std::string(cars[car]) + moves[motion] + (sustained ? ",sustained" : "") +
            (std::isnan(power) ? "" : ",power=" + std::to_string(power)) + (charge_block ? ",charging" : "") + ")";
  return e;
}
// A parked poll the car answers with charging PIDs (carFrame): current, and optionally voltage, the soc, kwh_charged
// and soe steps, the car's own is_dcfc and the rail this cycle read. NaN leaves a PID out.
SmEvent Charge(double amps, double volts = NAN, double soc_step = 0, double rail = NAN, double kwh_step = NAN,
               double dcfc = NAN, double soe_step = NAN) {
  std::ostringstream name;
  name << "CHARGE(a=" << amps;
  if (!std::isnan(volts)) name << ",v=" << volts;
  if (std::isnan(soc_step)) name << ",nosoc";
  else if (soc_step) name << ",soc+" << soc_step;
  if (!std::isnan(kwh_step)) name << ",kwh+" << kwh_step;
  if (!std::isnan(soe_step)) name << ",soe+" << soe_step;
  if (!std::isnan(dcfc)) name << ",dcfc=" << dcfc;
  if (!std::isnan(rail)) name << ",rail=" << rail;
  name << ")";
  SmEvent e{SmKind::POLL, name.str()};
  e.current = amps, e.voltage = volts, e.soc_step = soc_step, e.poll_rail = rail, e.kwh_step = kwh_step, e.dcfc = dcfc;
  e.soe_step = soe_step, e.car = SM_CAR_KEEP, e.motion = SM_MOVE_KEEP;
  return e;
}
// A poll the car does not answer.
SmEvent Silent(double rail = NAN) {
  SmEvent e{SmKind::POLL, std::isnan(rail) ? std::string("SILENT") : "SILENT(rail=" + std::to_string(rail) + ")"};
  e.silent = true, e.poll_rail = rail, e.has_data = false, e.car = SM_CAR_KEEP, e.motion = SM_MOVE_KEEP;
  return e;
}
SmEvent Advance(unsigned long ms) {
  SmEvent e{SmKind::ADVANCE, "ADVANCE(" + std::to_string(ms) + ")"};
  e.ms = ms;
  return e;
}
SmEvent Ladder(SmTo to) {
  const char* names[] = {"ACTIVE", "BLE", "CHARGING", "STANDBY"};
  SmEvent e{SmKind::LADDER, std::string("LADDER(") + names[(int)to] + ")"};
  e.to = to;
  return e;
}
SmEvent Hold(bool dump = false) {
  SmEvent e{SmKind::HOLD, dump ? "HOLD(dump)" : "HOLD(ota)"};
  e.dump = dump;
  return e;
}
SmEvent Maint(bool on) {
  SmEvent e{SmKind::MAINT, on ? "MAINT(on)" : "MAINT(off)"};
  e.on = on;
  return e;
}
SmEvent Sleep(bool modem_on = true, bool clock_ok = true, const char* reason = nullptr, bool sent = false,
             bool crash = false) {
  SmEvent e{SmKind::SLEEP, std::string("SLEEP(") + (modem_on ? "modem" : "nomodem") + (clock_ok ? "" : ",noclock") +
                             (sent ? ",sent" : "") + (crash ? ",crash" : "") + ")"};
  e.modem_on = modem_on, e.clock_ok = clock_ok, e.reason = reason, e.sent = sent, e.crash = crash;
  return e;
}
SmEvent Sent() { return Sleep(true, true, nullptr, true); }
SmEvent FlushCrash() { return Sleep(true, true, nullptr, false, true); }
SmEvent Crash() { return SmEvent{SmKind::CRASH, "CRASH"}; }
SmEvent Wake(int cause, float rail, bool awake = true, bool charge_latched = false) {
  const char* names[] = {"timer", "motion", "cold"};
  std::ostringstream name;
  name << "WAKE(" << names[cause] << "," << rail << (awake ? "" : ",asleep") << (charge_latched ? ",latched" : "") << ")";
  SmEvent e{SmKind::WAKE, name.str()};
  e.cause = cause, e.rail = rail, e.awake = awake, e.charge_latched = charge_latched;
  return e;
}
// A wake with an exact ATRV read, or with the OBD-alarm gate enforced.
SmEvent named(SmEvent e, const std::string& suffix) {
  e.name.insert(e.name.size() - 1, suffix);
  return e;
}
SmEvent Atrv(SmEvent e, double atrv) {
  e.atrv = atrv;
  return named(e, ",atrv=" + std::to_string(atrv).substr(0, 4));
}
SmEvent Alarmed(SmEvent e) {
  e.alarm = true;
  return named(e, ",alarm");
}
// A poll whose GNSS fix is ms old.
SmEvent Aged(SmEvent e, unsigned long ms) {
  e.fix_age_ms = ms;
  return named(e, ",fix=" + std::to_string(ms) + "ms");
}
// A completed full profile push from the wizard, which grants the alarm gate's setup permit; the phone leaving.
SmEvent Setup() {
  SmEvent e{SmKind::SETUP, "SETUP"};
  e.on = true;
  return e;
}
SmEvent Leave() { return SmEvent{SmKind::SETUP, "PHONE-LEFT"}; }

namespace sm_old {
#include "old_side.inc"
}
namespace sm_cur {
#include "cur_side.inc"
}

[[noreturn]] void fail(const std::string& message, const std::vector<SmEvent>& trace) {
  std::cerr << "FAIL: " << message << "\n  after:";
  for (const auto& e : trace) std::cerr << ' ' << e.name;
  std::cerr << '\n';
  std::exit(1);
}

void check(bool condition, const std::string& message, const std::vector<SmEvent>& trace) {
  if (!condition) fail(message, trace);
}

// A NaN PID (#87) is the same NaN on both sides; strings compare by text, never by pointer.
template <class A, class B> bool same(const A& a, const B& b) {
  if constexpr (std::is_enum<A>::value) return (int)a == (int)b;
  else if constexpr (std::is_floating_point<A>::value) return a == b || (std::isnan(a) && std::isnan(b));
  else if constexpr (std::is_same<A, const char*>::value) return a == b || (a && b && !strcmp(a, b));
  else return a == b;
}
template <class T> std::string text(const T& v) {
  if constexpr (std::is_enum<T>::value) return std::to_string((int)v);
  else if constexpr (std::is_same<T, std::string>::value) return '"' + v + '"';
  else if constexpr (std::is_same<T, const char*>::value) return v ? '"' + std::string(v) + '"' : "(null)";
  else { std::ostringstream s; s.precision(17); s << +v; return s.str(); }  // + prints a uint8_t as a number
}
std::string firstLineDiff(const char* what, const std::vector<std::string>& a, const std::vector<std::string>& b) {
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const std::string x = i < a.size() ? a[i] : "<none>", y = i < b.size() ? b[i] : "<none>";
    if (x != y) return std::string(what) + "[" + std::to_string(i) + "]: OLD " + x + " | CUR " + y;
  }
  return {};
}

// The first output where OLD and CUR differ, or empty: the scalars from the two snapshots, the buffered frames and the
// logs live.
std::string difference(const sm_old::Snap& a, const sm_cur::Snap& b) {
  const auto& fa = sm_old::obd_result_vector;
  const auto& fb = sm_cur::obd_result_vector;
#define SM_CMP(T, name, value) if (!same(a.name, b.name)) return #name ": OLD " + text(a.name) + " | CUR " + text(b.name);
  SM_RAM(SM_CMP) SM_RTC(SM_CMP) SM_STUB(SM_CMP) SM_WAKE(SM_CMP)
#undef SM_CMP
  if (a.gps_speed != b.gps_speed) return "gps speed";
  bool frames = fa.size() == fb.size();
  for (size_t i = 0; frames && i < fa.size(); ++i) {
    frames = fa[i].size() == fb[i].size();
    for (size_t j = 0; frames && j < fa[i].size(); ++j)
      frames = fa[i][j].first == fb[i][j].first && same(fa[i][j].second, fb[i][j].second);
  }
  if (!frames) return "buffered frames: OLD " + std::to_string(fa.size()) + " | CUR " + std::to_string(fb.size());
  if (sm_old::sm_log != sm_cur::sm_log) return firstLineDiff("logEvent", sm_old::sm_log, sm_cur::sm_log);
  if (sm_old::sm_serial != sm_cur::sm_serial) return firstLineDiff("Serial", sm_old::sm_serial, sm_cur::sm_serial);
  return {};
}

unsigned long events = 0;

// One event through both sides, then equality and both sides' properties.
void play(const SmEvent& e, std::vector<SmEvent>& trace, const sm_old::Snap& old_pre, const sm_cur::Snap& cur_pre) {
  trace.push_back(e);
  sm_old::step(e);
  sm_cur::step(e);
  events++;
  if (const char* broken = sm_old::propertyBroken(old_pre, e)) fail(std::string("OLD property: ") + broken, trace);
  if (const char* broken = sm_cur::propertyBroken(cur_pre, e)) fail(std::string("CUR property: ") + broken, trace);
  const std::string diff = difference(sm_old::save(false), sm_cur::save(false));
  if (!diff.empty()) fail("OLD and CUR differ at " + diff, trace);
}
void play(const SmEvent& e, std::vector<SmEvent>& trace) { play(e, trace, sm_old::save(), sm_cur::save()); }

void start(std::vector<SmEvent>& trace) {
  trace.clear();
  const SmEvent boot = Wake(SM_COLD, 12.6f);
  trace.push_back(boot);
  sm_old::powerOn(boot);
  sm_cur::powerOn(boot);
  const std::string diff = difference(sm_old::save(false), sm_cur::save(false));
  if (!diff.empty()) fail("OLD and CUR differ at power-on: " + diff, trace);
}

// The fast budget's cap (720 min) and the stale reset (360 min) are far past depth 5 from a cold start, so enumerate and
// random also start from these, set identically on both sides: a charge one fast poke short of the cap, and a charge
// with a budget one parked wake short of stale, released or still latched.
enum { SEED_NEAR_CAP, SEED_NEAR_STALE, SEED_LATCHED_STALE };
const char* const SEED_NAMES[] = {"SEED(near-cap)", "SEED(near-stale)", "SEED(latched-stale)"};
#define SM_SEED(ns, kind)                                                                                         \
  do {                                                                                                            \
    ns::recently_charging = kind != SEED_NEAR_STALE;                                                              \
    if (kind == SEED_NEAR_CAP) {                                                                                  \
      ns::charging_fast_minutes = (uint32_t)(WAKE_CHARGING_MAX_MINUTES - WAKE_CHARGING_MINUTES);                  \
    } else {                                                                                                      \
      ns::sm_rtc_ms += (unsigned long)CHARGE_STALE_RESET_MINUTES * 60000UL;                                       \
      ns::charging_fast_minutes = 120;                                                                            \
    }                                                                                                             \
    ns::charge_last_evidence_us = ns::esp_rtc_get_time_us() -                                                     \
      (kind == SEED_NEAR_CAP ? 0 : (uint64_t)(CHARGE_STALE_RESET_MINUTES - WAKE_PARKED_MINUTES) * 60000000ULL);   \
  } while (0)
void startSeeded(std::vector<SmEvent>& trace, int kind) {
  start(trace);
  SM_SEED(sm_old, kind);
  SM_SEED(sm_cur, kind);
  trace.push_back(SmEvent{SmKind::ADVANCE, SEED_NAMES[kind]});
}

const unsigned long AT = (unsigned long)(ACTIVE_TIMER);
const unsigned long MAX_AWAKE_MS = (unsigned long)MAX_AWAKE_MINUTES * 60UL * 1000UL;

// The exhaustive alphabet. Fractional speeds pin the %.1f text of the LEASH lines.
const std::vector<SmEvent> ALPHABET = {
  Poll(SM_NONE, 0), Poll(SM_SPEED, 0), Poll(SM_SPEED, 12.35), Poll(SM_VRS, 0.15), Poll(SM_NONE, 0, true, 7.25),
  Poll(SM_SPEED, 30.05, true, 88.85), Advance(AT), Advance(AT + 1), Advance(MAX_AWAKE_MS),
  Ladder(SmTo::CHARGING), Ladder(SmTo::STANDBY), Hold(), Sleep(), Wake(SM_MOTION, 12.6f), Wake(SM_COLD, 9.5f),
};

const unsigned long CONFIRM_MS = (unsigned long)CHARGE_STREAM_CONFIRM_SECONDS * 1000UL;
// The energy alphabet (#81): charge, release, silence and wake symbols (a taper-band wake among them), including
// soc-less kwh and soe profiles.
const std::vector<SmEvent> ENERGY_ALPHABET = {
  Charge(-50, 380, 0.5), Charge(-150, 400, 1), Charge(-30, NAN, 0, NAN, NAN, 1), Charge(1, NAN, NAN, NAN, 0.01),
  Charge(-50, NAN, NAN, NAN, NAN, NAN, 250), Charge(-50), Charge(0.479), Charge(30), Silent(), Silent(12.6),
  Advance(CONFIRM_MS), Wake(SM_TIMER, 13.6f), Wake(SM_TIMER, 13.1f), Wake(SM_TIMER, 12.6f), Wake(SM_MOTION, 13.6f),
  Sleep(),
};

// The wake alphabet (#82): timer wakes on each gate threshold (RAIL_AWAKE, the taper floor, the charge-end floor,
// the lockout's top), latched wakes in the taper band, the charge-end band, below it and on a weak rail, a latched
// motion wake, motion at the ATRV threshold and just under it, a weak cold boot, and an alarm-enforced poke and
// motion wake.
// From a seed, the energy symbols and an odd advance: a streaming charge's minutes, and a clock off the round values.
const std::vector<SmEvent> SEEDED_ALPHABET = [] {
  std::vector<SmEvent> a = ENERGY_ALPHABET;
  a.push_back(Advance(61007));
  return a;
}();

const std::vector<SmEvent> WAKE_ALPHABET = {
  Wake(SM_TIMER, 13.3f), Wake(SM_TIMER, 13.0f), Wake(SM_TIMER, 12.2f), Wake(SM_TIMER, 11.0f),
  Wake(SM_TIMER, 13.1f, true, true), Wake(SM_TIMER, 12.5f, true, true), Wake(SM_TIMER, 12.0f, true, true),
  Wake(SM_TIMER, 10.0f, true, true), Wake(SM_MOTION, 12.6f, true, true), Atrv(Wake(SM_MOTION, 12.6f), 8.0),
  Atrv(Wake(SM_MOTION, 12.6f), 7.99), Wake(SM_COLD, 10.0f), Alarmed(Wake(SM_TIMER, 13.6f)),
  Alarmed(Wake(SM_MOTION, 13.6f)), Charge(-50, 380, 0.5),
  Silent(), Sleep(), Advance(CONFIRM_MS),
};

unsigned long leaves = 0;
void enumerate(int depth, std::vector<SmEvent>& trace, const std::vector<SmEvent>& alphabet) {
  if (depth == 0) { leaves++; return; }
  const sm_old::Snap old_node = sm_old::save();
  const sm_cur::Snap cur_node = sm_cur::save();
  for (const auto& e : alphabet) {
    sm_old::restore(old_node);
    sm_cur::restore(cur_node);
    play(e, trace, old_node, cur_node);
    enumerate(depth - 1, trace, alphabet);
    trace.pop_back();
  }
}

SmEvent randomEvent(std::mt19937& rng) {
  auto pick = [&](std::initializer_list<double> values) {
    return *(values.begin() + std::uniform_int_distribution<size_t>(0, values.size() - 1)(rng));
  };
  switch (std::uniform_int_distribution<int>(0, 11)(rng)) {
  case 10: case 11: {
    if (pick({0, 0, 0, 1}) != 0) return Silent(pick({NAN, 12.6, 13.6}));
    return Charge(pick({-150, -50, -30, -2.5, -2, -1.5, 0, 0.479, 2, 2.5, 30, -3e8, NAN}), pick({NAN, 300, 380, 400}),
                  pick({0, 0.1, 0.2, 0.25, 1, -1, NAN}), pick({NAN, NAN, 12.6, 13.0, 13.29, 13.3, 14.4}),
                  pick({NAN, 0, 0.004, 0.005, 0.01}), pick({NAN, NAN, NAN, 0, 1}), pick({NAN, NAN, 0, 199, 200, 250}));
  }
  case 0: case 1: case 2: {
    const int key = (int)pick({SM_NONE, SM_SPEED, SM_SPEED, SM_VRS, SM_BOTH});
    const double speeds[] = {0, 0.05, 0.15, 0.25, 0.95, 12.35, 129.95, -1, 1e-9, NAN};
    auto speed = [&] { return speeds[std::uniform_int_distribution<int>(0, 9)(rng)]; };
    const double first = speed(), second = speed();
    const bool gnss = pick({0, 1}) != 0;
    return Poll(key, first, gnss, pick({5.0, 7.25, 88.85}), pick({0, 1, 1, 1}) != 0, second);
  }
  case 3: case 4:
    return Advance((unsigned long)pick({1, 4999, 5000, (double)(AT - 1), (double)AT, (double)(AT + 1), (double)CONFIRM_MS,
                                        (double)GPS_MOVING_MAX_AGE_MS, (double)MAX_AWAKE_MS,
                                        (double)std::uniform_int_distribution<unsigned long>(0, 400000)(rng)}));
  case 5: return Ladder((SmTo)std::uniform_int_distribution<int>(0, 3)(rng));
  case 6: return pick({0, 1}) != 0 ? Hold(pick({0, 1}) != 0) : Maint(pick({0, 1}) != 0);
  case 7: return Sleep(pick({0, 1, 1}) != 0, pick({0, 1, 1}) != 0);
  default: {
    const int cause = (int)pick({SM_TIMER, SM_MOTION, SM_COLD});
    const float rail = (float)pick({5.9, 7.99, 8.0, 9.5, 10.99, 11.0, 12.0, 12.19, 12.2, 12.5, 12.6, 12.99, 13.0,
                                    13.1, 13.29, 13.3, 14.8});
    SmEvent e = Wake(cause, rail, pick({0, 1, 1}) != 0, pick({0, 0, 1}) != 0);
    if (pick({0, 0, 0, 1}) != 0) e = Atrv(e, pick({0, 5.3, 7.99, 8.0, 13.5}));
    if (pick({0, 0, 0, 0, 0, 1}) != 0) e = Alarmed(e);
    return e;
  }
  }
}

void randomGroup() {
  std::vector<SmEvent> trace;
  for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
    std::mt19937 rng(seed);
    for (int sequence = 0; sequence < 1000; ++sequence) {
      start(trace);
      for (int i = 0; i < 30; ++i) play(randomEvent(rng), trace);
    }
  }
  for (int kind : {SEED_NEAR_CAP, SEED_NEAR_STALE, SEED_LATCHED_STALE}) {  // own seeds: the cold-start sequences above stay as they were
    std::mt19937 rng(100 + kind);
    for (int sequence = 0; sequence < 500; ++sequence) {
      startSeeded(trace, kind);
      for (int i = 0; i < 30; ++i) play(randomEvent(rng), trace);
    }
  }
  // An enforced car (#83): every wake enforced, and each poll's fix around ALARM_GNSS_MAX_AGE_MS, from its own
  // generator so the draws above stay as they were.
  std::mt19937 rng(200), ages(201);
  const unsigned long fix_ages[] = {0, ALARM_GNSS_MAX_AGE_MS, ALARM_GNSS_MAX_AGE_MS + 1, 60000};
  for (int sequence = 0; sequence < 500; ++sequence) {
    start(trace);
    play(Sleep(), trace);
    play(Alarmed(Wake(SM_MOTION, 13.6f)), trace);
    for (int i = 0; i < 30; ++i) {
      SmEvent e = randomEvent(rng);
      if (e.k == SmKind::POLL) e = Aged(e, fix_ages[std::uniform_int_distribution<int>(0, 3)(ages)]);
      if (e.k == SmKind::WAKE && !e.alarm) e = Alarmed(e);
      const int phone = std::uniform_int_distribution<int>(0, 29)(ages);
      if (phone < 2) e = phone ? Setup() : Leave();
      play(e, trace);
    }
  }
}

struct Sample { unsigned long ms; double current; };
#include "field.inc"  // DRIVE_0930, DRIVE_1012 from energy_checks.cpp

// ---- named field scenarios: equality throughout, plus what each one is about, asserted on CUR ----

size_t countLog(const std::string& prefix) {
  size_t n = 0;
  for (const auto& line : sm_cur::sm_log) n += line.compare(0, prefix.size(), prefix) == 0;
  return n;
}

void scenarios() {
  std::vector<SmEvent> t;
  using namespace sm_cur;

  // Speed keys: > 0.0 on speed or vehicle_reported_speed unlatches; 0 and negative do not.
  start(t);
  play(Poll(SM_SPEED, 0), t);
  play(Poll(SM_SPEED, -1), t);
  check(is_currently_parked && countLog("LEASH driving") == 0, "speed 0 and negative speed keep parked", t);
  play(Poll(SM_VRS, 0.15), t);
  check(!is_currently_parked && sm_alarm_calls == 1 && sm_alarm_last == 0.15,
        "vehicle_reported_speed unlatches and feeds the alarm", t);
  check(sm_log.back() == "LEASH driving speed=0.1 -> unlatch parked + hold awake (smooth-road guard)",
        "LEASH speed line text, double formatting", t);

  // Both keys: the first one above 0.0 decides; a NaN speed is not above it.
  start(t);
  play(Poll(SM_BOTH, NAN, false, 0, true, 0), t);
  check(is_currently_parked && sm_alarm_calls == 0, "a NaN speed and a zero vehicle_reported_speed keep parked", t);
  play(Poll(SM_BOTH, 0, false, 0, true, 5.0), t);
  check(!is_currently_parked && sm_alarm_calls == 1 && sm_alarm_last == 5.0,
        "speed 0 then vehicle_reported_speed 5 unlatches on the second key", t);

  // Smooth-road leash: speed alone holds driving and the awake leash past ACTIVE_TIMER.
  start(t);
  for (unsigned long elapsed = 0; elapsed <= 2 * AT; elapsed += 5000) {
    play(Poll(SM_SPEED, 88.0), t);
    check(!is_currently_parked && last_active == sm_now && g_last_speed_ms == sm_now,
          "every speed poll refreshes the latch, the leash and the hold", t);
    play(Advance(5000), t);
  }
  check(countLog("LEASH driving") == 1 && sm_freezes == 0, "one unlatch for the whole smooth drive", t);

  // Time re-latch boundary: exactly ACTIVE_TIMER after the last hold is still driving, one more ms parks.
  start(t);
  play(Poll(SM_SPEED, 20), t);
  play(Advance(AT), t);
  play(Poll(SM_NONE, 0), t);
  check(!is_currently_parked, "ACTIVE_TIMER after the last speed is still driving", t);
  play(Advance(1), t);
  play(Poll(SM_NONE, 0), t);
  check(is_currently_parked && sm_freezes == 1 && countLog("LEASH no speed for 300s") == 1,
        "ACTIVE_TIMER + 1 re-latches once, with one freeze", t);
  check(sm_log.size() >= 2 && sm_log[sm_log.size() - 2] == "PARKED_LOC (harness)",
        "the freeze is logged before the re-latch line", t);

  // Garage latch: GNSS movement holds only while fixes are fresh, then the re-latch fires on time.
  start(t);
  for (int i = 0; i < 6; ++i) { play(Poll(SM_NONE, 0, true, 32.4), t); play(Advance(5000), t); }
  const unsigned long last_hold = g_last_speed_ms;
  check(!is_currently_parked && countLog("LEASH driving gnss=32.4km/h") == 1, "fresh GNSS unlatches once", t);
  while (sm_now <= last_hold + AT) {
    play(Poll(SM_NONE, 0), t);  // fixes stopped: gpsMovingFresh is false past its age bound
    if (sm_now <= last_hold + AT) check(!is_currently_parked, "driving until ACTIVE_TIMER after the last fresh fix", t);
    play(Advance(5000), t);
  }
  play(Poll(SM_NONE, 0), t);
  check(is_currently_parked && sm_freezes == 1, "a stale GNSS verdict cannot pin the latch (garage)", t);

  // CHARGING/STANDBY re-latch: one freeze; the second idle stage and ACTIVE/BLE change nothing.
  start(t);
  play(Poll(SM_SPEED, 10), t);
  play(Ladder(SmTo::CHARGING), t);
  check(is_currently_parked && sm_freezes == 1, "CHARGING re-latches with one freeze", t);
  play(Ladder(SmTo::STANDBY), t);
  play(Ladder(SmTo::ACTIVE), t);
  play(Ladder(SmTo::BLE), t);
  check(is_currently_parked && sm_freezes == 1 && countLog("LEASH") == 1, "no second freeze, no LEASH line", t);
  play(Poll(SM_SPEED, 10), t);
  play(Ladder(SmTo::STANDBY), t);
  check(is_currently_parked && sm_freezes == 2, "STANDBY from driving re-latches too", t);

  // OTA mid-drive: holds keep the latch through a transfer longer than ACTIVE_TIMER.
  start(t);
  play(Poll(SM_SPEED, 50), t);
  for (unsigned long elapsed = 0; elapsed < 2 * AT; elapsed += 5000) { play(Advance(5000), t); play(Hold(), t); }
  play(Poll(SM_NONE, 0), t);
  check(!is_currently_parked && sm_freezes == 0, "the OTA hold keeps a drive through the transfer", t);
  start(t);
  play(Poll(SM_SPEED, 50), t);
  for (unsigned long elapsed = 0; elapsed < 2 * AT; elapsed += 5000) { play(Advance(5000), t); play(Hold(true), t); }
  play(Poll(SM_NONE, 0), t);
  check(!is_currently_parked && sm_freezes == 0, "the log-dump hold keeps a drive through the dump", t);

  // Maintenance mode (a console session) holds off the backstop; releasing it lets the next tick sleep.
  start(t);
  play(Maint(true), t);
  play(Advance(MAX_AWAKE_MS + 1), t);
  play(Ladder(SmTo::ACTIVE), t);
  check(!sm_asleep, "no backstop sleep during a console session", t);
  play(Maint(false), t);
  play(Ladder(SmTo::ACTIVE), t);
  check(sm_asleep && sm_sleep_reason == "backstop", "the backstop sleeps once the session ends", t);

  // Speed and GNSS in the same cycle: one LEASH line (speed wins), one alarm note.
  start(t);
  play(Poll(SM_SPEED, 30.05, true, 88.85), t);
  check(!is_currently_parked && countLog("LEASH driving") == 1 && countLog("LEASH driving speed=30.1") == 1 &&
        sm_alarm_calls == 1, "speed and GNSS together log one speed unlatch", t);

  // Trip end: one marker; a sleep mid-drive keeps the debt and queues none.
  start(t);
  play(Poll(SM_SPEED, 25), t);
  check(park_marker_owed, "a buffered driving frame owes the marker", t);
  play(Sleep(), t);
  check(obd_result_vector.size() == 1 && park_marker_owed, "a mid-drive sleep queues no marker and keeps the debt", t);
  play(Wake(SM_MOTION, 13.5f), t);
  play(Poll(SM_SPEED, 25), t);
  play(Ladder(SmTo::STANDBY), t);
  play(Sleep(), t);
  check(obd_result_vector.size() == 3 && !park_marker_owed && obd_result_vector.back().size() == 2,
        "the trip end queues one marker", t);
  play(Wake(SM_TIMER, 13.6f), t);
  play(Sleep(), t);
  check(obd_result_vector.size() == 3, "a parked heartbeat queues no second marker", t);

  // The no-speed-PID drives from the field (energy_checks.cpp): polled at their own times, the GNSS unlatches
  // at 367 s and 459 s, and nothing before that leaves PARK.
  for (const auto& drive : {std::make_pair(&DRIVE_0930, 367000UL), std::make_pair(&DRIVE_1012, 459000UL)}) {
    start(t);
    play(Wake(SM_MOTION, 13.6f), t);
    for (const auto& sample : *drive.first) {
      play(Advance(sample.ms - sm_now), t);
      play(Poll(SM_NONE, 0, sample.ms >= drive.second, 40.0), t);
      check(is_currently_parked == (sample.ms < drive.second), "the field drive latches only on fresh GNSS", t);
    }
  }

  // ---- energy (#81): the poll block's verdicts, each asserted on CUR while OLD and CUR stay equal ----
  auto poke = [&](const SmEvent& poll) {  // one charge-sample poke: sleep, a high-rail timer wake, one poll
    play(Sleep(), t);
    play(Wake(SM_TIMER, 13.6f), t);
    check(g_sample_poke, "a high-rail timer wake is a sample poke", t);
    play(poll, t);
  };
  auto until = [&](auto done, auto next, const char* what) {  // a loop that fails, rather than spins, if never done
    for (int i = 0; !done(); ++i) {
      if (i == 1000) fail(std::string("never reached: ") + what, t);
      next();
    }
  };
  auto bufferedHas = [&](const char* key, double value) {
    for (const auto& kv : obd_result_vector.back()) if (kv.first == key) return kv.second == value;
    return false;
  };

  // AC at sample pokes: the charging cadence; the field's one positive sample holds the latch for
  // CHARGE_RELEASE_POLLS, then the release arms the follow-up.
  start(t);
  poke(Charge(-50, 380, 0.5));
  check(recently_charging && g_next_wake_minutes == WAKE_CHARGING_MINUTES && bufferedHas("is_charging", 1) &&
        bufferedHas("is_dcfc", 0) && countLog("CHARGE 19.0kW (380.0V x -50.0A) -> AC") == 1, "an AC charge", t);
  poke(Charge(0.479));
  check(recently_charging && charge_release_polls == 1 && g_next_wake_minutes == WAKE_CHARGING_MINUTES,
        "one positive sample only starts the release", t);
  poke(Charge(0.479));
  check(!recently_charging && g_next_wake_minutes == WAKE_PARKED_MINUTES && charge_followup == 1 &&
        countLog("FOLLOWUP armed (latch released)") == 1, "the second releases and arms the follow-up", t);

  // DC by power: two inferred polls latch the session, which holds DC through a taper below DC_CHARGING_MIN_KW;
  // the car's own is_dcfc=0 ends it.
  start(t);
  poke(Charge(-150, 400, 1));
  check(g_dc_confirmed && !dc_session && g_dc_inferred_polls == 1 && g_next_wake_minutes == WAKE_DC_CHARGING_MINUTES,
        "one inferred DC poll: the DC cadence, no session yet", t);
  play(Charge(-150, 400, 1), t);
  check(dc_session && countLog("CHARGE 60.0kW") == 1, "the second latches the session, logged once", t);
  play(Charge(-50, 380, 0.5), t);
  check(dc_session && g_dc_confirmed && g_next_wake_minutes == WAKE_DC_CHARGING_MINUTES &&
        countLog("CHARGE 19.0kW (380.0V x -50.0A) -> AC") == 1, "the taper stays DC", t);
  play(Charge(-30, NAN, 0.5, NAN, NAN, 0), t);
  check(!dc_session && !g_dc_confirmed && g_next_wake_minutes == WAKE_CHARGING_MINUTES, "the car's is_dcfc=0 ends it", t);
  // On a streaming wake, a DC charge that falls silent with the rail down releases once the hold is spent, and the
  // release drops the session with the rest of the charge's state.
  start(t);
  play(Advance(CONFIRM_MS), t);
  play(Charge(-150, 400, 1), t);
  play(Charge(-150, 400, 1), t);
  check(dc_session && !g_sample_poke, "a DC session on a streaming wake", t);
  until([&] { return !recently_charging; }, [&] { play(Silent(12.6), t); }, "the silent DC release");
  check(!dc_session && charge_uncorroborated_s == 0 && charge_corr_soc_hw < 0, "its release drops the session", t);

  // Silence: CHARGE_SILENT_HOLD_POLLS silent pokes hold the latch; past that, a high rail waits at the parked
  // cadence still latched (parked-held) and never releases.
  start(t);
  poke(Charge(-50, 380, 0.5));
  for (int i = 0; i < CHARGE_SILENT_HOLD_POLLS; ++i) {
    poke(Silent());
    check(recently_charging && g_next_wake_minutes == WAKE_CHARGING_MINUTES, "a silent poll holds the charge", t);
  }
  poke(Silent());
  check(recently_charging && g_next_wake_minutes == WAKE_PARKED_MINUTES && g_cadence_reason &&
        std::string(g_cadence_reason) == "parked-held" && charge_release_polls == 0, "past the hold: parked-held", t);
  // A pause keeps the charge's budget, cap and soc mark until its last evidence is CHARGE_STALE_RESET_MINUTES old.
  const uint64_t STALE_US = (uint64_t)CHARGE_STALE_RESET_MINUTES * 60000000ULL;
  auto stale = [&] { return esp_rtc_get_time_us() - charge_last_evidence_us >= STALE_US; };
  const uint32_t held_fast = charging_fast_minutes;
  const double held_soc = prev_charge_soc, held_hw = charge_corr_soc_hw;
  check(held_fast > 0 && held_soc > 0 && held_hw >= 0, "the held charge has a budget and a soc mark", t);
  until(stale, [&] {
    check(charging_fast_minutes == held_fast && prev_charge_soc == held_soc && charge_corr_soc_hw == held_hw,
          "parked-held keeps the budget and the soc mark until stale", t);
    poke(Silent());
  }, "a stale parked-held charge");
  check(recently_charging && charging_fast_minutes == 0 && prev_charge_soc == 0.0 && charge_corr_soc_hw < 0,
        "the first parked-held poll once stale resets them", t);
  // The same pause ending in a skip wake: the release at a poke keeps the budget, the skip resets it once stale.
  start(t);
  for (int i = 0; i < 3; ++i) poke(Charge(-50, 380, 0.5));
  poke(Charge(0.479));
  poke(Charge(0.479));
  const uint32_t released_fast = charging_fast_minutes;
  const double released_hw = charge_corr_soc_hw;
  check(!recently_charging && released_fast > 0 && released_hw >= 0, "a release at a poke keeps the budget", t);
  play(Sleep(), t);
  until(stale, [&] {
    check(charging_fast_minutes == released_fast && charge_corr_soc_hw == released_hw,
          "a skip wake keeps the budget until stale", t);
    play(Wake(SM_TIMER, 12.6f), t);
    check(sm_asleep && !sm_gate_open, "a low-rail parked wake is a skip", t);
  }, "a stale skip");
  check(charging_fast_minutes == 0 && charge_corr_soc_hw < 0, "the first skip once stale resets the budget", t);

  // The post-drive window: a streaming wake ending at a high rail comes back on the charging cadence; each silent
  // high-rail poke inside the window is one more pdw poll, until the window expires.
  start(t);
  play(Sleep(), t);
  play(Wake(SM_MOTION, 13.6f), t);
  play(Sleep(), t);
  check(pdw_open && g_next_wake_minutes == WAKE_CHARGING_MINUTES && countLog("POSTDRIVE window opened") == 1,
        "the ladder end opens the window", t);
  const uint64_t WINDOW_US = (uint64_t)POST_DRIVE_WINDOW_MINUTES * 60000000ULL;
  unsigned pdw_expected = 0;
  until([&] { return !pdw_open; }, [&] {
    poke(Silent());
    if (esp_rtc_get_time_us() - pdw_open_us < WINDOW_US)
      check(pdw_open && pdw_polls == ++pdw_expected && g_next_wake_minutes == WAKE_CHARGING_MINUTES &&
            std::string(g_cadence_reason) == "pdw", "a silent high-rail poke inside the window", t);
  }, "the window's end");
  check(pdw_expected >= 2 && g_next_wake_minutes == WAKE_PARKED_MINUTES &&
        countLog("POSTDRIVE window closed reason=expired") == 1, "the window expires, logged once", t);

  // #22: a drive that parks with the DC-DC already off ends the ladder at a healthy but low rail. It comes back in
  // POST_DRIVE_RECHECK_MINUTES once, inside a window timed from the ladder end, then the unchanged gate decides.
  uint64_t recheck_us = 0;
  auto parkLow = [&] {
    start(t);
    play(Poll(SM_SPEED, 50), t);
    play(Advance(60000), t);
    play(Poll(SM_SPEED, 0), t);
    play(Silent(12.42), t);  // the car shut its DC-DC down as it parked (FIELD, #22: 12.42 V)
    play(Advance(AT + 1), t);
    play(Ladder(SmTo::CHARGING), t);
    play(Advance(AT + 1), t);
    play(Ladder(SmTo::STANDBY), t);
    play(Sleep(), t);
    check(sm_asleep && is_currently_parked && sm_armed_minutes == POST_DRIVE_RECHECK_MINUTES && g_cadence_reason &&
          std::string(g_cadence_reason) == "pdw-recheck" && pdw_open &&
          countLog("POSTDRIVE window recheck wake=POWERON rail=12.42") == 1,
          "a ladder ending at a healthy low rail re-checks in POST_DRIVE_RECHECK_MINUTES", t);
    recheck_us = esp_rtc_get_time_us();
  };
  parkLow();
  play(Wake(SM_TIMER, 12.42f), t);
  check(sm_rtc_ms * 1000ULL - recheck_us == (uint64_t)POST_DRIVE_RECHECK_MINUTES * 60000000ULL &&
        sm_asleep && !sm_gate_open && sm_armed_minutes == WAKE_PARKED_MINUTES, "a low-rail re-check wake skips to parked", t);
  play(Wake(SM_TIMER, 12.42f), t);
  check(sm_asleep && !sm_gate_open && sm_armed_minutes == WAKE_PARKED_MINUTES && countLog("POSTDRIVE window recheck") == 1,
        "and there is no second re-check", t);
  // Plugged in after parking: the re-check wake finds the rail up and the car answering a charge.
  parkLow();
  poke(Charge(-50, 380, 0.5));
  check(recently_charging && sm_asleep && sm_armed_minutes == WAKE_CHARGING_MINUTES && !pdw_open,
        "a re-check wake that finds a charge takes the charging cadence", t);
  // The rail up but the car silent (OBD not up yet): the window's bounded WAKE_CHARGING_MINUTES retries, then parked.
  parkLow();
  unsigned recheck_polls = 0;
  until([&] { return !pdw_open; }, [&] {
    poke(Silent());
    if (pdw_open) {
      check(sm_armed_minutes == WAKE_CHARGING_MINUTES && pdw_polls == ++recheck_polls && std::string(g_cadence_reason) == "pdw" &&
            esp_rtc_get_time_us() - recheck_us < (uint64_t)POST_DRIVE_WINDOW_MINUTES * 60000000ULL,
            "a silent high-rail re-check wake retries inside the window", t);
    }
  }, "the re-check window's end");
  check(recheck_polls >= 1 && recheck_polls <= POST_DRIVE_MAX_POLLS && sm_armed_minutes == WAKE_PARKED_MINUTES &&
        countLog("POSTDRIVE window closed reason=expired") == 1, "bounded retries, then the parked cadence", t);

  // A streaming wake trusts a charging sample only CHARGE_STREAM_CONFIRM_SECONDS after the last discharge one.
  start(t);
  play(Charge(-50), t);
  check(!recently_charging && !bufferedHas("is_charging", 1), "no verdict before the confirm delay", t);
  play(Advance(CONFIRM_MS), t);
  play(Charge(-50), t);
  check(recently_charging && bufferedHas("is_charging", 1), "a charge once it is trusted", t);
  play(Charge(30), t);
  play(Advance(CONFIRM_MS - 1), t);
  play(Charge(-50), t);
  check(!bufferedHas("is_charging", 1), "a discharge sample restarts the delay", t);

  // The fast budget: WAKE_CHARGING_MAX_MINUTES of charging cadence latches the cap, and it holds.
  start(t);
  until([&] { return charging_cap_latched; }, [&] { poke(Charge(-50, 380, 0.5)); }, "the cap");
  check(charging_fast_minutes >= (uint32_t)WAKE_CHARGING_MAX_MINUTES && g_next_wake_minutes == WAKE_PARKED_MINUTES &&
        std::string(g_cadence_reason) == "parked-cap", "the cap latches at the budget", t);
  poke(Charge(-50, 380, 0.5));
  check(charging_cap_latched && g_next_wake_minutes == WAKE_PARKED_MINUTES, "and stays latched while charging", t);
  // Answered pokes after the charge keep the cap until its last evidence is stale, then drop it.
  poke(Charge(30));
  poke(Charge(30));
  check(!recently_charging && charging_cap_latched, "a release at a poke keeps the cap", t);
  until([&] { return !charging_cap_latched; }, [&] {
    check(!stale(), "the cap holds until stale", t);
    poke(Charge(30));
  }, "a stale cap");
  check(charging_fast_minutes == 0, "the first answered poke once stale drops the cap and the budget", t);
  // A streaming wake's charge counts its time on the fast cadence: 30 minutes of 5 s polls is 30 minutes of budget
  // beyond the first poll's tier (the budget property checks every poll on the way).
  start(t);
  play(Advance(CONFIRM_MS), t);
  play(Charge(-50, 380, 0.5), t);
  const uint32_t first = charging_fast_minutes;
  for (int i = 0; i < 360; ++i) { play(Advance(5000), t); play(Charge(-50, 380, 0), t); }
  check(!g_sample_poke && charging_fast_minutes >= first + 29 && charging_fast_minutes <= first + 31,
        "a streaming charge adds its 30 minutes", t);
  // Past the cap, a release at a poke arms the follow-up: one silent high-rail poke after it looks again at the
  // charging cadence, with the cap still latched. By design, once per charge, and not the charging branch.
  start(t);
  until([&] { return charging_cap_latched; }, [&] { poke(Charge(-50, 380, 0.5)); }, "the cap");
  poke(Charge(0.479));
  poke(Charge(0.479));
  check(!recently_charging && charging_cap_latched && charge_followup == 1, "released past the cap, follow-up armed", t);
  poke(Silent());
  check(charging_cap_latched && g_next_wake_minutes == WAKE_CHARGING_MINUTES && g_cadence_reason &&
        std::string(g_cadence_reason) == "followup" && charge_followup == 0, "one follow-up look past the cap", t);
  poke(Silent());
  check(g_next_wake_minutes == WAKE_PARKED_MINUTES, "and only one", t);

  // Corroboration: a charging current with a flat soc for CHARGE_CORROBORATE_MINUTES goes slow; a rise restores it.
  start(t);
  until([&] { return charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60; },
        [&] { poke(Charge(-50)); }, "uncorroborated");
  check(countLog("CHARGE uncorroborated") == 1 && g_next_wake_minutes == WAKE_CHARGING_MINUTES,
        "the poll that crosses CHARGE_CORROBORATE_MINUTES logs it, at the charging cadence still", t);
  poke(Charge(-50));
  check(countLog("CHARGE uncorroborated") == 1 && g_next_wake_minutes == WAKE_PARKED_MINUTES &&
        std::string(g_cadence_reason) == "parked-uncorroborated", "the next one is slow, with no second line", t);
  poke(Charge(-50, NAN, 0.5));
  check(g_next_wake_minutes == WAKE_CHARGING_MINUTES && charge_uncorroborated_s == 0 &&
        countLog("CHARGE corroborated") == 1, "a soc rise restores it", t);

  // ---- the setup() wake gate (#82): what depth 5 cannot reach, each asserted on CUR ----
  auto wakeAt = [&](const SmEvent& wake) { play(Sleep(), t); play(wake, t); };

  // The taper bridge: TAPER_CONFIRM_MAX_WAKES SoC-stalled confirm-polls in the taper band, then the give-up: the
  // latch clears, the follow-up is armed and the wake skips.
  start(t);
  poke(Charge(-50, 380, 0.5));
  for (int i = 0; i < TAPER_CONFIRM_MAX_WAKES; ++i) {
    wakeAt(Wake(SM_TIMER, 13.1f));
    check(sm_gate_open && g_sample_poke && g_taper_confirm && recently_charging, "a taper-band wake confirm-polls", t);
    check(countLog("ATRVGATE atrv=13.50 rail=13.10 AWAKE poll(taper-confirm)") == (size_t)i + 1 &&
          countLog("TAPER_CONFIRM attempt=" + std::to_string(i) + "/") == 1, "each confirm-poll is logged", t);
    play(Charge(-50, 380, 0), t);
    check(taper_confirm_wakes == (uint32_t)i + 1, "a confirm-poll with no SoC gain spends the budget", t);
  }
  wakeAt(Wake(SM_TIMER, 13.1f));
  check(!sm_gate_open && sm_asleep && !recently_charging && taper_confirm_wakes == 0 && charge_followup == 1 &&
        countLog("FOLLOWUP armed (latch cleared at rail=13.10)") == 1, "a spent budget gives up and skips", t);

  // The charge-end read: one final poll per latch when the rail falls below the taper floor, none below
  // CHARGE_END_POLL_FLOOR; a rail-awake wake re-arms it.
  start(t);
  wakeAt(Wake(SM_TIMER, 12.5f, true, true));
  check(sm_gate_open && g_sample_poke && charge_end_polled && !recently_charging && charge_followup == 1 &&
        countLog("CHARGE_END rail=12.50 < taper floor 13.00 -> final poll, latch cleared") == 1, "the charge-end read", t);
  wakeAt(Wake(SM_TIMER, 12.5f, true, true));
  check(!sm_gate_open && !recently_charging && countLog("CHARGE_END") == 1, "only once per latch", t);
  wakeAt(Wake(SM_TIMER, 13.6f));
  check(!charge_end_polled, "a rail-awake wake re-arms it", t);
  wakeAt(Wake(SM_TIMER, 12.5f, true, true));
  check(sm_gate_open && countLog("CHARGE_END") == 2, "and the next latch reads again", t);
  wakeAt(Wake(SM_TIMER, 13.6f));
  wakeAt(Wake(SM_TIMER, 12.19f, true, true));
  check(!sm_gate_open && !recently_charging && countLog("CHARGE_END") == 2, "below the floor: cleared, no read", t);
  wakeAt(Wake(SM_MOTION, 12.6f));
  check(charge_followup == 0 && countLog("FOLLOWUP cleared reason=motion") == 1, "motion clears the follow-up", t);
  // A give-up on a stale charge starts the next charge's corroboration from nothing; short of stale it keeps it.
  start(t);
  for (int i = 0; i < 3; ++i) poke(Charge(-50, 380, 0.5));
  poke(Charge(0.479));
  poke(Charge(0.479));
  play(Sleep(), t);
  while (esp_rtc_get_time_us() + 2ULL * WAKE_PARKED_MINUTES * 60000000ULL - charge_last_evidence_us < STALE_US)
    play(Wake(SM_TIMER, 12.6f), t);
  play(Wake(SM_TIMER, 12.6f), t);
  check(charge_uncorroborated_s > 0 && !chargeStateStale(esp_rtc_get_time_us()), "a skip short of stale keeps it", t);
  play(Wake(SM_TIMER, 12.5f, true, true), t);
  check(sm_gate_open && charge_uncorroborated_s == 0 && charge_corr_soc_hw < 0, "the stale give-up resets it", t);

  // A weak-rail cold boot stays up for BLE with the modem off, and its window's expiry sleeps it parked, modem off.
  t.clear();
  t.push_back(Wake(SM_COLD, 10.0f));
  sm_old::powerOn(t.back());
  sm_cur::powerOn(t.back());
  check(difference(sm_old::save(false), sm_cur::save(false)).empty() && !sm_asleep && !sm_gate_open && g_weak_rail_cold_boot &&
        countLog("ATRVGATE weak-rail COLD BOOT (rail=10.00)") == 1, "a weak cold boot stays up", t);
  // Its cycles ask the car nothing (#104), even one the car would answer with a charge.
  until([&] { return sm_asleep; }, [&] {
    check(sm_now <= WEAK_RAIL_COLD_BOOT_AWAKE_MS + 10000, "the window lasts WEAK_RAIL_COLD_BOOT_AWAKE_MS", t);
    play(Advance(10000), t);
    play(sm_now < 30000 ? Charge(-50, 380) : Silent(), t);
  }, "the window's expiry");
  check(sm_polls == 0 && g_last_obd_answer_ms == 0 && !recently_charging,
        "no poll cycle of a weak cold boot's window reaches the car", t);
  check(sm_now > WEAK_RAIL_COLD_BOOT_AWAKE_MS && !sm_sleep_modem && g_next_wake_minutes == WAKE_PARKED_MINUTES &&
        countLog("WEAK-RAIL cold-boot BLE window expired") == 1, "then the next poll cycle sleeps, modem off", t);
  wakeAt(Wake(SM_TIMER, 10.0f));
  check(!sm_gate_open && sm_asleep && !g_weak_rail_cold_boot &&
        countLog("ATRVGATE skip -> deep sleep (WEAK RAIL, modem never powered)") == 1, "a weak timer wake skips", t);

  // The car switched on (a cycle's rail at RAIL_AWAKE_VOLTS) ends a weak cold boot, modem off and still unpolled, and
  // the next wake takes the normal path (#104). With no phone it ends at once, key-reset window or not.
  t.clear();
  t.push_back(Wake(SM_COLD, 10.0f));
  sm_old::powerOn(t.back());
  sm_cur::powerOn(t.back());
  play(Advance(10000), t);
  play(Charge(-50, 380, 0, RAIL_AWAKE_VOLTS), t);
  check(sm_asleep && sm_sleep_reason == "weak-rail-recovered" && !sm_sleep_modem && sm_polls == 0,
        "with no phone the car switched on ends a weak cold boot inside the key-reset window too", t);
  // A connected phone holds it while the key-reset window is open (sleeping would refuse a reset in progress for
  // good), and holds the window open past its length; after the key-reset window it cannot hold the recovery.
  t.clear();
  t.push_back(Wake(SM_COLD, 10.0f));
  sm_old::powerOn(t.back());
  sm_cur::powerOn(t.back());
  sm_old::sm_ble_connected = sm_cur::sm_ble_connected = true;
  play(Advance(10000), t);
  play(Charge(-50, 380, 0, RAIL_AWAKE_VOLTS), t);
  check(sm_now < AUTH_RESET_WINDOW_MS && !sm_asleep && g_weak_rail_cold_boot && sm_polls == 0 &&
        countLog("WEAK-RAIL cold boot ended") == 0,
        "a connected phone holds the recovery inside the key-reset window, still unpolled", t);
  play(Advance(WEAK_RAIL_COLD_BOOT_AWAKE_MS), t);
  play(Silent(12.6), t);
  check(!sm_asleep && g_weak_rail_cold_boot, "a connected phone holds the window open at a parked rail", t);
  play(Advance(10000), t);
  play(Charge(-50, 380, 0, RAIL_AWAKE_VOLTS), t);
  check(sm_asleep && !sm_sleep_modem && sm_sleep_reason == "weak-rail-recovered" &&
        sm_armed_minutes == WEAK_RAIL_RECOVERED_WAKE_MINUTES && sm_polls == 0 &&
        countLog("WEAK-RAIL cold boot ended: rail=13.30 (car awake)") == 1 &&
        countLog("WEAK-RAIL cold-boot BLE window expired") == 0,
        "the car switched on ends a phone-held weak cold boot, modem off, unpolled", t);
  wakeAt(Wake(SM_TIMER, 13.5f));
  check(sm_gate_open && !g_weak_rail_cold_boot && !sm_asleep, "and the next wake opens the gate as usual", t);

  // The counters and since_poll across wakes, and an enforced alarm flag suppressing a stationary poke.
  start(t);
  play(Poll(SM_NONE, 0), t);  // the poll that sets the clock; a cold boot zeroed it before the stamp
  check(polls_count == 1 && last_poll_epoch == 0, "a cold boot with no clock yet counts its poll but stamps none", t);
  wakeAt(Wake(SM_TIMER, 12.6f));
  check(skips_count == 1 && countLog("ATRVGATE atrv=13.50 rail=12.60 AWAKE skip since_poll=-1s skips=1 polls=1") == 1,
        "no stamp, no since_poll", t);
  wakeAt(Wake(SM_TIMER, 13.6f));
  const time_t stamped = last_poll_epoch;
  check(polls_count == 2 && std::labs((long)(rtc.getEpoch() - stamped)) <= 1, "a poke stamps the last poll", t);
  wakeAt(Wake(SM_TIMER, 12.6f));
  const std::string skip = "ATRVGATE atrv=13.50 rail=12.60 AWAKE skip since_poll=";
  long since = -1;
  for (const auto& line : sm_log)
    if (line.compare(0, skip.size(), skip) == 0) since = std::atol(line.c_str() + skip.size());
  check(since > 0 && std::labs(since - (long)(rtc.getEpoch() - stamped)) <= 1 &&
        countLog(skip + std::to_string(since) + "s skips=2 polls=2") == 1, "since_poll runs from the stamp", t);
  wakeAt(Alarmed(Wake(SM_TIMER, 13.6f)));
  check(!sm_gate_open && sm_asleep && countLog("ALARMGATE stationary poke suppressed (flag=1 rail=13.60)") == 1,
        "an enforced alarm flag suppresses a stationary poke", t);
  wakeAt(Alarmed(Wake(SM_MOTION, 13.6f)));
  check(sm_gate_open && !sm_asleep, "but not a motion wake", t);

  // The OBD-alarm permit (#83) on an enforced car woken by motion, rail awake: parked, nothing is asked and one DENY
  // line says so; a moving fix asks; a speed note at ALARM_SPEED_HOLD_KMH carries a tunnel for ALARM_SPEED_HOLD_MS
  // after the last one; a fix older than ALARM_GNSS_MAX_AGE_MS is no evidence.
  const SmEvent tunnel = Poll(SM_SPEED, ALARM_SPEED_HOLD_KMH), crawl = Poll(SM_SPEED, 10), parked = Poll(SM_NONE, 0);
  auto enforced = [&] { start(t); wakeAt(Alarmed(Wake(SM_MOTION, 13.6f))); };
  auto at = [&](unsigned long ms) {  // the clock to ms exactly, which a log line may already have passed
    check(sm_now <= ms, "the scenario's clock is past the boundary it means to hit", t);
    play(Advance(ms - sm_now), t);
  };
  enforced();
  play(parked, t);
  play(Advance(5000), t);
  play(parked, t);
  check(sm_tx == 0 && countLog("ALARMGATE DENY") == 1, "parked on an enforced car: nothing asked, one DENY line", t);
  play(Poll(SM_SPEED, 60, true, 60), t);
  check(sm_tx == SM_POLL_COMMANDS && countLog("ALARMGATE PERMIT") == 1, "a moving fix asks the car", t);
  for (int i = 0; i < 12; ++i) {
    play(Advance(5000), t);
    play(tunnel, t);
  }
  check(sm_tx == 13 * SM_POLL_COMMANDS, "a speed note at the hold threshold carries each poll through a tunnel", t);
  const unsigned long last_note = sm_now;
  for (bool held = true; held;) {  // ends: the clock moves on
    play(Advance(5000), t);
    held = sm_now - last_note < ALARM_SPEED_HOLD_MS;
    const unsigned long asked = sm_tx;
    play(crawl, t);
    check((sm_tx != asked) == held, "a slow speed neither extends the hold nor ends it", t);
  }
  const unsigned long asked = sm_tx;
  play(Aged(Poll(SM_NONE, 0, true, 60), ALARM_GNSS_MAX_AGE_MS + 1), t);
  check(sm_tx == asked && sm_log.back().compare(0, 14, "ALARMGATE DENY") == 0,
        "the hold expired, and a moving fix older than ALARM_GNSS_MAX_AGE_MS is denied", t);
  play(Aged(Poll(SM_NONE, 0, true, 60), ALARM_GNSS_MAX_AGE_MS), t);
  check(sm_tx == asked + SM_POLL_COMMANDS, "one exactly ALARM_GNSS_MAX_AGE_MS old is not", t);

  // A switch-off: the rail falls under RAIL_AWAKE_VOLTS. The next poll is denied at once; a dip shorter than
  // ALARM_RAIL_COLLAPSE_MS keeps the hold, a sustained one voids it, and a charger bringing the rail back up then gets
  // nothing.
  const SmEvent off = Rules(Poll(SM_NONE, 0), SM_CAR_OFF, SM_MOVE_NOW, false), on = Poll(SM_NONE, 0);
  for (bool sustained : {false, true}) {
    enforced();
    play(Poll(SM_SPEED, 60, true, 60), t);
    play(Advance(1000), t);
    play(off, t);
    at(sm_low_ms + ALARM_RAIL_COLLAPSE_MS - (sustained ? 0 : 1));
    play(off, t);
    check(sm_tx == SM_POLL_COMMANDS, "a switched-off car is denied at once", t);
    play(on, t);
    check(sm_tx == (sustained ? 1 : 2) * SM_POLL_COMMANDS && (g_alarm_speed_hold_until == 0) == sustained,
          sustained ? "a sustained collapse voids the hold: the rail back up asks nothing"
                    : "a dip shorter than ALARM_RAIL_COLLAPSE_MS keeps it", t);
  }

  // The setup permit: a completed full push from an authenticated phone asks a parked car for
  // ALARM_SETUP_PERMIT_MINUTES.
  enforced();
  play(Setup(), t);
  play(parked, t);
  check(sm_tx == SM_POLL_COMMANDS && countLog("ALARMGATE setup-session permit granted") == 1,
        "the wizard's push asks a parked enforced car", t);
  play(Leave(), t);
  at(sm_seen_ms + ALARM_PERMIT_BLE_GRACE_MS);
  play(parked, t);
  check(sm_tx == 2 * SM_POLL_COMMANDS, "the phone gone, for ALARM_PERMIT_BLE_GRACE_MS", t);
  at(sm_seen_ms + ALARM_PERMIT_BLE_GRACE_MS + 1);
  play(parked, t);
  check(sm_tx == 2 * SM_POLL_COMMANDS, "and not after", t);
  play(Setup(), t);
  at(sm_setup_ms - 1);
  play(parked, t);
  check(sm_tx == 3 * SM_POLL_COMMANDS, "a new push opens a new window", t);
  at(sm_setup_ms);
  play(parked, t);
  check(sm_tx == 3 * SM_POLL_COMMANDS, "until ALARM_SETUP_PERMIT_MINUTES", t);
}


// ---- the marker debt across a crash (#49), on CUR only ----
// These run CUR alone: what they check is absolute, not relative to SM_PIN (a crash never loses the debt or an
// unsent marker).
void playCur(const SmEvent& e, std::vector<SmEvent>& trace) {
  const sm_cur::Snap pre = sm_cur::save();
  trace.push_back(e);
  sm_cur::step(e);
  events++;
  if (const char* broken = sm_cur::propertyBroken(pre, e)) fail(std::string("CUR property: ") + broken, trace);
}
void startCur(std::vector<SmEvent>& trace) {
  trace.clear();
  const SmEvent boot = Wake(SM_COLD, 12.6f);
  trace.push_back(boot);
  sm_cur::powerOn(boot);
}
std::vector<SmEvent> crashAlphabet() {
  std::vector<SmEvent> a = ALPHABET;
  a.push_back(Crash());
  a.push_back(FlushCrash());
  a.push_back(Sent());
  return a;
}
unsigned long crash_leaves = 0;
void enumerateCur(int depth, const std::vector<SmEvent>& alphabet, std::vector<SmEvent>& trace) {
  if (depth == 0) { crash_leaves++; return; }
  const sm_cur::Snap node = sm_cur::save();
  for (const auto& e : alphabet) {
    sm_cur::restore(node);
    playCur(e, trace);
    enumerateCur(depth - 1, alphabet, trace);
    trace.pop_back();
  }
}

void crashes(int depth) {
  std::vector<SmEvent> t;
  using namespace sm_cur;
  // A panic mid-drive (#49): .rtc.data is reloaded and the clock zeroed, the debt comes back from the store,
  // and the first modem sleep once a poll has the clock again closes the trip.
  startCur(t);
  playCur(Poll(SM_SPEED, 25), t);
  playCur(Crash(), t);
  check(park_marker_owed && is_currently_parked && obd_result_vector.empty(), "a crash keeps the debt", t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(Sleep(true, false), t);
  check(park_marker_owed && sm_store_marker && obd_result_vector.empty(), "no clock, no marker: the debt waits", t);
  playCur(Wake(SM_MOTION, 12.6f), t);
  playCur(Poll(SM_NONE, 0, false, 0, false), t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(Sleep(), t);
  check(!park_marker_owed && obd_result_vector.size() == 1 && obd_result_vector[0].size() == 2 && sm_store_marker,
        "one marker queued; its crash copy stays until it is sent", t);
  // In the backlog, then a panic after the next wake loaded it: the marker is lost, the debt is not.
  playCur(Wake(SM_MOTION, 13.6f), t);  // a streaming wake: a sample poke would sleep after its poll
  playCur(Poll(SM_NONE, 0, false, 0, false), t);
  playCur(Crash(), t);
  check(park_marker_owed && obd_result_vector.empty(), "a crash after the backlog load keeps the debt", t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(Sent(), t);
  check(!park_marker_owed && obd_result_vector.empty() && !sm_store_marker, "a sent marker pays both copies", t);
  playCur(Wake(SM_TIMER, 13.6f), t);
  playCur(Crash(), t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(Sleep(), t);
  check(!park_marker_owed && obd_result_vector.empty(), "a crash after the trip was closed owes nothing", t);

  // A panic in the pre-sleep flush, after the marker block cleared the debt (#30's field crash site).
  startCur(t);
  playCur(Poll(SM_SPEED, 25), t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(FlushCrash(), t);
  check(park_marker_owed && obd_result_vector.empty(), "a crash in the flush keeps the debt it had just paid", t);
  playCur(Poll(SM_NONE, 0), t);
  playCur(Ladder(SmTo::STANDBY), t);
  playCur(Sleep(), t);
  check(!park_marker_owed && obd_result_vector.size() == 2 && obd_result_vector.back().size() == 2,
        "and the next modem sleep queues the marker again", t);

  const auto alphabet = crashAlphabet();
  startCur(t);
  enumerateCur(depth, alphabet, t);
  for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
    std::mt19937 rng(seed);
    for (int sequence = 0; sequence < 1000; ++sequence) {
      startCur(t);
      for (int i = 0; i < 30; ++i) {
        const int k = std::uniform_int_distribution<int>(0, 7)(rng);
        playCur(k == 0 ? Crash() : k == 1 ? FlushCrash() : k == 2 ? Sent() : randomEvent(rng), t);
      }
    }
  }
}


// ---- the driving rules (#6), on CUR only ----
// The differential only proves CUR equals SM_PIN; these are the absolute checks: the named cases of the issue,
// each asserting the state, what a frame publishes, the marker debt, the freezes and the LEASH lines.
int parkKey(const OBDVector& frame) {
  for (const auto& res : frame) if (res.first == "is_parked") return (int)res.second;
  return -1;
}
bool logged(const std::string& text) {
  for (const auto& line : sm_cur::sm_log) if (line == text) return true;
  return false;
}

void rules() {
  std::vector<SmEvent> t;
  using namespace sm_cur;
  auto begin = [&] { startCur(t); playCur(Advance(60000), t); };
  const SmEvent quiet = Poll(SM_NONE, 0);
  auto fix = [](double kmh = 6.0) { return Poll(SM_NONE, 0, true, kmh); };
  auto enterUnconfirmed = [&] {
    begin();
    playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true), t);
    check(is_currently_parked && drive_unconfirmed, "sustained motion on a car that is on is UNCONFIRMED", t);
  };

  // 1. Desk drift: fresh moving fixes with no accelerometer motion stay PARKED, car on or off.
  begin();
  for (int i = 0; i < 3; ++i) {
    playCur(Rules(fix(), SM_CAR_RAIL, SM_MOVE_NONE, false), t);
    playCur(Advance(5000), t);
  }
  check(is_currently_parked && !drive_unconfirmed && parkKey(obd_result_vector.back()) == 1 &&
        countLog("LEASH driving") == 0 && !park_marker_owed, "drift without motion stays PARKED and says PARK", t);
  check(countLog("LEASH gnss=") == 1 && logged("LEASH gnss=6.0km/h refused reason=motion"),
        "one refusal line for the whole streak", t);
  begin();
  playCur(Rules(fix(), SM_CAR_OFF, SM_MOVE_NOW, false), t);
  check(is_currently_parked && countLog("LEASH driving") == 0 && logged("LEASH gnss=6.0km/h refused reason=car-off"),
        "a moving fix on a car that is off is refused", t);

  // 2. Late-fix drive: sustained motion, no speed, no power -> UNCONFIRMED; a moving fix confirms DRIVING.
  enterUnconfirmed();
  check(parkKey(obd_result_vector.back()) == -1 && obd_result_vector.size() == 1 && !park_marker_owed && sm_published == -1,
        "UNCONFIRMED frames carry no is_parked and owe no marker", t);
  check(logged("LEASH unconfirmed accel act=100% rail=13.50 obd=1 -> live location, no drivingState"),
        "the UNCONFIRMED line", t);
  playCur(Advance(MAX_AWAKE_MS), t);
  playCur(Ladder(SmTo::ACTIVE), t);
  check(sm_asleep && sm_sleep_reason == "backstop", "the max-awake backstop still applies to UNCONFIRMED", t);
  enterUnconfirmed();
  playCur(Advance(5000), t);
  playCur(Rules(fix(40.0), SM_CAR_RAIL, SM_MOVE_NOW, false), t);
  check(!is_currently_parked && !drive_unconfirmed && parkKey(obd_result_vector.back()) == 0 && park_marker_owed &&
        logged("LEASH driving gnss=40.0km/h -> unlatch parked (no OBD speed needed)"),
        "a moving fix with motion confirms DRIVING from UNCONFIRMED: frames say DRIVE, the marker is owed", t);
  begin();
  SmEvent rail_only = Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true);
  rail_only.silent = true;
  playCur(rail_only, t);
  check(drive_unconfirmed && logged("LEASH unconfirmed accel act=100% rail=13.50 obd=0 -> live location, no drivingState"),
        "the rail alone counts as the car being on", t);
  begin();
  playCur(Rules(quiet, SM_CAR_OBD, SM_MOVE_NOW, true), t);
  check(drive_unconfirmed && logged("LEASH unconfirmed accel act=100% rail=12.00 obd=1 -> live location, no drivingState"),
        "an OBD answer within ACTIVE_TIMER counts as the car being on", t);
  begin();
  playCur(Rules(quiet, SM_CAR_OFF, SM_MOVE_NOW, true, 12.5), t);
  check(is_currently_parked && !drive_unconfirmed && countLog("LEASH") == 0, "sustained motion on a car that is off does nothing", t);

  // 3. Discharge confirms DRIVING without a fix: the power key, else current x voltage; neither cannot confirm.
  begin();
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 12.5), t);
  check(!is_currently_parked && !drive_unconfirmed && parkKey(obd_result_vector.back()) == 0 && park_marker_owed &&
        logged("LEASH driving accel act=100% power=12.5kW -> unlatch parked"), "power > 4 kW is DRIVING", t);
  begin();
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 4.0), t);
  check(drive_unconfirmed, "exactly 4.0 kW is not above the limit", t);
  begin();
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, -20.0), t);
  check(drive_unconfirmed, "charging power is not a discharge", t);
  begin();
  SmEvent iv = Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true);
  iv.current = 30, iv.voltage = 400;
  playCur(iv, t);
  check(!is_currently_parked && logged("LEASH driving accel act=100% power=12.0kW -> unlatch parked"),
        "current x voltage over 4 kW is DRIVING when the frame has no power key", t);
  begin();
  iv.current = 30, iv.voltage = NAN;
  playCur(iv, t);
  check(drive_unconfirmed, "a current with no voltage cannot confirm", t);
  enterUnconfirmed();
  playCur(Advance(5000), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 10.0), t);
  check(!is_currently_parked && !drive_unconfirmed && park_marker_owed, "UNCONFIRMED + discharge + sustained motion is DRIVING", t);

  // A drive confirmed by accel + discharge alone (no speed PID, no fix) is held by sustained motion while it
  // lasts, whatever the power: cruising at 2.5 kW must not re-latch it.
  begin();
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 12.5), t);
  for (int i = 0; i < 11; ++i) {
    playCur(Advance(60000), t);
    playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 2.5), t);
  }
  check(!is_currently_parked && sm_freezes == 0 && countLog("LEASH no speed") == 0 && countLog("LEASH driving") == 1,
        "sustained motion holds DRIVING past ACTIVE_TIMER at 2.5 kW: no flap, no freeze, one LEASH line", t);
  playCur(Advance(AT), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_KEEP, false), t);
  check(!is_currently_parked, "the hold ends ACTIVE_TIMER after the last sustained-motion poll", t);
  playCur(Advance(1), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_KEEP, false), t);
  check(is_currently_parked && sm_freezes == 1 && countLog("LEASH no speed for 300s") == 1,
        "then the normal re-latch fires, with one freeze", t);

  // A speed-less drive confirmed by GNSS + motion keeps going on fresh moving fixes alone: no new gate on continuation.
  begin();
  playCur(Rules(fix(40.0), SM_CAR_RAIL, SM_MOVE_NOW, false), t);
  check(!is_currently_parked, "GNSS with motion starts the drive", t);
  for (int i = 0; i < 11; ++i) {
    playCur(Advance(60000), t);
    playCur(Rules(fix(40.0), SM_CAR_RAIL, SM_MOVE_NONE, false), t);
  }
  check(!is_currently_parked && sm_freezes == 0 && countLog("LEASH gnss=") == 0 && countLog("LEASH no speed") == 0,
        "fresh moving fixes without motion keep a drive going past ACTIVE_TIMER, never refused", t);

  // 4. Charge block: the charge latch keeps sustained motion and GNSS out, and drops UNCONFIRMED to PARKED.
  begin();
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, true, 12.5, true), t);
  check(is_currently_parked && !drive_unconfirmed && countLog("LEASH") == 0, "sustained motion while charging stays PARKED", t);
  playCur(Rules(fix(), SM_CAR_RAIL, SM_MOVE_NOW, false, NAN, true), t);
  check(is_currently_parked && logged("LEASH gnss=6.0km/h refused reason=charging"), "a moving fix while charging is refused", t);
  enterUnconfirmed();
  playCur(Advance(5000), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_KEEP, false, NAN, true), t);
  check(is_currently_parked && !drive_unconfirmed && sm_freezes == 0 && !park_marker_owed &&
        logged("LEASH unconfirmed charging -> parked"), "the charge latch drops UNCONFIRMED to PARKED, no freeze", t);

  // 5. UNCONFIRMED exit: ACTIVE_TIMER without motion, each motion refreshing it; the idle stage; never a freeze.
  enterUnconfirmed();
  playCur(Advance(AT + 1), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_NOW, false), t);
  check(drive_unconfirmed, "a motion hit near the end of the timer keeps UNCONFIRMED", t);
  playCur(Advance(AT), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_KEEP, false), t);
  check(drive_unconfirmed, "ACTIVE_TIMER after the last motion is still UNCONFIRMED", t);
  playCur(Advance(1), t);
  playCur(Rules(quiet, SM_CAR_RAIL, SM_MOVE_KEEP, false), t);
  check(is_currently_parked && !drive_unconfirmed && sm_freezes == 0 && !park_marker_owed &&
        logged("LEASH unconfirmed no motion for 300s -> parked") && parkKey(obd_result_vector.back()) == 1,
        "one ms past ACTIVE_TIMER without motion: PARKED, no PARK marker, no refreeze", t);
  enterUnconfirmed();
  playCur(Ladder(SmTo::CHARGING), t);
  check(is_currently_parked && !drive_unconfirmed && sm_freezes == 0 && !park_marker_owed &&
        logged("LEASH unconfirmed idle -> parked"),
        "the idle stage drops UNCONFIRMED to PARKED without a freeze", t);
  playCur(Advance(AT), t);
  playCur(Sleep(), t);
  check(obd_result_vector.size() == 1, "a parked heartbeat after UNCONFIRMED queues no marker", t);
}

void playComposed(SmEvent e, std::vector<SmEvent>& trace) {
  using namespace sm_cur;
  const Snap pre = save(false);
  const size_t frames = obd_result_vector.size();
  e.charge_block = pre.recently_charging;
  playCur(e, trace);
  if (e.k != SmKind::POLL || pre.sm_asleep) return;
  const bool speed = sm_polls != pre.sm_polls && !e.silent &&
    ((e.key != SM_NONE && e.speed > 0) || (e.key == SM_BOTH && e.speed2 > 0));
  if (pre.is_currently_parked && pre.recently_charging && !speed)
    check(is_currently_parked && !drive_unconfirmed,
          "composition: the previous charge latch blocks non-speed starts, including UNCONFIRMED", trace);
  if (speed)
    check(!is_currently_parked && !drive_unconfirmed && sm_published == 0,
          "composition: an answered positive speed PID wins over the charge latch", trace);
  if (!pre.recently_charging && (sm_polls == pre.sm_polls || e.silent))
    check(!recently_charging, "composition: a denied or unanswered poll cannot establish a charge", trace);
  if (!is_currently_parked && obd_result_vector.size() > frames)
    for (const auto& kv : obd_result_vector.back())
      if (kv.first == "is_charging")
        check(kv.second == 0, "composition: a driving frame never reports charging (regen)", trace);
}

void composition() {
  using namespace sm_cur;
  std::vector<SmEvent> t;
  auto begin = [&] { startCur(t); playComposed(Advance(CONFIRM_MS), t); };
  auto has = [&](const char* key, double value) {
    if (obd_result_vector.empty()) return false;
    for (const auto& kv : obd_result_vector.back()) if (kv.first == key) return kv.second == value;
    return false;
  };
  const SmEvent charge = Charge(-50, 380, 0.5, 13.5);
  auto charged = [&] {
    begin();
    playComposed(charge, t);
    check(recently_charging && has("is_charging", 1) && g_next_wake_minutes == WAKE_CHARGING_MINUTES,
          "composition: trusted AC evidence establishes the real latch", t);
  };
  SmEvent gnss = Poll(SM_NONE, 0, true, 20);
  gnss.current = 30;
  gnss.voltage = 400;
  SmEvent accel = Rules(Poll(SM_NONE, 0), SM_CAR_RAIL, SM_MOVE_NOW, true, 12);
  accel.current = 30;
  accel.voltage = 400;

  for (bool use_accel : {false, true}) {
    charged();
    SmEvent move = use_accel ? accel : gnss;
    move.key = SM_SPEED;
    move.speed = NAN;
    for (unsigned i = 1; i <= CHARGE_RELEASE_POLLS; ++i) {
      playComposed(move, t);
      check(is_currently_parked && !drive_unconfirmed && sm_published == 1 && !park_marker_owed &&
            recently_charging == (i < CHARGE_RELEASE_POLLS),
            "composition: release is visible to energy now, but to drive only next poll", t);
    }
    playComposed(move, t);
    check(!is_currently_parked && sm_published == 0 && park_marker_owed && has("is_charging", 0),
          "composition: released charge permits the next GNSS/accel start", t);
  }
  for (int key : {SM_SPEED, SM_VRS}) {
    charged();
    SmEvent speed = Poll(key, 25);
    speed.current = -50;
    speed.voltage = 380;
    playComposed(speed, t);
    check(!is_currently_parked && recently_charging && charge_release_polls == 1 &&
          has("is_charging", 0) && park_marker_owed,
          "composition: speed wins immediately, energy sees DRIVE and starts its release", t);
    const auto hold = g_last_speed_ms;
    playComposed(Advance(AT + 1), t);
    playComposed(gnss, t);
    check(!is_currently_parked && g_last_speed_ms > hold && sm_freezes == 0 && sm_published == 0,
          "composition: the residual charge latch cannot block GNSS continuing an established drive", t);
  }

  startCur(t);
  playComposed(charge, t);
  check(!recently_charging && !has("is_charging", 1), "composition: an untrusted stream cannot latch charge", t);
  begin();
  playComposed(Poll(SM_SPEED, 25), t);
  playComposed(charge, t);
  check(!is_currently_parked && !recently_charging && has("is_charging", 0),
        "composition: negative current while driving is regen", t);
  playComposed(Advance(AT + 1), t);
  playComposed(charge, t);
  check(is_currently_parked && recently_charging && has("is_charging", 1) && sm_freezes == 1 &&
        sm_published == 1 && g_next_wake_minutes == WAKE_CHARGING_MINUTES,
        "composition: energy sees this poll's re-latch to PARK", t);

  begin();
  SmEvent uncertain = Rules(charge, SM_CAR_RAIL, SM_MOVE_NOW, true);
  playComposed(uncertain, t);
  check(drive_unconfirmed && recently_charging && sm_published == -1 && has("is_charging", 1) &&
        !park_marker_owed && sm_freezes == 0,
        "composition: energy can latch charge while drive is still UNCONFIRMED", t);
  playComposed(uncertain, t);
  check(is_currently_parked && !drive_unconfirmed && recently_charging && sm_published == 1 &&
        !park_marker_owed && sm_freezes == 0 && logged("LEASH unconfirmed charging -> parked"),
        "composition: that charge clears UNCONFIRMED on the next poll without a marker or freeze", t);

  charged();
  SmEvent silent = Silent(13.5);
  silent.gnss = true;
  silent.gnss_kmh = 20;
  silent.motion = SM_MOVE_NOW;
  for (unsigned i = 0; i < CHARGE_SILENT_HOLD_POLLS; ++i) {
    playComposed(silent, t);
    check(recently_charging && charge_release_polls == 0 && is_currently_parked,
          "composition: silent hold retains the real charge block", t);
  }
  for (unsigned i = 1; i <= CHARGE_RELEASE_POLLS; ++i) {
    playComposed(silent, t);
    check(is_currently_parked && recently_charging == (i < CHARGE_RELEASE_POLLS),
          "composition: silence past the hold releases only after hysteresis", t);
  }
  playComposed(silent, t);
  check(!is_currently_parked && !park_marker_owed,
        "composition: GNSS may drive after the silent release, but no buffered frame means no marker debt", t);
  playComposed(gnss, t);
  check(!is_currently_parked && park_marker_owed && has("is_parked", 0),
        "composition: the first buffered driving frame owes the marker", t);

  charged();
  playComposed(Poll(SM_SPEED, 25), t);
  check(!is_currently_parked && recently_charging, "composition: drive and release-hold coexist before sleep", t);
  playComposed(Sleep(), t);
  const auto retained = save(false);
  playComposed(Wake(SM_MOTION, 13.6f), t);
  check(!sm_asleep && sm_gate_open && !g_sample_poke && is_currently_parked && !drive_unconfirmed &&
        recently_charging && charge_release_polls == retained.charge_release_polls &&
        charging_fast_minutes == retained.charging_fast_minutes,
        "composition: deep sleep resets drive RAM and preserves energy RTC through the wake gate", t);
  const unsigned polls = sm_polls;
  playComposed(Advance(CONFIRM_MS), t);
  SmEvent moving_charge = charge;
  moving_charge.gnss = true;
  moving_charge.gnss_kmh = 20;
  moving_charge.motion = SM_MOVE_NOW;
  playComposed(moving_charge, t);
  check(sm_polls == polls + 1 && is_currently_parked && recently_charging && sm_published == 1,
        "composition: the retained charge blocks an actual post-wake GNSS poll", t);

  const std::vector<SmEvent> alphabet = {
    charge, gnss, accel, uncertain, Poll(SM_SPEED, 25), silent, Advance(AT + 1), Sleep(),
    Wake(SM_MOTION, 13.6f), Wake(SM_TIMER, 12.6f), Ladder(SmTo::CHARGING), Hold(false),
    Alarmed(Wake(SM_MOTION, 13.6f)), Setup(), Leave()
  };
  unsigned blocked = 0, speed_override = 0, parked_charge = 0;
  for (uint32_t seed : {83u, 831u, 832u, 833u, 834u}) {
    std::mt19937 rng(seed);
    for (unsigned sequence = 0; sequence < 100; ++sequence) {
      charged();
      for (unsigned i = 0; i < 40; ++i) {
        const SmEvent& e = alphabet[std::uniform_int_distribution<size_t>(0, alphabet.size() - 1)(rng)];
        if (e.k == SmKind::POLL) playComposed(Advance(20000), t);
        const auto pre = save(false);
        const size_t frames = obd_result_vector.size();
        playComposed(e, t);
        if (e.k != SmKind::POLL || pre.sm_asleep) continue;
        blocked += sm_polls != pre.sm_polls && !e.silent &&
                   pre.is_currently_parked && pre.recently_charging && is_currently_parked &&
                   (e.gnss || e.sustained);
        speed_override += pre.recently_charging && e.key == SM_SPEED && sm_polls != pre.sm_polls;
        parked_charge += !pre.is_currently_parked && is_currently_parked &&
                         obd_result_vector.size() > frames && has("is_charging", 1);
      }
    }
  }
  check(blocked && speed_override && parked_charge, "composition: generated traces reach both feedback directions", t);
}

int main(int argc, char** argv) {
  std::string group = "all";
  int depth = 5;  // caught every harness mutant that depth 6 did, in ~3 s; --depth 6 runs the 11 M sequences
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--depth" && i + 1 < argc) depth = std::atoi(argv[++i]);
    else group = arg;
  }
  if (group != "all" && group != "enumerate" && group != "random" && group != "scenarios" && group != "crash" &&
      group != "rules" && group != "composition") {
    std::cerr << "unknown group " << group << " (all, enumerate, random, scenarios, crash, rules, composition)\n";
    return 2;
  }
  using clock = std::chrono::steady_clock;
  if (group == "all" || group == "scenarios") {
    scenarios();
    std::cout << "PASS: scenarios\n";
  }
  if (group == "all" || group == "random") {
    randomGroup();
    std::cout << "PASS: random (5 seeds x 1000 sequences x 30 events, and 3 x 500 from near the cap, near stale and latched near stale, and 500 on an alarm-enforced car)\n";
  }
  if (group == "all" || group == "enumerate") {
    const auto t0 = clock::now();
    std::vector<SmEvent> trace;
    start(trace);
    enumerate(depth, trace, ALPHABET);
    const unsigned long drive_leaves = leaves;
    start(trace);
    enumerate(depth, trace, ENERGY_ALPHABET);
    const unsigned long energy_leaves = leaves - drive_leaves;
    start(trace);
    enumerate(depth, trace, WAKE_ALPHABET);
    const unsigned long wake_leaves = leaves - drive_leaves - energy_leaves;
    for (int kind : {SEED_NEAR_CAP, SEED_NEAR_STALE, SEED_LATCHED_STALE}) {
      startSeeded(trace, kind);
      enumerate(depth - 1, trace, SEEDED_ALPHABET);
    }
    const double s = std::chrono::duration<double>(clock::now() - t0).count();
    std::cout << "PASS: enumerate depth " << depth << " over " << ALPHABET.size() << " drive, " << ENERGY_ALPHABET.size()
              << " energy and " << WAKE_ALPHABET.size() << " wake symbols, " << drive_leaves << " + " << energy_leaves
              << " + " << wake_leaves << " sequences, and the energy symbols at depth " << depth - 1
              << " with an odd advance, from near the cap, near stale and latched near stale (" << leaves - drive_leaves - energy_leaves - wake_leaves
              << "), in " << s << "s\n";
  }
  if (group == "all" || group == "crash") {
    crashes(depth);
    std::cout << "PASS: crash (CUR only: scenarios, depth " << depth << " over " << crashAlphabet().size() << " symbols, "
              << crash_leaves << " sequences, 5 seeds x 1000 x 30)\n";
  }
  if (group == "all" || group == "rules") {
    rules();
    std::cout << "PASS: rules (CUR only: the #6 driving-rule scenarios)\n";
  }
  if (group == "all" || group == "composition") {
    composition();
    std::cout << "PASS: composition (CUR only: real drive/energy feedback, named transitions and 5 seeds x 100 x 40)\n";
  }
  std::cout << "events compared: " << events << '\n';
}
