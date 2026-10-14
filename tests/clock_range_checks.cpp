#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <sys/time.h>
#include <cstdint>
#include <cstdlib>
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
using OBDVector = std::vector<std::pair<std::string, double>>;

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

// The real wake/sync path (clockCompensateSleep, setClock, clockNoteRealSync, NVS) against a fake clock: the
// RTC holds the dongle's belief, real_us the truth. Sleep runs `ppm` fast; awake time is crystal-accurate.
namespace live {
int64_t now_us = 0, real_us = 0;
unsigned long millis() { return 0; }
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
    events.push_back(line);
}
enum ClockSource { CLK_GPS, CLK_HTTP, CLK_NITZ, CLK_NTP };
unsigned long last_gps_sync = 0;
const unsigned long GPS_STALE_MS = 60000;
#include "clock_quality.inc"
#include "clock_live.inc"
#undef gettimeofday
#undef settimeofday

void reset() {
    now_us = real_us = int64_t(1791100000) * 1000000;
    clock_drift_ppm = 0; clock_drift_learned = false; clock_sleep_enter_epoch = 0; clock_slept_since_sync = 0;
    last_clock_sync_epoch = 0; preferences.kv.clear(); events.clear();
    setClock(CLK_NITZ, time_t(real_us / 1000000));
}
double errorS() { return double(now_us - real_us) / 1e6; }
void sleepFor(int64_t real_s, int32_t ppm) {
    clock_sleep_enter_epoch = rtc.getEpoch();
    real_us += real_s * 1000000;
    now_us += real_s * 1000000 + real_s * ppm;
    clockCompensateSleep(true);
}
// One night of 30-minute timer wakes, 10 s awake each, then a NITZ resync (1 s resolution). Returns the error
// the morning resync found.
double night(int32_t ppm) {
    for (int wake = 0; wake < 24; ++wake) { sleepFor(1800, ppm); now_us += 10000000; real_us += 10000000; }
    const double error = errorS();
    setClock(CLK_NITZ, time_t(real_us / 1000000));
    return error;
}

void checks() {
    check(clockDriftCorrectionUs(1800, 5800) == -10440000, "a 30-minute sleep at 5800 ppm fast takes 10.44 s off, to the microsecond");

    reset();
    check(std::fabs(night(6000) - 259.2) < 1.5, "unlearned, a night at 6000 ppm fast ends ~4 min ahead");
    check(clock_drift_learned && std::abs(clock_drift_ppm - 6000) < 60 && preferences.kv.at("drift_ppm") == clock_drift_ppm &&
          !events.empty() && events.back().find("CLOCK drift") == 0, "the morning resync learns the rate, saves it and logs it");
    check(std::fabs(night(6000)) < 3, "once learned, a whole night stays within a few seconds");
    long raw = 0, residual = 0;
    check(std::sscanf(events.back().c_str(), "CLOCK drift %lds raw, %lds residual", &raw, &residual) == 2 &&
          std::labs(raw + 259) <= 2 && std::labs(residual) <= 2,
          "the log reports the uncompensated drift as raw and what compensation left as residual: " + events.back());

    reset();
    night(6000);
    const int32_t learned = clock_drift_ppm;
    clock_sleep_enter_epoch = rtc.getEpoch();
    real_us += 1800000000; now_us += 1800000000;
    clockCompensateSleep(false);
    check(clock_sleep_enter_epoch == 0 && clock_slept_since_sync == 0 && std::fabs(errorS()) < 1.5,
          "a reset that did not come through deep sleep consumes the stamp and corrects nothing");
    sleepFor(7200, 9000);  // ~22 s off: a sync here would teach if it were allowed to
    const size_t logged = events.size();
    setClock(CLK_GPS, time_t(real_us / 1000000));
    check(clock_drift_ppm == learned && preferences.kv.at("drift_ppm") == learned && events.size() == logged &&
          clock_slept_since_sync == 0, "a GPS sync restarts the window but teaches nothing");

    reset();
    night(6000);
    const int32_t settled = clock_drift_ppm;
    const size_t settled_events = events.size();
    sleepFor(CLOCK_DRIFT_MIN_SLEEP_S - 60, 12000);  // ~21 s left after compensation, over too short a window
    check(clock_slept_since_sync < CLOCK_DRIFT_MIN_SLEEP_S, "the window as the drifting clock counts it is short");
    setClock(CLK_NITZ, time_t(real_us / 1000000));
    check(clock_drift_ppm == settled && events.size() == settled_events && clock_slept_since_sync == 0,
          "a window under CLOCK_DRIFT_MIN_SLEEP_S restarts but teaches nothing");

    reset();
    last_clock_sync_epoch = 0;  // power-on: the clock has never been set from a real source
    sleepFor(7200, 6000);
    setClock(CLK_NITZ, time_t(real_us / 1000000));
    check(!clock_drift_learned && preferences.kv.count("drift_ppm") == 0 && clock_slept_since_sync == 0,
          "the first sync after power-on teaches nothing");

    reset();
    night(6000);
    const int32_t before_bad = clock_drift_ppm;
    for (int wake = 0; wake < 24; ++wake) sleepFor(1800, 6000);
    setClock(CLK_NITZ, time_t(real_us / 1000000) + 30);  // a source 30 s wrong
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
}

void drift() { live::checks(); }

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "formatter") targetFormatter();
    if (group == "all" || group == "clock") targetClock();
    if (group == "all" || group == "host") hostRange();
    if (group == "all" || group == "drift") drift();
    check(group == "all" || group == "formatter" || group == "clock" || group == "host" || group == "drift", "known clock range group");
    std::cout << "PASS clock range " << group << " native time_t=" << sizeof(::time_t) * 8 << " target time_t=32\n";
}
