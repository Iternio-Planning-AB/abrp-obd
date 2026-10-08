#pragma once
/*
  Network upload helpers (ENABLE_NETWORK) - the cloud upload pipeline.
  Contains: the bulk POST + response handling (logNetwork), the batched drain of the RAM
  frame buffer with the cellular byte-size cap (uploadBuffered), and the store-and-forward
  backlog that persists unsent frames across deep sleep (saveUploadBacklog /
  loadUploadBacklog).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

#if ENABLE_NETWORK
#if ENABLE_LOGGING
#include <mbedtls/sha256.h>
#include <cmath>

constexpr size_t UPLOAD_DIAGNOSTIC_MESSAGE_BYTES = 120;
constexpr size_t UPLOAD_DIAGNOSTIC_SHA256_BYTES = 6;
unsigned long g_upload_request_id = 0;

bool uploadErrorContains(const std::string& body, const char* phrase) {
  const size_t n = strlen(phrase);
  for (size_t i = 0; i + n <= body.size(); ++i) {
    size_t j = 0;
    for (; j < n; ++j) {
      unsigned char c = body[i + j];
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != (unsigned char)phrase[j]) break;
    }
    if (j == n) return true;
  }
  return false;
}

const char* uploadErrorCategory(const std::string& body) {
  if (body.empty()) return "empty";
  if ((!tlm_push_token.empty() && body.find(tlm_push_token) != std::string::npos) ||
      (!api_key.empty() && body.find(api_key) != std::string::npos)) return "redacted";
  for (const char* marker : {"token", "api_key", "api-key", "authorization", "bearer", "password", "secret"})
    if (uploadErrorContains(body, marker)) return "redacted";
  size_t credential_run = 0;
  for (unsigned char c : body) {
    bool credential_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-';
    credential_run = credential_char ? credential_run + 1 : 0;
    if (credential_run >= 24) return "redacted";
  }
  if (uploadErrorContains(body, "<html") || uploadErrorContains(body, "<!doctype")) return "html";
  if (uploadErrorContains(body, "invalid header")) return "gzip_header";
  if (uploadErrorContains(body, "checksum")) return "gzip_checksum";
  if (uploadErrorContains(body, "length check")) return "gzip_length";
  if (uploadErrorContains(body, "unexpected eof")) return "incomplete";
  if (uploadErrorContains(body, "gzip")) return "gzip_other";
  if (uploadErrorContains(body, "unauthorized") || uploadErrorContains(body, "forbidden")) return "auth";
  if (uploadErrorContains(body, "validation")) return "validation";
  return "other";
}
#endif
/*
  Use the activated network to send data to Iternio API.
  Takes a vector of multiple OBDVectors, allows for temporary storage of data if connection is lost for shorter periods of time
*/
bool logNetwork(const std::vector<OBDVector>& obd_result_vector) {
#if PRINT
  Serial.printf("Logging via network (bulk)\n");
  Serial.printf("URL: %s\n", network.getUrl().c_str());
#endif

  // All buffered frames go up in a single POST, instead of one request per frame. Far fewer requests ->
  // less data/airtime, which is the whole point of buffering.
  std::string payload = bulkStringV2(obd_result_vector);
  if (payload == "[]") {
    // Nothing in this batch survived the v2 mapping (every key unmapped, or no usable utc). Report
    // success so the caller clears the frames - re-sending them would fail the same way forever.
    g_last_status = "ok_empty";
    return true;
  }

  HeaderVector headers;
  // v2 authenticates the WRITE with the vehicle's telemetry token and ATTRIBUTES it with the api key,
  // both as headers - where v1 took the token as a ?token= query param and the key as Authorization.
  headers.emplace_back("X-TLM-TOKEN", tlm_push_token);
  headers.emplace_back("X-API-KEY", api_key);
  headers.emplace_back("Content-Type", "application/json");

  // Compress if we can, send plain if we cannot. A body that goes up uncompressed is worse - it carries
  // fewer points under the modem's cap - but it is still a working upload, and losing telemetry because
  // PSRAM was unavailable would be a far bigger failure than losing the ratio.
  const size_t raw_bytes = payload.size();
  std::string gzipped;
  const bool gzip_used = gzipBody(payload, gzipped);
  if (gzip_used) {
    headers.emplace_back("Content-Encoding", "gzip");
    payload.swap(gzipped);
  }
  network.set_headers(headers);
#if ENABLE_LOGGING
  // Both sizes, every POST. The compressor lives in PSRAM and is allocated at runtime, so NOTHING in a
  // build or a boot log says whether compression is actually engaging - and the failure mode is silent
  // by design (an uncompressed body still uploads, just with fewer points under the modem's cap). raw
  // == wire in the dump means gzip is off; that is the line to look for on the first drive.
  const unsigned long request_id = ++g_upload_request_id;
  unsigned char digest[32];
  char fingerprint[UPLOAD_DIAGNOSTIC_SHA256_BYTES * 2 + 1] = "?";
  if (mbedtls_sha256_ret((const unsigned char*)payload.data(), payload.size(), digest, 0) == 0) {
    for (size_t i = 0; i < UPLOAD_DIAGNOSTIC_SHA256_BYTES; ++i)
      snprintf(fingerprint + i * 2, 3, "%02x", digest[i]);
  }
  unsigned long source_first = 0, source_last = 0;
  for (const auto& frame : obd_result_vector) {
    for (const auto& res : frame) {
      if (res.first != "utc" || !std::isfinite(res.second) || res.second <= 0 || res.second > UINT32_MAX) continue;
      unsigned long utc = (unsigned long)res.second;
      if (!source_first || utc < source_first) source_first = utc;
      if (utc > source_last) source_last = utc;
    }
  }
  const std::string& raw = gzip_used ? gzipped : payload;
  unsigned points = 0;
  for (size_t pos = 0; (pos = raw.find("\"provider\":", pos)) != std::string::npos; ++pos) ++points;
  char diagnostic[UPLOAD_DIAGNOSTIC_MESSAGE_BYTES];
  snprintf(diagnostic, sizeof(diagnostic),
    "UPLOAD body raw=%uB wire=%uB id=%lu enc=%c sha=%s src=%u pts=%u sutc=%lu..%lu",
    (unsigned)raw_bytes, (unsigned)payload.size(), request_id, gzip_used ? 'g' : 'j', fingerprint,
    (unsigned)obd_result_vector.size(), points, source_first, source_last);
  logEvent("%s", diagnostic);
#endif

  unsigned long post_t0 = millis();
  HttpResponse response = network.sendBulk(payload);
  unsigned long post_ms = millis() - post_t0; // instant-fail vs full-timeout distinguishes wedge modes
  g_last_post_ms = post_ms;
  g_last_http = response.responseCode; // recorded in the data log
#if ENABLE_LOGGING
  const char* error_category = response.responseCode == 200 ? "none" : uploadErrorCategory(response.payload);
  const auto transfer = network.transferDiagnostics();
  snprintf(diagnostic, sizeof(diagnostic),
    "UPLOAD id=%lu http=%d err=%s t=%lu p/a/r=%d/%d/%d rx=%d/%d/%d cut=%d w=?",
    request_id, response.responseCode, error_category, post_ms, transfer.prompt_result,
    transfer.body_ack_result, transfer.post_result, transfer.declared_response_length,
    transfer.shread_length, transfer.captured_response_length, (int)transfer.response_incomplete);
  if (response.responseCode == 200) logEvent("%s", diagnostic);
#endif

  // The server's Date response header is a coarse time anchor. It's present on ANY HTTP response - even one
  // the API rejects for a missing utc - so it works as a fallback time source even before we have a clock.
  // setClock() ignores it while GPS is fresh, so it only takes effect when GPS is stale/absent.
  time_t serverEpoch = parseHttpDate(network.getDateHeader().c_str());
  if (serverEpoch) setClock(CLK_HTTP, serverEpoch);
#if PRINT
  Serial.printf("Bulk response: %d [body omitted]\n", response.responseCode);
#endif

  // HTTP error codes are reserved for severe (API key) issues.
  if (response.responseCode != 200) {
    ++logging_errors; // tracked so the caller resets the network after too many failures in a row
    g_last_status = "http_err";
    Serial.printf("Failed to upload bulk tlm (HTTP %d): [body omitted]\n", response.responseCode);
#if ENABLE_LOGGING
    // Classify the server's rejection without logging its body or credentials. This
    // keeps failure evidence in the flash DUMP instead of only on Serial.
    logEvent("%s", diagnostic);
#endif
    return false;
  }

  /*
    HTTP 200 IS the acceptance under v2, and the body is not consulted at all.

    v1 needed the extra check because it answered 200 for logical errors too and put the real verdict in
    a "status" field. v2 does not: it signals every failure with a status CODE (400/401/403/404/5xx),
    all of which the branch above already treats as a failed upload, and its 200 body is
    {mapInfo, elevation} with no status field anywhere in it. Carrying the old check across would have
    read that as `status != "ok"` on EVERY successful upload - frames retained, re-sent, and the backlog
    growing until the fail-cap silenced the drive. Which is exactly the snowball the ok_noparse branch
    was added to stop, so the parse-and-guess handling goes with it.
  */
  logging_errors = 0;
  g_last_status = "ok";
  return true;
}

// Upload the RAM buffer in bounded batches (<= MAX_FRAMES_PER_POST per POST) so a backlog can't build a
// giant, timeout-prone payload. Sent frames are erased on success. Assumes the link is already connected
// (caller does network.connect()/disconnect()). Stops on the first failed POST (never hammers a dead link).
//   drain=false: steady-state loop. Sends ONE batch normally; when a backlog has accumulated (link was down,
//                BLE session banked frames) it BURSTS up to UPLOAD_MAX_BATCHES_PER_CYCLE small POSTs so the
//                catch-up completes in one cycle instead of trickling at ~1 net frame/cycle (which left the
//                app minutes behind "now"). The modem's proven limit is per-POST body SIZE (>=7 frames never
//                delivered), NOT post rate - the field dump shows 52- and 67-frame backlogs draining as
//                back-to-back 3-4-frame POSTs ~1.5s apart without a hiccup.
//   drain=true : keep sending until the buffer is empty or a POST fails (pre-sleep flush - last chance
//                before deep sleep wipes RAM). Returns true if every attempted POST succeeded.
bool uploadBuffered(bool drain) {
  bool ok = true;
  int batches = 0;
  while (!obd_result_vector.empty()) {
    esp_task_wdt_reset(); // a full drain (pre-sleep flush of a big backlog) is many sequential POSTs, each
                          // up to ~22s - legitimate, so feed per batch; a wedge INSIDE one POST still trips
    // After a connection-level failure, probe with a SMALL batch: in the field each retry grew the payload
    // (2->5->8->11->14->17 frames, ~4.5KB) with every POST timing out 15-22s - retrying BIGGER exactly when
    // the link is weakest. A ~800B probe has far better odds; once it succeeds the streak resets and the
    // backlog drains at full batch size again.
    size_t cap = (g_http0_streak > 0) ? 3 : (size_t)MAX_FRAMES_PER_POST;
    size_t n = obd_result_vector.size();
    if (n > cap) n = cap;
    std::vector<OBDVector> batch(obd_result_vector.begin(), obd_result_vector.begin() + n);
#if ENABLE_CELL
    // Enforce the modem's per-POST body ceiling in BYTES, not frames: the SIM7070 session is configured
    // with SHCONF BODYLEN=1024 (its hard maximum), and an oversized body makes the POST undeliverable.
    //
    // Measured on what actually goes on the wire, which is the GZIPPED body - trimming against the raw
    // JSON would cut batches to a fifth of what fits and throw away the entire point of compressing.
    // Compression is not free (a ~300 KB PSRAM compressor pass per call), so this shrinks by a ratio
    // estimate first and only then verifies, instead of compressing once per popped frame.
    auto wireSize = [](const std::vector<OBDVector>& b) {
      std::string body = bulkStringV2(b);
      std::string gz;
      return gzipBody(body, gz) ? gz.size() : body.size();
    };
    size_t wire = wireSize(batch);
    while (batch.size() > 1 && wire > CELL_MAX_POST_BYTES) {
      // Drop proportionally to the overshoot rather than one frame at a time: at 12 frames a
      // frame-at-a-time walk is up to 11 compressor passes for one POST.
      size_t keep = (batch.size() * CELL_MAX_POST_BYTES) / wire;
      if (keep >= batch.size()) keep = batch.size() - 1;
      if (keep < 1) keep = 1;
      batch.resize(keep);
      wire = wireSize(batch);
    }
    n = batch.size();
    if (n == 1 && wire > CELL_MAX_POST_BYTES) {
      // A single frame alone exceeds the modem cap - it can never be delivered and would block the
      // queue forever. Drop it (logged) rather than stall all telemetry behind it.
      Serial.printf("Dropping oversized frame (> %d bytes on the wire)\n", (int)CELL_MAX_POST_BYTES);
#if ENABLE_LOGGING
      logEvent("UPLOAD dropped oversized frame (>%dB body)", (int)CELL_MAX_POST_BYTES);
#endif
      obd_result_vector.erase(obd_result_vector.begin());
      continue;
    }
#endif
#if ENABLE_LOGGING
    // Heap probe right before the HTTPS POST. TLS needs a large CONTIGUOUS block, so a 404/0 upload correlates
    // with a low maxblock (largest free block) - which can be small even when total free looks OK, because
    // Bluedroid fragments the heap. ble=1 means the BLE stack is up this wake. This is the diagnostic that
    // confirms the BLE-vs-cellular coexistence failure (and later confirms whether freeing heap fixes it).
    logEvent("UPLOAD try frames=%d free=%lu maxblock=%lu ble=%d", (int)n,
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(), g_ble_up ? 1 : 0);
#endif
    if (!logNetwork(batch)) { ok = false; break; }
    obd_result_vector.erase(obd_result_vector.begin(), obd_result_vector.begin() + n);
    if (!drain && ++batches >= UPLOAD_MAX_BATCHES_PER_CYCLE) break; // bounded burst; drain=true is unlimited
  }
  return ok;
}

#if ENABLE_LOGGING
/*
  Store-and-forward backlog: persist unsent telemetry across deep sleep. Deep sleep wipes RAM, and the
  diagnostic CSV log is NOT an upload queue - so frames still buffered when we sleep (wedged bearer mid-drive,
  no coverage) were silently lost. Instead: write them to LittleFS (one JSON object per line, same shape as a
  bulk tlm_list entry - utc included, so late upload is still correctly timestamped) and reload on the next
  polling wake, where the freshly-booted modem drains them through the normal upload path.
*/
static const char* UPLOAD_BACKLOG_PATH = "/upload_backlog.jsonl";

// Called from enterDeepSleep AFTER the flush attempt. Overwrite semantics are correct: the RAM buffer already
// contains any previously-loaded backlog (the file is deleted at load), so the buffer is the full truth.
void saveUploadBacklog() {
  if (obd_result_vector.empty()) return; // everything uploaded (or nothing polled) - leave no stale file
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, "w");
  if (!f) { Serial.printf("Backlog: open for write failed\n"); return; }
  for (const auto& frame : obd_result_vector) {
    f.println(encodeUploadFrame(frame).dump().c_str());
  }
  f.close();
  Serial.printf("Backlog: %d unsent frame(s) persisted to flash\n", (int)obd_result_vector.size());
  logEvent("BACKLOG saved %d unsent frame(s)", (int)obd_result_vector.size());
}

// Called at the end of setup() on polling wakes only (gate skips sleep before this, leaving the file for the
// next real wake). Loads into the normal RAM buffer, bounded by MAX_UPLOAD_BUFFER, then deletes the file -
// from here the frames are RAM-owned and re-persisted at the next sleep if still unsent.
void loadUploadBacklog() {
  if (!LittleFS.exists(UPLOAD_BACKLOG_PATH)) return;
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, "r");
  if (!f) return;
  int n = 0;
  while (f.available() && obd_result_vector.size() < MAX_UPLOAD_BUFFER) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    try {
      json tlm = json::parse(line.c_str());
      OBDVector frame = decodeUploadFrame(tlm);
      if (!frame.empty()) { obd_result_vector.push_back(frame); n++; }
    }
    catch (const json::exception&) { /* skip a corrupt line (e.g. power lost mid-write) */ }
  }
  f.close();
  LittleFS.remove(UPLOAD_BACKLOG_PATH);
  if (n) {
    Serial.printf("Backlog: restored %d frame(s) from flash for upload\n", n);
    logEvent("BACKLOG restored %d frame(s) from flash", n);
  }
}
#endif

#endif
