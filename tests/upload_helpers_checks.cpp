#define PRINT 1
#include "upload_transport_fixture.h"
#include <cstdarg>
#include <ctime>
#include <limits>
#include <sstream>
#include "networkwrapper.h"
#include "cellwrapper.h"

#define ENABLE_NETWORK 1
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif
#define ENABLE_CELL 1
#include "../baseLogger/config.h"
#define MAX_UPLOAD_BUFFER 120

struct RTCStub {
    uint32_t epoch = 1791112000;
    unsigned reads = 0;
    bool scripted = false;
    std::deque<uint32_t> scripted_epochs;
    unsigned scriptUploadReads(uint32_t snapshot, bool buffered) {
        scripted = true;
        scripted_epochs.push_back(snapshot);
        const unsigned event_reads = ENABLE_LOGGING ? (buffered ? 3 : 2) : 0;
        for (unsigned i = 0; i < event_reads; ++i) scripted_epochs.push_back(0);
        return 1 + event_reads;
    }
    uint32_t getEpoch() {
        ++reads;
        if (!scripted) return epoch;
        check(!scripted_epochs.empty(), "public sender must not make an unscripted extra RTC read");
        const auto value = scripted_epochs.front();
        scripted_epochs.pop_front();
        return value;
    }
} rtc;
#define RTC_DATA_ATTR
#include "clock_quality.inc"
int current_state = 0;
const char* stateName(int) { return "ACTIVE"; }
double g_last_voltage = 12.5, g_last_io36 = 12.6;
struct LogStub {
    std::vector<std::string> lines;
    void writeLine(const char* line) { lines.emplace_back(line); }
} datalogger;
#if ENABLE_LOGGING
#include "upload_log_event.inc"
#endif
#include "support/backlog_fs.h"
struct ESPStub {
    unsigned long getFreeHeap() { return 80000; }
    unsigned long getMaxAllocHeap() { return 40000; }
} ESP;
bool g_ble_up = false;
void esp_task_wdt_reset() {}
enum ClockSource { CLK_HTTP };
time_t parseHttpDate(const char* value) { return std::strtoll(value, nullptr, 10); }
void setClock(ClockSource, time_t value) { rtc.epoch = value; last_clock_sync_epoch = value; }
#include "telemetry_format.h"
#include "geo_backlog.h"
int gzip_calls = 0;
bool gzip_enabled = false, force_oversized = false;
int64_t clock_after_gzip = 0;
time_t sync_after_gzip = -1;
bool gzipBody(const std::string& raw, std::string& wire) {
    ++gzip_calls;
    if (clock_after_gzip) rtc.epoch = clock_after_gzip;
    if (sync_after_gzip >= 0) last_clock_sync_epoch = sync_after_gzip;
    if (!gzip_enabled) return false;
    wire.assign(force_oversized ? 1025 : raw.size() / 5 + 16, '\0');
    wire[0] = '\x1f'; wire[1] = '\x8b'; wire[2] = '\x08'; wire[3] = '\r'; wire[4] = '\n';
    for (size_t i = 5; i < wire.size(); ++i) wire[i] = raw[(i - 5) % raw.size()];
    return true;
}

struct FakeNetwork : NetworkWrapper {
    CFreematics uart;
    HTTPClientSIM client{&uart};
    std::vector<std::string> sent;
    std::vector<HeaderVector> headers;
    std::deque<int> codes;
    std::deque<int64_t> response_epochs;
    std::string getDateHeader() override {
        if (response_epochs.empty()) return {};
        const auto epoch = response_epochs.front(); response_epochs.pop_front();
        return std::to_string(epoch);
    }
    int code = 200, ack = 1;
    bool unknown_diagnostics = false;
    std::string body = "{}";
    FakeNetwork() : NetworkWrapper(FreematicsESP32{}) {}
    bool init(bool) override { return true; }
    void restart() override {}
    void set_headers(HeaderVector h) override { headers.push_back(h); }
    bool connect() override { return true; }
    bool connected() override { return true; }
    void disconnect() override {}
    HttpResponse getEpoch() override { return {0, ""}; }
    HttpResponse sendData(std::string) override { return {0, ""}; }
    HttpResponse sendBulk(std::string payload) override {
        sent.push_back(payload);
        int status = code;
        if (!codes.empty()) { status = codes.front(); codes.pop_front(); }
        uart.replies = {{1, ">"}, {ack, ack == 1 ? "\r\nOK" : "\r\nERROR"},
            {status ? 1 : 0, status ? "+SHREQ: \"POST\"," + std::to_string(status) + "," + std::to_string(body.size()) + "\r\n" : ""}};
        if (status && !body.empty()) uart.replies.push_back({1, "+SHREAD: " + std::to_string(std::min<size_t>(body.size(), 496)) + "\r\n" + body.substr(0, 496)});
        auto response = client.send("/2/app/tlm", payload.data(), payload.size(), nullptr);
        check(uart.writes[uart.writes.size() - (status && !body.empty() ? 3 : 2)] == payload, "actual helper final payload streamed unchanged");
        return {response.returnValue, response.buffer};
    }
#if ENABLE_LOGGING
    HTTPTransferDiagnostics transferDiagnostics() const override {
        return unknown_diagnostics ? NetworkWrapper::transferDiagnostics() : client.transferDiagnostics();
    }
#endif
} network;
int logging_errors = 0, g_last_http = 0, g_http0_streak = 0;
unsigned long g_last_post_ms = 0;
std::string g_last_status, tlm_push_token = "test-token-actual", api_key = "test-key-actual";
std::vector<OBDVector> obd_result_vector;
#include "network_upload.h"

void reset() {
    rtc.epoch = 1791112000; rtc.reads = 0;
    rtc.scripted = false; rtc.scripted_epochs.clear();
    last_clock_sync_epoch = rtc.epoch;
    clock_after_gzip = 0; sync_after_gzip = -1;
    LittleFS.files.clear(); LittleFS.fail_open = false;
    network.response_epochs.clear();
    network.sent.clear(); network.headers.clear(); network.codes.clear(); network.uart = CFreematics{};
    network.code = 200; network.ack = 1; network.body = "{}"; network.unknown_diagnostics = false;
    datalogger.lines.clear(); Serial.output.clear(); gzip_calls = 0;
    gzip_enabled = force_oversized = false;
    logging_errors = g_last_http = g_http0_streak = 0; g_last_status.clear(); obd_result_vector.clear();
#if ENABLE_LOGGING
    g_upload_request_id = 0; upload_hash_inputs.clear(); upload_hash_failure = false;
#endif
}
OBDVector frame(unsigned index) { return {{"utc", 1791112000 + index}, {"soc", 50 + index / 100.0}, {"voltage", 350}, {"current", 12}, {"speed", 50}}; }
#if ENABLE_LOGGING
std::vector<std::string> events(const std::string& prefix) {
    std::vector<std::string> result;
    for (const auto& line : datalogger.lines) {
        const auto start = line.find(prefix);
        if (start != std::string::npos) result.push_back(line.substr(start));
        check(line.size() < 210, "real logEvent line fits 210 bytes");
        const auto text = line.find("UPLOAD ");
        if (text != std::string::npos) check(line.size() - text < 120, "message fits 120 bytes");
        for (unsigned char c : line) check(c >= 32 && c < 127, "no raw CR LF NUL or other control in persistent event");
    }
    return result;
}
std::string digest12(const std::string& payload) {
    unsigned char digest[32];
    const auto n = upload_hash_inputs.size();
    check(mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, 0) == 0, "host SHA256");
    upload_hash_inputs.resize(n);
    char text[13] = {};
    for (size_t i = 0; i < 6; ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
    return text;
}
void fingerprint() {
    for (bool compressed : {false, true}) {
        reset(); gzip_enabled = compressed;
        check(logNetwork({{{"utc", 1791111990}, {"_private", 1}}}), "empty serialization success");
        check(network.sent.empty() && g_upload_request_id == 0 && upload_hash_inputs.empty() && datalogger.lines.empty(), "no request ID hash or event for unsent empty serialization");
        const std::vector<OBDVector> frames = {frame(0), frame(10), {{"utc", 1791111990}, {"_private", 1}}};
        const auto raw = bulkStringV2(frames);
        network.code = 400; network.ack = 0; network.body = "{\"message\":\"gzip: invalid header\"}";
        check(!logNetwork(frames), "400 rejected without changing acceptance");
        check(!logNetwork(frames), "identical retry still rejected");
        auto bodies = events("UPLOAD body raw="); auto results = events("UPLOAD id=");
        check(bodies.size() == 2 && results.size() == 2 && datalogger.lines.size() == 4, "exactly two diagnostic events per POST");
        check(network.sent[0] == network.sent[1] && upload_hash_inputs == network.sent, "hashes only exact final sends including gzip binary NULs");
        check(gzip_calls == 2 && network.headers.size() == 2, "no extra compression or header changes for diagnostics");
        for (size_t i = 0; i < 2; ++i) {
            check(bodies[i].find("id=" + std::to_string(i + 1)) != std::string::npos && results[i].find("id=" + std::to_string(i + 1)) != std::string::npos, "sequential final-send correlation");
            check(bodies[i].find("sha=" + digest12(network.sent[i])) != std::string::npos, "fingerprint reflects actual final wire payload");
            check(bodies[i].find("raw=" + std::to_string(raw.size()) + "B wire=" + std::to_string(network.sent[i].size()) + "B") != std::string::npos, "factual raw and wire sizes");
            check(bodies[i].find(compressed ? "enc=g" : "enc=j") != std::string::npos && bodies[i].find("src=3 pts=2 sutc=1791111990..1791112010") != std::string::npos, "encoding and source-versus-serialized counts and source UTC span");
            check(results[i].find("err=gzip_header") != std::string::npos && results[i].find("p/a/r=1/0/1") != std::string::npos && results[i].find("w=?") != std::string::npos, "error detail and actual absent ACK, UART writes unknown");
        }
        check(g_last_http == 400 && g_last_status == "http_err" && logging_errors == 2 && g_last_post_ms > 0, "existing status accounting untouched");
        network.code = 200; network.body = "not JSON, no ACK and no acceptance-body parsing";
        check(logNetwork(frames) && logging_errors == 0 && g_last_status == "ok", "200 accepted regardless of response and body ACK");
        const auto oracle_digest = digest12(network.sent[0]);
        std::cout << "HASH " << oracle_digest << ' ';
        for (unsigned char c : network.sent[0]) std::printf("%02x", c);
        std::cout << '\n';
    }
    reset(); upload_hash_failure = true;
    check(logNetwork({frame(0)}), "hash failure cannot change upload success");
    check(events("UPLOAD body raw=")[0].find("sha=?") != std::string::npos, "failed hash explicitly unknown");
    reset(); network.unknown_diagnostics = true;
    check(logNetwork({frame(0)}), "transport with default unknown getter works");
    check(events("UPLOAD id=")[0].find("p/a/r=-1/-1/-1 rx=-1/-1/-1 cut=-1 w=?") != std::string::npos, "default wrapper reports unknown, not fabricated modem stages");
}
void safeErrors() {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"{\"message\":\"gzip: invalid header\"}", "gzip_header"},
        {"{\"message\":\"gzip: invalid checksum\"}", "gzip_checksum"},
        {"{\"message\":\"incorrect length check\"}", "gzip_length"},
        {"{\"message\":\"unexpected EOF\"}", "incomplete"},
        {"{\"message\":\"gzip something else\"}", "gzip_other"},
        {"\r\n\1{\"message\":\"checksum\"}\t", "gzip_checksum"},
        {std::string("\0", 1) + "checksum", "gzip_checksum"},
        {"<HTML>arbitrary checksum response</HTML>", "html"},
        {"unauthorized", "auth"}, {"validation error", "validation"}, {"", "empty"},
        {"gzip: invalid header " + tlm_push_token, "redacted"},
        {"gzip: invalid checksum " + api_key, "redacted"},
        {"{\"message\":\"checksum\",\"TOKEN\":\"small\"}", "redacted"},
        {"{\"api_key\":\"short\",\"message\":\"invalid header\"}", "redacted"},
        {"Bearer abc.def.ghi", "redacted"},
        {"gzip: checksum 12345678-abcd-abcd-abcd-123456789abc", "redacted"},
        {"gzip: invalid header " + std::string(300, ' ') + tlm_push_token, "redacted"},
        {"gzip: invalid header " + std::string(300, ' ') + "api-key=short", "redacted"},
        {"arbitrary response content, not allowlisted", "other"},
    };
    for (const auto& c : cases) {
        reset();
        check(uploadErrorCategory(c.first) == c.second, "whole response classified/redacted before any clipping: " + c.second);
        network.code = 400; network.body = c.first;
        check(!logNetwork({frame(0)}), "safe error remains failure");
        const auto result = events("UPLOAD id=");
        const std::string captured_category = !c.first.empty() && c.first[0] == '\0' ? "empty" : c.second;
        check(result.size() == 1 && result[0].find("err=" + captured_category) != std::string::npos,
            "allowlisted bounded error category " + captured_category + ": " + (result.empty() ? "missing" : result[0]));
        check(result[0].find(tlm_push_token) == std::string::npos && result[0].find(api_key) == std::string::npos && Serial.output.find(tlm_push_token) == std::string::npos && Serial.output.find(api_key) == std::string::npos, "no secret in persistent or PRINT serial response");
    }
    reset(); network.code = 400; network.body = std::string(600, 'x');
    check(!logNetwork({frame(0)}), "oversized response error");
    check(events("UPLOAD id=")[0].find("rx=600/496/496 cut=1") != std::string::npos, "error response declared/captured/truncated metadata");
    reset(); g_upload_request_id = UINT32_MAX - 1;
    check(logNetwork({frame(0)}), "large wake sequence id");
    check(events("UPLOAD body raw=")[0].find("sutc=1791112000..1791112000") != std::string::npos &&
        events("UPLOAD id=")[0].find("cut=0 w=?") != std::string::npos,
        "full source span and transfer fields still fit at maximum ESP32 request id");
}
#endif
void queue() {
    reset();
    for (unsigned i = 0; i < 120; ++i) obd_result_vector.push_back(frame(i));
    const auto original = obd_result_vector;
    network.codes = {0};
    check(!uploadBuffered(true) && obd_result_vector == original && network.sent.size() == 1, "HTTP0 keeps entire FIFO and stops at first failure");
    check(gzip_calls > (int)network.sent.size(), "queue sizing compressions occur without requests");
#if ENABLE_LOGGING
    check(upload_hash_inputs.size() == network.sent.size() && g_upload_request_id == network.sent.size(), "sizing compressions have no request ID hash or diagnostic body");
#endif
    const auto failed_body = network.sent[0];
    network.code = 400;
    check(!uploadBuffered(true) && obd_result_vector == original && network.sent.back() == failed_body, "rejected retry retains identical FIFO payload");
    network.code = 200;
    const size_t successful_start = network.sent.size();
    check(uploadBuffered(true) && obd_result_vector.empty(), "successful drain erases accepted frames");
    std::vector<json> received;
    for (size_t i = successful_start; i < network.sent.size(); ++i)
        for (auto& point : json::parse(network.sent[i])) received.push_back(point);
    const auto expected = json::parse(bulkStringV2(original));
    check(json(received) == expected, "all 120 frames delivered once and in FIFO order with battery intact");
#if ENABLE_LOGGING
    check(upload_hash_inputs == network.sent && events("UPLOAD body raw=").size() == network.sent.size() && events("UPLOAD id=").size() == network.sent.size(), "exactly one final fingerprint and paired events per real queue POST");
#endif
    reset(); gzip_enabled = true; force_oversized = true; obd_result_vector = {frame(0)};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.empty(), "existing oversized single-frame drop unchanged");
#if ENABLE_LOGGING
    check(upload_hash_inputs.empty() && g_upload_request_id == 0, "drop has no POST diagnostics");
#endif
}
void geoExpiry() {
    reset();
    rtc.epoch = 0;
    const OBDVector unknown_clock = {{"utc", 1791112000}, {"soc", 75}, {"lat", 59.3}, {"lon", 18.1},
        {"heading", 90}, {"__geo_loc_utc", 1791112000}, {"__geo_head_utc", 1791112000}};
    check(logNetwork({unknown_clock}), "unknown clock keeps eligible battery upload");
    const auto unknown_point = json::parse(network.sent.back())[0];
    check(!unknown_point.contains("location") && !unknown_point.contains("heading") &&
        unknown_point["soc"]["frac"] == 0.75 && unknown_point["soc"]["time"] == v2Time(1791112000),
        "unknown send clock must omit valid fresh geo while preserving original SOC");
    reset();
    const double observed = rtc.epoch - 35 * 24 * 60 * 60;
    const OBDVector frozen = {{"utc", double(rtc.epoch)}, {"soc", 75}, {"current", -12},
        {"is_parked", 1}, {"is_charging", 1}, {"is_dcfc", 1},
        {"lat", 59.3}, {"lon", 18.1}, {"heading", 90},
        {"__geo_loc_utc", observed}, {"__geo_head_utc", observed}};
    const auto original = frozen;
    check(logNetwork({frozen}), "public upload accepts fresh battery with 35-day frozen geo");
    const auto points = json::parse(network.sent.back());
    check(points.size() == 1 && !points[0].contains("location") && !points[0].contains("heading"),
        "send-time expiry omits 35-day frozen location and heading");
    check(points[0]["soc"]["frac"] == 0.75 && points[0]["soc"]["time"] == v2Time(rtc.epoch) &&
        points[0]["chargingState"]["state"] == "CHARGING_DC" && points[0]["drivingState"]["state"] == "PARK",
        "expired geo keeps original fresh SOC and charging/driving flags");
    check(frozen == original, "send-time filtering never mutates the source frame");
}
OBDVector geoFrame(double utc, double loc_utc, double head_utc, bool battery = true) {
    OBDVector result = {{"utc", utc}, {"lat", 59.3}, {"lon", 18.1}, {"heading", 360},
        {"__geo_loc_utc", loc_utc}, {"__geo_head_utc", head_utc}};
    if (battery) {
        result.insert(result.end(), {{"soc", 75}, {"current", -12}, {"is_parked", 1},
            {"is_charging", 1}, {"is_dcfc", 0}});
    }
    return result;
}
json withoutGeo(json point) {
    point.erase("location"); point.erase("heading");
    return point;
}
void geoExplicitClock() {
    const auto source = geoFrame(1791112000, 1791112000, 1791112000);
    const auto trusted = json::parse(bulkStringV2({source}, 1791112000))[0];
    check(trusted.contains("location") && trusted.contains("heading"), "explicit trusted fixed formatter time keeps valid geo");
    check(json::parse(bulkStringV2({source}))[0] == withoutGeo(trusted), "missing explicit formatter time fails closed geo");
    for (double send_clock : {0.0, 1.0, 999999999.0, 1000000000.0, -1.0, double(UINT32_MAX) + 1,
            1791112000.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()}) {
        check(json::parse(bulkStringV2({source}, send_clock))[0] == withoutGeo(trusted),
            "invalid explicit formatter time omits valid geo with battery UTC unchanged: " + std::to_string(send_clock));
    }
}
void geoClockQuality() {
    const time_t now = 1791112000;
    for (time_t sync : {time_t(0), time_t(1), time_t(1000000000), time_t(-1),
            time_t(uint64_t(UINT32_MAX) + 1), now + 1, now - 901, now - 900, now}) {
        reset(); last_clock_sync_epoch = sync;
        const auto snapshot = uploadSendUtc();
        check(rtc.reads == 1 && snapshot == ((sync == now || sync == now - 900) ? double(now) : 0),
            "trust-aware snapshot validates one captured uint32 RTC read");
        const auto source = geoFrame(now, now, now);
        check(logNetwork({source}), "clock quality public upload keeps battery");
        const auto point = json::parse(network.sent.back())[0];
        const bool trusted = sync == now || sync == now - 900;
        check(point.contains("location") == trusted && point.contains("heading") == trusted &&
            point["soc"]["frac"] == 0.75 && point["soc"]["time"] == v2Time(now),
            "public geo requires valid consistent sync within inclusive 900 seconds: " + std::to_string(sync));
        check(source == geoFrame(now, now, now), "clock trust filtering leaves source numeric provenance unchanged");
    }
}
void geoBoundaries() {
    check(std::numeric_limits<decltype(rtc.getEpoch())>::digits == 32 &&
        !std::numeric_limits<decltype(rtc.getEpoch())>::is_signed,
        "public RTC fixture matches target unsigned 32-bit epoch API");
    reset();
    const auto formerly_eligible = geoFrame(rtc.epoch, rtc.epoch - 30UL * 24UL * 60UL * 60UL, rtc.epoch - 30UL * 24UL * 60UL * 60UL);
    check(logNetwork({formerly_eligible}), "30-day geo still preserves battery upload");
    const auto formerly_point = json::parse(network.sent.back())[0];
    check(!formerly_point.contains("location") && !formerly_point.contains("heading"),
        "formerly 30-day eligible geo now omitted by one-day sender margin");
    check(GEO_MAX_SEND_AGE_SECONDS == 29UL * 24UL * 60UL * 60UL, "production expiry is conservative 29 days");
    for (int loc_delta : {-1, 0, 1}) {
        for (int head_delta : {-1, 0, 1}) {
            reset();
            const double loc = rtc.epoch - GEO_MAX_SEND_AGE_SECONDS - loc_delta;
            const double head = rtc.epoch - GEO_MAX_SEND_AGE_SECONDS - head_delta;
            const auto source = geoFrame(rtc.epoch, loc, head);
            check(logNetwork({source}), "independent cutoff public upload");
            const auto point = json::parse(network.sent.back())[0];
            check(point.contains("location") == (loc_delta <= 0) && point.contains("heading") == (head_delta <= 0),
                "location/heading cutoff -1/equal/+1 seconds independently inclusive");
            if (loc_delta <= 0) check(point["location"]["time"] == v2Time(loc), "location observation UTC unchanged");
            if (head_delta <= 0) check(point["heading"]["time"] == v2Time(head) && point["heading"]["degrees"] == 0, "heading observation UTC and north unchanged");
            check(withoutGeo(point) == withoutGeo(json::parse(bulkStringV2({source}))[0]), "all battery and state mappings unchanged");
        }
    }
    for (uint32_t epoch : {uint32_t(0), uint32_t(1), uint32_t(999999999), uint32_t(1000000000)}) {
        reset(); rtc.epoch = epoch; last_clock_sync_epoch = epoch;
        const auto source = geoFrame(1791112000, 1791112000, 1791112000);
        check(logNetwork({source}), "representable unknown send clock public upload");
        check(json::parse(network.sent.back())[0] == withoutGeo(json::parse(bulkStringV2({source}, 1791112000))[0]),
            "unknown public RTC omits valid geo without restamping battery");
    }
    reset(); rtc.epoch = static_cast<uint32_t>(-1); last_clock_sync_epoch = rtc.epoch;
    const auto converted_source = geoFrame(1791112000, 1780000000, 1780000001);
    check(rtc.getEpoch() == UINT32_MAX && logNetwork({converted_source}), "target API converts negative integer to known uint32 maximum");
    check(json::parse(network.sent.back())[0] == withoutGeo(json::parse(bulkStringV2({converted_source}))[0]),
        "converted negative integer is a known clock and expires old geo without pruning battery/state");
    reset(); rtc.epoch = static_cast<uint32_t>(uint64_t(UINT32_MAX) + 1);
    check(rtc.getEpoch() == 0 && logNetwork({converted_source}), "target API oversized integer wraps to unknown zero");
    check(network.sent.back() == bulkStringV2({converted_source}), "wrapped unknown zero clock omits geo and retains original battery timestamps");
    for (uint32_t epoch : {uint32_t(1000000001), uint32_t(UINT32_MAX) - 1, uint32_t(UINT32_MAX)}) {
        reset(); rtc.epoch = epoch; last_clock_sync_epoch = epoch;
        const double loc = double(epoch) - GEO_MAX_SEND_AGE_SECONDS;
        const auto source = geoFrame(epoch, loc, loc - 1);
        check(logNetwork({source}), "known threshold/near-wrap clock upload");
        const auto point = json::parse(network.sent.back())[0];
        check(point.contains("location") && !point.contains("heading") && point["location"]["time"] == v2Time(loc), "known clock cutoff subtraction never wraps");
    }
    for (bool stale_location : {false, true}) {
        reset();
        const double old = rtc.epoch - 35 * 24 * 60 * 60;
        const auto source = geoFrame(rtc.epoch, stale_location ? old : rtc.epoch, stale_location ? rtc.epoch : old);
        check(logNetwork({source}), "35-day location/heading independent public upload");
        const auto point = json::parse(network.sent.back())[0];
        check(point.contains("location") == !stale_location && point.contains("heading") == stale_location,
            "35-day stale location/fresh heading and inverse remain independent");
    }
    reset();
    const auto future = geoFrame(rtc.epoch + 3600, rtc.epoch + 3600, rtc.epoch + 1800);
    check(logNetwork({future}) && network.sent.back() == bulkStringV2({future}, 1791112000), "future relative to corrected send clock retains existing policy");
    const double old = rtc.epoch - GEO_MAX_SEND_AGE_SECONDS - 1;
    const auto old_battery = geoFrame(old, old, old);
    check(logNetwork({old_battery}), "old battery upload remains eligible");
    check(json::parse(network.sent.back())[0] == withoutGeo(json::parse(bulkStringV2({old_battery}))[0]), "old battery/state are not restamped or age-pruned");
    const auto geo_only = geoFrame(rtc.epoch, old, old, false);
    const auto posts = network.sent.size();
    check(logNetwork({geo_only}) && network.sent.size() == posts && g_last_status == "ok_empty", "expired geo-only direct upload is no-op without POST");
    obd_result_vector = {geo_only};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.size() == posts, "expired geo-only queued batch is no-op without POST");
}
void geoPublicClockReads() {
    const uint32_t now = 1791112000;
    const double observed = double(now) - GEO_MAX_SEND_AGE_SECONDS;
    for (bool buffered : {false, true}) {
        reset(); rtc.epoch = 0;
        const auto source = geoFrame(now, observed, observed);
        obd_result_vector = {source}; network.code = 503;
        const auto expected_first = bulkStringV2({source}, now);
        const auto expected_retry = bulkStringV2({source}, double(now) + 1);
        auto attempt = [buffered, &source]() { return buffered ? uploadBuffered(true) : logNetwork({source}); };
        const auto first_reads = rtc.scriptUploadReads(now, buffered);
        check(!attempt() && network.sent.size() == 1 && network.sent.back() == expected_first,
            "public " + std::string(buffered ? "buffered" : "direct") + " sender serializes first scripted RTC snapshot, not a resample");
        check(rtc.reads == first_reads && rtc.scripted_epochs.empty(),
            "public sender uses one policy RTC read plus exact diagnostic reads on failed attempt");
        check(obd_result_vector == std::vector<OBDVector>{source} && last_clock_sync_epoch == now && network.response_epochs.empty(),
            "failed scripted-clock upload retains source and receives no response Date sync");
        const auto retry_reads = rtc.scriptUploadReads(now + 1, buffered);
        check(!attempt() && network.sent.size() == 2 && network.sent.back() == expected_retry,
            "public " + std::string(buffered ? "buffered" : "direct") + " retry recaptures scripted cutoff clock exactly once");
        check(rtc.reads == first_reads + retry_reads && rtc.scripted_epochs.empty() &&
            obd_result_vector == std::vector<OBDVector>{source} && last_clock_sync_epoch == now,
            "failed retry has no extra clock reads, sync mutation or numeric backlog mutation");
    }
    reset(); gzip_enabled = true; rtc.epoch = 0;
    for (unsigned i = 0; i < MAX_FRAMES_PER_POST + 1; ++i) {
        auto source = geoFrame(double(now) + i, observed, observed, false);
        source.emplace_back("soc", 60 + i);
        obd_result_vector.push_back(source);
    }
    const auto original = obd_result_vector;
    const std::vector<OBDVector> first(original.begin(), original.begin() + MAX_FRAMES_PER_POST);
    const std::vector<OBDVector> second(original.begin() + MAX_FRAMES_PER_POST, original.end());
    std::string first_wire, second_wire;
    check(gzipBody(bulkStringV2(first, now), first_wire) && first_wire.size() <= CELL_MAX_POST_BYTES &&
        gzipBody(bulkStringV2(second, 0), second_wire) && second_wire.size() <= CELL_MAX_POST_BYTES,
        "scripted multi-batch fixture respects modem cap without changing production batch sizing");
    const auto first_reads = rtc.scriptUploadReads(now, true);
    const auto second_reads = rtc.scriptUploadReads(now + CLOCK_RESYNC_SECONDS + 1, true);
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent == std::vector<std::string>{first_wire, second_wire},
        "each public drained batch captures its scripted RTC once and rechecks clock trust before serializing FIFO");
    check(rtc.reads == first_reads + second_reads && rtc.scripted_epochs.empty() &&
        last_clock_sync_epoch == now && network.response_epochs.empty(),
        "multiple batches consume exactly one policy clock read per batch plus diagnostic reads with no implicit Date sync");
}
void geoTrustRetry() {
    for (bool buffered : {false, true}) {
        reset();
        const time_t now = rtc.epoch;
        const auto source = geoFrame(now, now, now);
        obd_result_vector = {source}; network.code = 503; last_clock_sync_epoch = 0;
        auto attempt = [buffered, &source]() { return buffered ? uploadBuffered(true) : logNetwork({source}); };
        check(!attempt() && obd_result_vector == std::vector<OBDVector>{source}, "untrusted failed send keeps source queue");
        check(!json::parse(network.sent.back())[0].contains("location"), "unsynced attempt omits valid geo");
        network.response_epochs.push_back(now);
        check(!attempt() && !json::parse(network.sent.back())[0].contains("location"), "failed response clock sync cannot change attempted batch policy");
        check(last_clock_sync_epoch == now && !attempt() && json::parse(network.sent.back())[0].contains("location") &&
            json::parse(network.sent.back())[0].contains("heading"), "accepted response sync restores geo on next failed retry");
        last_clock_sync_epoch = now - 901;
        check(!attempt() && !json::parse(network.sent.back())[0].contains("location"), "retry rechecks newly stale sync metadata");
        last_clock_sync_epoch = now + 1;
        check(!attempt() && !json::parse(network.sent.back())[0].contains("heading"), "retry rechecks newly future sync metadata");
        setClock(CLK_HTTP, now); network.code = 200;
        check(attempt() && json::parse(network.sent.back())[0] == json::parse(bulkStringV2({source}, 1791112000))[0],
            "trusted successful retry restores original independent geo and battery timestamps");
        if (buffered) check(obd_result_vector.empty(), "trusted successful retry drains source queue");
    }
    reset(); last_clock_sync_epoch = 0;
    obd_result_vector = {geoFrame(1791112000, 1791112000, 1791112000, false)};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.empty() && g_last_status == "ok_empty",
        "untrusted geo-only batch remains successful no-POST no-op");
}
#if ENABLE_LOGGING
void geoRetry() {
    reset();
    const int64_t now = rtc.epoch;
    const double observed = now - GEO_MAX_SEND_AGE_SECONDS;
    auto driving = geoFrame(now - 1, observed - 1, observed - 1);
    for (auto& value : driving) if (value.first == "is_parked") value.second = 0;
    obd_result_vector = {geoFrame(now, observed, observed), driving};
    const auto expected_core = json::parse(bulkStringV2(obd_result_vector));
    saveUploadBacklog();
    const auto persisted = LittleFS.contents();
    obd_result_vector.clear(); loadUploadBacklog();
    const auto restored = obd_result_vector;
    network.code = 503;
    check(!uploadBuffered(true) && obd_result_vector == restored, "failed public restored-backlog upload retains exact numeric FIFO/provenance");
    check(json::parse(network.sent.back())[0].contains("location") && json::parse(network.sent.back())[0].contains("heading"), "queued cutoff geo eligible on first failed send");
    rtc.epoch = now + 1;
    check(!uploadBuffered(true) && obd_result_vector == restored, "expired retry retains queue and provenance");
    for (const auto& point : json::parse(network.sent.back())) check(!point.contains("location") && !point.contains("heading"), "retry re-evaluates elapsed queue time, not frame time");
    saveUploadBacklog();
    check(LittleFS.contents() == persisted, "failed age-pruned send resaves original backlog observation UTC exactly");
    rtc.epoch = 0;
    check(!uploadBuffered(true) && obd_result_vector == restored, "unknown-clock retry retains exact restored source");
    for (const auto& point : json::parse(network.sent.back())) check(!point.contains("location") && !point.contains("heading"), "unknown-clock retry omits retained geo provenance");
    rtc.epoch = now - 1;
    check(!uploadBuffered(true) && obd_result_vector == restored, "inconsistent backward-clock retry retains exact source");
    for (const auto& point : json::parse(network.sent.back())) check(!point.contains("location") && !point.contains("heading"), "future sync metadata makes backward-clock retry untrusted");
    setClock(CLK_HTTP, now - 1);
    check(!uploadBuffered(true) && obd_result_vector == restored, "backward clock correction failed retry retains source");
    for (const auto& point : json::parse(network.sent.back())) check(point.contains("location") && point.contains("heading"), "backward correction makes retained geo eligible again");
    rtc.epoch = now + 1; network.code = 200;
    const auto start = network.sent.size();
    check(uploadBuffered(true) && obd_result_vector.empty(), "restored retry succeeds and drains FIFO");
    json received = json::array();
    for (size_t i = start; i < network.sent.size(); ++i) for (const auto& point : json::parse(network.sent[i])) received.push_back(point);
    check(received.size() == expected_core.size(), "all restored battery frames arrive once after success");
    for (size_t i = 0; i < received.size(); ++i) check(received[i] == withoutGeo(expected_core[i]), "restored old/fresh battery and PARK/DRIVE/charging fields survive in FIFO order");
    reset(); rtc.epoch = now;
    const auto queued = geoFrame(now, observed, observed);
    network.code = 503;
    check(!logNetwork({queued}), "direct first send fails");
    rtc.epoch = now + 1;
    check(!logNetwork({queued}) && !json::parse(network.sent.back())[0].contains("location"), "default direct logNetwork retry also refreshes clock");
}
#endif
void geoSizing() {
    reset();
    const int64_t now = rtc.epoch;
    const double old = now - GEO_MAX_SEND_AGE_SECONDS - 1;
    auto rich = geoFrame(now, old, old);
    const std::vector<std::string> fields = {"soh", "soe", "capacity", "kwh_charged", "power", "hvac_power", "voltage",
        "batt_temp", "batt_temp_min", "batt_temp_max", "ext_temp", "cabin_temp", "odometer", "est_battery_range", "speed", "elevation"};
    bool found = false;
    for (const auto& field : fields) {
        rich.emplace_back(field, 23);
        const auto unfiltered = bulkStringV2({rich}, now - GEO_MAX_SEND_AGE_SECONDS);
        const auto filtered = bulkStringV2({rich}, now);
        if (unfiltered.size() > CELL_MAX_POST_BYTES && filtered.size() <= CELL_MAX_POST_BYTES) { found = true; break; }
    }
    check(found, "oversize fixture straddles real modem cap only because of expired geo");
    obd_result_vector = {rich};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.size() == 1,
        "expiry-aware sizing must not drop deliverable battery as oversized");
    check(network.sent[0] == bulkStringV2({rich}, now) && network.sent[0].size() <= CELL_MAX_POST_BYTES, "sizing and final expiry-aware singleton body agree");
    const auto cutoff = geoFrame(now, now - GEO_MAX_SEND_AGE_SECONDS, now - GEO_MAX_SEND_AGE_SECONDS);
    for (bool compressed : {false, true}) {
        reset(); gzip_enabled = compressed;
        const auto raw = bulkStringV2({cutoff}, now);
        std::string wire;
        if (!gzipBody(raw, wire)) wire = raw;
        obd_result_vector = {cutoff}; clock_after_gzip = now + 1;
        check(uploadBuffered(true) && network.sent.size() == 1 && network.sent[0] == wire,
            "sizing/final body share snapshot across cutoff during compression");
    }
    for (bool compressed : {false, true}) {
        for (bool initially_trusted : {false, true}) {
            reset(); gzip_enabled = compressed;
            last_clock_sync_epoch = initially_trusted ? now : 0;
            const auto raw = bulkStringV2({cutoff}, initially_trusted ? now : 0);
            std::string wire;
            if (!gzipBody(raw, wire)) wire = raw;
            sync_after_gzip = initially_trusted ? 0 : now;
            obd_result_vector = {cutoff};
            check(uploadBuffered(true) && network.sent.size() == 1 && network.sent[0] == wire,
                "clock-quality changes during sizing cannot change final captured eligible policy");
        }
    }
    reset(); last_clock_sync_epoch = 0; sync_after_gzip = now;
    auto rich_fresh = rich;
    for (auto& value : rich_fresh) if (isGeoMetaKey(value.first)) value.second = now;
    obd_result_vector = {rich_fresh};
    check(uploadBuffered(true) && network.sent.size() == 1 && network.sent[0] == bulkStringV2({rich_fresh}, 0) &&
        network.sent[0].size() <= CELL_MAX_POST_BYTES,
        "new sync during sizing must not expand initially untrusted final body beyond modem cap");
    reset();
    obd_result_vector = {rich}; clock_after_gzip = now - 1;
    check(uploadBuffered(true) && network.sent.size() == 1 && network.sent[0] == bulkStringV2({rich}, now) && network.sent[0].size() <= CELL_MAX_POST_BYTES,
        "backward clock change during sizing cannot expand final payload past cap");
    reset();
    for (unsigned i = 0; i < MAX_FRAMES_PER_POST + 1; ++i) obd_result_vector.push_back(geoFrame(now + i, now - GEO_MAX_SEND_AGE_SECONDS, now - GEO_MAX_SEND_AGE_SECONDS));
    const auto original = obd_result_vector;
    network.response_epochs.push_back(now + 1);
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.size() > 1, "multiple bounded batches drain with server clock correction");
    size_t received = 0;
    for (size_t batch = 0; batch < network.sent.size(); ++batch) {
        check(network.sent[batch].size() <= CELL_MAX_POST_BYTES, "all final drained payloads respect real cap");
        for (const auto& point : json::parse(network.sent[batch])) {
            check(point.contains("location") == (batch == 0) && point.contains("heading") == (batch == 0), "each drained batch uses current corrected clock");
            check(withoutGeo(point) == withoutGeo(json::parse(bulkStringV2({original[received]}))[0]), "bounded batch FIFO battery/state timestamps unchanged");
            ++received;
        }
    }
    check(received == original.size(), "all bounded FIFO frames delivered once");
    for (bool restore_trust : {false, true}) {
        reset(); obd_result_vector = original;
        last_clock_sync_epoch = restore_trust ? 0 : now;
        if (restore_trust) network.response_epochs.push_back(now);
        else sync_after_gzip = 0;
        check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.size() > 1,
            "multiple batches drain while clock trust changes");
        size_t index = 0;
        for (size_t batch = 0; batch < network.sent.size(); ++batch) {
            for (const auto& point : json::parse(network.sent[batch])) {
                const bool eligible = restore_trust ? batch > 0 : batch == 0;
                check(point.contains("location") == eligible && point.contains("heading") == eligible,
                    "new batch re-evaluates sync trust rather than reusing earlier batch policy");
                check(withoutGeo(point) == withoutGeo(json::parse(bulkStringV2({original[index]}, 1791112000))[0]),
                    "clock trust changes preserve numeric FIFO battery state and original UTC");
                ++index;
            }
        }
        check(index == original.size(), "clock-quality batch transitions deliver every battery frame once");
    }
}
int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
#if ENABLE_LOGGING
    if (group == "all" || group == "fingerprint") fingerprint();
    if (group == "all" || group == "errors") safeErrors();
#endif
    if (group == "all" || group == "queue") queue();
    if (group == "all" || group == "geo-expiry") { geoExpiry(); geoClockQuality(); geoExplicitClock(); geoBoundaries(); }
    if (group == "all" || group == "geo-retry" || group == "geo-sizing") geoPublicClockReads();
    if (group == "all" || group == "geo-retry") geoTrustRetry();
#if ENABLE_LOGGING
    if (group == "all" || group == "geo-retry") geoRetry();
#endif
    if (group == "all" || group == "geo-sizing") geoSizing();
    std::cout << "PASS upload helpers " << group << " logging=" << ENABLE_LOGGING << '\n';
}
