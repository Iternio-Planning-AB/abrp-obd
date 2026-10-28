#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <sys/time.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "json.hpp"
#include "../baseLogger/config.h"

using json = nlohmann::json;
#include "../libraries/OBDWrapper/obd_vector.h"

void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct RTCStub {
    uint32_t epoch = 1791112000;
    unsigned reads = 0;
    uint32_t getEpoch() { ++reads; return epoch; }
};

#define RTC_DATA_ATTR
RTCStub rtc;
#include "clock_quality.inc"
#include "upload_snapshot.inc"
#include "telemetry_format.h"

namespace target32 {
using time_t = int32_t;
RTCStub rtc;
#include "clock_quality.inc"
#include "upload_snapshot.inc"
struct tm* gmtime_r(const time_t* value, struct tm* result) {
    const ::time_t host_value = *value;
    return ::gmtime_r(&host_value, result);
}
#include "target_formatter.inc"
static_assert(sizeof(last_clock_sync_epoch) == 4, "retained target sync metadata is still signed32");
static_assert(std::is_same<decltype(last_clock_sync_epoch), int32_t>::value, "retained target sync ABI unchanged");
}

void targetFormatter() {
    const double supported = INT32_MAX;
    const OBDVector source = {{"utc", supported}, {"soc", 75}, {"lat", 59.3}, {"lon", 18.1}, {"heading", 90},
        {"__geo_loc_utc", supported}, {"__geo_head_utc", supported}};
    const auto eligible = json::parse(target32::bulkStringV2({source}, supported))[0];
    check(eligible.contains("location") && eligible.contains("heading") &&
        eligible["location"]["time"] == "2038-01-19T03:14:07Z" && eligible["soc"]["frac"] == 0.75,
        "target32 INT32_MAX inclusive formatter keeps representable geo and original battery");
    auto battery = eligible;
    battery.erase("location"); battery.erase("heading");
    for (double unsupported : {double(INT32_MAX) + 1, double(UINT32_MAX)}) {
        check(json::parse(target32::bulkStringV2({source}, unsupported))[0] == battery,
            "target32 unrepresentable send UTC must omit geo before native calendar cast: " + std::to_string(unsupported));
    }
    check(target32::geoValidUtc(double(INT32_MAX) + 1, UINT32_MAX),
        "target32 historical numeric provenance range remains uint32 outside wire eligibility");
    check(!target32::geoEligibleAtSend(double(INT32_MAX) + 1, UINT32_MAX, INT32_MAX) &&
        !target32::geoEligibleAtSend(UINT32_MAX, UINT32_MAX, INT32_MAX),
        "target32 future observation UTC above native calendar range must not reach narrowing calendar cast");
    check(target32::geoEligibleAtSend(INT32_MAX, INT32_MAX, double(INT32_MAX) - 1),
        "representable future-relative observation retains existing corrected-clock policy");
}

template<typename Epoch> bool acceptsFullRtcEpoch(bool (*)(Epoch)) {
    return uint64_t(std::numeric_limits<Epoch>::max()) >= UINT32_MAX;
}

void targetClock() {
    check(acceptsFullRtcEpoch(target32::clockValid) && acceptsFullRtcEpoch(target32::clockStale) &&
        acceptsFullRtcEpoch(target32::clockNeedsSync),
        "target32 extracted public predicates must accept raw uint32 RTC without narrowing before validation");
    for (uint32_t epoch : {uint32_t(1000000001), uint32_t(1791112000), uint32_t(INT32_MAX),
            uint32_t(INT32_MAX) + 1, uint32_t(UINT32_MAX)}) {
        target32::rtc.epoch = epoch;
        target32::last_clock_sync_epoch = epoch <= INT32_MAX ? int32_t(epoch) : INT32_MAX;
        const bool supported = epoch <= INT32_MAX;
        check(target32::clockValid(uint64_t(epoch)) == supported && target32::clockValid() == supported,
            "target32 explicit and noarg clock validity reject unsupported raw uint32 epochs");
        check(target32::clockNeedsSync(uint64_t(epoch)) == !supported && target32::clockNeedsSync() == !supported,
            "target32 explicit and noarg acquisition predicates request sync for unsupported clocks");
        check(target32::clockStale(uint64_t(epoch)) == !supported && target32::clockStale() == !supported,
            "target32 explicit and noarg stale predicate fails closed on unsupported raw clocks");
        target32::rtc.reads = 0;
        check(target32::uploadSendUtc() == (supported ? double(epoch) : 0) && target32::rtc.reads == 1,
            "extracted target32 sender snapshot rejects unsupported clock with one raw RTC read");
        check(target32::last_clock_sync_epoch == (supported ? int32_t(epoch) : INT32_MAX),
            "target32 policy never changes retained signed32 sync metadata");
    }
    target32::rtc.epoch = INT32_MAX;
    for (int age : {0, 900, 901}) {
        target32::last_clock_sync_epoch = INT32_MAX - age;
        check(target32::uploadSendUtc() == (age <= 900 ? double(INT32_MAX) : 0),
            "target32 representable maximum respects inclusive 900-second sync freshness");
    }
    for (int32_t sync : {int32_t(0), int32_t(-1), std::numeric_limits<int32_t>::min(), int32_t(1000000000)}) {
        target32::last_clock_sync_epoch = sync;
        check(target32::clockNeedsSync() && target32::uploadSendUtc() == 0,
            "target32 invalid retained signed metadata cannot restore geo trust");
    }
}

void hostRange() {
    const uint64_t supported = std::numeric_limits<::time_t>::max() < int64_t(UINT32_MAX) ?
        uint64_t(std::numeric_limits<::time_t>::max()) : uint64_t(UINT32_MAX);
    rtc.epoch = uint32_t(supported); last_clock_sync_epoch = ::time_t(supported);
    check(clockValid() && uploadSendUtc() == double(supported), "host native calendar range stays valid through its uint32-capped maximum");
    check(geoEligibleAtSend(double(supported), double(supported), double(supported)), "host formatter uses its actual native calendar range");
}

// The real wake/sync path (clockCompensateSleep, setClock, clockNoteRealSync, clockResync, NVS) against a fake
// clock: the RTC holds the dongle's belief, real_us the truth. Sleep runs `ppm` fast; awake time is
// crystal-accurate. millis() restarts on every wake, as it does on the dongle.
namespace live {
int64_t now_us = 0, real_us = 0;
unsigned long fake_ms = 0;
unsigned long millis() { return fake_ms; }
struct RTCFake {
    uint32_t getEpoch() { return uint32_t(now_us / 1000000); }
    void setTime(uint32_t epoch) { now_us = int64_t(epoch) * 1000000; }
} rtc;
int fakeGettimeofday(struct timeval* tv, void*) { tv->tv_sec = now_us / 1000000; tv->tv_usec = now_us % 1000000; return 0; }
int fakeSettimeofday(const struct timeval* tv, const void*) { now_us = int64_t(tv->tv_sec) * 1000000 + tv->tv_usec; return 0; }
#define gettimeofday fakeGettimeofday  // never the host clock
#define settimeofday fakeSettimeofday
struct PrefsFake {
    std::map<std::string, int32_t> kv;
    void begin(const char*, bool) {}
    void end() {}
    void putInt(const char* key, int32_t value) { kv[key] = value; }
    int32_t getInt(const char* key, int32_t fallback) { auto it = kv.find(key); return it == kv.end() ? fallback : it->second; }
} preferences;
std::vector<std::string> events;
void logEvent(const char* fmt, ...) {
    char line[256]; va_list args; va_start(args, fmt); vsnprintf(line, sizeof line, fmt, args); va_end(args);
    check(std::strlen(line) <= 119, std::string("log line over logEvent's 120-byte buffer: ") + line);
    events.push_back(line);
}
struct NtpResult { int code = -1; unsigned long ms = 0; std::string utc; };
// The modem: AT+CCLK returns `cclk` (or `readback` once an NTP sync succeeded), AT+CNTP answers `code`.
struct NetFake {
    bool up = true, synced = false;
    int code = 1, calls = 0;
    unsigned long ms = 900;
    std::string cclk, utc, readback;
    std::string readClock() { return synced && !readback.empty() ? readback : cclk; }
    bool connected() { return up; }
    bool syncNtp(NtpResult* r) {
        ++calls; synced = code == 1;
        if (r) { r->code = code; r->ms = ms; r->utc = synced ? utc : ""; }
        return synced;
    }
} network;
enum ClockSource { CLK_GPS, CLK_HTTP, CLK_NITZ, CLK_NTP };
unsigned long last_gps_sync = 0;
const unsigned long GPS_STALE_MS = 60000;
#include "clock_quality.inc"
#include "clock_live.inc"
#include "clock_parse.inc"
#include "clock_resync.inc"
#include "upload_snapshot.inc"
#undef gettimeofday
#undef settimeofday

time_t real() { return time_t(real_us / 1000000); }
double errorS() { return double(now_us - real_us) / 1e6; }
size_t count(const std::string& prefix) {
    size_t n = 0;
    for (const auto& e : events) n += e.find(prefix) == 0;
    return n;
}
std::string last(const std::string& prefix) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->find(prefix) == 0) return *it;
    return "";
}
// What a deep sleep wipes: RAM, and millis() with it.
void wake() {
    g_clock_gps_epoch = 0; g_clock_gps_ms = 0; g_clock_logged = 0; g_clock_ntp_tried = false; g_clock_no_cclk = false;
    last_gps_sync = 0; fake_ms = 0; network.synced = false;
}
void awake(int64_t s) { now_us += s * 1000000; real_us += s * 1000000; fake_ms += (unsigned long)(s * 1000); }
void reset() {
    now_us = real_us = int64_t(1791100000) * 1000000;
    clock_drift_ppm = 0; clock_drift_learned = false; clock_sleep_enter_epoch = 0; clock_slept_since_sync = 0;
    last_clock_sync_epoch = 0; clock_unref_step_ms = 0; clock_ref_anchored = false;
    clock_ntp_attempt_epoch = 0; clock_ntp_fails = 0;
    preferences.kv.clear(); events.clear(); network = NetFake();
    wake();
    setClock(CLK_NTP, real());  // the first teaching sync anchors the window
}
void sleepFor(int64_t real_s, int32_t ppm) {
    clock_sleep_enter_epoch = rtc.getEpoch();
    real_us += real_s * 1000000;
    now_us += real_s * 1000000 + real_s * ppm;
    wake();
    clockCompensateSleep(true);
}
// One night of 30-minute timer wakes, 10 s awake each, then a resync from `src` (1 s resolution). Returns the
// error the morning resync found.
double night(int32_t ppm, ClockSource src = CLK_NTP, int offset_s = 0) {
    for (int wake = 0; wake < 24; ++wake) { sleepFor(1800, ppm); awake(10); }
    const double error = errorS();
    setClock(src, real() + offset_s);
    return error;
}
// Two GNSS records 10 s apart, advancing as millis() does: a confirmed pair.
void gpsPair() { setClock(CLK_GPS, real()); awake(10); setClock(CLK_GPS, real()); }
std::string cclkAt(int64_t utc, int tzq) {
    const ::time_t local = ::time_t(utc + int64_t(tzq) * 900);
    struct tm t; ::gmtime_r(&local, &t);
    char out[32];
    std::snprintf(out, sizeof out, "%02d/%02d/%02d,%02d:%02d:%02d%+03d", t.tm_year % 100, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec, tzq);
    return out;
}
std::string ntpAt(int64_t utc) {
    const ::time_t value = ::time_t(utc);
    struct tm t; ::gmtime_r(&value, &t);
    char out[32];
    std::snprintf(out, sizeof out, "%04d/%02d/%02d,%02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    return out;
}
// An upload of `sent` frames went out on a wake whose car is in state `active` (nullptr = asleep), with the modem's
// clock reading the truth `cclk_off` s off and NTP reading the truth.
bool upload(const char* active = "obd", int sent = 3, int cclk_off = 0) {
    network.cclk = cclkAt(real() + cclk_off, 4);
    network.utc = ntpAt(real());
    return clockNtpAfterUpload(sent, active);
}

// logger.ino's own call sites, compiled against the globals they read.
int obd_count = 0;
bool is_currently_parked = true, recently_charging = false;
float g_last_io36 = 12.4f;
void uploadOk(int sent) {
#include "clock_call_site.inc"
}
// sleep_power.h's pre-sleep flush: n frames queued, obd_result_vector what the flush left.
unsigned long g_last_obd_answer_ms = 0;
std::vector<int> obd_result_vector;
void flushOk(int n) {
    const bool ok = true;
#include "clock_flush_site.inc"
}
void loopRecovery() {
#include "clock_loop_site.inc"
}

void driftChecks() {
    check(clockDriftCorrectionUs(1800, 5800) == -10440000, "a 30-minute sleep at 5800 ppm fast takes 10.44 s off, to the microsecond");

    reset();
    check(std::fabs(night(6000) - 259.2) < 1.5, "unlearned, a night at 6000 ppm fast ends ~4 min ahead");
    check(clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 60 && preferences.kv.at("drift_ppm") == clock_drift_ppm &&
          last("CLOCK drift").find("src=ntp") != std::string::npos, "the morning NTP resync learns the rate, saves it and logs it");
    check(std::fabs(night(6000)) < 3, "once learned, a whole night stays within a few seconds");
    long raw = 0, residual = 0;
    check(std::sscanf(last("CLOCK drift").c_str(), "CLOCK drift %lds raw, %lds residual", &raw, &residual) == 2 &&
          std::labs(raw + 259) <= 2 && std::labs(residual) <= 2,
          "the log reports the uncompensated drift as raw and what compensation left as residual: " + last("CLOCK drift"));

    // Field shape: an unlearned night, the morning CCLK 170 s ahead of the truth, then the drive's GNSS.
    reset();
    for (int wake = 0; wake < 24; ++wake) { sleepFor(1800, 6000); awake(10); }
    setClock(CLK_NITZ, real() + 170);
    check(!clock_drift_learned && count("CLOCK nitz step") == 1, "the CCLK step is logged and teaches nothing");
    awake(60);
    gpsPair();
    check(clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 60 && std::fabs(errorS()) < 1 &&
          last("CLOCK drift").find("src=gps") != std::string::npos,
          "a CCLK step then a confirmed GNSS pair teaches the true rate, not CCLK's, got " + std::to_string(clock_drift_ppm));

    reset();
    night(6000);
    const int32_t learned = clock_drift_ppm;
    network.code = 62;
    sleepFor(2 * 3600, 6000);
    upload();  // fails: the next attempt would wait 12 h
    setClock(CLK_NITZ, real() + 120);
    clock_sleep_enter_epoch = rtc.getEpoch();  // a stamp from a sleep that never happened
    real_us += 1800000000; now_us += 1800000000;
    wake();
    clockCompensateSleep(false);
    check(std::fabs(errorS() - 120) < 1.5 && clockNeedsSync(), "a reset that did not come through deep sleep corrects "
          "nothing, and the clock it kept is not trusted for geo");
    network.code = 1;
    sleepFor(2 * 3600, 6000);
    check(upload() && network.calls == 2 && clock_drift_ppm == learned,
          "after it, NTP is asked without the old backoff, and its first sync teaches nothing from the old window");

    reset();
    night(6000);
    const int32_t settled = clock_drift_ppm;
    sleepFor(7200, 9000);  // ~22 s ahead after compensation
    const size_t taught = count("CLOCK drift");
    setClock(CLK_GPS, real());
    check(clock_drift_ppm == settled && count("CLOCK drift") == taught && std::fabs(errorS()) < 1,
          "a single GNSS record sets the clock but does not teach");
    awake(120);  // past GPS_STALE_MS, and no second record: never a pair
    setClock(CLK_NTP, real());
    // 1 s resolution over a 2 h window is ~140 ppm; without the accounting the rate would not move at all.
    check(std::abs(clock_drift_ppm - (settled + (9000 - settled) / CLOCK_DRIFT_GAIN)) < 150,
          "the next teaching sync measures the whole window through the GNSS step, got " + std::to_string(clock_drift_ppm));

    reset();
    night(6000);
    const int32_t before_replay = clock_drift_ppm;
    for (int wake = 0; wake < 24; ++wake) { sleepFor(1800, 7000); awake(10); }  // ~43 s ahead after compensation
    const time_t stale = real() - 780;
    setClock(CLK_GPS, stale);       // a stale record from the co-processor
    awake(10);
    setClock(CLK_GPS, stale);       // frozen: the same record again
    awake(10);
    setClock(CLK_GPS, stale - 10);  // replayed: older still
    check(clock_drift_ppm == before_replay && count("CLOCK drift") == 1 && std::fabs(errorS() + 810) < 1 &&
          count("CLOCK gps step") == 1, "frozen and replayed GNSS records set the clock (GPS always wins), never teach, "
          "and log one step line per wake");
    awake(10);
    gpsPair();
    check(std::abs(clock_drift_ppm - (before_replay + (7000 - before_replay) / CLOCK_DRIFT_GAIN)) < 60 &&
          std::fabs(errorS()) < 1, "a confirming pair after them learns the true rate, got " + std::to_string(clock_drift_ppm));

    reset();
    night(6000);
    const int32_t paired = clock_drift_ppm;
    sleepFor(7200, 9000);
    setClock(CLK_GPS, real());
    awake(10);
    setClock(CLK_GPS, real() + 3);   // GNSS moved 13 s while millis() moved 10 s
    check(clock_drift_ppm == paired && count("CLOCK drift") == 1, "records out of step with millis() do not pair");
    awake(301);
    setClock(CLK_GPS, real() + 3);   // in step with the last record, but more than 300 s after it
    check(clock_drift_ppm == paired && count("CLOCK drift") == 1, "records further apart than 300 s do not pair");
    awake(10);
    setClock(CLK_GPS, real() + 3);
    check(clock_drift_ppm != paired && count("CLOCK drift") == 2, "the next consistent record pairs and teaches");

    reset();
    night(6000);
    const int32_t settled_short = clock_drift_ppm;
    const size_t settled_events = count("CLOCK drift");
    sleepFor(CLOCK_DRIFT_MIN_SLEEP_S - 60, 12000);  // ~21 s left after compensation, over too short a window
    check(clock_slept_since_sync < CLOCK_DRIFT_MIN_SLEEP_S, "the window as the drifting clock counts it is short");
    setClock(CLK_NTP, real());
    check(clock_drift_ppm == settled_short && count("CLOCK drift") == settled_events && clock_slept_since_sync == 0,
          "a window under CLOCK_DRIFT_MIN_SLEEP_S restarts but teaches nothing");

    reset();
    wake();
    clockCompensateSleep(false);  // power-on: the clock is zeroed and set again, with no teaching sync since
    now_us = 5 * 1000000;
    setClock(CLK_NITZ, real());
    sleepFor(7200, 6000);
    setClock(CLK_NTP, real());
    check(!clock_drift_learned && preferences.kv.count("drift_ppm") == 0, "the first teaching sync after power-on teaches nothing");
    sleepFor(7200, 6000);
    setClock(CLK_NTP, real());
    check(clock_drift_learned, "the next one does");

    reset();
    night(6000);
    const int32_t before_bad = clock_drift_ppm;
    for (int wake = 0; wake < 24; ++wake) sleepFor(1800, 6000);
    setClock(CLK_NTP, real() + 30);  // a source 30 s wrong
    const int32_t bad_implied = before_bad - int32_t(30 * 1000000LL / (24 * 1800));
    check(std::abs((clock_drift_ppm - before_bad) - (bad_implied - before_bad) / CLOCK_DRIFT_GAIN) < 30,
          "one bad window only moves the rate 1/GAIN of the way");

    reset();
    clock_drift_ppm = -14000; clock_drift_learned = true;  // a bad rate, inside the bound
    for (int n = 0; n < 8; ++n) night(6000);
    check(std::fabs(night(6000)) < 5, "a wrong learned rate is pulled back by honest nights instead of locking in");

    reset();
    night(6000);
    const int32_t steady = clock_drift_ppm;
    night(CLOCK_DRIFT_MAX_PPM + 6000);
    check(clock_drift_ppm == steady, "a window implying a rate past the bound is ignored");
    for (int n = 0; n < 5; ++n) night(5000);
    check(std::fabs(night(5000)) < 5, "a rate that shifts (temperature) is tracked within a few nights");

    // CCLK has whole seconds and the RTC does not: 48 steps at random sub-second phase must not add up.
    reset();
    uint32_t phase = 12345;
    for (int n = 0; n < 48; ++n) {
        phase = phase * 1103515245u + 12345u;
        const int64_t frac_us = int64_t(phase >> 8) % 1000000;
        clock_sleep_enter_epoch = rtc.getEpoch();
        real_us += 1800 * int64_t(1000000) + frac_us;
        now_us += 1800 * int64_t(1000000) + frac_us + (1800 * int64_t(1000000) + frac_us) * 6000 / 1000000;
        wake();
        clockCompensateSleep(true);
        setClock(CLK_NITZ, real());  // truncated to its second, as AT+CCLK reads
    }
    setClock(CLK_NTP, real());
    check(clock_drift_learned && std::abs(clock_drift_ppm - 5964) < 20,
          "48 sub-second-phase CCLK steps still learn within a few ppm, got " + std::to_string(clock_drift_ppm));

    // A phone utc push is not a source: what it stepped must not be taught from.
    reset();
    sleepFor(1800, 0);
    setClock(CLK_NITZ, real() + 120);
    rtc.setTime(uint32_t(real()));  // the BLE push, which bypasses setClock
    clockForgetWindow();
    sleepFor(2 * 3600, 6000);
    setClock(CLK_NTP, real());
    check(!clock_drift_learned, "a teaching sync after a phone time push only anchors; it does not learn the push");

    // Long parks: every product of seconds and ppm is int64.
    check(clockDriftCorrectionUs(30 * 86400, 6000) == -15552000000LL &&
          clockDriftCorrectionUs(60 * 86400, CLOCK_DRIFT_MAX_PPM) == -77760000000LL, "30 and 60 days of correction in int64");
    reset();
    clock_drift_ppm = 5964; clock_drift_learned = true;  // 6000 ppm of real time, per second the fast clock counts
    sleepFor(30 * 86400, 6000);
    check(g_clock_slept_s == 30 * 86400 + 15552 && g_clock_comp_us == -int64_t(g_clock_slept_s) * 5964 && std::fabs(errorS()) < 1,
          "a 30-day park is compensated in int64, to the second");
    reset();
    sleepFor(30 * 86400, 6000);
    setClock(CLK_NITZ, real() + 100);
    sleepFor(30 * 86400, 6000);
    setClock(CLK_NTP, real());
    check(clock_drift_learned && std::abs(clock_drift_ppm - 5964) <= 1 && last("CLOCK drift").find("-31104s raw, -31104s residual over 5215104s") != std::string::npos,
          "a 60-day unlearned park with a CCLK step in it learns the rate exactly from NTP: " + last("CLOCK drift"));

    reset();
    preferences.kv["drift_ppm"] = 6100;
    clockDriftRestore();
    check(clock_drift_learned && clock_drift_ppm == 6100, "a cold boot restores the rate from NVS");
    clock_drift_ppm = 6200;
    preferences.kv["drift_ppm"] = 1;
    clockDriftRestore();
    check(clock_drift_ppm == 6200, "a rate already in RTC memory wins over NVS");
    clock_drift_learned = false; clock_drift_ppm = 0;
    preferences.kv["drift_ppm"] = CLOCK_DRIFT_MAX_PPM + 1;
    clockDriftRestore();
    check(!clock_drift_learned && clock_drift_ppm == 0, "a corrupt NVS rate is ignored");
}

void resyncChecks() {
    auto label = [](const char* active) { return std::string(active ? active : "asleep"); };
    check(label(clockCarActive(true, false, 12.0f, false)) == "obd" && label(clockCarActive(false, true, 12.0f, false)) == "drive" &&
          label(clockCarActive(false, false, 12.0f, true)) == "charge" &&
          label(clockCarActive(false, false, RAIL_AWAKE_VOLTS, false)) == "rail" &&
          label(clockCarActive(false, false, RAIL_AWAKE_VOLTS - 0.1f, false)) == "asleep",
          "the car counts as in use when OBD answers, it drives, it charges or the rail is awake");

    reset();
    sleepFor(2 * 3600, 6000);
    network.cclk = cclkAt(real() + 30, 4);
    check(clockResync("boot") && network.calls == 0 && std::fabs(errorS() - 30) < 1 && !clock_drift_learned,
          "the resync is AT+CCLK only: it sets the clock, never asks NTP and never teaches");
    check(!upload(nullptr) && network.calls == 0 && count("CLOCK ntp=gated") == 0,
          "a parked wake with the car asleep never asks NTP after an upload, and a gated teach is not logged");
    check(!upload("obd", 0) && network.calls == 0, "an active wake with nothing sent does not ask NTP");
    check(upload("obd") && network.calls == 1 && last("CLOCK ntp=") == "CLOCK ntp=ok/900ms on=upload+obd for=teach" &&
          clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 150 && std::fabs(errorS()) < 1,
          "an upload while the car is in use, with a window due, asks NTP, teaches through the CCLK step, and says why");

    reset();
    check(!upload("obd") && network.calls == 0, "a good clock with no sleep to teach does not ask NTP");

    // The call site in logger.ino, with its own globals: parked, OBD silent, rail low, not charging.
    reset();
    sleepFor(2 * 3600, 0);
    obd_count = 0; is_currently_parked = true; g_last_io36 = RAIL_AWAKE_VOLTS - 1; recently_charging = false;
    network.cclk = cclkAt(real(), 4); network.utc = ntpAt(real());
    uploadOk(3);
    check(network.calls == 0, "logger.ino: an upload on a parked wake with the car asleep does not ask NTP");
    is_currently_parked = false;
    uploadOk(3);
    check(network.calls == 1 && last("CLOCK ntp=") == "CLOCK ntp=ok/900ms on=upload+drive for=teach",
          "logger.ino: the same upload while driving does");
    is_currently_parked = true;

    // The pre-sleep flush, the only upload a parked wake makes: the car answering OBD makes it in use.
    reset();
    sleepFor(2 * 3600, 0);
    g_last_obd_answer_ms = 0; g_last_io36 = RAIL_AWAKE_VOLTS - 1;
    network.cclk = cclkAt(real(), 4); network.utc = ntpAt(real());
    flushOk(3);
    check(network.calls == 0, "sleep_power.h: a flush on a wake the car slept through does not ask NTP");
    g_last_obd_answer_ms = 1234;
    flushOk(3);
    check(network.calls == 1 && last("CLOCK ntp=") == "CLOCK ntp=ok/900ms on=upload+obd for=teach",
          "sleep_power.h: a parked flush after the car answered teaches the clock");
    flushOk(3);
    check(network.calls == 1, "sleep_power.h: once per wake");
    g_last_obd_answer_ms = 0; g_last_io36 = 12.4f;

    // NTP is unauthenticated: with a valid clock its step is bounded.
    reset();
    sleepFor(2 * 3600, 0);
    network.cclk = cclkAt(real(), 4); network.utc = ntpAt(real() - 3 * 86400);
    const int64_t before_spoof = now_us;
    check(!clockNtpAfterUpload(3, "obd") && now_us == before_spoof && !clock_drift_learned &&
          last("CLOCK ntp=") == "CLOCK ntp=disagree/900ms on=upload+obd for=teach d=-259200s retry=12h",
          "with a CCLK reading, an NTP time days away from it neither sets nor teaches");
    reset();
    sleepFor(2 * 3600, 0);  // 650 s is inside the no-CCLK drift bound here (708 s), but CCLK is the tighter reference
    network.cclk = cclkAt(real(), 4); network.utc = ntpAt(real() - 650);
    check(!clockNtpAfterUpload(3, "obd") && std::fabs(errorS()) < 1, "with a CCLK reading, 650 s from it is refused");
    sleepFor(12 * 3600, 0);
    network.synced = false;
    network.cclk = cclkAt(real(), 4); network.utc = ntpAt(real() - CLOCK_NTP_MAX_DISAGREE_S + 1);
    check(clockNtpAfterUpload(3, "obd") && std::fabs(errorS() + CLOCK_NTP_MAX_DISAGREE_S - 1) < 1,
          "within CLOCK_NTP_MAX_DISAGREE_S of CCLK, NTP sets the clock");

    reset();
    sleepFor(2 * 3600, 0);  // the drift bound over this window: 7200 s at CLOCK_DRIFT_MAX_PPM = 108 s
    const int64_t bound = 7200LL * CLOCK_DRIFT_MAX_PPM / 1000000 + CLOCK_NTP_MAX_DISAGREE_S;
    network.cclk = ""; network.utc = ntpAt(real() - bound - 5);
    check(!clockNtpAfterUpload(3, "obd") && std::fabs(errorS()) < 1, "without CCLK, a step past the drift bound plus the margin is refused");
    sleepFor(12 * 3600, 0);
    network.synced = false;
    const int64_t wider = int64_t(clock_slept_since_sync) * CLOCK_DRIFT_MAX_PPM / 1000000 + CLOCK_NTP_MAX_DISAGREE_S;
    network.cclk = ""; network.utc = ntpAt(real() - wider + 5);
    check(clockNtpAfterUpload(3, "obd") && std::fabs(errorS() + wider - 5) < 1, "and a step inside it is taken");

    reset();
    now_us = 5 * 1000000;
    clockCompensateSleep(false);
    network.cclk = cclkAt(real() + 86400, 4); network.utc = ntpAt(real());
    check(clockNtpAfterUpload(1, "rail") && std::fabs(errorS()) < 1, "with no valid clock NTP is the fallback, whatever CCLK says");

    // No valid clock at all: one NTP per wake on a modem already up, before the poll, even with the car asleep.
    reset();
    now_us = 5 * 1000000;
    clockCompensateSleep(false);
    network.cclk = ""; network.utc = ntpAt(real());
    loopRecovery();
    check(network.calls == 1 && rtc.getEpoch() > 1000000000 && uploadSendUtc() != 0 &&
          last("CLOCK ntp=") == "CLOCK ntp=ok/900ms on=noclock for=time",
          "logger.ino loop recovery: no clock, car asleep, bearer up - NTP first, so the frames go up with a utc");
    loopRecovery();
    check(network.calls == 1, "and only once per wake");
    reset();
    sleepFor(2 * 3600, 0);
    network.cclk = "";
    loopRecovery();
    check(network.calls == 0, "a valid clock on a parked wake with the car asleep: no NTP");

    reset();
    now_us = 5 * 1000000;  // a cold boot: the clock was zeroed
    clockCompensateSleep(false);
    network.cclk = "";
    check(!clockResync("boot") && !clockResync("loop") && count("CLOCK resync FAILED") == 1 &&
          last("CLOCK resync FAILED") == "CLOCK resync FAILED why=boot (no nitz), UNSET" && network.calls == 0,
          "no CCLK: the resync does not reach for NTP, and FAILED is logged once per wake");
    check(!upload(nullptr) && network.calls == 0 && last("CLOCK ntp=gated") == "CLOCK ntp=gated why=asleep for=time",
          "no reliable time on a parked wake with the car asleep: CCLK only, no NTP after the upload");
    network.cclk = ""; network.utc = ntpAt(real());
    check(clockNtpAfterUpload(1, "rail") && network.calls == 1 && std::fabs(errorS()) < 1 &&
          last("CLOCK ntp=") == "CLOCK ntp=ok/900ms on=upload+rail for=time" && count("CLOCK ntp set, was unset") == 1,
          "no reliable time: the next upload while the car is in use asks NTP, which sets the clock from its result");
    sleepFor(1800, 0);
    clockResync("boot");
    check(count("CLOCK resync FAILED") == 2 && last("CLOCK resync FAILED") == "CLOCK resync FAILED why=boot (no nitz), keeping RTC",
          "the next wake without CCLK logs FAILED once more, now keeping the NTP-set RTC");

    reset();
    network.code = 62; network.cclk = "";
    clockResync("boot");
    check(!upload("drive") && !upload("drive") && network.calls == 1,
          "NTP is never retried within a wake, even when it is the only source");
    sleepFor(1800, 0);
    network.cclk = "";
    clockResync("boot");
    network.cclk = ""; network.utc = "";
    clockNtpAfterUpload(1, "drive");
    check(network.calls == 2, "the next active upload wake asks again when NTP is the only source");

    reset();
    clock_ntp_attempt_epoch = rtc.getEpoch();
    sleepFor(2 * 3600, 0);
    upload();
    check(network.calls == 0, "a teachable window inside CLOCK_NTP_MIN_INTERVAL_S of the last attempt does not ask NTP");
    sleepFor(4 * 3600 + 60, 0);
    upload();
    check(network.calls == 1 && clock_ntp_attempt_epoch == rtc.getEpoch(), "6 h after the last attempt, NTP is asked");

    reset();
    network.code = 62; network.ms = 4100;
    sleepFor(2 * 3600, 0);
    upload("charge");
    check(network.calls == 1 && clock_ntp_fails == 1 &&
          last("CLOCK ntp=") == "CLOCK ntp=62/4100ms on=upload+charge for=teach retry=12h", "a failed NTP backs off to 12 h");
    sleepFor(7 * 3600, 0); upload();
    check(network.calls == 1, "7 h after one failure: still backing off");
    sleepFor(6 * 3600, 0); upload();
    check(network.calls == 2 && clock_ntp_fails == 2 && last("CLOCK ntp=").find("retry=24h") != std::string::npos,
          "12 h after one failure it is asked again, and the next wait is 24 h");
    sleepFor(13 * 3600, 0); upload();
    check(network.calls == 2, "13 h after two failures: still backing off");
    sleepFor(12 * 3600, 0); upload();
    check(network.calls == 3 && clock_ntp_fails == 2, "the backoff is capped at 24 h");
    network.code = 1;
    sleepFor(25 * 3600, 0); upload();
    check(network.calls == 4 && clock_ntp_fails == 0 && last("CLOCK ntp=") == "CLOCK ntp=ok/4100ms on=upload+obd for=teach",
          "a success resets the backoff");
    sleepFor(6 * 3600 + 60, 0); upload();
    check(network.calls == 5, "after a success the interval is 6 h again");

    reset();
    sleepFor(2 * 3600, 6000);
    network.cclk = cclkAt(real() + 30, 4);
    network.utc = ntpAt(real());
    network.readback = cclkAt(real() + 30, 0);  // a NITZ push overwrote the RTC before the readback
    clockNtpAfterUpload(3, "obd");
    check(std::fabs(errorS()) < 1 && clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 150,
          "the time NTP's own result carries wins over the readback, and teaches");

    reset();
    sleepFor(2 * 3600, 6000);
    network.cclk = cclkAt(real() + 60, 4);
    network.readback = network.cclk;  // no time in the result, and the readback is just CCLK again
    clockNtpAfterUpload(3, "obd");
    check(last("CLOCK ntp=") == "CLOCK ntp=same/900ms on=upload+obd for=teach" && !clock_drift_learned &&
          std::fabs(errorS() - 60) < 1, "a readback indistinguishable from CCLK sets the clock as CCLK and does not teach");
    sleepFor(7 * 3600, 6000);
    network.cclk = cclkAt(real() + 60, 4);
    network.readback = cclkAt(real(), 0);
    network.synced = false;
    clockNtpAfterUpload(3, "obd");
    check(clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 60 && std::fabs(errorS()) < 1,
          "a readback that differs from CCLK is NTP, and teaches the whole window through the CCLK step");

    reset();
    network.up = false;
    sleepFor(2 * 3600, 0);
    check(!upload() && network.calls == 0, "no bearer: no NTP");

    reset();
    awake(5);  // millis() 0 reads as "never synced from GPS"
    setClock(CLK_GPS, real());
    const int64_t gps_set = now_us;
    setClock(CLK_NITZ, real() + 50);
    setClock(CLK_NITZ, real() + 50);
    check(now_us == gps_set && count("CLOCK nitz ignored (gps fresh)") == 1, "CCLK is ignored while GNSS is fresh, logged once");
}

void boundsChecks() {
    reset();
    const int64_t kept = now_us;
    for (::time_t garbage : {::time_t(0), ::time_t(-5), ::time_t(999999999), ::time_t(CLOCK_MAX_EPOCH + 1)})
        setClock(CLK_NITZ, garbage);
    check(now_us == kept && count("CLOCK nitz rejected out-of-range time") == 1, "garbage and oversized times never set the clock");

    auto utc = [](int y, int mo, int d, int h, int mi, int s) {
        struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
        return int64_t(::timegm(&t));
    };
    for (int year = 2024; year <= 2070; ++year)
        for (int month : {1, 2, 3, 12})
            check(civilToEpoch64(year, month, 1, 0, 0, 0) == utc(year, month, 1, 0, 0, 0), "int64 calendar matches timegm");
    check(civilToEpoch64(2070, 12, 31, 23, 59, 59) == 3187295999LL && civilToEpoch64(2028, 2, 29, 0, 0, 0) > 0 &&
          civilToEpoch64(2027, 2, 29, 0, 0, 0) == -1 && civilToEpoch64(2026, 4, 31, 0, 0, 0) == -1 &&
          civilToEpoch64(2026, 13, 1, 0, 0, 0) == -1 && civilToEpoch64(2026, 1, 1, 24, 0, 0) == -1,
          "the int64 helper reaches 2070 and range-checks every field");

    check(parseCclk("26/10/25,02:59:59+08") == utc(2026, 10, 25, 0, 59, 59) &&
          parseCclk("26/10/25,02:00:00+04") == utc(2026, 10, 25, 1, 0, 0), "DST end: 02:59:59 +2 h is followed by 02:00:00 +1 h");
    check(parseCclk("26/03/29,01:59:59+04") + 1 == parseCclk("26/03/29,03:00:00+08"), "DST start is one second");
    for (int tzq : {-56, -20, -4, -1, 0, 1, 4, 56}) {
        char text[32]; std::snprintf(text, sizeof text, "26/06/15,12:00:00%+03d", tzq);
        check(parseCclk(text) == utc(2026, 6, 15, 12, 0, 0) - tzq * 900, std::string("tz quarter-hours: ") + text);
    }
    for (const char* bad : {"26/06/15,12:00:00+57", "26/06/15,12:00:00-57", "26/00/15,12:00:00+00", "26/13/15,12:00:00+00",
            "26/06/00,12:00:00+00", "26/06/31,12:00:00+00", "27/02/29,12:00:00+00", "26/06/15,24:00:00+00",
            "26/06/15,12:60:00+00", "26/06/15,12:00:60+00", "23/06/15,12:00:00+00", "80/01/06,00:31:48+02",
            "", "garbage", "26/10/25,02:59:59"})
        check(parseCclk(bad) == 0, std::string("rejected CCLK: ") + bad);
    check(parseNtpTime("\"2026/10/21,10:00:00\"") == utc(2026, 10, 21, 10, 0, 0) &&
          parseNtpTime("26/10/21,10:00:00") == utc(2026, 10, 21, 10, 0, 0) &&
          parseNtpTime("") == 0 && parseNtpTime("1980/01/06,00:00:00") == 0 && parseNtpTime("2026/02/30,00:00:00") == 0,
          "the AT+CNTP result time parses, quoted or not, and rejects defaults");
}
}

namespace target32 {
#include "clock_parse.inc"
}

void targetParse() {
    check(target32::parseCclk("54/01/01,00:00:00+00") == 0 && target32::parseCclk("38/01/19,03:14:08+00") == 0,
          "target32: CCLK times past INT32_MAX are rejected without overflow");
    check(target32::parseCclk("38/01/19,03:14:07+00") == INT32_MAX && target32::parseCclk("38/01/19,04:14:07+04") == INT32_MAX,
          "target32: INT32_MAX is inclusive, after the tz is backed out");
    check(target32::parseCclk("37/12/31,23:59:59+00") == 2145916799, "target32: 2037 still parses");
    check(target32::parseNtpTime("2038/01/19,03:14:08") == 0 && target32::parseNtpTime("2038/01/19,03:14:07") == INT32_MAX,
          "target32: the NTP time honours the same bound");
}

void drift() { live::driftChecks(); }

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "formatter") targetFormatter();
    if (group == "all" || group == "clock") targetClock();
    if (group == "all" || group == "host") hostRange();
    if (group == "all" || group == "drift") drift();
    if (group == "all" || group == "resync") live::resyncChecks();
    if (group == "all" || group == "bounds") { live::boundsChecks(); targetParse(); }
    check(group == "all" || group == "formatter" || group == "clock" || group == "host" || group == "drift" ||
          group == "resync" || group == "bounds", "known clock range group");
    std::cout << "PASS clock range " << group << " native time_t=" << sizeof(::time_t) * 8 << " target time_t=32\n";
}
