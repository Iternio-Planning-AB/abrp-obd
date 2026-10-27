// Relay acks (#90): run by tests/run_relay_checks.py, which extracts the production pieces this file includes.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"

#define RTC_DATA_ATTR
#define PRINT 0
#define ENABLE_LOGGING 0
#define ENABLE_NETWORK 1
#define ENABLE_CELL 0
#define ENABLE_BLE 1
#include "../baseLogger/config.h"

using json = nlohmann::json;
#include "../libraries/OBDWrapper/obd_vector.h"
using HeaderVector = std::vector<std::pair<std::string, std::string>>;
#include "header_budget.inc"
#include "ble_write_summary.h"

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct ESPStub {
  uint64_t efuse_mac = 0xA1B2C3D4E5F6ull; // the fixtures' dongle id
  unsigned long getFreeHeap() const { return 4000000; }
  unsigned long getMaxAllocHeap() const { return 4000000; }
  unsigned long getMaxAllocPsram() const { return 0; }
  unsigned long getPsramSize() const { return 0; }
  unsigned long getFreePsram() const { return 0; }
  uint64_t getEfuseMac() const { return efuse_mac; }
} ESP;

// authSendNonce's randomness and the key-reset window (auth_gate.h): the nonce is the fixtures' nonce_hex.
enum esp_reset_reason_t { ESP_RST_POWERON = 1, ESP_RST_DEEPSLEEP = 8 };
esp_reset_reason_t fake_reset_reason = ESP_RST_DEEPSLEEP;
esp_reset_reason_t esp_reset_reason() { return fake_reset_reason; }
uint8_t fake_nonce[16] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf};
size_t fake_random_at = 0;
uint32_t esp_random() {
  uint32_t word;
  std::memcpy(&word, fake_nonce + fake_random_at % sizeof(fake_nonce), sizeof(word)); // the bytes authSendNonce copies back
  fake_random_at += sizeof(word);
  return word;
}

unsigned long fake_ms = 1000;
unsigned long millis() { return fake_ms; }
struct SerialStub {
  void printf(const char* text) { std::fputs(text, stdout); }
  template<typename... Args> void printf(const char* format, Args... args) { std::printf(format, args...); }
  void println() {}
} Serial;

time_t clock_epoch = 0;
struct RTCStub { uint32_t getEpoch() { return clock_epoch; } } rtc;
enum ClockSource { CLK_GPS, CLK_HTTP };
#include "clock_quality.inc"
time_t parseHttpDate(const char*) { return 0; }
void setClock(ClockSource, time_t epoch) { clock_epoch = (uint32_t)epoch; last_clock_sync_epoch = epoch; }
void esp_task_wdt_reset() {}
void driveMarkerRestored() {}
void driveFrameBuffered() {}

#include "telemetry_format.h"

struct HttpResponse { int responseCode; std::string payload; };
struct FakeNetwork {
  std::vector<json> received;
  int responseCode = 200;
  bool set_headers(const HeaderVector&) { return true; }
  std::string getDateHeader() { return {}; }
  bool connected() { return false; } // offline: only the reconnect gate stands between the loop and the modem
  HttpResponse sendBulk(const std::string& payload) {
    check(payload.find("relay_seq") == std::string::npos, "a relay seq never reaches an upload body");
    if (responseCode == 200) for (const auto& point : json::parse(payload)) received.push_back(point);
    return {responseCode, "{}"};
  }
} network;
#include "gzip_fake.h"
GzipOutcome gzipBody(const std::string&, std::string&) { return GZIP_UNAVAILABLE; }
int logging_errors = 0, g_last_http = 0, g_http0_streak = 0;
unsigned long g_last_post_ms = 0;
std::string g_last_status, tlm_push_token = "test-token", api_key = "test-key";
std::vector<OBDVector> obd_result_vector;
unsigned long upload_capped = 0, upload_heap_dropped = 0;
#define MAX_UPLOAD_BUFFER 120
#include "tlm_signature.h"
#include "backlog_fs.h"
#include "network_upload.h"
#include "geo_backlog.h"
#include "backlog.inc"

bool g_ble_handoff_pending = false, g_ota_active = false;
volatile bool g_logdump_active = false;
unsigned long g_ble_disconnect_ms = 0, last_broadcast = 0;

struct FakeBle {
  bool linked = false, authenticated = false, setup_allowed = false;
  bool telemetry_subscribed = false, session_subscribed = false; // the ABD1 and ABD9 CCCDs
  uint32_t grants = 0;
  uint32_t auth_grant() const { return grants; }
  uint16_t negotiated = 185;
  std::vector<std::string> notifies, values, auth_notifies, session_notifies, calls;
  bool connected() { return linked; }
  bool get_authenticated() { return authenticated; }
  bool get_setup_allowed() { return setup_allowed; }
  // BLEWrapper's: a grant counts on every false -> true, and ending one clears setup_allowed (pinned by the runner).
  void set_authenticated(bool v) {
    if (v && !authenticated) ++grants;
    if (!v) setup_allowed = false;
    authenticated = v;
    calls.push_back(v ? "authenticated=1" : "authenticated=0");
  }
  void set_setup_allowed(bool v) {
    setup_allowed = v;
    calls.push_back(v ? "setup_allowed=1" : "setup_allowed=0");
  }
  uint16_t mtu() { return negotiated; }
  bool telemetrySubscribed() { return telemetry_subscribed; }
  bool logData(const std::string& notify, const std::string& value) {
    if (!linked || !authenticated) return false;
    notifies.push_back(notify);
    values.push_back(value);
    return true;
  }
  bool notifyAuth(const uint8_t* frame, size_t len) {
    auth_notifies.emplace_back((const char*)frame, len);
    return true;
  }
  bool notifySession(const char* event) { // an indicate goes out only to an armed CCCD
    if (!linked || !session_subscribed) return false;
    session_notifies.push_back(event);
    return true;
  }
} ble;

// Every logEvent line the relay and protocol code writes, held to logEvent's buffer (119 characters).
std::vector<std::string> g_log;
void captureLog(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  check(n >= 0 && n <= 119, std::string("a log line fits logEvent's buffer: ") + line);
  g_log.push_back(line);
}
size_t logged(const std::string& text) { // lines starting with text
  size_t n = 0;
  for (const auto& line : g_log) n += line.compare(0, text.size(), text) == 0;
  return n;
}
size_t loggedLine(const std::string& text) { // lines that are exactly text
  size_t n = 0;
  for (const auto& line : g_log) n += line == text;
  return n;
}
#define logEvent captureLog

bool bleProtoPhoneIsTransport();
bool bleProtoRelayAcks();
#include "ble_predicates.inc"
#include "ble_relay.h"
#include "ble_protocol.h"
#include <mbedtls/base64.h>
#include "auth_nonce.inc"
namespace main_fw { // authSendNonce as PIN (main before negotiation) has it: the fw-none fixtures' nonce frame
#include "main_auth_nonce.inc"
}

namespace historical {
#include "historical_ble_frame.inc"
}

// The top of loop(): the relay session, then the ABD9 session channel, in production order.
void services() {
  relayAckService();
  sessionChannelService();
}

// logger.ino's cellular gates: the modem reconnect (outside the poll block, so it runs during a log dump) and the
// upload's condition with the grace and handoff it reads; the cadence term is zero.
bool g_weak_rail_cold_boot = false;
const unsigned long WIFI_RETRY_MS = 20000;
unsigned long last_reconnect_try = 0;
bool reconnectGate() {
  return
#include "reconnect_gate.inc"
  ;
}
bool uploadGate() {
#include "upload_grace.inc"
  const unsigned long upload_delay = 0;
  const bool dc_first_upload = false;
  return
#include "upload_gate.inc"
  ;
}

// One loop iteration from the session services to the cellular upload gate, all production code but the fakes.
const double T = 1791500000;
unsigned polls = 0, posts = 0;
OBDVector g_extra_values; // more values every poll reads, for the frame-shape checks
OBDVector pollFrame(unsigned i) {
  OBDVector frame{{"utc", T + i}, {"soc", 10.0 + i}, {"voltage", 350}, {"current", -20}};
  frame.insert(frame.end(), g_extra_values.begin(), g_extra_values.end());
  return frame;
}
const OBDVector* g_poll_values = nullptr; // a fixture's poll values instead of pollFrame's
void poll() {
  services();
  ++polls;
  OBDVector current_frame = g_poll_values ? *g_poll_values : pollFrame(polls);
  const bool frame_has_data = true;
  GeoMarks poll_geo{};
#include "ble_send.inc"
#include "upload_buffer.inc"
  if (uploadGate()) {
    ++posts;
    check(uploadBuffered(true), "the cellular upload succeeds");
    g_ble_handoff_pending = false;
    last_broadcast = millis();
  }
  fake_ms += 1000;
}

// The soc each uploaded point carries is 10 + its poll number, which names the frame.
std::vector<unsigned> uploadedPolls() {
  std::vector<unsigned> out;
  for (const auto& point : network.received) out.push_back((unsigned)(point["soc"]["frac"].get<double>() * 100 + 0.5) - 10);
  return out;
}
std::vector<unsigned> queuedSeqs() {
  std::vector<unsigned> out;
  for (const auto& frame : obd_result_vector)
    for (const auto& value : frame) if (value.first == RELAY_SEQ_KEY) out.push_back((unsigned)value.second);
  return out;
}
std::vector<unsigned> queuedPolls() {
  std::vector<unsigned> out;
  for (const auto& frame : obd_result_vector)
    for (const auto& value : frame) if (value.first == "soc") out.push_back((unsigned)(value.second + 0.5) - 10);
  return out;
}
// The seq an ABD1 frame carries beside tlm, 0 for none.
uint32_t frameSeq(const std::string& text) {
  const auto frame = json::parse(text);
  return frame.contains("seq") ? frame["seq"].get<uint32_t>() : 0;
}
uint32_t notifiedSeq(size_t i) { return frameSeq(ble.notifies.at(i)); }

// ABD9, as BLEWrapper's SessionCallbacks drive it on the Bluedroid task.
void sessionWrite(const std::string& text) { relaySessionWrite((const uint8_t*)text.data(), text.size()); }
std::string sessionRead() {
  uint8_t value[128];
  const size_t n = relaySessionRead(value, sizeof(value));
  check(n <= 93, "the ABD9 state is never a long read (<= 93 B)");
  return std::string((const char*)value, n);
}
json sessionState() { return json::parse(sessionRead()); }
void ack(uint32_t seq) { sessionWrite("{\"relay_ack\":" + std::to_string(seq) + "}"); }
void stop() { sessionWrite("{\"relay\":\"stop\"}"); }
void resume() { sessionWrite("{\"relay\":\"resume\"}"); }
const std::string kLapsedStop = "{\"relay\":\"lapsed\",\"why\":\"stop\"}";
const std::string kLapsedTimeout = "{\"relay\":\"lapsed\",\"why\":\"timeout\"}";
const std::string kLive = "{\"relay\":\"live\"}";

// ---- The ABD8 handshake, from the point where the proof passed the HMAC check (auth_gate.h).

// The fixtures' proof: base64(HMAC-SHA256(secret, nonce)).
const std::string kProof = "{\"proof\":\"sUiXJ7JgKg4EfLI3WApMeVqvkCPFvGsLhlx/oX4KI2c=\"";
std::string proofWith(const char* proto) { return proto ? kProof + ",\"proto\":" + proto + "}" : kProof + "}"; }

// What auth_gate.h does once the HMAC matched, and the reply it notifies.
std::string prove(const std::string& proof) {
  char reply[64];
  bleProtoAccept(json::parse(proof), reply, sizeof(reply));
  return reply;
}

void link(uint16_t mtu = 185) {
  ble.linked = true;
  ble.negotiated = mtu;
  ble.calls.clear();
}

void unlink() { // ServerCallbacks::onDisconnect: clearAuthentication(), and the ABD1/ABD8/ABD9 CCCDs cleared
  ble.linked = false;
  ble.set_authenticated(false);
  ble.telemetry_subscribed = ble.session_subscribed = false;
  ble.calls.clear();
}

void reset() {
  fake_ms = 1000;
  setClock(CLK_HTTP, T);
  polls = posts = 0;
  const uint32_t grants = ble.grants; // BLEWrapper's counter never restarts within a boot
  ble = FakeBle{};
  ble.grants = grants;
  network = FakeNetwork{};
  obd_result_vector.clear();
  g_upload_held.clear();
  g_ble_handoff_pending = g_ota_active = false;
  g_logdump_active = false;
  g_ble_disconnect_ms = 0;
  last_broadcast = millis();
  stop();     // a stale entry in the mailbox must not survive a session with no phone
  services();
  g_relay_seq = 0;
  g_log.clear();
  g_extra_values.clear();
}

// A session of an app that does not negotiate: its proof carries no proto.
void connect() {
  link();
  prove(proofWith(nullptr));
}

// A session of an app that negotiated v1 and subscribed to ABD1 and ABD9, and its first poll.
void negotiate() {
  link();
  prove(proofWith("1"));
  ble.telemetry_subscribed = ble.session_subscribed = true;
  poll();
}

// ---- The locked acceptance scenarios (#90), as negotiated v1 sessions.

void oldApp() {
  for (const char* proto : {(const char*)nullptr, "0"}) {
    const std::string who = proto ? "an app that negotiated v0" : "an app that does not negotiate";
    reset();
    link();
    prove(proofWith(proto));
    ble.telemetry_subscribed = true;
    for (int i = 0; i < 5; ++i) poll();
    check(ble.notifies.size() == 5, who + " is notified every frame");
    for (size_t i = 0; i < ble.notifies.size(); ++i) {
      const auto expected = historical::bleTlmFrame(pollFrame(i + 1), bleNotifyBudget(ble.negotiated));
      check(ble.notifies[i] == expected.notify && ble.values[i] == expected.full,
            who + " reads frames byte-identical to today's");
    }
    check(obd_result_vector.empty(), "frames relayed to " + who + " are not queued for cellular");
    ack(3);
    stop();
    poll();
    ack(4);
    poll();
    check(loggedLine("BLE session proto=0 ignored key=relay_ack") == 1 && loggedLine("BLE session proto=0 ignored key=relay") == 1,
          "ABD9 messages in a v0 session are ignored, and logged once per key and session");
    fake_ms += RELAY_ACK_TIMEOUT_MS * 10;
    poll();
    check(posts == 0 && bleRelaying(), who + " stays the transport however long it is connected");
  }
}

void acking() {
  reset();
  negotiate();
  check(notifiedSeq(0) == 1, "once negotiated v1, the frame carries seq 1");
  poll();
  poll();
  check(notifiedSeq(1) == 2 && notifiedSeq(2) == 3, "seqs count up per notified frame");
  check(queuedSeqs() == std::vector<unsigned>({1, 2, 3}), "relayed frames are queued under their seq until acked");
  ack(2);
  poll();
  check(queuedSeqs() == std::vector<unsigned>({3, 4}), "an ack on ABD9 takes the frames up to its seq off the queue");
  for (int i = 0; i < 100; ++i) {
    ack(g_relay_seq);
    poll();
  }
  check(posts == 0 && bleRelaying(), "while acks keep coming the phone is the transport and cellular stays off");
  check(queuedSeqs().size() == 1, "only the frame relayed since the last ack is queued");
  ble.negotiated = 23;
  poll();
  const uint32_t notify = frameSeq(ble.notifies.back()), value = frameSeq(ble.values.back());
  check(value == g_relay_seq && (notify == 0 || notify == g_relay_seq),
        "the readable value always carries the seq, and a notify that carries one carries the same");
  const uint32_t sent = g_relay_seq;
  const size_t queued = obd_result_vector.size();
  ack(sent + 5);
  poll();
  check(obd_result_vector.size() == queued + 1, "an ack beyond any seq sent removes nothing");
}

void stall() {
  reset();
  negotiate();
  for (int i = 0; i < 3; ++i) poll();
  ack(2);
  poll();
  const unsigned long acked_at = fake_ms - 1000;
  for (int i = 0; i < 3; ++i) poll();
  fake_ms = acked_at + RELAY_ACK_TIMEOUT_MS - 1;
  check(bleRelaying(), "the phone is still the transport 1 ms before the timeout");
  fake_ms = acked_at + RELAY_ACK_TIMEOUT_MS;
  check(!bleRelaying() && bleAuthenticated(), "the transport lapses exactly at the timeout after the last ack");
  const unsigned before = polls;
  poll();
  check(posts == 1, "cellular posts once the relay has lapsed");
  std::vector<unsigned> expected;
  for (unsigned p = 3; p <= before + 1; ++p) expected.push_back(p);
  check(uploadedPolls() == expected, "every frame the app never acked goes up once, in order");
  poll();
  check(posts == 2 && uploadedPolls().size() == expected.size() + 1, "and the frames after it follow over cellular");
  ack(2);
  poll();
  check(!bleRelaying(), "an ack that does not move forward does not bring the transport back");
}

void neverAck() {
  reset();
  negotiate();
  const unsigned long first_frame_at = fake_ms - 1000;
  while (fake_ms < first_frame_at + RELAY_ACK_TIMEOUT_MS - 1000) poll();
  check(posts == 0 && bleRelaying(), "a negotiated v1 app is the transport until the timeout");
  fake_ms = first_frame_at + RELAY_ACK_TIMEOUT_MS;
  check(!bleRelaying(), "one that never acks lapses at the timeout after the grant's first outstanding frame");
  const unsigned before = polls;
  poll();
  std::vector<unsigned> expected;
  for (unsigned p = 1; p <= before + 1; ++p) expected.push_back(p);
  check(posts == 1 && uploadedPolls() == expected, "every frame since the grant goes up over cellular");
}

void resumeAfterLapse() {
  reset();
  negotiate();
  poll();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  poll();
  check(posts == 1 && !bleRelaying(), "lapsed to cellular");
  poll();
  poll();
  const unsigned posted = posts;
  const uint32_t acked = g_relay_seq - 1;
  ack(acked);
  poll();
  check(bleRelaying(), "an ack after a lapse makes the phone the transport again");
  check(posts == posted, "and cellular stands down");
  for (unsigned seq : queuedSeqs()) check(seq > acked, "frames up to that ack are dropped");
  check(queuedSeqs().size() == 1, "only the frame relayed after that ack is left");
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  poll();
  check(!bleRelaying(), "lapsed again");
  resume();
  services();
  check(bleRelaying(), "a resume after a timeout lapse also restores relaying");
  const unsigned before = posts;
  poll();
  check(posts == before, "and cellular stands down");
}

void disconnect() {
  reset();
  negotiate();
  poll();
  poll();
  ack(1);
  poll();
  stop(); // lands as the link drops: the next session must not inherit it
  unlink();
  g_ble_disconnect_ms = fake_ms; // changeState() on leaving BLE after a session that relayed
  g_ble_handoff_pending = true;
  poll();
  check(posts == 0, "cellular waits out the grace after a disconnect");
  fake_ms = g_ble_disconnect_ms + BLE_UPLOAD_GRACE_MS;
  poll();
  check(posts == 1 && uploadedPolls() == std::vector<unsigned>({2, 3, 4, 5, 6}),
        "the frames the app never acked upload after the grace, with the ones polled since");
  check(loggedLine("BLE session stale grant") == 1, "the stop written as the link dropped is discarded, and logged");
  connect();
  ble.telemetry_subscribed = true;
  poll();
  check(frameSeq(ble.notifies.back()) == 0, "a new session that did not negotiate v1 carries no seq");
}

void presence() {
  reset();
  negotiate();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  check(!bleRelaying() && bleAuthenticated(), "during a lapse the phone still counts as present for the leash and alarm gate");
  services();
  check(!bleRelaying() && bleAuthenticated(), "also once the lapse is entered");
  stop();
  services();
  check(!bleRelaying() && bleAuthenticated(), "and during a stop");
}

// A session's acks name its own frames only, however quickly the next session follows.
void sessions() {
  reset();
  negotiate();
  poll();
  poll();
  ack(1);
  poll();
  unlink();
  g_ble_disconnect_ms = fake_ms;
  poll();
  negotiate();
  ack(g_relay_seq);
  poll();
  check(queuedPolls() == std::vector<unsigned>({2, 3, 4, 5, 7}),
        "a reconnect inside the grace: the new session's ack leaves the old session's unacked frames queued");
  check(queuedSeqs() == std::vector<unsigned>({g_relay_seq}), "and they lost their seq with their session");

  reset();
  negotiate();
  poll();
  unlink();
  connect(); // dropped and granted again between two loop passes, to an app that does not negotiate
  ble.telemetry_subscribed = true;
  poll();
  check(ble.notifies.back() == historical::bleTlmFrame(pollFrame(polls), bleNotifyBudget(ble.negotiated)).notify,
        "a session granted between two passes starts with its own version, here v0 with no seq");
  check(queuedPolls() == std::vector<unsigned>({1, 2}) && queuedSeqs().empty(),
        "and its relayed frames are not queued, while the last session's go up as plain frames");

  reset();
  negotiate();
  ack(g_relay_seq);
  poll();
  ack(g_relay_seq);
  for (int i = 0; i < 3; ++i) {
    fake_ms += RELAY_ACK_TIMEOUT_MS;
    services();
  }
  check(bleRelaying(), "with nothing outstanding there is nothing to be late with");

  reset();
  negotiate();
  poll();
  stop(); // stashed under this grant, serviced only after the next one, granted to an app that negotiated v1 again
  unlink();
  link();
  prove(proofWith("1"));
  ble.telemetry_subscribed = true;
  poll();
  check(bleRelaying() && queuedSeqs() == std::vector<unsigned>({g_relay_seq}),
        "a stop stashed in one session is never applied to the next");

  uint32_t v = 7;
  check(relayAckValue(json::parse("0"), v) && v == 0 && relayAckValue(json::parse("4294967295"), v) &&
        v == 4294967295u, "relay_ack takes any uint32 the app writes");
  for (const char* bad : {"-1", "1.5", "\"3\"", "4294967296", "null"})
    check(!relayAckValue(json::parse(bad), v), std::string("relay_ack refuses ") + bad);
}

// A log dump or an update pauses polling, so a negotiated v1 app has nothing to ack meanwhile.
void hold() {
  reset();
  negotiate();
  for (int busy = 0; busy < 2; ++busy) {
    (busy ? g_ota_active : g_logdump_active) = true;
    fake_ms += RELAY_ACK_TIMEOUT_MS * 3;
    services();
    check(bleRelaying(), "the relay clock waits while a log dump or an update runs");
    g_ota_active = g_logdump_active = false;
    fake_ms += RELAY_ACK_TIMEOUT_MS - 1;
    check(bleRelaying(), "and restarts when it ends");
  }
}

void persistence() {
  reset();
  negotiate();
  poll();
  check(queuedSeqs().size() == 2 && saveUploadBacklog() && LittleFS.exists("/upload_backlog.jsonl") &&
        LittleFS.contents().find("relay_seq") == std::string::npos, "a relay seq is never written to the backlog");
}

// ---- The session channel (ABD9) and the session state machine (Helpers/ble_relay.h).

void stopResume() {
  reset();
  negotiate();
  poll();
  ack(2);
  poll();
  stop();
  services();
  check(!bleRelaying() && bleAuthenticated() && g_ble_handoff_pending, "a stop hands the transport to cellular at once");
  check(ble.session_notifies == std::vector<std::string>({kLapsedStop}), "and notifies the lapse on ABD9");
  check(sessionState() == json::parse("{\"relay\":\"lapsed\",\"why\":\"stop\",\"acked\":2,\"sent\":3,\"queued\":1}"),
        "the readable state says so: " + sessionRead());
  check(logged("BLE relay stop by app") == 1, "and the log");
  ack(3); // a post that was already in flight when the app stopped
  services();
  check(!bleRelaying() && queuedSeqs().empty(), "an ack in flight takes its frames off, but does not end the stop");
  check(sessionState()["acked"] == 3 && sessionState()["why"] == "stop", "the state shows it: " + sessionRead());
  stop();
  fake_ms += RELAY_ACK_TIMEOUT_MS * 3;
  services();
  check(!bleRelaying() && ble.session_notifies.size() == 1, "a repeated stop is idempotent, and time does not end it");
  resume();
  services();
  check(bleRelaying() && ble.session_notifies.back() == kLive && logged("BLE relay resume by app") == 1,
        "only a resume ends a stop, notified live");
  resume();
  services();
  check(ble.session_notifies.size() == 2, "a resume while live changes nothing");
  poll();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  services();
  check(!bleRelaying() && ble.session_notifies.back() == kLapsedTimeout, "a timeout lapse is notified too");
  stop();
  services();
  check(ble.session_notifies.back() == kLapsedStop && sessionState()["why"] == "stop",
        "a stop during a timeout lapse makes it a stop");
  ack(g_relay_seq);
  services();
  check(!bleRelaying(), "which a forward ack does not end");
  g_logdump_active = true;
  resume();
  services();
  stop();
  services();
  check(!bleRelaying(), "a stop during a log dump is applied");
  g_logdump_active = false;
  services();
  check(!bleRelaying() && sessionState()["why"] == "stop", "and still holds when the dump ends");
}

void mailbox() {
  reset();
  link();
  stop();
  ack(1);
  services();
  prove(proofWith("1"));
  ble.telemetry_subscribed = ble.session_subscribed = true;
  poll();
  check(bleRelaying() && queuedSeqs() == std::vector<unsigned>({1}), "ABD9 writes before the grant are dropped");
  poll();
  poll();
  poll();
  ack(3);
  ack(1);
  resume();
  ack(4);
  ack(2);
  stop();
  services();
  check(queuedSeqs().empty() && sessionState()["acked"] == 4 && sessionState()["why"] == "stop",
        "a burst between two loop passes collapses to the highest ack and the last verb: " + sessionRead());
  resume();
  services();
  poll();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  services();
  const size_t before = ble.session_notifies.size();
  stop();
  ack(g_relay_seq);
  services();
  check(std::vector<std::string>(ble.session_notifies.begin() + before, ble.session_notifies.end()) ==
        std::vector<std::string>({kLive, kLapsedStop}), "the ack is applied first, then the verb");
  stop();
  unlink();
  negotiate();
  check(bleRelaying() && logged("BLE session stale grant") == 1, "an entry stamped with an ended grant is discarded");
  sessionWrite("{\"foo\":1}");
  services();
  sessionWrite("{\"foo\":2}");
  services();
  sessionWrite("not json");
  services();
  sessionWrite("{\"relay_ack\":-1}");
  services();
  sessionWrite("{\"relay\":\"sideways\"}");
  services();
  check(loggedLine("BLE session proto=1 ignored key=foo") == 1 && loggedLine("BLE session proto=1 ignored key=?") == 1 &&
        loggedLine("BLE session proto=1 ignored key=relay_ack") == 1 && loggedLine("BLE session proto=1 ignored key=relay") == 1,
        "a key the version does not know, or a value it does not take, is logged once per session by key");
  check(bleRelaying(), "and changes nothing");
}

void state() {
  reset();
  check(sessionRead() == "{}", "with no v1 session the ABD9 state is empty");
  connect();
  services();
  check(sessionRead() == "{}", "and in a v0 session");
  unlink();
  negotiate();
  services();
  check(sessionState() == json::parse("{\"relay\":\"live\",\"acked\":0,\"sent\":1,\"queued\":1}"),
        "in a v1 session it is the relay state: " + sessionRead());
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  services();
  check(ble.session_notifies.back() == kLapsedTimeout &&
        sessionState() == json::parse("{\"relay\":\"lapsed\",\"why\":\"timeout\",\"acked\":0,\"sent\":1,\"queued\":1}"),
        "a notify never takes the place of the state: " + sessionRead());
  ack(1);
  services();
  check(sessionState() == json::parse("{\"relay\":\"live\",\"acked\":1,\"sent\":1,\"queued\":0}"),
        "the state follows every transition: " + sessionRead());
  ble.session_subscribed = false;
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  poll();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  services();
  check(ble.session_notifies.back() == kLive && sessionState()["relay"] == "lapsed",
        "an app not subscribed to ABD9 reads the state instead");
  unlink();
  services();
  check(sessionRead() == "{}", "and once the session ends it is empty again");
}

void subscribedSeq() {
  reset();
  link();
  prove(proofWith("1"));
  poll();
  poll();
  check(frameSeq(ble.values.back()) == 0 && queuedPolls() == std::vector<unsigned>({1, 2}) && queuedSeqs().empty(),
        "before the app subscribes to ABD1, v1 frames carry no seq and are plain cellular frames");
  for (int i = 0; i < 200; ++i) poll();
  check(bleRelaying() && posts == 0, "a profile push longer than RELAY_ACK_TIMEOUT_MS does not lapse the transport");
  ble.telemetry_subscribed = true;
  poll();
  check(frameSeq(ble.notifies.back()) == 1 && queuedSeqs() == std::vector<unsigned>({1}), "once subscribed, seqs start");
  unlink();
  link();
  prove(proofWith("1"));
  poll();
  check(frameSeq(ble.values.back()) == 0, "after a reconnect within one wake, frames carry no seq until it resubscribes");
  ble.telemetry_subscribed = true;
  poll();
  check(frameSeq(ble.notifies.back()) == 2, "and then they do");
  const auto queued = queuedSeqs();
  check(prove(proofWith("0")) == "{\"auth\":\"ok\",\"proto\":1}", "a second proof keeps v1");
  poll();
  check(queuedSeqs() == std::vector<unsigned>({2, 3}) && queued == std::vector<unsigned>({2}),
        "and drops no unposted frame");
  ble.telemetry_subscribed = false;
  poll();
  check(frameSeq(ble.values.back()) == 0, "unsubscribed, no seq");
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  services();
  check(!bleRelaying(), "an app that unsubscribes with frames unacked still lapses");
}

// v1's seq goes beside tlm, never inside it: frame metadata released apps ignore (they read JSON.parse(value).tlm),
// which cannot collide with a profile key, and which the trim never drops.
void seqFrame() {
  reset();
  negotiate();
  const json notify = json::parse(ble.notifies.back()), value = json::parse(ble.values.back());
  check(notify.size() == 2 && notify["seq"] == 1 && !notify["tlm"].contains("seq") && value == notify,
        "a v1 frame is {\"tlm\":{...},\"seq\":n}: " + ble.notifies.back());
  check(ble.notifies.back() == "{\"tlm\":{\"soc\":11,\"voltage\":350,\"current\":-20},\"seq\":1}",
        "with the seq after tlm, as the fixtures have it: " + ble.notifies.back());
  g_extra_values = {{"seq", 42}};
  poll();
  const json keyed = json::parse(ble.notifies.back());
  check(keyed["seq"] == 2 && keyed["tlm"]["seq"] == 42, "a profile key named seq stays inside tlm, untouched");
  g_extra_values.clear();
  for (int i = 0; i < 24; ++i) g_extra_values.push_back({"_profile_value_" + std::to_string(i), 1234.56 + i});
  ble.negotiated = 96;
  poll();
  const std::string trimmed = ble.notifies.back();
  check(trimmed.size() <= bleNotifyBudget(96) && json::parse(trimmed)["seq"] == g_relay_seq &&
        json::parse(trimmed)["tlm"]["soc"] == 10 + polls && json::parse(ble.values.back())["tlm"].size() == 3 + 24,
        "at the 93-byte notify of MTU 96 the trim drops telemetry fields only, never the seq: " + trimmed);
}

// A log dump pauses polling and keeps the phone busy on the link: it changes no relay state, but no cellular gate may
// open under it, the modem reconnect included (it runs outside the poll block and can block the loop for seconds).
void dumpCellular() {
  reset();
  negotiate();
  poll();
  g_logdump_active = true;
  stop();
  services();
  check(!bleRelaying() && sessionState()["why"] == "stop", "a stop during a log dump is applied at once");
  fake_ms += WIFI_RETRY_MS + 1;
  check(!reconnectGate() && !uploadGate(), "but cellular stays off while the dump runs: no modem reconnect, no upload");
  g_logdump_active = false;
  check(reconnectGate() && uploadGate(), "and takes over the moment it ends");
  g_ota_active = true;
  check(!reconnectGate() && !uploadGate(), "as for an update");
}

// ---- protocol/fixtures (see its README.md): the firmware side of the wire contract with the app.

std::string g_fixture; // the file being replayed, for failure messages
void fixtureCheck(bool condition, size_t step, const std::string& message) {
  check(condition, g_fixture + " step " + std::to_string(step) + ": " + message);
}
// Parsed and re-serialised: key order does not matter, an integer token stays one (1 and 1.0 differ).
bool sameJson(const std::string& text, const json& expected) {
  const json parsed = json::parse(text, nullptr, false);
  return !parsed.is_discarded() && parsed.dump() == expected.dump();
}

void replayFixture(const json& doc) {
  const json& inputs = doc["inputs"];
  reset();
  const std::string nonce_hex = inputs["nonce_hex"];
  for (size_t i = 0; i < sizeof(fake_nonce); ++i) fake_nonce[i] = (uint8_t)std::stoul(nonce_hex.substr(2 * i, 2), nullptr, 16);
  ESP.efuse_mac = std::stoull(inputs["dongle_id"].get<std::string>(), nullptr, 16);
  fake_reset_reason = inputs["reset_window"].get<bool>() ? ESP_RST_POWERON : ESP_RST_DEEPSLEEP;
  link(inputs["mtu"].get<uint16_t>());
  const bool main_firmware = doc["firmware"]["range"].is_null();
  std::map<unsigned, unsigned> poll_of_soc; // a poll's frame is told apart by its soc
  for (const auto& step : doc["steps"])
    if (step.contains("poll")) poll_of_soc[(unsigned)std::lround(step["values"]["soc"].get<double>())] = step["poll"];
  auto polls_of = [&](const std::vector<unsigned>& socs_minus_10) {
    std::vector<unsigned> out;
    for (unsigned s : socs_minus_10) out.push_back(poll_of_soc.count(s + 10) ? poll_of_soc[s + 10] : 0);
    return out;
  };
  size_t auth_seen = 0, session_seen = 0;
  std::string abd2_reply;
  OBDVector values;
  for (size_t i = 0; i < doc["steps"].size(); ++i) {
    const json& step = doc["steps"][i];
    // Main's firmware is not this one: only its nonce frame is checked, against PIN's authSendNonce.
    if (main_firmware && !(step.value("char", "") == "ABD8" && step.value("from", "") == "dongle" &&
                           step["json"].contains("nonce")))
      continue;
    if (step.contains("wait")) {
      fake_ms += step["wait"].get<unsigned long>();
      services();
      continue;
    }
    if (step.contains("check")) {
      const json& c = step["check"];
      if (c.contains("transport"))
        fixtureCheck(bleRelaying() == (c["transport"] == "phone"), i, "the transport is " + c["transport"].get<std::string>());
      if (c.contains("queued"))
        fixtureCheck(polls_of(queuedPolls()) == c["queued"].get<std::vector<unsigned>>(), i, "the queued polls are " + c["queued"].dump());
      if (c.contains("uploaded"))
        fixtureCheck(polls_of(uploadedPolls()) == c["uploaded"].get<std::vector<unsigned>>(), i, "the uploaded polls are " + c["uploaded"].dump());
      continue;
    }
    const std::string from = step["from"], ch = step["char"], op = step["op"];
    // A notify is listed right after the step that caused it: anything else first means one the file does not list.
    if (!(from == "dongle" && (ch == "ABD8" || ch == "ABD9")))
      fixtureCheck(auth_seen == ble.auth_notifies.size() && session_seen == ble.session_notifies.size(), i,
                   "no ABD8 or ABD9 notify the file does not list");
    if (from == "dongle" && ch == "ABD8") {
      if (step["json"].contains("nonce")) {
        fake_random_at = 0;
        main_firmware ? main_fw::authSendNonce() : authSendNonce();
      }
      fixtureCheck(auth_seen < ble.auth_notifies.size(), i, "the dongle notifies on ABD8");
      const std::string& frame = ble.auth_notifies[auth_seen++];
      fixtureCheck(sameJson(frame, step["json"]), i, "ABD8 notifies " + step["json"].dump() + ", not " + frame);
      if (step.contains("length"))
        fixtureCheck(frame.size() == step["length"].get<size_t>(), i, "the nonce frame is " + step["length"].dump() + " B, not " + std::to_string(frame.size()));
    } else if (from == "dongle" && ch == "ABD1") {
      values.clear();
      for (auto it = step["values"].begin(); it != step["values"].end(); ++it) values.emplace_back(it.key(), it.value().get<double>());
      const size_t before = ble.values.size();
      g_poll_values = &values;
      poll();
      g_poll_values = nullptr;
      if (op == "none") {
        fixtureCheck(ble.values.size() == before, i, "nothing is written to ABD1");
      } else {
        fixtureCheck(ble.values.size() == before + 1 && sameJson(ble.values.back(), step["json"]), i,
                     "ABD1's value is " + step["json"].dump() + (ble.values.size() > before ? ", not " + ble.values.back() : ""));
        fixtureCheck(ble.telemetry_subscribed == (op == "notify") && (op != "notify" || sameJson(ble.notifies.back(), step["json"])), i,
                     "the frame is notified exactly when the app is subscribed, and the notify is the frame");
      }
    } else if (from == "dongle" && ch == "ABD9") {
      fixtureCheck(session_seen < ble.session_notifies.size(), i, "the dongle notifies on ABD9");
      fixtureCheck(sameJson(ble.session_notifies[session_seen], step["json"]), i,
                   "ABD9 notifies " + step["json"].dump() + ", not " + ble.session_notifies[session_seen]);
      ++session_seen;
    } else if (from == "dongle" && ch == "ABD2") {
      fixtureCheck(abd2_reply == step["text"].get<std::string>(), i, "ABD2 answers " + step["text"].get<std::string>() + ", not " + abd2_reply);
    } else if (from == "app" && ch == "ABD8") {
      ble.auth_notifies.push_back(prove(step.contains("text") ? step["text"].get<std::string>() : step["json"].dump())); // authNotifyJson
      services();
    } else if (from == "app" && (op == "subscribe" || op == "unsubscribe")) {
      (ch == "ABD1" ? ble.telemetry_subscribed : ble.session_subscribed) = op == "subscribe";
      services();
    } else if (from == "app" && ch == "ABD9" && op == "write") {
      sessionWrite(step["json"].dump());
      services();
    } else if (from == "app" && ch == "ABD9" && op == "read") {
      fixtureCheck(sameJson(sessionRead(), step["json"]), i, "ABD9 reads " + step["json"].dump() + ", not " + sessionRead());
    } else if (from == "app" && ch == "ABD2") {
      // The ABD2 handler runs on the Bluedroid task (blewrapper.cpp, pinned by the runner); the sketch decides only its
      // two gates: NOAUTH before a grant, PROTO in a session with no protocol.
      abd2_reply = !ble.get_authenticated() ? "NOAUTH" : !ble.get_setup_allowed() ? "PROTO" : "";
      services();
    } else {
      fixtureCheck(false, i, "a step this replay does not know");
    }
  }
  fixtureCheck(auth_seen == ble.auth_notifies.size() && session_seen == ble.session_notifies.size(), doc["steps"].size(),
               "no ABD8 or ABD9 notify the file does not list");
  if (main_firmware) return;
  const json& session = doc["expect"]["session"];
  fixtureCheck(session == "none" ? bleProto()->version == BLE_PROTO_NONE : bleProto()->version == session.get<unsigned>(),
               doc["steps"].size(), "the session runs " + session.dump());
}

void fixtures() {
  const std::string build = BLE_PROTO_MIN == 0 ? "" : "-DBLE_PROTO_MIN=" + std::to_string(BLE_PROTO_MIN);
  std::vector<std::filesystem::path> files;
  for (const char* version : {"v0", "v1"})
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(FIXTURE_DIR) / version))
      if (entry.path().extension() == ".json") files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  unsigned replayed = 0, nonces = 0;
  for (const auto& path : files) {
    std::ifstream in(path);
    const json doc = json::parse(in);
    const auto& replay = doc["replay"];
    const bool firmware = std::find(replay.begin(), replay.end(), "firmware") != replay.end();
    const bool main_nonce = doc["firmware"]["range"].is_null() && !doc["firmware"].contains("nonce_proto") &&
                            !doc["steps"].empty() && doc["steps"][0].value("char", "") == "ABD8";
    if (doc["firmware"].value("build", "") != build || (!firmware && !main_nonce)) continue;
    g_fixture = path.parent_path().filename().string() + "/" + path.filename().string();
    replayFixture(doc);
    (firmware ? replayed : nonces) += 1;
  }
  check(replayed >= (BLE_PROTO_MIN == 0 ? 9u : 1u) && (BLE_PROTO_MIN != 0 || nonces == 3),
        "every fixture for this build was found (" + std::to_string(replayed) + " replayed, " + std::to_string(nonces) +
        " main nonce frames)");
  std::cout << "  " << replayed << " fixtures replayed, " << nonces << " main-firmware nonce frames checked\n";
}

// Every sequence of `depth` events from three starting points, with the session invariants (design §5.4) checked
// after each event. The model tracks what each poll's frame may legitimately become.
struct Model {
  std::vector<uint32_t> seq, grant;   // per poll: its seq (0: none) and the grant it was sent under
  std::vector<bool> v0_relayed;        // relayed to a phone trusted without acks: not queued, by design
  std::map<uint32_t, uint32_t> acked_by; // per grant: the highest ack its app wrote and the loop applied
  uint32_t sent_in_grant = 0;          // the highest seq notified under the current grant
  int version_grant = -1, version = -1;
} model;

uint32_t currentGrant() { return bleAuthenticated() ? bleAuthGrant() : 0; }

void modelPoll() {
  const bool subscribed = ble.telemetry_subscribed;
  const uint32_t grant = currentGrant();
  const size_t before = ble.values.size(), uploaded = network.received.size();
  const bool v0 = grant && !bleProto()->relayNeedsSeq && bleProto()->setupAllowed;
  poll();
  const unsigned p = polls;
  const uint32_t s = ble.values.size() > before ? frameSeq(ble.values.back()) : 0;
  check(!s || subscribed, "invariant: no seq while the app is not subscribed to ABD1");
  const auto queued = queuedPolls();
  const bool in_queue = std::find(queued.begin(), queued.end(), p) != queued.end();
  bool went_up = false;
  for (size_t i = uploaded; i < network.received.size(); ++i)
    went_up |= (unsigned)(network.received[i]["soc"]["frac"].get<double>() * 100 + 0.5) - 10 == p;
  check(!s || in_queue || went_up, "invariant: no v1 frame counted as relayed without being queued");
  model.seq.push_back(s);
  model.grant.push_back(grant);
  model.v0_relayed.push_back(v0 && !in_queue && !went_up);
  if (s) model.sent_in_grant = std::max(model.sent_in_grant, s);
}

// then_reconnect: the ack and a stop land as the link drops, and a new v1 session is granted before the loop runs.
void modelAck(bool then_reconnect) {
  const uint32_t grant = currentGrant(), seq = model.sent_in_grant;
  ack(seq);
  if (then_reconnect) {
    stop();
    unlink();
    g_ble_disconnect_ms = fake_ms;
    link();
    prove(proofWith("1"));
    ble.telemetry_subscribed = ble.session_subscribed = true;
    model.sent_in_grant = 0;
  }
  services();
  if (!then_reconnect && grant) {
    if (bleProto()->relayNeedsSeq) model.acked_by[grant] = std::max(model.acked_by[grant], seq);
  }
}

void modelCheck(bool was_stopped, uint32_t grant_before, bool resumed) {
  const uint32_t grant = currentGrant();
  const auto queued = queuedPolls();
  std::vector<unsigned> up = uploadedPolls();
  for (size_t i = 0; i < model.seq.size(); ++i) {
    const unsigned p = (unsigned)i + 1;
    const bool kept = std::find(queued.begin(), queued.end(), p) != queued.end() ||
                      std::find(up.begin(), up.end(), p) != up.end();
    const uint32_t g = model.grant[i];
    const bool acked = model.seq[i] && model.acked_by.count(g) && model.acked_by[g] >= model.seq[i];
    check(kept || model.v0_relayed[i] || acked, "invariant: no frame dropped that its own session did not ack (poll " +
          std::to_string(p) + ")");
  }
  for (const auto& frame : obd_result_vector) {
    bool tagged = false;
    for (const auto& value : frame) tagged |= value.first == RELAY_SEQ_KEY;
    check(!tagged || (grant && bleProto()->relayNeedsSeq), "invariant: no tagged frame from an ended session");
  }
  if (grant) {
    if (model.version_grant != (int)grant) {
      model.version_grant = (int)grant;
      model.version = bleProto()->version;
    }
    check(bleProto()->version == model.version, "invariant: the version is fixed for the grant");
  }
  if (grant && grant != grant_before && bleProto()->relayNeedsSeq)
    check(bleRelaying() && sessionState()["relay"] == "live", "invariant: a new v1 session starts live");
  if (was_stopped && grant == grant_before && !resumed)
    check(!bleRelaying() && sessionState().value("why", "") == "stop", "invariant: only a resume ends a stop");
}

enum Event { POLL, SUB, UNSUB, ACK, ACK_RECONNECT, STOP, RESUME, WAIT, DUMP, REPROVE, DROP, V1, V0, EVENTS };

void apply(int e) {
  switch (e) {
    case POLL: modelPoll(); return;
    case SUB: ble.telemetry_subscribed = ble.linked; break;
    case UNSUB: ble.telemetry_subscribed = false; break;
    case ACK: modelAck(false); return;
    case ACK_RECONNECT: modelAck(true); return;
    case STOP: stop(); break;
    case RESUME: resume(); break;
    case WAIT: fake_ms += RELAY_ACK_TIMEOUT_MS; break;
    case DUMP: g_logdump_active = !g_logdump_active; break;
    case REPROVE: if (bleAuthenticated()) prove(proofWith("0")); break;
    case DROP: if (ble.linked) { unlink(); g_ble_disconnect_ms = fake_ms; } break;
    case V1: case V0:
      if (!bleAuthenticated()) {
        link();
        prove(proofWith(e == V1 ? "1" : nullptr));
        model.sent_in_grant = 0;
      }
      break;
  }
  services();
}

void start(int which) {
  reset();
  model = Model{};
  if (which == 0) return; // no phone
  link();
  prove(proofWith("1"));
  ble.telemetry_subscribed = ble.session_subscribed = true;
  apply(POLL);
  apply(POLL);
  ack(1);
  services();
  model.acked_by[currentGrant()] = 1;
  if (which == 2) apply(STOP); // v1 lapsed by a stop, one frame outstanding
}

void enumerate(int depth) {
  std::vector<int> events(depth, 0);
  unsigned long sequences = 0;
  for (int which = 0; which < 3; ++which) {
    std::fill(events.begin(), events.end(), 0);
    while (true) {
      start(which);
      modelCheck(false, currentGrant(), false);
      for (int e : events) {
        const bool stopped = bleAuthenticated() && bleProto()->relayNeedsSeq && sessionState().value("why", "") == "stop";
        const uint32_t grant_before = currentGrant();
        apply(e);
        modelCheck(stopped, grant_before, e == RESUME);
      }
      ++sequences;
      int i = depth - 1;
      while (i >= 0 && ++events[i] == EVENTS) events[i--] = 0;
      if (i < 0) break;
    }
  }
  std::cout << "  " << sequences << " sequences of " << depth << " events\n";
}
int g_depth = 4;
void enumerateGroup() { enumerate(g_depth); }

const std::vector<std::string> kSetupBeforeGrant{"setup_allowed=1", "authenticated=1"};

void negotiation() {
  check(BLE_PROTO_MIN == 0 && BLE_PROTO_MAX == 1, "this firmware speaks [0,1]");
  struct Case { const char* proto; uint16_t mtu; bool offered; uint8_t version; const char* echo; };
  const Case cases[] = {
    {nullptr, 185, false, 0, "{\"auth\":\"ok\"}"},
    {"0", 185, true, 0, "{\"auth\":\"ok\",\"proto\":0}"},
    {"1", 185, true, 1, "{\"auth\":\"ok\",\"proto\":1}"},
    {"7", 185, true, 1, "{\"auth\":\"ok\",\"proto\":1}"},
    {"255", 185, true, 1, "{\"auth\":\"ok\",\"proto\":1}"},
    {"\"1\"", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"-1", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"1.5", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"1.0", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"256", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"1099511627776", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"null", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"[0,1]", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"{}", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"true", 185, false, 0, "{\"auth\":\"ok\"}"},
    {"1", 95, true, 0, "{\"auth\":\"ok\",\"proto\":0}"},
    {"1", 96, true, 1, "{\"auth\":\"ok\",\"proto\":1}"},
  };
  for (const Case& c : cases) {
    const std::string what = std::string("a proof with proto ") + (c.proto ? c.proto : "absent") + " at MTU " +
                             std::to_string(c.mtu);
    uint8_t asked = 99;
    const bool offered = bleProtoParse(json::parse(proofWith(c.proto)), asked);
    check(offered == c.offered, what + (c.offered ? " offers a version" : " is treated as offering none"));
    check(bleProtoSelect(offered, asked, c.mtu) == c.version, what + " selects v" + std::to_string(c.version));
    reset();
    link(c.mtu);
    const std::string reply = prove(proofWith(c.proto));
    check(reply == c.echo, what + " is answered " + c.echo + ", not " + reply);
    check(bleAuthenticated() && bleProto()->version == c.version, what + ": the session runs v" + std::to_string(c.version));
    check(ble.calls == kSetupBeforeGrant, what + ": setup is allowed before the grant, then the grant");
  }
}

void grant() {
  reset();
  link();
  prove(proofWith("1"));
  const uint32_t granted = ble.grants;
  check(prove(proofWith("0")) == "{\"auth\":\"ok\",\"proto\":1}" && bleProto()->version == 1 && ble.grants == granted,
        "a second proof on a live grant keeps its version and echoes it");
  check(prove(proofWith(nullptr)) == "{\"auth\":\"ok\"}" && bleProto()->version == 1,
        "one without proto gets no echo, and the version stays");
  unlink();
  link();
  prove(proofWith(nullptr));
  check(bleProto()->version == 0, "a disconnect, reconnect and proof between two loop passes select the new grant's version");
  unlink();
  link();
  prove(proofWith("1"));
  check(bleProto()->version == 1, "in either direction");
  unlink();
  check(bleProto() == &kProtoV0, "with no session the protocol is v0");
  ble.linked = true;
  ble.set_authenticated(true); // a grant set without bleProtoAccept (none exists in the firmware)
  check(bleProto() == &kProtoV0, "and a grant no proof selected runs v0");
}

void nonce() {
  reset();
  link();
  fake_random_at = 0;
  fake_reset_reason = ESP_RST_DEEPSLEEP;
  authSendNonce();
  const std::string closed = ble.auth_notifies.back();
  check(closed == "{\"nonce\":\"oKGio6SlpqeoqaqrrK2urw==\",\"id\":\"A1B2C3D4E5F6\",\"proto\":[" +
                  std::to_string(BLE_PROTO_MIN) + "," + std::to_string(BLE_PROTO_MAX) + "]}",
        "every nonce frame carries the range [BLE_PROTO_MIN,BLE_PROTO_MAX]: " + closed);
  check(closed.size() == 70, "and is 70 B");
  fake_random_at = 0;
  fake_reset_reason = ESP_RST_POWERON;
  authSendNonce();
  const std::string open = ble.auth_notifies.back();
  check(open == "{\"nonce\":\"oKGio6SlpqeoqaqrrK2urw==\",\"id\":\"A1B2C3D4E5F6\",\"reset\":1,\"proto\":[" +
                std::to_string(BLE_PROTO_MIN) + "," + std::to_string(BLE_PROTO_MAX) + "]}" && open.size() == 80,
        "80 B with the key-reset window open: " + open);
  fake_reset_reason = ESP_RST_DEEPSLEEP;
}

// Built with -DBLE_PROTO_MIN=1: a proof below the minimum gets a grant with no protocol.
void noProtocol() {
  check(BLE_PROTO_MIN == 1, "this build raises BLE_PROTO_MIN to 1");
  for (const char* proto : {(const char*)nullptr, "0", "\"1\""}) {
    const std::string what = std::string("below the minimum (proto ") + (proto ? proto : "absent") + ")";
    reset();
    link();
    const std::string reply = prove(proofWith(proto));
    check(reply == "{\"auth\":\"ok\"}", what + ": the proof is accepted with no version echoed, not " + reply);
    check(bleProto()->version == BLE_PROTO_NONE && !ble.setup_allowed, what + ": a session with no protocol");
    check(ble.calls == std::vector<std::string>({"setup_allowed=0", "authenticated=1"}),
          what + ": setup is refused before the grant is set");
    check(bleAuthenticated() && !bleRelaying(), what + ": authenticated (OTA, log dump), never the transport");
  }
  reset();
  link(95);
  prove(proofWith("1"));
  check(bleProto()->version == BLE_PROTO_NONE, "below MTU 96 the dongle offers v0, which is below the minimum");
  reset();
  link();
  check(prove(proofWith("1")) == "{\"auth\":\"ok\",\"proto\":1}" && bleProto()->version == 1 && ble.setup_allowed,
        "a proof at the minimum runs it");
  reset();
  link();
  prove(proofWith(nullptr));
  for (int i = 0; i < 3; ++i) poll();
  check(ble.notifies.empty() && ble.values.empty(), "a no-protocol session hands no frame to ABD1");
  check(uploadedPolls() == std::vector<unsigned>({1, 2, 3}) && obd_result_vector.empty(),
        "its frames stay queued and cellular carries every one");
  fake_random_at = 0;
  authSendNonce();
  check(ble.auth_notifies.back().find(",\"proto\":[1,1]}") != std::string::npos, "and the nonce frame says [1,1]");
}

int main(int argc, char** argv) {
  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--depth" && i + 1 < argc) g_depth = std::atoi(argv[++i]);
    else only = argv[i];
  }
#if BLE_PROTO_MIN == 0
  const std::vector<std::pair<std::string, void (*)()>> groups{
    {"old-app", oldApp}, {"acking", acking}, {"stall", stall}, {"never-ack", neverAck},
    {"resume", resumeAfterLapse}, {"disconnect", disconnect}, {"presence", presence}, {"hold", hold},
    {"sessions", sessions}, {"persistence", persistence},
    {"negotiation", negotiation}, {"grant", grant}, {"nonce", nonce},
    {"stop-resume", stopResume}, {"mailbox", mailbox}, {"state", state}, {"subscribed-seq", subscribedSeq},
    {"seq-frame", seqFrame}, {"dump-cellular", dumpCellular}, {"fixtures", fixtures},
    {"enumerate", enumerateGroup},
  };
#else
  const std::vector<std::pair<std::string, void (*)()>> groups{{"no-protocol", noProtocol}, {"fixtures", fixtures}};
#endif
  for (const auto& group : groups) {
    if (!only.empty() && group.first != only) continue;
    group.second();
    std::cout << "ok " << group.first << '\n';
  }
}
