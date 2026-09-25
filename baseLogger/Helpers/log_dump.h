#ifndef LOG_DUMP_H
#define LOG_DUMP_H

/*
  BLE log retrieval (ABD7) - the app pulls the DataLogger CSV segments over Bluetooth instead of the
  serial DUMP + python collector (no more unplug-from-car / plug-to-computer round trip).

  PROTOCOL (app-driven pull; the app paces everything, the dongle never streams unprompted):
    app writes  {"LOGDUMP":"START"}  -> reply {"size":<total>,"chunk":<bytes>,"window":<chunks>}
    app writes  {"LOGDUMP":<offset>} -> up to <window> binary chunks, then {"eow":<next>,"eof":0|1}
    app writes  {"LOGDUMP":"END"}    -> session over, normal duty resumes
  Binary chunk frame: [0xD7][u32 offset LE][payload] - the 0xD7 magic disambiguates chunks from the JSON
  replies (which always start with '{'). Chunks carry their own offset so the app can detect a dropped
  notification and simply re-request from the last contiguous offset: reads are idempotent, so a re-request
  can never corrupt anything (unlike a write retry).

  Task discipline mirrors ota_update.h: logDumpCtrlHook runs on the Bluedroid task and only copies bytes +
  sets a flag; logDumpService() on the loop task does every LittleFS read and every notify. While a dump is
  active g_logdump_active pauses OBD polling (same reasons as the OTA pause: readPIDs blocks up to ~5s and
  writes rows to the same flash we are reading) - that is also what the app's confirm dialog tells the user.

  Liveness: the session self-releases on app END, 30s of inactivity, BLE disconnect, or an OTA start (the
  update owns the loop and the flash from that moment). RAM-only state - any reset clears it.
*/

#if ENABLE_BLE && ENABLE_LOGGING

static const uint8_t  LOGDUMP_CHUNK_MAGIC = 0xD7;
// Per-request read budget. A BYTE budget, not a chunk count: the chunk size follows the negotiated
// MTU (177 at iOS's 185, 239 at Android's 247), and a byte budget keeps the static buffer fixed.
// ~8KB keeps the one readAt() per request amortized and cheap.
static const size_t   LOGDUMP_WINDOW_BYTES = 8192;
// Inter-chunk yield. BOTH directions measured (2026-08-11, iOS, MTU 247), do not re-run the
// experiment: at 4ms, ~795ms/window and retries=0 across two full dumps; at 1ms, the SAME
// ~865ms/window (no gain - the ~20ms/chunk ceiling is the radio link granting ~1 notification per
// connection event, not this gap) but the TX queue eventually overflowed and Bluedroid hard-dropped
// a frame (esp_ble_gatts_send_ notify rc=-1) - it does NOT block on congestion. 4ms is matched to
// the drain rate: zero drops, zero cost. Faster transfers need link-level changes, not less pacing.
static const uint32_t LOGDUMP_NOTIFY_GAP_MS = 4;
static const uint32_t LOGDUMP_INACTIVITY_MS = 30000;
// Largest possible chunk payload (247 MTU ceiling - 3 ATT header - 5 frame header). 247 is the
// single-radio-packet limit - see the setMTU comment in blewrapper.cpp; bigger chunks are LOST.
static const size_t   LOGDUMP_CHUNK_MAX = 239;

// Command stash, Bluedroid task -> loop task (single-owner handoff like ota_cmd_present).
static volatile bool g_logdump_cmd_present = false;
static uint8_t g_logdump_cmd[64];
static volatile size_t g_logdump_cmd_len = 0;

// Session state - loop task only.
static size_t logdump_total = 0;
static size_t logdump_chunk = 0;
static size_t logdump_window_chunks = 0; // chunks per window at this session's negotiated chunk size
static unsigned long logdump_last_ms = 0;
// Session diagnostics, reported by the end event. `retries` is the tell: the app only re-requests an
// offset it has already been served when notifications were LOST in flight, so retries=0 with a slow
// transfer means link pacing, while a high retry count means chunks are being dropped and recovered.
static unsigned long logdump_started_ms = 0;
static uint32_t logdump_req_count = 0;
static uint32_t logdump_retry_count = 0;
static size_t logdump_next_expected = 0; // the offset a loss-free app would request next

/* Bluedroid-task hook: copy + flag, nothing else. A command arriving while one is still unconsumed is
   dropped - the protocol is strictly request/response, so the app never has two in flight. */
void logDumpCtrlHook(const uint8_t* d, size_t n) {
  if (!d || n == 0 || n >= sizeof(g_logdump_cmd) || g_logdump_cmd_present) return;
  memcpy(g_logdump_cmd, d, n);
  g_logdump_cmd_len = n;
  g_logdump_cmd_present = true;
}

static void logDumpNotifyJson(const char* s) {
  ble.notifyLogDump((const uint8_t*)s, strlen(s));
}

static void logDumpEnd(const char* why) {
  if (!g_logdump_active) return;
  g_logdump_active = false;
  logEvent("LOGDUMP end (%s) reqs=%lu retries=%lu served=%u/%u in %lus", why,
    (unsigned long)logdump_req_count, (unsigned long)logdump_retry_count,
    (unsigned)logdump_next_expected, (unsigned)logdump_total,
    (unsigned long)((millis() - logdump_started_ms) / 1000));
}

/* Loop-task service: consume one command per call, enforce the liveness guards. Cheap flag tests idle. */
void logDumpService() {
  if (g_logdump_active) {
    if (g_ota_active) logDumpEnd("ota takeover");
    else if (!ble.connected()) logDumpEnd("disconnect");
    else if (millis() - logdump_last_ms > LOGDUMP_INACTIVITY_MS) logDumpEnd("inactivity");
  }
  if (!g_logdump_cmd_present) return;

  json j = json::parse(std::string((const char*)g_logdump_cmd, g_logdump_cmd_len), nullptr, false);
  g_logdump_cmd_present = false;
  if (j.is_discarded() || !j.contains("LOGDUMP")) return;
  json v = j["LOGDUMP"];

  if (v.is_string() && v.get<std::string>() == "START") {
    if (g_ota_active)             { logDumpNotifyJson("{\"err\":\"busy\"}");   return; }
    if (!ble.get_authenticated()) { logDumpNotifyJson("{\"err\":\"noauth\"}"); return; }
    // A client that never negotiated MTU (Android default = 23) can't even receive the size header
    // in one notification - refuse with an error small enough to fit ANY MTU (20-byte notify limit)
    // instead of replying with something the client sees only a truncated prefix of.
    if (ble.mtu() < 64)           { logDumpNotifyJson("{\"err\":\"mtu\"}");    return; }
    // An unsubscribed client can't receive ANY reply - don't arm a session (pausing telemetry for
    // 30s) for a dump whose every frame would be dropped. The err lands in the readable value.
    if (!ble.logDumpSubscribed()) { logDumpNotifyJson("{\"err\":\"nosub\"}");  return; }
    // Idempotent re-START: the app retries START when the header notification is lost. Re-sending
    // the CURRENT header (no re-snapshot, no counter reset) keeps a delayed first header and the
    // retry's header identical - re-snapshotting here made them disagree on `size`, and the app
    // could adopt the stale smaller one and then never complete the final window.
    if (g_logdump_active) {
      logdump_last_ms = millis();
      char again[96];
      snprintf(again, sizeof(again), "{\"size\":%u,\"chunk\":%u,\"window\":%u}",
        (unsigned)logdump_total, (unsigned)logdump_chunk, (unsigned)logdump_window_chunks);
      logDumpNotifyJson(again);
      return;
    }
    // Log the start BEFORE snapshotting the size, so this row is part of the dump instead of dangling
    // past the size the app was told (rows appended after the snapshot are simply not fetched).
    logEvent("LOGDUMP start");
    datalogger.flush();
    logdump_total = datalogger.totalSize();
    uint16_t mtu = ble.mtu();
    if (mtu < 23) mtu = 23;
    if (mtu > 247) mtu = 247; // single-radio-packet ceiling; larger notifications are lost (measured)
    logdump_chunk = (size_t)mtu - 3 /*ATT header*/ - 5 /*chunk frame header*/;
    logdump_window_chunks = LOGDUMP_WINDOW_BYTES / logdump_chunk; // >=34 at 239, >=46 at 177
    g_logdump_active = true;
    logdump_last_ms = millis();
    logdump_started_ms = millis();
    logdump_req_count = 0;
    logdump_retry_count = 0;
    logdump_next_expected = 0;
    char reply[96];
    snprintf(reply, sizeof(reply), "{\"size\":%u,\"chunk\":%u,\"window\":%u}",
      (unsigned)logdump_total, (unsigned)logdump_chunk, (unsigned)logdump_window_chunks);
    logDumpNotifyJson(reply);
    // Diagnostic (cheap, once per START): sub=0 means the app's CCCD write never took effect and
    // notify() dropped the header silently; sub=1 with the app still timing out means the phone's
    // stack lost the delivery. These need different fixes - this line tells them apart.
    logEvent("LOGDUMP header sub=%d mtu=%u chunk=%u total=%u",
      ble.logDumpSubscribed() ? 1 : 0, (unsigned)ble.mtu(), (unsigned)logdump_chunk,
      (unsigned)logdump_total);
    return;
  }

  if (v.is_string() && v.get<std::string>() == "END") {
    logDumpEnd("app");
    return;
  }

  if (v.is_number_integer() || v.is_number_unsigned()) {
    if (!g_logdump_active) {
      // The session ended without the app's knowledge (inactivity, OTA takeover). Answer instead of
      // staying silent - silence costs the app its full stall-timeout budget (~45s) before it fails;
      // an err fails it in one round trip and the user can simply re-fetch.
      logDumpNotifyJson("{\"err\":\"ended\"}");
      return;
    }
    // ONE readAt per window, sliced into notifies from RAM. readAt walks the LittleFS directory and
    // opens segment files to locate the byte range - paying that per chunk throttled the whole
    // transfer to ~1-2KB/s (the "minutes for 10%" bug); amortized per ~8KB window it is noise.
    static uint8_t window_buf[LOGDUMP_WINDOW_BYTES];
    static uint8_t frame[5 + LOGDUMP_CHUNK_MAX];
    size_t off = (size_t)v.get<uint64_t>();
    logdump_last_ms = millis();
    logdump_req_count += 1;
    if (off < logdump_next_expected) logdump_retry_count += 1; // re-reading served bytes = loss recovery
    size_t want = logdump_chunk * logdump_window_chunks;
    if (off < logdump_total && logdump_total - off < want) want = logdump_total - off;
    size_t got = (off < logdump_total) ? datalogger.readAt(off, window_buf, want) : 0;
    if (got == 0 && off < logdump_total) {
      // The stream shrank under us (a mid-dump logEvent landed on a segment boundary and the budget
      // enforcement deleted the oldest segment) - the snapshot is invalid. Fail the session loudly:
      // replying eow at the same offset would have the app re-request it forever, and each request
      // refreshes the inactivity timer, so neither side would ever break the loop.
      logDumpNotifyJson("{\"err\":\"readfail\"}");
      logDumpEnd("read failure");
      return;
    }
    for (size_t sent = 0; sent < got; ) {
      size_t n = got - sent < logdump_chunk ? got - sent : logdump_chunk;
      frame[0] = LOGDUMP_CHUNK_MAGIC;
      frame[1] = (uint8_t)(off & 0xFF);
      frame[2] = (uint8_t)((off >> 8) & 0xFF);
      frame[3] = (uint8_t)((off >> 16) & 0xFF);
      frame[4] = (uint8_t)((off >> 24) & 0xFF);
      memcpy(frame + 5, window_buf + sent, n);
      ble.notifyLogDump(frame, 5 + n);
      sent += n;
      off += n;
      delay(LOGDUMP_NOTIFY_GAP_MS);
    }
    if (off > logdump_next_expected) logdump_next_expected = off;
    char eow[64];
    snprintf(eow, sizeof(eow), "{\"eow\":%u,\"eof\":%d}", (unsigned)off, off >= logdump_total ? 1 : 0);
    logDumpNotifyJson(eow);
    logdump_last_ms = millis();
    return;
  }
}

#else
inline void logDumpService() {}
#endif // ENABLE_BLE && ENABLE_LOGGING

#endif // LOG_DUMP_H
