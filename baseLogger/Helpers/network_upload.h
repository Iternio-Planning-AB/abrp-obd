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

// The server's gzip complaints (Go's compress/gzip errors), for the log category and the failure budget.
const char* uploadGzipCategory(const std::string& body) {
  if (uploadErrorContains(body, "invalid header")) return "gzip_header";
  if (uploadErrorContains(body, "checksum")) return "gzip_checksum";
  if (uploadErrorContains(body, "length check")) return "gzip_length";
  if (uploadErrorContains(body, "unexpected eof")) return "incomplete";
  if (uploadErrorContains(body, "gzip")) return "gzip_other";
  return nullptr;
}

#if ENABLE_LOGGING
#include <cmath>

constexpr size_t UPLOAD_DIAGNOSTIC_MESSAGE_BYTES = 120;
constexpr size_t UPLOAD_DIAGNOSTIC_SHA256_BYTES = 6;
unsigned long g_upload_request_id = 0;

const char* uploadErrorCategory(const std::string& body) {
  if (body.empty()) return "empty";
  if ((!tlm_push_token.empty() && body.find(tlm_push_token) != std::string::npos) ||
      (!api_key.empty() && body.find(api_key) != std::string::npos)) return "redacted";
  size_t credential_run = 0;
  for (unsigned char c : body) {
    bool credential_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '-';
    credential_run = credential_char ? credential_run + 1 : 0;
    if (credential_run >= 24) return "redacted";
  }
  // Checked before the credential-word markers: a 400 naming the absent header ("X-TLM-TOKEN ... value is
  // required but missing") contains "token", and logging it as "redacted" hid the only explanation for a
  // whole drive of rejected uploads. The label carries no response text.
  if (uploadErrorContains(body, "required but missing")) return "missing_header";
  for (const char* marker : {"token", "api_key", "api-key", "authorization", "bearer", "password", "secret"})
    if (uploadErrorContains(body, marker)) return "redacted";
  if (uploadErrorContains(body, "<html") || uploadErrorContains(body, "<!doctype")) return "html";
  if (const char* gzip = uploadGzipCategory(body)) return gzip;
  if (uploadErrorContains(body, "unauthorized") || uploadErrorContains(body, "forbidden")) return "auth";
  if (uploadErrorContains(body, "validation")) return "validation";
  return "other";
}
#endif
// "abrp-obd/<x.y.z>" from OTA_FW_VERSION (major * 1000000 + minor * 1000 + patch); 0.0.0 unversioned.
std::string uploadUserAgent(unsigned long v = (unsigned long)OTA_FW_VERSION) {
  char ua[32];
  snprintf(ua, sizeof(ua), "abrp-obd/%lu.%lu.%lu", v / 1000000UL, v / 1000UL % 1000UL, v % 1000UL);
  return ua;
}

// Best-effort: added only when it still fits SIM7070_HEADER_BUDGET after the headers the request cannot go
// without, so it can never be the reason an upload is not sent.
void appendUserAgent(HeaderVector& headers, const std::string& ua) {
  headers.emplace_back("User-Agent", ua);
  if (headerBytes(headers) > SIM7070_HEADER_BUDGET) headers.pop_back();
}

// The last location and heading a 200 accepted. Kept across deep sleep so a parked car's unchanged fix is not
// re-sent every wake; any other boot clears it, which costs at most one resend and never depends on another
// image's RTC layout.
RTC_DATA_ATTR GeoMarks g_geo_accepted = {};
void resetGeoAccepted(bool deep_sleep) {
  if (!deep_sleep) g_geo_accepted = {};
}
// The marks as of the newest buffered frame, so an unsent fix is buffered once, not once per poll. Only meaningful
// while the buffer is non-empty; an empty buffer starts again from g_geo_accepted.
GeoMarks g_geo_queued = {};
// False after a backlog restore until a trusted clock lets seedGeoQueued() see the restored fixes. The restore
// itself usually runs untrusted: a backlog exists because coverage, and with it NITZ/NTP, was missing.
bool g_geo_queued_seeded = true;

// The last logNetworkAt() failed before anything was sent - not a verdict on the bearer (see loop()).
bool g_upload_local_fail = false;
// Frames over the cap only because gzip is off this wake; saveUploadBacklog() puts them back first.
std::vector<OBDVector> g_upload_held;

// A batch that was not POSTed: the frames stay for the next attempt, and the reason is the status.
static bool uploadNotSent(const char* status) {
  g_last_status = status;
  g_last_http = 0;
  g_upload_local_fail = true;
#if ENABLE_LOGGING
  logEvent("UPLOAD not sent: %s", status);
#endif
  return false;
}

/*
  Use the activated network to send data to Iternio API.
  Takes a vector of multiple OBDVectors, allows for temporary storage of data if connection is lost for shorter periods of time
*/
static bool logNetworkAt(const std::vector<OBDVector>& obd_result_vector, double send_utc) {
  g_upload_local_fail = false;
#if PRINT
  Serial.printf("Logging via network (bulk)\n");
  Serial.printf("URL: %s\n", network.getUrl().c_str());
#endif

  // All buffered frames go up in a single POST, instead of one request per frame. Far fewer requests ->
  // less data/airtime, which is the whole point of buffering.
  GeoMarks sent_geo = g_geo_accepted;
  unsigned location_dups = 0;
  std::string payload = bulkStringV2(obd_result_vector, send_utc, &sent_geo, &location_dups);
  if (payload == "[]") {
    // Nothing in this batch survived the v2 mapping (every key unmapped, or no usable utc). Report
    // success so the caller clears the frames - re-sending them would fail the same way forever.
    g_last_status = "ok_empty";
    return true;
  }

  // The token signs the request; it is never sent (Helpers/tlm_signature.h). Without one there is nothing
  // to sign with, so nothing goes up and the frames stay.
  if (tlm_push_token.empty()) return uploadNotSent("no_token");

  // Compress if we can, send plain if we cannot. A body that goes up uncompressed is worse - it carries
  // fewer points under the modem's cap - but it is still a working upload, and losing telemetry because
  // PSRAM was unavailable would be a far bigger failure than losing the ratio.
  const size_t raw_bytes = payload.size();
  std::string gzipped;
  const GzipOutcome gzip = gzipBody(payload, gzipped);
  const bool gzip_used = gzip == GZIP_OK;
  if (gzip_used) payload.swap(gzipped);
  // A failed member may be corruption, and the batch was sized for gzip: send neither, keep the frames.
  else if (gzip == GZIP_FAIL) return uploadNotSent("gzip_fail");
#if ENABLE_CELL
  else if (payload.size() > CELL_MAX_POST_BYTES) return uploadNotSent("plain_too_big");
#endif

  // Over the bytes exactly as sent - the gzip stream when compressed - for Content-Digest and the log
  // fingerprint alike.
  unsigned char wire_sha256[32];
  TlmSignatureHeaders signature;
  if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char*)payload.data(),
        payload.size(), wire_sha256) != 0 ||
      !tlmSignatureHeaders(tlm_push_token, wire_sha256, signature)) return uploadNotSent("sign_fail");

  // The api key attributes the write; the signature authenticates it.
  HeaderVector headers;
  headers.emplace_back("X-API-KEY", api_key);
  headers.emplace_back("Content-Type", "application/json");
  if (gzip_used) headers.emplace_back("Content-Encoding", "gzip");
  headers.emplace_back("Content-Digest", signature.content_digest);
  headers.emplace_back("Signature-Input", signature.signature_input);
  headers.emplace_back("Signature", signature.signature);
  appendUserAgent(headers, uploadUserAgent());
  // A header the modem refused would send the request unsigned or without its digest: don't send it.
  if (!network.set_headers(headers)) return uploadNotSent("hdr_fail");
#if ENABLE_LOGGING
  // Both sizes, every POST. The compressor lives in PSRAM and is allocated at runtime, so NOTHING in a
  // build or a boot log says whether compression is actually engaging - and the failure mode is silent
  // by design (an uncompressed body still uploads, just with fewer points under the modem's cap). raw
  // == wire in the dump means gzip is off; that is the line to look for on the first drive.
  const unsigned long request_id = ++g_upload_request_id;
  char fingerprint[UPLOAD_DIAGNOSTIC_SHA256_BYTES * 2 + 1];
  for (size_t i = 0; i < UPLOAD_DIAGNOSTIC_SHA256_BYTES; ++i)
    snprintf(fingerprint + i * 2, 3, "%02x", wire_sha256[i]);
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
  unsigned points = 0, locations = 0;
  for (size_t pos = 0; (pos = raw.find("\"provider\":", pos)) != std::string::npos; ++pos) ++points;
  for (size_t pos = 0; (pos = raw.find("\"location\":", pos)) != std::string::npos; ++pos) ++locations;
  char diagnostic[UPLOAD_DIAGNOSTIC_MESSAGE_BYTES];
  const int body_len = snprintf(diagnostic, sizeof(diagnostic),
    "UPLOAD body raw=%uB wire=%uB id=%lu enc=%c sha=%s src=%u pts=%u sutc=%lu..%lu",
    (unsigned)raw_bytes, (unsigned)payload.size(), request_id, gzip_used ? 'g' : 'j', fingerprint,
    (unsigned)obd_result_vector.size(), points, source_first, source_last);
  // Appended whole or not at all: at the extremes of the other fields a clipped digit would be a wrong count.
  char counts[32];
  const int counts_len = snprintf(counts, sizeof(counts), " loc=%u ldup=%u", locations, location_dups);
  if (body_len >= 0 && counts_len >= 0 && (size_t)body_len + counts_len < sizeof(diagnostic))
    memcpy(diagnostic + body_len, counts, counts_len + 1);
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
    if (gzip_used && uploadGzipCategory(response.payload)) gzipNoteFailure();
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
  g_geo_accepted = sent_geo;
  return true;
}

// One frame at a time, so a 120-frame backlog never builds one JSON tree.
void seedGeoQueued(double send_utc) {
  if (g_geo_queued_seeded || !send_utc) return;
  g_geo_queued = g_geo_accepted;
  for (const auto& frame : obd_result_vector) bulkStringV2({ frame }, send_utc, &g_geo_queued);
  g_geo_queued_seeded = true;
}

static double uploadSendUtc() {
  const uint32_t now = rtc.getEpoch();
  return clockNeedsSync(now) ? 0 : (double)now;
}

bool logNetwork(const std::vector<OBDVector>& frames) {
  return logNetworkAt(frames, uploadSendUtc());
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
  if (upload_spilled) return true; // older frames are parked in flash; these wait behind them for the next wake
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
    const double send_utc = uploadSendUtc();
#if ENABLE_CELL
    // Enforce the modem's per-POST body ceiling in BYTES, not frames: the SIM7070 session is configured
    // with SHCONF BODYLEN=1024 (its hard maximum), and an oversized body makes the POST undeliverable.
    //
    // Measured on what actually goes on the wire, which is the GZIPPED body - trimming against the raw
    // JSON would cut batches to a fifth of what fits and throw away the entire point of compressing.
    // Compression is not free (a deflate pass per call), so this shrinks by a ratio estimate first and
    // only then measures again, instead of compressing once per popped frame.
    GzipOutcome sized = GZIP_OK;
    auto wireSize = [send_utc, &sized](const std::vector<OBDVector>& b) {
      GeoMarks marks = g_geo_accepted;
      std::string body = bulkStringV2(b, send_utc, &marks);
      std::string gz;
      sized = gzipBody(body, gz);
      return sized == GZIP_OK ? gz.size() : body.size();
    };
    size_t wire = wireSize(batch);
    while (sized != GZIP_FAIL && batch.size() > 1 && wire > CELL_MAX_POST_BYTES) {
      // Drop proportionally to the overshoot rather than one frame at a time: at 12 frames a
      // frame-at-a-time walk is up to 11 compressor passes for one POST.
      size_t keep = (batch.size() * CELL_MAX_POST_BYTES) / wire;
      if (keep >= batch.size()) keep = batch.size() - 1;
      if (keep < 1) keep = 1;
      batch.resize(keep);
      wire = wireSize(batch);
    }
    // A plain size is no measure of what fits once gzip is back: keep the frames for a later attempt.
    if (sized == GZIP_FAIL) { uploadNotSent("gzip_fail"); ok = false; break; }
    n = batch.size();
    if (n == 1 && wire > CELL_MAX_POST_BYTES && sized == GZIP_OFF) {
      // Over the cap only because gzip is off this wake: park it for the backlog, send what is behind it.
      g_upload_held.push_back(obd_result_vector.front());
      if (g_upload_held.size() > MAX_UPLOAD_BUFFER) g_upload_held.erase(g_upload_held.begin());
      obd_result_vector.erase(obd_result_vector.begin());
#if ENABLE_LOGGING
      logEvent("UPLOAD held plain_too_big frame for the next wake (%u held)", (unsigned)g_upload_held.size());
#endif
      continue;
    }
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
    if (!logNetworkAt(batch, send_utc)) { ok = false; break; }
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

// Past the cap the oldest frame goes on every poll: one line per capped stretch, at the next accepted
// upload or before the backlog is saved.
void logUploadCapped() {
  if (!upload_capped) return;
  logEvent("UPLOAD discarded n=%lu reason=cap", upload_capped);
  upload_capped = 0;
}

// Called from enterDeepSleep AFTER the flush attempt, before an OTA reboot, and to spill. Overwrite semantics
// are correct: the RAM buffer already contains any previously-loaded backlog (the file is deleted at load).
// Only a spill from this wake can still be in the file, and it is older than RAM, so RAM goes behind it.
bool saveUploadBacklog() {
  logUploadCapped();
  obd_result_vector.insert(obd_result_vector.begin(), g_upload_held.begin(), g_upload_held.end());
  g_upload_held.clear();
  if (obd_result_vector.empty()) return true; // everything uploaded (or nothing polled) - leave no stale file
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, upload_spilled ? "a" : "w");
  if (!f) { Serial.printf("Backlog: open for write failed\n"); return false; }
  for (const auto& frame : obd_result_vector) {
    f.println(encodeUploadFrame(frame).dump().c_str());
  }
  f.close();
  Serial.printf("Backlog: %d unsent frame(s) persisted to flash\n", (int)obd_result_vector.size());
  logEvent("BACKLOG saved %d unsent frame(s)", (int)obd_result_vector.size());
  return true;
}

// A relaying phone's session (pairing, profile push, OTA, log dump) needs the internal heap the queue holds,
// about 1 KB a frame, next to Bluedroid's: park the queue in the backlog file until relaying ends.
void spillUploadBacklog() {
  const size_t n = obd_result_vector.size();
  if (!n || !saveUploadBacklog()) return;
  obd_result_vector.clear();
  upload_spilled += n;
}

bool uploadBacklogPending() { return LittleFS.exists(UPLOAD_BACKLOG_PATH); }

// Called at the end of setup() on polling wakes only (gate skips sleep before this, leaving the file for the
// next real wake), and when a relay session that spilled ends. The file's frames are older than the RAM
// buffer's, so they go in front, bounded by MAX_UPLOAD_BUFFER; then the file is deleted - from here the
// frames are RAM-owned and re-persisted at the next sleep if still unsent.
void loadUploadBacklog() {
  upload_spilled = 0;
  if (!LittleFS.exists(UPLOAD_BACKLOG_PATH)) return;
  // Over the cap the oldest lines go, as in RAM: a sleep in a relay session can append behind a full spill.
  size_t skip = 0;
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) skip++;
  }
  f.close();
  skip = skip > MAX_UPLOAD_BUFFER ? skip - MAX_UPLOAD_BUFFER : 0;
  upload_capped += skip;
  f = LittleFS.open(UPLOAD_BACKLOG_PATH, "r");
  if (!f) return;
  std::vector<OBDVector> restored;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (skip) { skip--; continue; }
    try {
      json tlm = json::parse(line.c_str());
      OBDVector frame = decodeUploadFrame(tlm);
      if (!frame.empty()) restored.push_back(std::move(frame));
    }
    catch (const json::exception&) { /* skip a corrupt line (e.g. power lost mid-write) */ }
  }
  f.close();
  LittleFS.remove(UPLOAD_BACKLOG_PATH);
  const int n = (int)restored.size();
  obd_result_vector.insert(obd_result_vector.begin(), std::make_move_iterator(restored.begin()),
    std::make_move_iterator(restored.end()));
  if (obd_result_vector.size() > MAX_UPLOAD_BUFFER) {
    upload_capped += obd_result_vector.size() - MAX_UPLOAD_BUFFER;
    obd_result_vector.erase(obd_result_vector.begin(),
      obd_result_vector.begin() + (obd_result_vector.size() - MAX_UPLOAD_BUFFER));
  }
  g_geo_queued_seeded = false;
  if (n) {
    Serial.printf("Backlog: restored %d frame(s) from flash for upload\n", n);
    logEvent("BACKLOG restored %d frame(s) from flash", n);
  }
}
#endif

#endif
