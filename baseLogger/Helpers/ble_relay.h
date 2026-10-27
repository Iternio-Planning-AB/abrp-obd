#pragma once
/*
  The relay session (#90) - whether a connected, authenticated phone is actually carrying the telemetry - and the ABD9
  session channel it is driven over.
  Contains: the session state machine (relayAckService), v1's protocol entries (relayNextSeq, relayFrameQueued,
  relayTransportLive, relaySessionApply), the Bluedroid-task ABD9 hooks (relaySessionWrite, relaySessionRead) and the
  loop-task service that applies what they stash (sessionChannelService).

  A connected, authenticated phone used to count as the transport for as long as it stayed connected, and a
  frame it was notified was never queued for cellular. An app that stopped posting (backgrounded, logged out,
  offline) then silenced the car: an ID.5 drove 65 minutes in the BLE state with nothing reaching ABRP. ABD1 is
  notify-only, so the dongle cannot see that on its own; the app has to say what it posted.

  That is protocol v1 (Helpers/ble_protocol.h), negotiated in the ABD8 handshake; a v0 session (every app that does
  not negotiate) is trusted as before, frames included. In v1:
    ABD1 frames sent while the app is subscribed carry "seq":n beside "tlm" (per wake, from 1), and such a frame is
    ALSO queued for cellular under its seq (RELAY_SEQ_KEY). A frame sent while it is not subscribed carries no seq
    and is queued as a plain cellular frame.
    ABD9 is the session channel (write with response; notify; a readable state):
      app    -> {"relay_ack":n}       every frame up to seq n was posted (cumulative); its frames come off the queue
      app    -> {"relay":"stop"}      the app cannot post (offline, logged out, 401/403): cellular takes over at once
      app    -> {"relay":"resume"}    it can again; the only thing that ends a stop
      dongle -> {"relay":"lapsed","why":"timeout"|"stop"|"unsubscribed"} / {"relay":"live"}   on each transition
      state     {"relay":"live"|"lapsed"[,"why":..],"acked":n,"sent":m,"queued":k} (readable; {} outside v1)
    The timeout is the safety net for what the app cannot report (iOS suspending it): once tagged frames have waited
    RELAY_ACK_TIMEOUT_MS with no ack moving forward, cellular takes over with every frame still queued, and the next
    forward ack or a resume hands the transport back. Nothing outstanding, an update or a log dump holds the clock.
    A second bound covers an app that stops listening on ABD1 with everything acked: once frames have been queued
    untagged for RELAY_UNSUBSCRIBED_MAX_MS while it is not subscribed, the session lapses ("why":"unsubscribed"); it
    subscribing again, a forward ack or a resume hands the transport back.
  An ack beyond the last seq sent is ignored, so a peer cannot drop frames it was never sent, and a session's unacked
  frames lose their seq when it ends, so the next session's acks cannot drop them either.

  The session is one state machine, driven on the loop task:
    NO_SESSION -> GRANTED (v0 and no-protocol sessions stay here) or LIVE (a protocol that acks)
    LIVE -> LAPSED(stop) on a stop; LIVE -> LAPSED(timeout) when the clock runs out
    LIVE -> LAPSED(unsubscribed) when untagged frames have waited, the app unsubscribed, past the second bound
    LAPSED(timeout) -> LIVE on a forward ack or a resume; LAPSED(stop) -> LIVE on a resume only;
    LAPSED(unsubscribed) -> LIVE on subscribing to ABD1 again, a forward ack or a resume
    any -> NO_SESSION when the session ends: disconnect, authreset, AUTHCLEAR or a new grant
  Being subscribed to ABD1 is a condition, not a state: it decides whether a frame gets a seq, in LIVE and LAPSED.

  Task discipline: relaySessionWrite and relaySessionRead run on the Bluedroid task. The write hook parses one small
  JSON message (at most 64 B, like the ABD2 handler parses its writes) into the mailbox under the lock, and the read hook
  copies the state out under it; neither touches NVS, crypto, the log or ble. Everything else runs on the loop task.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

#include <ble_write_summary.h> // bleKeyForLog: an app's key reaches the log only if it is identifier-shaped

bool bleProtoRelayAcks(); // Helpers/ble_protocol.h (after this file): does the session's protocol ack its frames?

// The ABD9 mailbox from the Bluedroid task and the state it reads back, both under the lock. Acks are cumulative and the
// verbs idempotent, so a burst of writes between two loop passes collapses to {highest ack, last verb} without losing
// meaning and can never overflow. Entries carry the session grant they arrived under, so a stop written just before a
// disconnect is never applied to the next session.
portMUX_TYPE g_relay_mux = portMUX_INITIALIZER_UNLOCKED;
enum RelayVerb : uint8_t { RELAY_VERB_NONE, RELAY_VERB_STOP, RELAY_VERB_RESUME };
bool g_session_pending = false;       // anything below waiting
uint32_t g_session_pending_grant = 0; // the session grant it arrived under
uint32_t g_session_ack_pending = 0;   // highest relay_ack waiting; 0 = none
RelayVerb g_session_verb_pending = RELAY_VERB_NONE;
char g_session_bad_pending[25] = "";  // the first key in the burst that no version could take ("?" if unloggable)
char g_session_state[96] = "{}";      // ABD9's readable value, served by relaySessionRead
// "queued" is clamped to RELAY_STATE_QUEUED_MAX (the queue holds MAX_UPLOAD_BUFFER frames plus a few held ones).
static const unsigned long RELAY_STATE_QUEUED_MAX = 65535;
static_assert(sizeof("{\"relay\":\"lapsed\",\"why\":\"unsubscribed\",\"acked\":4294967295,\"sent\":4294967295,"
                     "\"queued\":65535}") - 1 <= 93,
  "the ABD9 state must be one read at the MTU 96 floor, never a long read");
static_assert(sizeof(g_session_state) > 93, "room for the worst-case state");

// Loop-task state. The seq counts every wake from 1 and never resets within it, so a seq names one frame.
enum RelayState : uint8_t { RELAY_NO_SESSION, RELAY_GRANTED, RELAY_LIVE, RELAY_LAPSED };
enum RelayWhy : uint8_t { RELAY_WHY_TIMEOUT, RELAY_WHY_STOP, RELAY_WHY_UNSUBSCRIBED };
uint32_t g_relay_seq = 0;          // the last seq notified this wake
uint32_t g_relay_grant = 0;        // the session grant the state below belongs to
RelayState g_relay_state = RELAY_NO_SESSION;
RelayWhy g_relay_why = RELAY_WHY_TIMEOUT; // why LAPSED
uint32_t g_relay_acked = 0;        // highest seq acked (or sent before the session) this session
unsigned long g_relay_live_ms = 0; // LIVE: when the clock last started (the session, a forward ack, a resume, a hold)
bool g_relay_ignored = false;      // logged an ack beyond the last seq this session
bool g_relay_subscribed = false;   // the app's ABD1 subscription, read once per loop pass
bool g_relay_unsub = false;        // an untagged frame was queued while the app was not subscribed (the second bound)
unsigned long g_relay_unsub_ms = 0; // when that bound's clock last started
std::vector<std::string> g_session_logged; // keys logged as ignored this session (a few at most)

// A relay_ack value: any uint32. Anything else is not one.
bool relayAckValue(const json& value, uint32_t& out) {
  if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX) return false;
  out = value.get<uint32_t>();
  return true;
}

static bool relaySession() { return g_relay_state == RELAY_LIVE || g_relay_state == RELAY_LAPSED; }

// The clock runs while tagged frames wait and nothing pauses polling. An update or a log dump pauses polling and is a
// phone busy on this link, so it has nothing to ack meanwhile; it changes no state, and the cellular gates stay closed
// for it on their own (!g_ota_active, !g_logdump_active).
static bool relayClockRunning() { return g_relay_acked != g_relay_seq && !g_ota_active && !g_logdump_active; }

static bool relayLate(unsigned long now) {
  return relayClockRunning() && now - g_relay_live_ms >= RELAY_ACK_TIMEOUT_MS;
}

// The second bound: frames have waited untagged, the app not listening on ABD1, for RELAY_UNSUBSCRIBED_MAX_MS. An app
// that acked everything and then lost its subscription (link and grant alive) acks nothing, so the clock above never
// runs; this keeps it from holding cellular off for the whole connection. An update or a log dump holds it too.
static bool relayUnsubscribedTooLong(unsigned long now) {
  return g_relay_unsub && !g_ota_active && !g_logdump_active && now - g_relay_unsub_ms >= RELAY_UNSUBSCRIBED_MAX_MS;
}

// v1's phoneIsTransport: LIVE and within both bounds. A grant the service has not started yet is LIVE from its start.
bool relayTransportLive() {
  if (g_relay_grant != bleAuthGrant()) return true;
  const unsigned long now = millis();
  return g_relay_state == RELAY_LIVE && !relayLate(now) && !relayUnsubscribedTooLong(now);
}

// v1's nextSeq: the seq the next ABD1 frame carries, 0 (none) unless the app is subscribed to ABD1.
uint32_t relayNextSeq() {
  return relaySession() && g_relay_grant == bleAuthGrant() && g_relay_subscribed ? g_relay_seq + 1 : 0;
}

// v1's frameQueued: a frame with data was queued for cellular, tagged with the seq relayNextSeq() named and notified on
// ABD1, or untagged (0) because the app was not subscribed. The first untagged one starts the second bound.
void relayFrameQueued(uint32_t seq) {
  if (seq) g_relay_seq = seq;
  else if (relaySession() && !g_relay_subscribed && !g_relay_unsub) {
    g_relay_unsub = true;
    g_relay_unsub_ms = millis();
  }
}

static size_t relayQueueSize() {
#if ENABLE_NETWORK
  return obd_result_vector.size() + g_upload_held.size();
#else
  return 0;
#endif
}

// Rebuild ABD9's readable state. The loop task never calls setValue on ABD9: relaySessionRead hands this buffer to the
// read callback under the lock, so a read can never race a write of the value, and a notify never replaces it.
static void relayStateRefresh() {
  char text[sizeof(g_session_state)];
  if (!relaySession()) snprintf(text, sizeof(text), "{}");
  else
    snprintf(text, sizeof(text), "{\"relay\":\"%s\"%s,\"acked\":%lu,\"sent\":%lu,\"queued\":%lu}",
      g_relay_state == RELAY_LIVE ? "live" : "lapsed",
      g_relay_state == RELAY_LIVE ? "" : g_relay_why == RELAY_WHY_STOP ? ",\"why\":\"stop\""
        : g_relay_why == RELAY_WHY_UNSUBSCRIBED ? ",\"why\":\"unsubscribed\"" : ",\"why\":\"timeout\"",
      (unsigned long)g_relay_acked, (unsigned long)g_relay_seq,
      std::min((unsigned long)relayQueueSize(), RELAY_STATE_QUEUED_MAX));
  portENTER_CRITICAL(&g_relay_mux);
  memcpy(g_session_state, text, sizeof(text));
  portEXIT_CRITICAL(&g_relay_mux);
}

#if ENABLE_NETWORK
// A frame's relay seq; 0 when it was not relayed to an app that acks.
static uint32_t relaySeqOf(const OBDVector& frame) {
  for (const auto& value : frame)
    if (isRelaySeqKey(value.first)) return (uint32_t)value.second;
  return 0;
}

// Take the frames up to seq off the cellular queue.
static void relayDropAcked(std::vector<OBDVector>& frames, uint32_t seq) {
  frames.erase(std::remove_if(frames.begin(), frames.end(), [&](const OBDVector& frame) {
    const uint32_t frame_seq = relaySeqOf(frame);
    return frame_seq && frame_seq <= seq;
  }), frames.end());
}

// At a session's end its unacked frames become plain cellular frames: the next session's acks name its own seqs only.
static size_t relayUntag() {
  size_t n = 0;
  for (auto* frames : { &obd_result_vector, &g_upload_held })
    for (auto& frame : *frames) {
      const size_t before = frame.size();
      frame.erase(std::remove_if(frame.begin(), frame.end(), [](const OBDPair& value) { return isRelaySeqKey(value.first); }),
        frame.end());
      n += before - frame.size();
    }
  return n;
}

static size_t relayQueued() {
  size_t n = 0;
  for (const auto* frames : { &obd_result_vector, &g_upload_held })
    for (const auto& frame : *frames) n += relaySeqOf(frame) != 0;
  return n;
}
#endif

// Entering LAPSED: cellular takes over, and its first upload goes now rather than at the end of a batching interval.
// No grace: the link is still up, so there is no reconnect handshake for the POST to collide with.
static void relayLapse(RelayWhy why, unsigned long now) {
  g_relay_state = RELAY_LAPSED;
  g_relay_why = why;
  g_ble_handoff_pending = true;
#if ENABLE_NETWORK
  const unsigned unacked = (unsigned)relayQueued();
#else
  const unsigned unacked = 0;
#endif
  if (why == RELAY_WHY_STOP)
    logEvent("BLE relay stop by app acked=%lu sent=%lu unacked=%u -> cellular", (unsigned long)g_relay_acked,
      (unsigned long)g_relay_seq, unacked);
  else if (why == RELAY_WHY_UNSUBSCRIBED)
    logEvent("BLE relay lapsed unsubscribed for %lus queued=%u -> cellular",
      (unsigned long)((now - g_relay_unsub_ms) / 1000), (unsigned)relayQueueSize());
  else
    logEvent("BLE relay lapsed no ack for %lus acked=%lu sent=%lu unacked=%u -> cellular",
      (unsigned long)((now - g_relay_live_ms) / 1000), (unsigned long)g_relay_acked, (unsigned long)g_relay_seq, unacked);
  relayStateRefresh();
  bleNotifySession(why == RELAY_WHY_STOP ? "{\"relay\":\"lapsed\",\"why\":\"stop\"}"
                   : why == RELAY_WHY_UNSUBSCRIBED ? "{\"relay\":\"lapsed\",\"why\":\"unsubscribed\"}"
                   : "{\"relay\":\"lapsed\",\"why\":\"timeout\"}");
}

// Entering LIVE again (a forward ack, a resume, or the app subscribing to ABD1 again): the phone carries the data, and
// both clocks start over.
enum RelayBack : uint8_t { RELAY_BACK_ACK, RELAY_BACK_RESUME, RELAY_BACK_SUBSCRIBED };
static void relayLive(unsigned long now, RelayBack how, uint32_t ack) {
  const unsigned long after = (now - g_relay_live_ms) / 1000;
  if (how == RELAY_BACK_ACK) logEvent("BLE relay resumed ack=%lu after %lus", (unsigned long)ack, after);
  else if (how == RELAY_BACK_RESUME) logEvent("BLE relay resume by app after %lus", after);
  else logEvent("BLE relay resubscribed after %lus", after);
  g_relay_state = RELAY_LIVE;
  g_relay_live_ms = now;
  g_relay_unsub = false;
  relayStateRefresh();
  bleNotifySession("{\"relay\":\"live\"}");
}

static void relayAck(uint32_t ack) {
  if (ack > g_relay_seq) {
    if (!g_relay_ignored) logEvent("BLE relay ack ignored ack=%lu sent=%lu", (unsigned long)ack, (unsigned long)g_relay_seq);
    g_relay_ignored = true;
    return;
  }
  // A repeat of an old ack proves nothing and does not hold cellular off.
  if (ack <= g_relay_acked) return;
#if ENABLE_NETWORK
  relayDropAcked(obd_result_vector, ack);
  relayDropAcked(g_upload_held, ack);
#endif
  g_relay_acked = ack;
  const unsigned long now = millis();
  // Moving forward: the app is posting. It ends a timeout or unsubscribed lapse and restarts both clocks; a stop it does
  // not end (an ack from a post that was already in flight when the app stopped takes its frames off, and the stop
  // stands until the app resumes).
  if (g_relay_state == RELAY_LAPSED && g_relay_why != RELAY_WHY_STOP) relayLive(now, RELAY_BACK_ACK, ack);
  else if (g_relay_state == RELAY_LIVE) {
    g_relay_live_ms = now;
    g_relay_unsub = false;
  }
}

// v1's sessionApply: an ABD9 burst, the ack first (0: none), then the verb. False outside a relay session.
bool relaySessionApply(uint32_t ack, RelayVerb verb) {
  if (!relaySession()) return false;
  if (ack) relayAck(ack);
  if (verb == RELAY_VERB_STOP && !(g_relay_state == RELAY_LAPSED && g_relay_why == RELAY_WHY_STOP))
    relayLapse(RELAY_WHY_STOP, millis());
  if (verb == RELAY_VERB_RESUME && g_relay_state == RELAY_LAPSED) relayLive(millis(), RELAY_BACK_RESUME, 0);
  return true;
}

// The session ended: its unacked frames go up over cellular as plain frames.
static void relayEnd() {
  if (relaySession()) {
#if ENABLE_NETWORK
    const size_t unacked = relayUntag();
    logEvent("BLE relay end acked=%lu sent=%lu unacked=%u", (unsigned long)g_relay_acked,
      (unsigned long)g_relay_seq, (unsigned)unacked);
#else
    logEvent("BLE relay end acked=%lu sent=%lu", (unsigned long)g_relay_acked, (unsigned long)g_relay_seq);
#endif
  }
  g_relay_state = RELAY_NO_SESSION;
  g_relay_ignored = false;
  g_relay_unsub = false;
  g_session_logged.clear();
}

// A new grant: GRANTED, or LIVE when its protocol acks frames (a table field, never a version number).
static void relayStart(unsigned long now) {
  if (!bleProtoRelayAcks()) {
    g_relay_state = RELAY_GRANTED;
    return;
  }
  g_relay_state = RELAY_LIVE;
  g_relay_acked = g_relay_seq; // frames sent before the session were never tagged: nothing to ack
  g_relay_live_ms = now;
  logEvent("BLE relay acks on from seq=%lu timeout=%lus", (unsigned long)(g_relay_seq + 1),
    (unsigned long)(RELAY_ACK_TIMEOUT_MS / 1000));
}

// Loop task, every pass, after authGateService (which selects a new grant's protocol) and before the ABD1 send.
void relayAckService() {
  const bool authenticated = bleAuthenticated();
  const uint32_t grant = bleAuthGrant();
  const unsigned long now = millis();
  if (!authenticated || grant != g_relay_grant) {
    // The session is over, even when a new one was granted since the last pass.
    if (g_relay_state != RELAY_NO_SESSION) relayEnd();
    g_relay_grant = grant;
    if (authenticated) relayStart(now); // ABD9's state follows in sessionChannelService, later this pass
  }
  // Read once per pass, before the ABD1 send, so the state machine and the send agree.
  g_relay_subscribed = relaySession() && bleTelemetrySubscribed();
  if (g_relay_subscribed) {
    g_relay_unsub = false;
    if (g_relay_state == RELAY_LAPSED && g_relay_why == RELAY_WHY_UNSUBSCRIBED) relayLive(now, RELAY_BACK_SUBSCRIBED, 0);
  }
  if (g_relay_unsub && (g_ota_active || g_logdump_active)) g_relay_unsub_ms = now; // held, like the ack clock
  if (g_relay_state != RELAY_LIVE) return;
  // Nothing outstanding is nothing to be late with, and an update or a log dump holds the clock.
  if (!relayClockRunning()) g_relay_live_ms = now;
  else if (relayLate(now)) relayLapse(RELAY_WHY_TIMEOUT, now);
  if (g_relay_state == RELAY_LIVE && relayUnsubscribedTooLong(now)) relayLapse(RELAY_WHY_UNSUBSCRIBED, now);
}

// The longest session message is {"relay_ack":4294967295} (24 B); nothing longer is parsed on the Bluedroid task.
static const size_t RELAY_SESSION_WRITE_MAX = 64;

// Bluedroid task (BLEWrapper's ABD9 onWrite): stash only. Dropped unless the link holds a session grant; parsed here, so
// the mailbox holds only what can collapse, but applied on the loop task. It parses one small JSON message (at most
// RELAY_SESSION_WRITE_MAX bytes, as the ABD2 handler parses its writes) and does nothing else: no NVS, no crypto, no
// logging, no ble calls.
void relaySessionWrite(const uint8_t* data, size_t len) {
  if (!bleAuthenticated() || !data || !len) return;
  const uint32_t grant = bleAuthGrant();
  // Through std::string, as every other parse in the sketch: a pointer range would instantiate a second parser (~20 KB).
  const json message = len > RELAY_SESSION_WRITE_MAX ? json(json::value_t::discarded)
                                                     : json::parse(std::string((const char*)data, len), nullptr, false);
  uint32_t ack = 0;
  RelayVerb verb = RELAY_VERB_NONE;
  char bad[sizeof(g_session_bad_pending)] = "";
  if (message.is_discarded() || !message.is_object() || message.empty()) snprintf(bad, sizeof(bad), "?");
  else
    for (auto it = message.begin(); it != message.end(); ++it) {
      uint32_t v;
      if (it.key() == "relay_ack" && relayAckValue(it.value(), v)) ack = std::max(ack, v);
      else if (it.key() == "relay" && it.value() == "stop") verb = RELAY_VERB_STOP;
      else if (it.key() == "relay" && it.value() == "resume") verb = RELAY_VERB_RESUME;
      else if (!bad[0]) snprintf(bad, sizeof(bad), "%s", bleKeyForLog(it.key()));
    }
  portENTER_CRITICAL(&g_relay_mux);
  if (g_session_pending && g_session_pending_grant != grant) {
    g_session_ack_pending = 0;
    g_session_verb_pending = RELAY_VERB_NONE;
    g_session_bad_pending[0] = '\0';
  }
  g_session_pending = true;
  g_session_pending_grant = grant;
  if (ack > g_session_ack_pending) g_session_ack_pending = ack;
  if (verb != RELAY_VERB_NONE) g_session_verb_pending = verb;
  if (bad[0] && !g_session_bad_pending[0]) memcpy(g_session_bad_pending, bad, sizeof(bad));
  portEXIT_CRITICAL(&g_relay_mux);
}

// Bluedroid task (BLEWrapper's ABD9 onRead, just before the library builds the read response): the current state.
size_t relaySessionRead(uint8_t* out, size_t size) {
  if (!out || !size) return 0;
  portENTER_CRITICAL(&g_relay_mux);
  size_t n = strnlen(g_session_state, sizeof(g_session_state));
  if (n > size) n = size;
  memcpy(out, g_session_state, n);
  portEXIT_CRITICAL(&g_relay_mux);
  return n;
}

// Loop task: take what ABD9 stashed since the last pass. False when nothing was waiting. sessionChannelService
// (Helpers/ble_protocol.h, which knows the session's protocol) applies it.
static bool relayTakeMailbox(uint32_t& grant, uint32_t& ack, RelayVerb& verb, char* bad, size_t bad_size) {
  portENTER_CRITICAL(&g_relay_mux);
  const bool pending = g_session_pending;
  grant = g_session_pending_grant;
  ack = g_session_ack_pending;
  verb = g_session_verb_pending;
  memcpy(bad, g_session_bad_pending, bad_size < sizeof(g_session_bad_pending) ? bad_size : sizeof(g_session_bad_pending));
  g_session_pending = false;
  g_session_ack_pending = 0;
  g_session_verb_pending = RELAY_VERB_NONE;
  g_session_bad_pending[0] = '\0';
  portEXIT_CRITICAL(&g_relay_mux);
  bad[bad_size - 1] = '\0';
  return pending;
}

// A message the session's version does not take is logged once per key and session, by key only, and ignored. At most
// RELAY_IGNORED_KEYS_LOGGED keys per session, then one line: a peer cycling through keys must not fill the flash log.
static const size_t RELAY_IGNORED_KEYS_LOGGED = 8;
static bool relayFirstIgnore(const char* key) {
  for (const auto& logged : g_session_logged)
    if (logged == key) return false;
  if (g_session_logged.size() < RELAY_IGNORED_KEYS_LOGGED) {
    g_session_logged.push_back(key);
    return true;
  }
  if (g_session_logged.size() == RELAY_IGNORED_KEYS_LOGGED) {
    g_session_logged.push_back(""); // the marker: the line below is written once per session
    logEvent("BLE session further ignored keys not logged");
  }
  return false;
}
