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
#define MAX_FRAMES_PER_POST 12
#define CELL_MAX_POST_BYTES 1024
#define UPLOAD_MAX_BATCHES_PER_CYCLE 3
#define MAX_UPLOAD_BUFFER 120

struct RTCStub { unsigned long getEpoch() { return 1791112000; } } rtc;
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
struct String {
    const char* c_str() const { return ""; }
    void trim() {}
    size_t length() const { return 0; }
};
struct File {
    operator bool() const { return false; }
    bool available() { return false; }
    void println(const char*) {}
    String readStringUntil(char) { return {}; }
    void close() {}
};
struct FSStub {
    File open(const char*, const char*) { return {}; }
    bool exists(const char*) { return false; }
    void remove(const char*) {}
} LittleFS;
struct ESPStub {
    unsigned long getFreeHeap() { return 80000; }
    unsigned long getMaxAllocHeap() { return 40000; }
} ESP;
bool g_ble_up = false;
void esp_task_wdt_reset() {}
enum ClockSource { CLK_HTTP };
time_t parseHttpDate(const char*) { return 0; }
void setClock(ClockSource, time_t) {}
#include "telemetry_format.h"
#include "geo_backlog.h"
int gzip_calls = 0;
bool gzip_enabled = false, force_oversized = false;
bool gzipBody(const std::string& raw, std::string& wire) {
    ++gzip_calls;
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
int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
#if ENABLE_LOGGING
    if (group == "all" || group == "fingerprint") fingerprint();
    if (group == "all" || group == "errors") safeErrors();
#endif
    if (group == "all" || group == "queue") queue();
    std::cout << "PASS upload helpers " << group << " logging=" << ENABLE_LOGGING << '\n';
}
