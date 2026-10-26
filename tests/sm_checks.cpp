// Differential checks for the park/drive decision; built and run by run_sm_checks.py, which writes the
// old_side.inc / cur_side.inc / field.inc files from the pinned commit and the working tree.
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

enum class SmKind { POLL, ADVANCE, LADDER, HOLD, SLEEP, WAKE, MAINT, CRASH };
enum class SmTo { ACTIVE, BLE, CHARGING, STANDBY };
enum { SM_NONE, SM_SPEED, SM_VRS, SM_BOTH };  // which speed keys the poll carries (BOTH: speed, then vrs)
enum { SM_TIMER, SM_MOTION, SM_COLD };   // wake cause

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
  bool dump = false;  // HOLD: the log dump rather than an OTA update
  bool on = false;    // MAINT: the console's maintenance_mode
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

template <class A, class B> bool same(const A& a, const B& b) {
  if constexpr (std::is_enum<A>::value) return (int)a == (int)b;
  else return a == b;
}
template <class T> std::string text(const T& v) {
  if constexpr (std::is_enum<T>::value) return std::to_string((int)v);
  else if constexpr (std::is_same<T, std::string>::value) return '"' + v + '"';
  else { std::ostringstream s; s.precision(17); s << v; return s.str(); }
}
std::string firstLineDiff(const char* what, const std::vector<std::string>& a, const std::vector<std::string>& b) {
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const std::string x = i < a.size() ? a[i] : "<none>", y = i < b.size() ? b[i] : "<none>";
    if (x != y) return std::string(what) + "[" + std::to_string(i) + "]: OLD " + x + " | CUR " + y;
  }
  return {};
}

// The first output where OLD and CUR differ, or empty.
std::string difference(const sm_old::Snap& a, const sm_cur::Snap& b) {
#define SM_CMP(T, name, value) if (!same(a.name, b.name)) return #name ": OLD " + text(a.name) + " | CUR " + text(b.name);
  SM_RAM(SM_CMP) SM_RTC(SM_CMP) SM_STUB(SM_CMP)
#undef SM_CMP
  if (a.gps_speed != b.gps_speed) return "gps speed";
  auto value = [](double x, double y) { return x == y || (std::isnan(x) && std::isnan(y)); };
  bool frames = a.frames.size() == b.frames.size();
  for (size_t i = 0; frames && i < a.frames.size(); ++i) {
    frames = a.frames[i].size() == b.frames[i].size();
    for (size_t j = 0; frames && j < a.frames[i].size(); ++j)
      frames = a.frames[i][j].first == b.frames[i][j].first && value(a.frames[i][j].second, b.frames[i][j].second);
  }
  if (!frames) return "buffered frames: OLD " + std::to_string(a.frames.size()) + " | CUR " +
                                   std::to_string(b.frames.size());
  if (a.log != b.log) return firstLineDiff("logEvent", a.log, b.log);
  if (a.serial != b.serial) return firstLineDiff("Serial", a.serial, b.serial);
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
  const std::string diff = difference(sm_old::save(), sm_cur::save());
  if (!diff.empty()) fail("OLD and CUR differ at " + diff, trace);
}
void play(const SmEvent& e, std::vector<SmEvent>& trace) { play(e, trace, sm_old::save(), sm_cur::save()); }

void start(std::vector<SmEvent>& trace) {
  trace.clear();
  const SmEvent boot = Wake(SM_COLD, 12.6f);
  trace.push_back(boot);
  sm_old::powerOn(boot);
  sm_cur::powerOn(boot);
  const std::string diff = difference(sm_old::save(), sm_cur::save());
  if (!diff.empty()) fail("OLD and CUR differ at power-on: " + diff, trace);
}

const unsigned long AT = (unsigned long)(ACTIVE_TIMER);
const unsigned long MAX_AWAKE_MS = (unsigned long)MAX_AWAKE_MINUTES * 60UL * 1000UL;

// The exhaustive alphabet. Fractional speeds pin the %.1f text of the LEASH lines.
const std::vector<SmEvent> ALPHABET = {
  Poll(SM_NONE, 0), Poll(SM_SPEED, 0), Poll(SM_SPEED, 12.35), Poll(SM_VRS, 0.15), Poll(SM_NONE, 0, true, 7.25),
  Poll(SM_SPEED, 30.05, true, 88.85), Advance(AT), Advance(AT + 1), Advance(MAX_AWAKE_MS),
  Ladder(SmTo::CHARGING), Ladder(SmTo::STANDBY), Hold(), Sleep(), Wake(SM_MOTION, 12.6f), Wake(SM_COLD, 9.5f),
};

unsigned long leaves = 0;
void enumerate(int depth, std::vector<SmEvent>& trace) {
  if (depth == 0) { leaves++; return; }
  const sm_old::Snap old_node = sm_old::save();
  const sm_cur::Snap cur_node = sm_cur::save();
  for (const auto& e : ALPHABET) {
    sm_old::restore(old_node);
    sm_cur::restore(cur_node);
    play(e, trace, old_node, cur_node);
    enumerate(depth - 1, trace);
    trace.pop_back();
  }
}

SmEvent randomEvent(std::mt19937& rng) {
  auto pick = [&](std::initializer_list<double> values) {
    return *(values.begin() + std::uniform_int_distribution<size_t>(0, values.size() - 1)(rng));
  };
  switch (std::uniform_int_distribution<int>(0, 9)(rng)) {
  case 0: case 1: case 2: {
    const int key = (int)pick({SM_NONE, SM_SPEED, SM_SPEED, SM_VRS, SM_BOTH});
    const double speeds[] = {0, 0.05, 0.15, 0.25, 0.95, 12.35, 129.95, -1, 1e-9, NAN};
    auto speed = [&] { return speeds[std::uniform_int_distribution<int>(0, 9)(rng)]; };
    const double first = speed(), second = speed();
    const bool gnss = pick({0, 1}) != 0;
    return Poll(key, first, gnss, pick({5.0, 7.25, 88.85}), pick({0, 1, 1, 1}) != 0, second);
  }
  case 3: case 4:
    return Advance((unsigned long)pick({1, 4999, 5000, (double)(AT - 1), (double)AT, (double)(AT + 1),
                                        (double)GPS_MOVING_MAX_AGE_MS, (double)MAX_AWAKE_MS,
                                        (double)std::uniform_int_distribution<unsigned long>(0, 400000)(rng)}));
  case 5: return Ladder((SmTo)std::uniform_int_distribution<int>(0, 3)(rng));
  case 6: return pick({0, 1}) != 0 ? Hold(pick({0, 1}) != 0) : Maint(pick({0, 1}) != 0);
  case 7: return Sleep(pick({0, 1, 1}) != 0, pick({0, 1, 1}) != 0);
  default: {
    const int cause = (int)pick({SM_TIMER, SM_MOTION, SM_COLD});
    const float rail = (float)pick({5.9, 7.99, 8.0, 9.5, 10.99, 11.0, 12.6, 13.0, 13.29, 13.3, 14.8});
    return Wake(cause, rail, pick({0, 1, 1}) != 0, pick({0, 0, 1}) != 0);
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
}

struct Sample { unsigned long ms; double current; };
#include "field.inc"  // DRIVE_0930, DRIVE_1012 from energy_checks.cpp

// ---- named field scenarios: equality throughout, plus what each one is about, asserted on CUR ----

size_t countLog(const char* prefix) {
  size_t n = 0;
  for (const auto& line : sm_cur::sm_log) n += line.compare(0, strlen(prefix), prefix) == 0;
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
}


// ---- the marker debt across a crash (#49), on CUR only ----
// The setup() restore is a drive writer SM_PIN does not have, so a crash that owes the marker is meant to differ
// from OLD: these run CUR alone, with its properties (a crash never loses the debt or an unsent marker).
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
  playCur(Wake(SM_TIMER, 13.6f), t);
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

int main(int argc, char** argv) {
  std::string group = "all";
  int depth = 5;  // caught every harness mutant that depth 6 did, in ~3 s; --depth 6 runs the 11 M sequences
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--depth" && i + 1 < argc) depth = std::atoi(argv[++i]);
    else group = arg;
  }
  if (group != "all" && group != "enumerate" && group != "random" && group != "scenarios" && group != "crash") {
    std::cerr << "unknown group " << group << " (all, enumerate, random, scenarios, crash)\n";
    return 2;
  }
  using clock = std::chrono::steady_clock;
  if (group == "all" || group == "scenarios") {
    scenarios();
    std::cout << "PASS: scenarios\n";
  }
  if (group == "all" || group == "random") {
    randomGroup();
    std::cout << "PASS: random (5 seeds x 1000 sequences x 30 events)\n";
  }
  if (group == "all" || group == "enumerate") {
    const auto t0 = clock::now();
    std::vector<SmEvent> trace;
    start(trace);
    enumerate(depth, trace);
    const double s = std::chrono::duration<double>(clock::now() - t0).count();
    std::cout << "PASS: enumerate depth " << depth << " over " << ALPHABET.size() << " symbols, " << leaves
              << " sequences in " << s << "s\n";
  }
  if (group == "all" || group == "crash") {
    crashes(depth);
    std::cout << "PASS: crash (CUR only: scenarios, depth " << depth << " over " << crashAlphabet().size() << " symbols, "
              << crash_leaves << " sequences, 5 seeds x 1000 x 30)\n";
  }
  std::cout << "events compared: " << events << '\n';
}
