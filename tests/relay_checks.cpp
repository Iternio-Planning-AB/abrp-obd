// Relay acks (#90): run by tests/run_relay_checks.py, which extracts the production pieces this file includes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
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
  unsigned long getFreeHeap() const { return 4000000; }
  unsigned long getMaxAllocHeap() const { return 4000000; }
  unsigned long getMaxAllocPsram() const { return 0; }
  unsigned long getPsramSize() const { return 0; }
  unsigned long getFreePsram() const { return 0; }
} ESP;

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
  bool linked = false, authenticated = false;
  uint32_t grants = 0;
  uint32_t auth_grant() const { return grants; }
  uint16_t negotiated = 185;
  std::vector<std::string> notifies, values;
  bool connected() { return linked; }
  bool get_authenticated() { return authenticated; }
  uint16_t mtu() { return negotiated; }
  bool logData(const std::string& notify, const std::string& value) {
    if (!linked || !authenticated) return false;
    notifies.push_back(notify);
    values.push_back(value);
    return true;
  }
} ble;
bool relayTransportLive();
#include "ble_predicates.inc"
#include "ble_relay.h"

namespace historical {
#include "historical_ble_frame.inc"
}

// One loop iteration from relayAckService() to the cellular upload gate, all production code but the fakes.
const double T = 1791500000;
unsigned polls = 0, posts = 0;
OBDVector pollFrame(unsigned i) { return {{"utc", T + i}, {"soc", 10.0 + i}, {"voltage", 350}, {"current", -20}}; }
void poll() {
  relayAckService();
  OBDVector current_frame = pollFrame(++polls);
  const bool frame_has_data = true;
  GeoMarks poll_geo{};
#include "ble_send.inc"
#include "upload_buffer.inc"
#include "upload_grace.inc"
  const unsigned long upload_delay = 0;
  const bool dc_first_upload = false;
  if (
#include "upload_gate.inc"
  ) {
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
uint32_t notifiedSeq(size_t i) {
  const auto frame = json::parse(ble.notifies.at(i))["tlm"];
  return frame.contains("seq") ? frame["seq"].get<uint32_t>() : 0;
}

void reset() {
  fake_ms = 1000;
  setClock(CLK_HTTP, T);
  polls = posts = 0;
  ble = FakeBle{};
  network = FakeNetwork{};
  obd_result_vector.clear();
  g_upload_held.clear();
  g_ble_handoff_pending = g_ota_active = false;
  g_logdump_active = false;
  g_ble_disconnect_ms = 0;
  last_broadcast = millis();
  relayAckHook(0);       // a stale announce in the mailbox must not survive a session with no phone
  relayAckService();
  g_relay_seq = 0;
}

void connect() {
  if (!ble.authenticated) ++ble.grants; // BLEWrapper::set_authenticated(true)
  ble.linked = ble.authenticated = true;
}

void announce() {
  relayAckHook(0);
  poll();
}

void oldApp() {
  reset();
  connect();
  for (int i = 0; i < 5; ++i) poll();
  for (size_t i = 0; i < ble.notifies.size(); ++i) {
    const auto expected = historical::bleTlmFrame(pollFrame(i + 1), bleNotifyBudget(ble.negotiated));
    check(ble.notifies[i] == expected.notify && ble.values[i] == expected.full,
          "an app that never announced reads frames byte-identical to today's");
  }
  check(obd_result_vector.empty(), "frames relayed to an app without acks are not queued for cellular");
  fake_ms += RELAY_ACK_TIMEOUT_MS * 10;
  poll();
  check(posts == 0 && bleRelaying(), "an app without acks stays the transport however long it is connected");
}

void acking() {
  reset();
  connect();
  announce();
  check(notifiedSeq(0) == 1, "after the announce the frame carries seq 1");
  poll();
  poll();
  check(notifiedSeq(1) == 2 && notifiedSeq(2) == 3, "seqs count up per notified frame");
  check(queuedSeqs() == std::vector<unsigned>({1, 2, 3}), "relayed frames are queued under their seq until acked");
  relayAckHook(2);
  poll();
  check(queuedSeqs() == std::vector<unsigned>({3, 4}), "an ack takes the frames up to its seq off the queue");
  for (int i = 0; i < 100; ++i) {
    relayAckHook(g_relay_seq);
    poll();
  }
  check(posts == 0 && bleRelaying(), "while acks keep coming the phone is the transport and cellular stays off");
  check(queuedSeqs().size() == 1, "only the frame relayed since the last ack is queued");
  ble.negotiated = 23;
  poll();
  const auto notify = json::parse(ble.notifies.back())["tlm"];
  const auto value = json::parse(ble.values.back())["tlm"];
  check(value["seq"] == g_relay_seq && (!notify.contains("seq") || notify["seq"] == g_relay_seq),
        "the readable value always carries the seq, and a notify that carries one carries the same");
  const uint32_t sent = g_relay_seq;
  const size_t queued = obd_result_vector.size();
  relayAckHook(sent + 5);
  poll();
  check(obd_result_vector.size() == queued + 1, "an ack beyond any seq sent removes nothing");
}

void stall() {
  reset();
  connect();
  announce();
  for (int i = 0; i < 3; ++i) poll();
  relayAckHook(2);
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
  relayAckHook(2);
  poll();
  check(!bleRelaying(), "an ack that does not move forward does not bring the transport back");
}

void neverAck() {
  reset();
  connect();
  announce();
  const unsigned long announced_at = fake_ms - 1000;
  while (fake_ms < announced_at + RELAY_ACK_TIMEOUT_MS - 1000) poll();
  check(posts == 0 && bleRelaying(), "an announced app is the transport until the timeout");
  fake_ms = announced_at + RELAY_ACK_TIMEOUT_MS;
  check(!bleRelaying(), "an app that announced and never acked lapses at the timeout after the announce");
  const unsigned before = polls;
  poll();
  std::vector<unsigned> expected;
  for (unsigned p = 1; p <= before + 1; ++p) expected.push_back(p);
  check(posts == 1 && uploadedPolls() == expected, "every frame since the announce goes up over cellular");
}

void resume() {
  reset();
  connect();
  announce();
  poll();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  poll();
  check(posts == 1 && !bleRelaying(), "lapsed to cellular");
  poll();
  poll();
  const unsigned posted = posts;
  const uint32_t acked = g_relay_seq - 1;
  relayAckHook(acked);
  poll();
  check(bleRelaying(), "an ack after a lapse makes the phone the transport again");
  check(posts == posted, "and cellular stands down");
  for (unsigned seq : queuedSeqs()) check(seq > acked, "frames up to that ack are dropped");
  check(queuedSeqs().size() == 1, "only the frame relayed after that ack is left");
}

void disconnect() {
  reset();
  connect();
  announce();
  poll();
  poll();
  relayAckHook(1);
  poll();
  relayAckHook(0); // lands as the link drops: the next session must not inherit it
  ble.linked = ble.authenticated = false;
  g_ble_disconnect_ms = fake_ms; // changeState() on leaving BLE after a session that relayed
  g_ble_handoff_pending = true;
  poll();
  check(posts == 0, "cellular waits out the grace after a disconnect");
  fake_ms = g_ble_disconnect_ms + BLE_UPLOAD_GRACE_MS;
  poll();
  check(posts == 1 && uploadedPolls() == std::vector<unsigned>({2, 3, 4, 5, 6}),
        "the frames the app never acked upload after the grace, with the ones polled since");
  ble.linked = ble.authenticated = true;
  poll();
  check(json::parse(ble.notifies.back())["tlm"].count("seq") == 0, "a new session starts unannounced");
}

void presence() {
  reset();
  connect();
  announce();
  fake_ms += RELAY_ACK_TIMEOUT_MS;
  check(!bleRelaying() && bleAuthenticated(), "during a lapse the phone still counts as present for the leash and alarm gate");
}

std::vector<unsigned> queuedPolls() {
  std::vector<unsigned> out;
  for (const auto& frame : obd_result_vector)
    for (const auto& value : frame) if (value.first == "soc") out.push_back((unsigned)(value.second + 0.5) - 10);
  return out;
}

// A session's acks name its own frames only, however quickly the next session follows.
void sessions() {
  reset();
  connect();
  announce();
  poll();
  poll();
  relayAckHook(1);
  poll();
  ble.linked = ble.authenticated = false;
  g_ble_disconnect_ms = fake_ms;
  poll();
  connect();
  announce();
  relayAckHook(g_relay_seq);
  poll();
  check(queuedPolls() == std::vector<unsigned>({2, 3, 4, 5, 7}),
        "a reconnect inside the grace: the new session's ack leaves the old session's unacked frames queued");
  check(queuedSeqs() == std::vector<unsigned>({g_relay_seq}), "and they lost their seq with their session");

  reset();
  connect();
  announce();
  poll();
  ble.authenticated = false;
  connect(); // dropped and granted again between two loop iterations, to an app that never announces
  poll();
  check(ble.notifies.back() == historical::bleTlmFrame(pollFrame(polls), bleNotifyBudget(ble.negotiated)).notify,
        "a session granted between two iterations starts unannounced");
  check(queuedPolls() == std::vector<unsigned>({1, 2}) && queuedSeqs().empty(),
        "and its relayed frames are not queued, while the last session's go up as plain frames");

  reset();
  connect();
  announce();
  relayAckHook(g_relay_seq);
  poll();
  relayAckHook(g_relay_seq);
  for (int i = 0; i < 3; ++i) {
    fake_ms += RELAY_ACK_TIMEOUT_MS;
    relayAckService();
  }
  check(bleRelaying(), "with nothing outstanding there is nothing to be late with");

  uint32_t v = 7;
  check(bleRelayAckValue(json::parse("0"), v) && v == 0 && bleRelayAckValue(json::parse("4294967295"), v) &&
        v == 4294967295u && bleRelayAckWrite("{\"relay_ack\":12}"), "relay_ack takes any uint32 the app writes");
  for (const char* bad : {"-1", "1.5", "\"3\"", "4294967296", "null"})
    check(!bleRelayAckValue(json::parse(bad), v) && !bleRelayAckWrite(std::string("{\"relay_ack\":") + bad + "}"),
          std::string("relay_ack refuses ") + bad + ", and the rx log keeps it");
}

// A log dump or an update pauses polling, so an announced app has nothing to ack meanwhile.
void hold() {
  reset();
  connect();
  announce();
  for (int busy = 0; busy < 2; ++busy) {
    (busy ? g_ota_active : g_logdump_active) = true;
    fake_ms += RELAY_ACK_TIMEOUT_MS * 3;
    relayAckService();
    check(bleRelaying(), "the relay clock waits while a log dump or an update runs");
    g_ota_active = g_logdump_active = false;
    fake_ms += RELAY_ACK_TIMEOUT_MS - 1;
    check(bleRelaying(), "and restarts when it ends");
  }
}

void persistence() {
  reset();
  connect();
  announce();
  poll();
  check(queuedSeqs().size() == 2 && saveUploadBacklog() && LittleFS.exists("/upload_backlog.jsonl") &&
        LittleFS.contents().find("relay_seq") == std::string::npos, "a relay seq is never written to the backlog");
  check(bleRelayAckWrite("{\"relay_ack\":3}") && !bleRelayAckWrite("{\"obd_alarm\":1}") &&
        !bleRelayAckWrite("{\"relay_ack\":3,\"x\":1}"), "only a lone relay_ack write is left out of the rx log");
}

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, void (*)()>> groups{
    {"old-app", oldApp}, {"acking", acking}, {"stall", stall}, {"never-ack", neverAck},
    {"resume", resume}, {"disconnect", disconnect}, {"presence", presence}, {"hold", hold}, {"sessions", sessions}, {"persistence", persistence},
  };
  for (const auto& group : groups) {
    if (argc > 1 && group.first != argv[1]) continue;
    group.second();
    std::cout << "ok " << group.first << '\n';
  }
}
