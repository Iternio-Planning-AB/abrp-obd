#pragma once
/*
  Relay acks (#90) - whether a connected, authenticated phone is actually carrying the telemetry.
  Contains: the Bluedroid-task stash for the app's ABD2 "relay_ack" writes (relayAckHook), the loop-task
  service that applies them (relayAckService), the transport predicate bleRelaying() adds to authentication
  (relayTransportLive) and the seq the next ABD1 frame carries (relayNextSeq).

  A connected, authenticated phone used to count as the transport for as long as it stayed connected, and a
  frame it was notified was never queued for cellular. An app that stopped posting (backgrounded, logged out,
  offline) then silenced the car: an ID.5 drove 65 minutes in the BLE state with nothing reaching ABRP. ABD1 is
  notify-only, so the dongle cannot see that on its own; the app has to say what it posted.

  The protocol, all on ABD2 after AUTH, and only from an app that saw "relay_ack":1 in the ABD5 info JSON:
    {"relay_ack":0}  announce: this app acks. From here every ABD1 frame carries "seq":n (per wake, from 1).
    {"relay_ack":n}  every frame up to seq n was posted (cumulative).
  An app that never announces is trusted as before, so today's apps keep today's behaviour, frames included.
  Once announced, a relayed frame is ALSO queued for cellular under its seq (RELAY_SEQ_KEY), and an ack takes it
  off. The phone stays the transport while acks move forward; RELAY_ACK_TIMEOUT_MS after the announce or the last
  ack that did, cellular takes over with every frame still queued, and the next ack that moves forward hands the
  transport back. An ack beyond the last seq sent is ignored, so a peer cannot drop frames it was never sent.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

// Mailbox from the Bluedroid task. Written under the lock by relayAckHook, emptied by relayAckService.
portMUX_TYPE g_relay_mux = portMUX_INITIALIZER_UNLOCKED;
bool g_relay_announce_pending = false;
uint32_t g_relay_ack_pending = 0; // highest ack waiting; 0 = none

// Loop-task state. The seq counts every wake from 1 and never resets within it, so a seq names one frame.
uint32_t g_relay_seq = 0;          // the last seq notified this wake
bool g_relay_announced = false;    // this session's app acks
uint32_t g_relay_acked = 0;        // highest seq acked (or sent before the announce) this session
unsigned long g_relay_live_ms = 0; // the announce, or the last ack that moved forward
bool g_relay_lapsed = false;       // logged the lapse, not yet a resume
bool g_relay_ignored = false;      // logged an ack beyond the last seq this session

// Bluedroid task: stash only.
void relayAckHook(uint32_t v) {
  portENTER_CRITICAL(&g_relay_mux);
  if (v == 0) g_relay_announce_pending = true;
  else if (v > g_relay_ack_pending) g_relay_ack_pending = v;
  portEXIT_CRITICAL(&g_relay_mux);
}

bool relayTransportLive() {
  return !g_relay_announced || millis() - g_relay_live_ms < RELAY_ACK_TIMEOUT_MS;
}

// The seq the next ABD1 frame carries: 0 (none) unless this session's app announced.
uint32_t relayNextSeq() {
  return g_relay_announced ? g_relay_seq + 1 : 0;
}

#if ENABLE_NETWORK
// Take the frames up to seq off the cellular queue.
static void relayDropAcked(std::vector<OBDVector>& frames, uint32_t seq) {
  frames.erase(std::remove_if(frames.begin(), frames.end(), [&](const OBDVector& frame) {
    for (const auto& value : frame)
      if (isRelaySeqKey(value.first)) return value.second <= seq;
    return false;
  }), frames.end());
}

static size_t relayQueued() {
  size_t n = 0;
  for (const auto* frames : { &obd_result_vector, &g_upload_held })
    for (const auto& frame : *frames)
      for (const auto& value : frame)
        if (isRelaySeqKey(value.first)) { ++n; break; }
  return n;
}
#endif

void relayAckService() {
  portENTER_CRITICAL(&g_relay_mux);
  const bool announce = g_relay_announce_pending;
  const uint32_t ack = g_relay_ack_pending;
  g_relay_announce_pending = false;
  g_relay_ack_pending = 0;
  portEXIT_CRITICAL(&g_relay_mux);

  if (!bleAuthenticated()) {
    // The session is over: its frames still queued go up over cellular, and what is in the mailbox was its.
    if (g_relay_announced) {
#if ENABLE_NETWORK
      logEvent("BLE relay end acked=%lu sent=%lu unacked=%u", (unsigned long)g_relay_acked,
        (unsigned long)g_relay_seq, (unsigned)relayQueued());
#endif
    }
    g_relay_announced = g_relay_lapsed = g_relay_ignored = false;
    return;
  }
  const unsigned long now = millis();
  if (announce && !g_relay_announced) {
    g_relay_announced = true;
    g_relay_acked = g_relay_seq; // frames sent before the announce were never queued: nothing to ack
    g_relay_live_ms = now;
    logEvent("BLE relay acks on from seq=%lu timeout=%lus", (unsigned long)(g_relay_seq + 1),
      (unsigned long)(RELAY_ACK_TIMEOUT_MS / 1000));
  }
  if (!g_relay_announced) return;
  if (ack > g_relay_seq) {
    if (!g_relay_ignored) logEvent("BLE relay ack ignored ack=%lu sent=%lu", (unsigned long)ack, (unsigned long)g_relay_seq);
    g_relay_ignored = true;
  } else if (ack && (ack > g_relay_acked || g_relay_acked == g_relay_seq)) {
    // Moving forward, or nothing outstanding: the app is posting. A repeat of an old ack with frames still
    // unacked proves nothing and does not hold cellular off.
#if ENABLE_NETWORK
    if (ack > g_relay_acked) {
      relayDropAcked(obd_result_vector, ack);
      relayDropAcked(g_upload_held, ack);
    }
#endif
    if (ack > g_relay_acked) g_relay_acked = ack;
    if (g_relay_lapsed)
      logEvent("BLE relay resumed ack=%lu after %lus", (unsigned long)ack, (unsigned long)((now - g_relay_live_ms) / 1000));
    g_relay_live_ms = now;
    g_relay_lapsed = false;
  }
  if (g_ota_active || g_logdump_active) {
    // An update or a log dump pauses polling, so there is nothing to ack, and either is a phone busy on this link:
    // the clock waits for it, and cellular stays off so a reconnect cannot block the loop under the transfer.
    g_relay_live_ms = now;
    g_relay_lapsed = false;
  }
  if (!g_relay_lapsed && !relayTransportLive()) {
    // Cellular takes over: the first upload goes now rather than at the end of a batching interval. No grace:
    // the link is still up, so there is no reconnect handshake for the POST to collide with.
    g_relay_lapsed = true;
    g_ble_handoff_pending = true;
#if ENABLE_NETWORK
    logEvent("BLE relay lapsed no ack for %lus acked=%lu sent=%lu unacked=%u -> cellular",
      (unsigned long)((now - g_relay_live_ms) / 1000), (unsigned long)g_relay_acked, (unsigned long)g_relay_seq,
      (unsigned)relayQueued());
#endif
  }
}
