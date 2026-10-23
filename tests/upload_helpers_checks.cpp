#define PRINT 1
#include "upload_transport_fixture.h"
#include <cstdarg>
#include <ctime>
#include <algorithm>
#include <limits>
#include <sstream>
#include "networkwrapper.h"
#include "cellwrapper.h"
#include "header_budget.inc"

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
enum ResetReason { ESP_RST_POWERON, ESP_RST_DEEPSLEEP };
ResetReason reset_reason = ESP_RST_POWERON;
ResetReason esp_reset_reason() { return reset_reason; }
#include "telemetry_format.h"
#include "geo_backlog.h"
#include "gzip_fake.h"
int gzip_calls = 0;
bool gzip_enabled = false, force_oversized = false;
// Overrides the outcome: scripted per call (front first), else on every call.
GzipOutcome gzip_outcome = GZIP_OK;
std::deque<GzipOutcome> gzip_script;
std::vector<std::string> gzip_inputs;
std::vector<size_t> gzip_inputs_at_post;
int64_t clock_after_gzip = 0;
time_t sync_after_gzip = -1;
GzipOutcome gzipBody(const std::string& raw, std::string& wire) {
    ++gzip_calls;
    gzip_inputs.push_back(raw);
    if (clock_after_gzip) rtc.epoch = clock_after_gzip;
    if (sync_after_gzip >= 0) last_clock_sync_epoch = sync_after_gzip;
    GzipOutcome outcome = gzip_outcome;
    if (!gzip_script.empty()) { outcome = gzip_script.front(); gzip_script.pop_front(); }
    if (outcome != GZIP_OK) return outcome;
    if (!gzip_enabled) return GZIP_UNAVAILABLE;
    wire.assign(force_oversized ? 1025 : raw.size() / 5 + 16, '\0');
    wire[0] = '\x1f'; wire[1] = '\x8b'; wire[2] = '\x08'; wire[3] = '\r'; wire[4] = '\n';
    for (size_t i = 5; i < wire.size(); ++i) wire[i] = raw[(i - 5) % raw.size()];
    return GZIP_OK;
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
    bool headers_ok = true;
    bool set_headers(HeaderVector h) override { headers.push_back(h); return headers_ok; }
    bool connect() override { return true; }
    bool connected() override { return true; }
    void disconnect() override {}
    HttpResponse getEpoch() override { return {0, ""}; }
    HttpResponse sendBulk(std::string payload) override {
        sent.push_back(payload);
        gzip_inputs_at_post.push_back(gzip_inputs.size());
        int status = code;
        if (!codes.empty()) { status = codes.front(); codes.pop_front(); }
        uart.replies = {{1, ">"}, {ack, ack == 1 ? "\r\nOK" : "\r\nERROR"},
            {status ? 1 : 0, status ? "+SHREQ: \"POST\"," + std::to_string(status) + "," + std::to_string(body.size()) + "\r\n" : ""}};
        if (status && !body.empty()) uart.replies.push_back({1, "+SHREAD: " + std::to_string(std::min<size_t>(body.size(), 496)) + "\r\n" + body.substr(0, 496)});
        auto response = client.send("/2/app/tlm", payload.data(), payload.size());
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
unsigned long upload_capped = 0, upload_spilled = 0;
#include "tlm_signature.h"
#include "network_upload.h"
void pushToken(json newConfig) {
#include "token_push.inc"
}

void reset() {
    rtc.epoch = 1791112000; rtc.reads = 0;
    rtc.scripted = false; rtc.scripted_epochs.clear();
    last_clock_sync_epoch = rtc.epoch;
    clock_after_gzip = 0; sync_after_gzip = -1;
    LittleFS.files.clear(); LittleFS.fail_open = false;
    network.response_epochs.clear();
    network.sent.clear(); network.headers.clear(); network.codes.clear(); network.uart = CFreematics{};
    network.code = 200; network.ack = 1; network.body = "{}"; network.unknown_diagnostics = false; network.headers_ok = true;
    tlm_push_token = "test-token-actual";
    datalogger.lines.clear(); Serial.output.clear(); gzip_calls = 0;
    gzip_enabled = force_oversized = false;
    gzip_outcome = GZIP_OK; gzip_script.clear(); gzip_inputs.clear(); gzip_inputs_at_post.clear();
    gzip_noted_failures = 0; g_upload_held.clear();
    logging_errors = g_last_http = g_http0_streak = 0; g_last_status.clear(); obd_result_vector.clear();
    host_md_fail = 0;
    g_geo_accepted = g_geo_queued = {}; reset_reason = ESP_RST_POWERON;
#if ENABLE_LOGGING
    g_upload_request_id = 0;
#endif
}
void sha256(const std::string& bytes, unsigned char digest[32]) {
    check(mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const unsigned char*>(bytes.data()),
                     bytes.size(), digest) == 0, "host SHA256");
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
    sha256(payload, digest);
    char text[13] = {};
    for (size_t i = 0; i < 6; ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
    return text;
}
void fingerprint() {
    for (bool compressed : {false, true}) {
        reset(); gzip_enabled = compressed;
        check(logNetwork({{{"utc", 1791111990}, {"_private", 1}}}), "empty serialization success");
        check(network.sent.empty() && g_upload_request_id == 0 && datalogger.lines.empty(), "no request ID hash or event for unsent empty serialization");
        const std::vector<OBDVector> frames = {frame(0), frame(10), {{"utc", 1791111990}, {"_private", 1}}};
        const auto raw = bulkStringV2(frames);
        network.code = 400; network.ack = 0; network.body = "{\"message\":\"gzip: invalid header\"}";
        check(!logNetwork(frames), "400 rejected without changing acceptance");
        check(!logNetwork(frames), "identical retry still rejected");
        auto bodies = events("UPLOAD body raw="); auto results = events("UPLOAD id=");
        check(bodies.size() == 2 && results.size() == 2 && datalogger.lines.size() == 4, "exactly two diagnostic events per POST");
        check(network.sent[0] == network.sent[1], "identical retry sends identical bytes, gzip binary NULs included");
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
        // The runner checks the fingerprint the firmware logged against Python's own hashlib of the bytes sent.
        const auto logged = events("UPLOAD body raw=").back();
        const auto at = logged.find("sha=") + 4;
        std::cout << "HASH " << logged.substr(at, logged.find(' ', at) - at) << ' ';
        for (unsigned char c : network.sent.back()) std::printf("%02x", c);
        std::cout << '\n';
    }
    reset(); host_md_fail = 1;
    check(!logNetwork({frame(0)}) && network.sent.empty() && g_last_status == "sign_fail",
        "without the body digest nothing can be signed, so nothing is sent");
    check(events("UPLOAD not sent: sign_fail").size() == 1, "the reason is logged");
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
        {"unauthorized", "auth"},
        {"parameter \"X-TLM-TOKEN\" in header has an error: value is required but missing", "missing_header"},
        {"X-TLM-TOKEN value is required but missing " + tlm_push_token, "redacted"}, {"validation error", "validation"}, {"", "empty"},
        {"gzip: invalid header " + tlm_push_token, "redacted"},
        {"gzip: invalid checksum " + api_key, "redacted"},
        {"{\"message\":\"checksum\",\"TOKEN\":\"small\"}", "redacted"},
        {"{\"api_key\":\"short\",\"message\":\"invalid header\"}", "redacted"},
        {"Bearer abc.def.ghi", "redacted"},
        {"gzip: checksum 12345678-abcd-abcd-abcd-123456789abc", "redacted"},
        {"gzip: invalid header " + std::string(300, ' ') + tlm_push_token, "redacted"},
        {"gzip: invalid header " + std::string(300, ' ') + "api-key=short", "redacted"},
        {"{\"message\":\"TLM point time is too old (2026-09-01 10:00:00 +0000 UTC)\"}", "too_old"},
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
    check(events("UPLOAD body raw=")[0].find(" loc=") == std::string::npos, "counts that do not fit whole are left out, never clipped");
    reset(); g_upload_request_id = 41;
    check(logNetwork({frame(0)}) && events("UPLOAD body raw=")[0].find(" loc=0 ldup=0") != std::string::npos, "counts that fit are logged");
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
    check(g_upload_request_id == network.sent.size(), "sizing compressions have no request ID hash or diagnostic body");
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
    check(events("UPLOAD body raw=").size() == network.sent.size() && events("UPLOAD id=").size() == network.sent.size(), "exactly one final fingerprint and paired events per real queue POST");
#endif
    reset(); gzip_enabled = true; force_oversized = true; obd_result_vector = {frame(0)};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.empty(), "existing oversized single-frame drop unchanged");
#if ENABLE_LOGGING
    check(g_upload_request_id == 0, "drop has no POST diagnostics");
#endif
}
// A relay session parks the queue in flash and hands it back in order; a new token drops it from both (#58).
void bleHandover() {
    reset();
    auto utcs = [] { std::vector<double> u; for (const auto& f : obd_result_vector) for (const auto& kv : f) if (kv.first == "utc") u.push_back(kv.second); return u; };
    const std::vector<double> parked_then_new{1791112000, 1791112001, 1791112002};
#if ENABLE_LOGGING
    obd_result_vector = {frame(0), frame(1)};
    spillUploadBacklog();
    check(obd_result_vector.empty() && upload_spilled == 2, "a relay session parks the queue in flash");
    obd_result_vector = {frame(2)};
    loadUploadBacklog();
    check(utcs() == parked_then_new && upload_spilled == 0 && !LittleFS.exists("/upload_backlog.jsonl"),
          "the parked frames come back ahead of the ones queued since");
    obd_result_vector = {frame(0), frame(1)};
    spillUploadBacklog();
    obd_result_vector = {frame(2)}; // the trip-end marker of a sleep during the session
    check(uploadBuffered(true) && network.sent.empty() && obd_result_vector.size() == 1,
          "nothing goes up ahead of the parked frames");
    saveUploadBacklog();
    obd_result_vector.clear();
    loadUploadBacklog();
    check(utcs() == parked_then_new, "a sleep during the session saves behind them, and the next wake restores all");
    LittleFS.fail_open = true;
    spillUploadBacklog();
    check(obd_result_vector.size() == 3 && upload_spilled == 0, "a queue flash refused stays in RAM");
    LittleFS.fail_open = false;
    upload_capped = 3; saveUploadBacklog(); saveUploadBacklog();
    check(events("UPLOAD discarded n=3 reason=cap").size() == 1 && events("UPLOAD discarded").size() == 1,
          "a capped stretch logs once, before the backlog is saved");
    LittleFS.files.clear();
    obd_result_vector.clear();
    for (unsigned i = 0; i < MAX_UPLOAD_BUFFER; ++i) obd_result_vector.push_back(frame(i));
    spillUploadBacklog();
    obd_result_vector = {frame(MAX_UPLOAD_BUFFER)};
    upload_capped = 0; saveUploadBacklog(); obd_result_vector.clear(); loadUploadBacklog();
    check(utcs().size() == MAX_UPLOAD_BUFFER && utcs().front() == 1791112001 &&
          utcs().back() == 1791112000 + MAX_UPLOAD_BUFFER && upload_capped == 1,
          "over the cap the oldest parked frame goes, not the trip-end marker");
    upload_capped = 0; obd_result_vector.clear(); LittleFS.files.clear();
    obd_result_vector = {frame(0)};
    spillUploadBacklog();
#endif
    obd_result_vector.push_back(frame(1));
    pushToken({{"tlm_push_token", tlm_push_token}});
    check(obd_result_vector.size() == 1 && upload_spilled == ENABLE_LOGGING &&
          LittleFS.exists("/upload_backlog.jsonl") == ENABLE_LOGGING, "a re-push for the same vehicle keeps the queue, parked or not");
    const std::string token = tlm_push_token;
    pushToken({{"tlm_push_token", "other-vehicle"}});
    check(obd_result_vector.empty() && !LittleFS.exists("/upload_backlog.jsonl") && upload_spilled == 0 &&
          tlm_push_token == "other-vehicle", "a new token drops what was polled under the old one, parked or not");
#if ENABLE_LOGGING
    check(events("UPLOAD discarded n=2 reason=token").size() == 1, "and says how much");
#endif
    tlm_push_token = token;
}
// On a trusted send clock the queue drops frames past the 6-day bound before sizing; otherwise it keeps them.
OBDVector battery(double utc) { return {{"utc", utc}, {"soc", 40}, {"voltage", 350}}; }
void frameAge() {
    const double now = 1791112000, old = now - BACKLOG_MAX_SEND_AGE_SECONDS - 1;
    OBDVector restamped = battery(now);
    restamped.insert(restamped.begin(), {"utc", old}); // the serializer sends the last utc
    const std::vector<OBDVector> mixed = {battery(old), {{"soc", 41}}, battery(now - BACKLOG_MAX_SEND_AGE_SECONDS),
        battery(now + 300), restamped};
    const std::vector<OBDVector> kept(mixed.begin() + 1, mixed.end());
    reset(); obd_result_vector = mixed;
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent == std::vector<std::string>{bulkStringV2(kept, now)},
        "trusted clock drops the frame past 6 days; the bound, a future and a fresh frame go up");
#if ENABLE_LOGGING
    check(events("UPLOAD dropped") == std::vector<std::string>{"UPLOAD dropped n=1 reason=age sutc=1790593599..1790593599 now=1791112000"},
        "only the aged frame is counted, not the one without a utc");
#endif
    reset(); obd_result_vector = mixed; last_clock_sync_epoch = 0;
    check(uploadBuffered(true) && network.sent == std::vector<std::string>{bulkStringV2(mixed, 0)},
        "an untrusted clock judges nothing: the aged frame still goes up");
#if ENABLE_LOGGING
    check(events("UPLOAD dropped").empty(), "nothing dropped, nothing logged");
#endif
    // Swept before the http=0 probe cap and over the whole queue, so the 3-frame probe is the fresh frames.
    reset(); g_http0_streak = 1; network.code = 503;
    const std::vector<OBDVector> fresh = {battery(now - 2), battery(now - 1), battery(now)};
    obd_result_vector = {battery(old), battery(old - 2)};
    obd_result_vector.insert(obd_result_vector.end(), fresh.begin(), fresh.end());
    obd_result_vector.push_back(battery(old - 1));
    check(!uploadBuffered(true) && network.sent == std::vector<std::string>{bulkStringV2(fresh, now)} && obd_result_vector == fresh,
        "a failed send keeps the fresh frames in order and the aged ones stay dropped");
#if ENABLE_LOGGING
    check(events("UPLOAD dropped") == std::vector<std::string>{"UPLOAD dropped n=3 reason=age sutc=1790593597..1790593599 now=1791112000"},
        "one line names the count, the dropped span and the judging clock");
#endif
    reset(); obd_result_vector = {battery(old - 5), battery(old)}; g_last_status = "http_err";
    check(uploadBuffered(false) && obd_result_vector.empty() && network.sent.empty() && g_last_status == "ok_aged",
        "an all-aged queue empties without a POST");
#if ENABLE_LOGGING
    reset(); obd_result_vector = {battery(old), battery(now)};
    saveUploadBacklog(); obd_result_vector.clear(); loadUploadBacklog();
    const auto restored = obd_result_vector;
    check(restored.size() == 2 && uploadBuffered(true) && network.sent == std::vector<std::string>{bulkStringV2({restored[1]}, now)},
        "a backlog restored from flash is judged at send like the RAM queue");
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
    check(gzipBody(bulkStringV2(first, now), first_wire) == GZIP_OK && first_wire.size() <= CELL_MAX_POST_BYTES &&
        gzipBody(bulkStringV2(second, 0), second_wire) == GZIP_OK && second_wire.size() <= CELL_MAX_POST_BYTES,
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
        if (gzipBody(raw, wire) != GZIP_OK) wire = raw;
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
            if (gzipBody(raw, wire) != GZIP_OK) wire = raw;
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
    // A distinct fix per frame, so send-once never hides each batch's own clock.
    for (unsigned i = 0; i < MAX_FRAMES_PER_POST + 1; ++i) {
        auto source = geoFrame(now + i, now - GEO_MAX_SEND_AGE_SECONDS, now - GEO_MAX_SEND_AGE_SECONDS);
        for (auto& value : source) {
            if (value.first == "heading") value.second = 10 + i;
            if (value.first == "lat") value.second = 59.32 + i * 0.001;
        }
        obd_result_vector.push_back(source);
    }
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
// A location or heading goes up once per new GPS observation, and counts as sent only on a 200.
OBDVector dedupFix(double utc, double loc_utc, double head_utc, double lat = 59.32, double lon = 18.06, double heading = 90) {
    return {{"utc", utc}, {"soc", 60}, {"is_parked", 1}, {"lat", lat}, {"lon", lon}, {"heading", heading},
        {"__geo_loc_utc", loc_utc}, {"__geo_head_utc", head_utc}};
}
json lastPoint() { return json::parse(network.sent.back())[0]; }
void geoResetFromSetup() {
#include "geo_reset.inc"
}
void geoDedup() {
    const double now = 1791112000, seen = now - 60;
    reset();
    const std::vector<OBDVector> same = {dedupFix(now - 2, seen, seen), dedupFix(now - 1, seen, seen), dedupFix(now, seen, seen)};
    const auto points = json::parse(bulkStringV2(same, now));
    check(points.size() == 3, "every frame of a repeated fix keeps its point");
    for (size_t i = 0; i < points.size(); ++i) {
        check(points[i].contains("location") == (i == 0) && points[i].contains("heading") == (i == 0),
            "a fix repeated within one batch is sent on its first point only");
        check(points[i]["soc"]["time"] == v2Time(now - 2 + i) && points[i]["drivingState"]["state"] == "PARK",
            "battery and state stay on every point");
    }
    check(points[0]["location"]["time"] == v2Time(seen) && points[0]["location"]["lat"] == 59.32 &&
        points[0]["heading"]["time"] == v2Time(seen), "the one location and heading keep their observation times");
    check(logNetwork(same), "repeated-fix batch accepted");
#if ENABLE_LOGGING
    check(events("UPLOAD body raw=").back().find(" loc=1 ldup=2") != std::string::npos, "UPLOAD body counts sent and suppressed locations");
#endif
    check(logNetwork({dedupFix(now + 1, seen, seen)}), "next batch accepted");
    check(!lastPoint().contains("location") && !lastPoint().contains("heading") && lastPoint()["soc"]["frac"] == 0.6,
        "an accepted location and heading are not sent again, the battery is");
#if ENABLE_LOGGING
    check(events("UPLOAD body raw=").back().find(" loc=0 ldup=1") != std::string::npos, "a suppressed resend is counted");
#endif

    reset(); obd_result_vector = {dedupFix(now, seen, seen)}; network.code = 503;
    check(!uploadBuffered(true) && lastPoint().contains("location"), "first buffered attempt carries the fix");
    check(!uploadBuffered(true) && lastPoint().contains("location") && lastPoint().contains("heading"),
        "neither the sizing pass nor a failed POST marks the fix sent");
    network.code = 200;
    check(uploadBuffered(true) && lastPoint().contains("location") && lastPoint().contains("heading"), "the retry carries it");
    obd_result_vector = {dedupFix(now + 1, seen, seen)};
    check(uploadBuffered(true) && !lastPoint().contains("location") && !lastPoint().contains("heading"), "and once accepted, it is not sent again");

    reset(); network.code = 0;
    check(!logNetwork({dedupFix(now, seen, seen)}), "unanswered upload fails");
    network.code = 200;
    check(logNetwork({dedupFix(now, seen, seen)}) && lastPoint().contains("location") && lastPoint().contains("heading"),
        "an unanswered upload leaves the fix owed");
    // No ok_empty check: an empty body emitted nothing, so its marks equal the accepted ones by construction.
    reset(); last_clock_sync_epoch = 0;
    check(logNetwork({dedupFix(now, seen, seen)}) && !lastPoint().contains("location") && lastPoint().contains("soc"),
        "an untrusted clock sends the battery without the fix");
    setClock(CLK_HTTP, now);
    check(logNetwork({dedupFix(now + 1, seen, seen)}) && lastPoint().contains("location") && lastPoint().contains("heading"),
        "the withheld fix is sent once the clock is trusted");
    check(logNetwork({dedupFix(now + 2, seen, seen)}) && !lastPoint().contains("location"), "and only once");

    reset();
    check(logNetwork({dedupFix(now, seen, seen)}), "fix accepted");
    check(logNetwork({dedupFix(now + 1, seen + 1, seen)}) && lastPoint()["location"]["time"] == v2Time(seen + 1) &&
        !lastPoint().contains("heading"), "a new location observation time is sent, the old heading is not");
    check(logNetwork({dedupFix(now + 2, seen + 1, seen, 59.33)}) && lastPoint()["location"]["lat"] == 59.33,
        "new coordinates with an unchanged observation time are sent");
    check(logNetwork({dedupFix(now + 3, seen + 1, seen, 59.33)}) && !lastPoint().contains("location"), "then not again");
    check(logNetwork({dedupFix(now + 4, seen + 1, seen + 5, 59.33)}) && lastPoint()["heading"]["time"] == v2Time(seen + 5) &&
        !lastPoint().contains("location"), "a new heading observation time is sent");
    check(logNetwork({dedupFix(now + 5, seen + 1, seen + 5, 59.33, 18.06, 180)}) && lastPoint()["heading"]["degrees"] == 180,
        "a new heading with an unchanged observation time is sent");
    check(logNetwork({dedupFix(now + 6, seen + 1, seen + 5, 59.33, 18.06, 180)}) && !lastPoint().contains("heading"), "then not again");

    reset(); g_http0_streak = 1; // three-frame probe batches
    obd_result_vector = {dedupFix(now - 4, seen, seen), dedupFix(now - 3, seen, seen), dedupFix(now - 2, seen, seen),
        dedupFix(now - 1, seen, seen), dedupFix(now, seen + 30, seen + 30)};
    network.codes = {200, 503};
    check(!uploadBuffered(true) && obd_result_vector.size() == 2 && network.sent.size() == 2, "batch 1 accepted, batch 2 failed");
    auto second = json::parse(network.sent[1]);
    check(json::parse(network.sent[0])[0].contains("location") && !second[0].contains("location") && !second[0].contains("heading") &&
        second[1]["location"]["time"] == v2Time(seen + 30), "batch 2 omits the fix batch 1 delivered and carries the new one");
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent[2] == network.sent[1],
        "the retry does not resend batch 1's fix and still carries the unaccepted one");

    reset();
    const auto accepted_fix = dedupFix(seen, seen, seen);
    check(logNetwork({accepted_fix}), "fix accepted");
    auto big = dedupFix(now, seen, seen);
    bool straddles = false;
    for (int extra = 0; extra < 40 && !straddles; ++extra) {
        big.emplace_back("_pad" + std::to_string(extra), 1);
        big.emplace_back(extra % 2 ? "cabin_temp" : "batt_temp_min", 20 + extra);
        for (const char* field : {"soh", "soe", "capacity", "kwh_charged", "power", "hvac_power", "voltage", "batt_temp",
                "batt_temp_max", "ext_temp", "odometer", "est_battery_range", "speed", "elevation"}) {
            if (std::none_of(big.begin(), big.end(), [&](const auto& v) { return v.first == field; })) { big.emplace_back(field, 23); break; }
        }
        GeoMarks marks = g_geo_accepted;
        straddles = bulkStringV2({big}, now).size() > CELL_MAX_POST_BYTES &&
            bulkStringV2({big}, now, &marks).size() <= CELL_MAX_POST_BYTES;
    }
    check(straddles, "fixture is oversized only because of an already-accepted fix");
    obd_result_vector = {big};
    check(uploadBuffered(true) && network.sent.size() == 2 && !lastPoint().contains("location"),
        "sizing uses the accepted mark, so the frame is sent, not dropped as oversized");

    reset();
    check(logNetwork({dedupFix(now, seen, seen)}), "fix accepted");
    reset_reason = ESP_RST_DEEPSLEEP; geoResetFromSetup();
    check(logNetwork({dedupFix(now + 1, seen, seen)}) && !lastPoint().contains("location") && !lastPoint().contains("heading"),
        "a deep-sleep wake keeps the accepted mark");
    reset_reason = ESP_RST_POWERON; geoResetFromSetup();
    check(logNetwork({dedupFix(now + 2, seen, seen)}) && lastPoint().contains("location") && lastPoint().contains("heading"),
        "any other boot sends the fix once more");
}
std::string hex(const std::string& bytes) {
    std::string out;
    char pair[3];
    for (unsigned char c : bytes) { std::snprintf(pair, sizeof(pair), "%02x", c); out += pair; }
    return out;
}
// The runner recomputes all three values from the token and the exact body bytes with Python's
// hashlib/hmac/base64 and fails on any difference.
void pythonRecomputes(const std::string& token, const std::string& wire, const std::string& digest,
                      const std::string& input, const std::string& signature) {
    std::cout << "SIGCHECK " << hex(token) << ' ' << hex(wire) << ' ' << hex(digest) << ' ' << hex(input) << ' '
              << hex(signature) << std::endl;
}
std::string header(const HeaderVector& headers, const std::string& name) {
    std::string value;
    int count = 0;
    for (const auto& h : headers) if (h.first == name) { value = h.second; ++count; }
    check(count <= 1, "header " + name + " at most once");
    return value;
}
const std::string VECTOR_UUID = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
// loop()'s stale-bearer bookkeeping after a failed upload, extracted from logger.ino.
void streakAfterFailedUpload() {
#include "upload_streak.inc"
}
void signing() {
    // The shared vector, byte for byte (the same strings are hard-coded in the backend's tests).
    const std::string body = "[{\"utc\":1760360000,\"soc\":50}]";
    unsigned char digest[32];
    sha256(body, digest);
    TlmSignatureHeaders h;
    check(tlmSignatureHeaders(VECTOR_UUID, digest, h), "shared vector signs");
    check(h.content_digest == "sha-256=:04AX1We39fbKv2dKjfojft1+HxpZdbfMK2oFVt8xWIQ=:", "vector Content-Digest: " + h.content_digest);
    check(h.signature_input == "s=(\"content-digest\");keyid=\"40ed71cc50fb44c60cf82ea1061807ec\"",
          "vector Signature-Input: " + h.signature_input);
    check(h.signature == "s=:4f9JYu+fOS6wvT/Yn7ZhG1zEQjk0fDBzJvVEXXZ06cg=:", "vector Signature: " + h.signature);
    pythonRecomputes(VECTOR_UUID, body, h.content_digest, h.signature_input, h.signature);
    check(!tlmSignatureHeaders("", digest, h), "no token, no signature");
    host_md_fail = 1;
    check(!tlmSignatureHeaders(VECTOR_UUID, digest, h), "a crypto failure is reported, not signed");
    host_md_fail = 0;

    // What actually goes up: no token header, the signature covers the exact wire bytes, and the set fits
    // the modem's budget with a production-length (36-character) api key.
    const std::string saved_key = api_key;
    for (bool compressed : {false, true}) {
        reset(); gzip_enabled = compressed; tlm_push_token = VECTOR_UUID; api_key = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
        check(logNetwork({frame(0), frame(5)}), "signed upload accepted");
        check(network.sent.size() == 1 && network.headers.size() == 1, "one POST, one header set");
        const auto& sent = network.headers[0];
        for (const auto& x : sent) {
            check(x.first != "X-TLM-TOKEN", "the token header is never sent");
            check(x.second.find(VECTOR_UUID) == std::string::npos, "no header carries the token");
        }
        const size_t wire = headerBytes(sent);
        check(header(sent, "User-Agent") == "abrp-obd/0.0.0", "best-effort User-Agent from OTA_FW_VERSION: " + header(sent, "User-Agent"));
        check(wire <= SIM7070_HEADER_BUDGET, "headers within SIM7070_HEADER_BUDGET: " + std::to_string(wire));

        // The boundary: a User-Agent that exactly fills the budget is sent, one byte more is left off and
        // everything the request needs is still there.
        const HeaderVector mandatory(sent.begin(), sent.end() - 1);
        const std::string ua_line = "User-Agent: " + header(sent, "User-Agent") + "\r\n";
        const size_t room = SIM7070_HEADER_BUDGET - (wire - ua_line.size()) - std::strlen("User-Agent: \r\n");
        HeaderVector fits = mandatory, over = mandatory;
        appendUserAgent(fits, std::string(room, 'u'));
        appendUserAgent(over, std::string(room + 1, 'u'));
        check(fits.size() == mandatory.size() + 1, "a User-Agent that exactly fills the budget is still sent");
        check(over == mandatory, "one byte more is dropped, the signed headers untouched");
        check(header(sent, "X-API-KEY") == api_key && header(sent, "Content-Type") == "application/json", "attribution headers kept");
        check(header(sent, "Content-Encoding") == (compressed ? "gzip" : ""), "Content-Encoding only when compressed");
        check((network.sent[0].compare(0, 2, "\x1f\x8b") == 0) == compressed, "and then the body sent is the gzip stream");
        pythonRecomputes(VECTOR_UUID, network.sent[0], header(sent, "Content-Digest"), header(sent, "Signature-Input"),
                         header(sent, "Signature"));
    }
    api_key = saved_key;
    check(uploadUserAgent(1002003) == "abrp-obd/1.2.3" && uploadUserAgent(999999999) == "abrp-obd/999.999.999",
          "User-Agent carries the release version the way OTA_FW_VERSION encodes it");

    // Not sent: no token, or a header refused. The frames stay, and the stale-bearer ladder does not move.
    reset(); tlm_push_token.clear(); g_http0_streak = 2;
    check(!logNetwork({frame(0)}) && network.sent.empty() && g_last_status == "no_token", "empty token skips the POST");
    streakAfterFailedUpload();
    check(g_http0_streak == 2, "a local failure leaves the http=0 streak alone");
    reset(); network.headers_ok = false; g_http0_streak = 2;
    check(!logNetwork({frame(0)}) && network.sent.empty() && g_last_status == "hdr_fail", "refused headers abort the POST");
    streakAfterFailedUpload();
    check(g_http0_streak == 2, "a refused header leaves the http=0 streak alone");
    reset(); network.code = 0; g_http0_streak = 2;
    check(!logNetwork({frame(0)}) && network.sent.size() == 1, "a sent request that got no answer");
    streakAfterFailedUpload();
    check(g_http0_streak == 3, "an http=0 after sending still counts toward the restart");
    reset(); network.code = 400; g_http0_streak = 2;
    check(!logNetwork({frame(0)}), "a sent request the server rejected");
    streakAfterFailedUpload();
    check(g_http0_streak == 0, "an answer from the server clears the streak");
}
// A single frame whose plain JSON is over the modem cap, from mapped fields only.
OBDVector oversizedFrame() {
    OBDVector rich = frame(0);
    for (const char* field : {"soh", "soe", "capacity", "kwh_charged", "power", "hvac_power", "batt_temp", "batt_temp_min",
                              "batt_temp_max", "ext_temp", "cabin_temp", "odometer", "est_battery_range", "elevation"})
        rich.emplace_back(field, 12345.678901);
    check(bulkStringV2({rich}).size() > CELL_MAX_POST_BYTES, "fixture frame is over the cap as plain JSON");
    return rich;
}
void gzipFallback() {
    // Fails while sizing, or only on the send pass: no POST, nothing dropped, frames wait.
    for (bool on_send : {false, true}) {
        reset(); gzip_enabled = true;
        if (on_send) gzip_script = {GZIP_OK, GZIP_FAIL}; else gzip_outcome = GZIP_FAIL;
        for (unsigned i = 0; i < MAX_FRAMES_PER_POST; ++i) obd_result_vector.push_back(frame(i * 10));
        check(!uploadBuffered(true) && network.sent.empty() && obd_result_vector.size() == MAX_FRAMES_PER_POST,
            "a gzip failure sends nothing and keeps every frame");
        check(g_last_status == "gzip_fail" && g_upload_local_fail, "gzip failure is a local not-sent, not a bearer verdict");
    }
    // Sized for gzip, then no compressor on the send pass: the plain body is over the cap, keep the frames.
    reset(); gzip_enabled = true; gzip_script = {GZIP_OK, GZIP_UNAVAILABLE};
    for (unsigned i = 0; i < MAX_FRAMES_PER_POST; ++i) obd_result_vector.push_back(frame(i * 10));
    check(!uploadBuffered(true) && network.sent.empty() && obd_result_vector.size() == MAX_FRAMES_PER_POST &&
        g_last_status == "plain_too_big", "an over-cap plain body is never sent");
    // Off for the wake: a small frame still goes up, plain.
    reset(); gzip_outcome = GZIP_OFF;
    obd_result_vector = {frame(0)};
    check(uploadBuffered(true) && network.sent.size() == 1 && network.sent[0] == bulkStringV2({frame(0)}),
        "with gzip off a body that fits goes up plain");
    // A single frame over the cap as plain JSON is dropped only when gzip is gone for the whole wake.
    for (GzipOutcome outcome : {GZIP_FAIL, GZIP_UNAVAILABLE}) {
        reset(); gzip_outcome = outcome;
        obd_result_vector = {oversizedFrame()};
        const bool dropped = outcome == GZIP_UNAVAILABLE;
        check(uploadBuffered(true) == dropped && network.sent.empty() && obd_result_vector.size() == (dropped ? 0u : 1u),
            "an oversized frame is never dropped because gzip failed");
    }
    // Off for the wake: an oversized head frame is held for the backlog and the frames behind it still go.
    reset(); gzip_outcome = GZIP_OFF;
    const auto big = oversizedFrame();
    obd_result_vector = {big, frame(10), frame(20)};
    check(uploadBuffered(true) && obd_result_vector.empty() && network.sent.size() == 1 &&
        network.sent[0] == bulkStringV2({frame(10), frame(20)}), "frames behind a held frame are sent");
    check(g_upload_held.size() == 1 && g_upload_held[0] == big, "the oversized frame is held, not dropped");
#if ENABLE_LOGGING
    obd_result_vector = {frame(30)};
    saveUploadBacklog();
    obd_result_vector.clear();
    loadUploadBacklog();
    check(g_upload_held.empty() && obd_result_vector.size() == 2 && bulkStringV2({obd_result_vector[1]}) == bulkStringV2({frame(30)}) &&
        bulkStringV2({obd_result_vector[0]}) == bulkStringV2({big}), "held frames reach the backlog first, for the next wake");
#endif
    // The body sized is the body sent, byte for byte.
    reset(); gzip_enabled = true;
    for (unsigned i = 0; i < MAX_FRAMES_PER_POST + 3; ++i) obd_result_vector.push_back(frame(i * 10));
    check(uploadBuffered(false) && obd_result_vector.empty() && gzip_inputs_at_post.size() == 2, "batches drain");
    for (size_t at : gzip_inputs_at_post)
        check(at >= 2 && gzip_inputs[at - 1] == gzip_inputs[at - 2], "the sent body is the last body sized");
    // Every server gzip complaint about a gzip body counts against the wake's budget; others do not.
    for (const char* message : {"gzip: invalid checksum", "gzip: invalid header", "flate: corrupt input: gzip",
                                "unexpected EOF", "incorrect length check", "forbidden"}) {
        for (bool compressed : {true, false}) {
            reset(); gzip_enabled = compressed; network.code = 403;
            network.body = std::string("{\"message\":\"") + message + "\"}";
            obd_result_vector = {frame(0)};
            check(!uploadBuffered(true) && obd_result_vector.size() == 1, "rejected batch kept");
            const bool counts = compressed && std::string(message) != "forbidden";
            check(gzip_noted_failures == (counts ? 1u : 0u), message);
        }
    }
}

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
#if ENABLE_LOGGING
    if (group == "all" || group == "fingerprint") fingerprint();
    if (group == "all" || group == "errors") safeErrors();
#endif
    if (group == "all" || group == "queue") { queue(); bleHandover(); frameAge(); }
    if (group == "all" || group == "geo-expiry") { geoExpiry(); geoClockQuality(); geoExplicitClock(); geoBoundaries(); }
    if (group == "all" || group == "geo-retry" || group == "geo-sizing") geoPublicClockReads();
    if (group == "all" || group == "geo-retry") geoTrustRetry();
#if ENABLE_LOGGING
    if (group == "all" || group == "geo-retry") geoRetry();
#endif
    if (group == "all" || group == "geo-sizing") geoSizing();
    if (group == "all" || group == "geo-dedup") geoDedup();
    if (group == "all" || group == "signing") signing();
    if (group == "all" || group == "gzip-fallback") gzipFallback();
    std::cout << "PASS upload helpers " << group << " logging=" << ENABLE_LOGGING << '\n';
}
