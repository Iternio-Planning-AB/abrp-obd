#include <cmath>
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

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "formatter") targetFormatter();
    if (group == "all" || group == "clock") targetClock();
    if (group == "all" || group == "host") hostRange();
    check(group == "all" || group == "formatter" || group == "clock" || group == "host", "known clock range group");
    std::cout << "PASS clock range " << group << " native time_t=" << sizeof(::time_t) * 8 << " target time_t=32\n";
}
