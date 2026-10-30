#pragma once
/*
  Batched cloud uploads and a persistent backlog for unsent frames (ENABLE_NETWORK).

  Included into logger.ino's single translation unit; shares its globals and config.h flags.
*/

#include <new>

// The trip-end marker enterDeepSleep queues; uploadFrameLost re-arms its debt when it is lost.
bool isSyntheticParkMarker(const OBDVector& candidate) {
  if (candidate.size() != 2) return false;
  bool has_parked = false, has_utc = false;
  for (const auto& value : candidate) {
    if (value.first == "is_parked" && value.second == 1.0 && !has_parked) has_parked = true;
    else if (value.first == "utc" && !has_utc) has_utc = true;
    else return false;
  }
  return has_parked && has_utc;
}

// Every path that drops a queued frame unsent calls this.
void uploadFrameLost(const OBDVector& frame) {
  if (isSyntheticParkMarker(frame)) driveMarkerRestored();
}

#ifdef ESP_PLATFORM
static_assert(MAX_UPLOAD_BUFFER * sizeof(OBDVector) >= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
  "the queue's outer array, reserved for the whole cap, must be big enough for malloc to put it in PSRAM");
#endif

bool queueUploadFrame(std::vector<OBDVector>& frames, const OBDVector& frame) {
  auto markerLost = uploadFrameLost;
  // A copy's pairs and keys go to PSRAM (obd_vector.h). With PSRAM the outer array is reserved for the whole cap at
  // once, which puts it in PSRAM too, so only the PSRAM reserve applies. It is judged on free PSRAM, which evicting
  // the oldest raises: a freed frame seldom grows the largest block, and judging that emptied the queue on the bench.
  // Without PSRAM all of it is internal and the outer array grows by one.
  const bool psram = ESP.getPsramSize() > 0;
  size_t grow_to = 0;
  auto copyBytes = [&]() {
    size_t bytes = frame.size() * sizeof(OBDPair) + 16;
    for (const auto& value : frame)
      if (value.first.size() > 15) bytes += value.first.size() + 1 + 16;
    grow_to = frames.size() < frames.capacity() ? 0 : psram ? MAX_UPLOAD_BUFFER : frames.size() + 1;
    if (grow_to) bytes += grow_to * sizeof(OBDVector) + 16;
    return bytes;
  };
  auto hasHeadroom = [&](size_t bytes) {
    if (psram) {
      const unsigned long free_psram = ESP.getFreePsram();
      return free_psram >= UPLOAD_MIN_FREE_PSRAM && bytes <= free_psram - UPLOAD_MIN_FREE_PSRAM &&
        bytes <= ESP.getMaxAllocPsram();
    }
    const unsigned long free_heap = ESP.getFreeHeap();
    const unsigned long max_block = ESP.getMaxAllocHeap();
    if (free_heap < UPLOAD_MIN_FREE_HEAP || max_block < UPLOAD_MIN_MAX_BLOCK) return false;
    return bytes <= free_heap - UPLOAD_MIN_FREE_HEAP &&
      bytes <= max_block - UPLOAD_MIN_MAX_BLOCK;
  };
  while (true) {
    if (frames.size() >= MAX_UPLOAD_BUFFER) {
      markerLost(frames.front());
      frames.erase(frames.begin());
      ++upload_capped;
      continue;
    }
    if (hasHeadroom(copyBytes())) {
      try {
        if (grow_to) frames.reserve(grow_to);
        frames.push_back(frame);
        return true;
      }
      catch (const std::bad_alloc&) {
        markerLost(frame);
        ++upload_heap_dropped;
        return false;
      }
    }
    if (frames.empty()) {
      markerLost(frame);
      ++upload_heap_dropped;
      return false;
    }
    markerLost(frames.front());
    frames.erase(frames.begin());
    ++upload_heap_dropped;
  }
}

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
  if (uploadErrorContains(body, "too old")) return "too_old";
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
// The csq of loop()'s pre-POST netDiag() and its millis(), for the weak-signal budget (#52). -1 = no sample this
// wake; a logging-off build never takes one.
int g_upload_csq = -1;
unsigned long g_upload_csq_ms = 0;

#if ENABLE_CELL
// The byte budget the batch trim aims under: CELL_WEAK_MAX_POST_BYTES while the last pre-POST sample is younger than
// CELL_CSQ_FRESH_MS and weak (<= CELL_WEAK_CSQ, or 99), else CELL_MAX_POST_BYTES. No sample is not weak.
static size_t uploadPostBudget() {
  const bool fresh = g_upload_csq >= 0 && (unsigned long)(millis() - g_upload_csq_ms) < CELL_CSQ_FRESH_MS;
  const bool weak = fresh && (g_upload_csq <= CELL_WEAK_CSQ || g_upload_csq == 99);
  return weak ? CELL_WEAK_MAX_POST_BYTES : CELL_MAX_POST_BYTES;
}
#endif

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

// Frames past BACKLOG_MAX_SEND_AGE_SECONDS are dropped (config.h); only on a trusted send clock, without which there
// is no age. Judges the utc bulkStringV2 sends, the last one.
static unsigned dropAgedFrames(double send_utc) {
  if (!send_utc) return 0;
  unsigned n = 0;
  unsigned long first = 0, last = 0;
  auto aged = [&](const OBDVector& frame) {
    double utc = 0;
    for (const auto& res : frame) if (res.first == "utc") utc = res.second;
    if (!std::isfinite(utc) || utc <= 0 || send_utc - utc <= BACKLOG_MAX_SEND_AGE_SECONDS) return false;
    if (!n++ || (unsigned long)utc < first) first = (unsigned long)utc;
    if ((unsigned long)utc > last) last = (unsigned long)utc;
    return true;
  };
  obd_result_vector.erase(std::remove_if(obd_result_vector.begin(), obd_result_vector.end(), aged),
    obd_result_vector.end());
#if ENABLE_LOGGING
  if (n) logEvent("UPLOAD dropped n=%u reason=age sutc=%lu..%lu now=%lu", n, first, last, (unsigned long)send_utc);
#endif
  return n;
}

// Frames waiting for cellular: in RAM, or in a backlog file this wake has not read.
bool uploadBacklogUnread();
bool uploadQueued() { return !obd_result_vector.empty() || uploadBacklogUnread(); }

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
  bool ok = true, attempted = false;
  int batches = 0;
#if ENABLE_CELL
  // Once per call: a drain outlasting the sample's freshness keeps the budget it started with.
  const size_t budget = uploadPostBudget();
#endif
  while (!obd_result_vector.empty()) {
    esp_task_wdt_reset(); // a full drain (pre-sleep flush of a big backlog) is many sequential POSTs, each
                          // up to ~22s - legitimate, so feed per batch; a wedge INSIDE one POST still trips
    const double send_utc = uploadSendUtc();
    if (dropAgedFrames(send_utc) && obd_result_vector.empty()) {
      if (!attempted) g_last_status = "ok_aged"; // no batch this call: not the last call's status
      break;
    }
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
    // The trim aims under `budget`, smaller at weak signal (#52). The single-frame checks below keep the modem's cap:
    // a lone frame over the weak budget still goes, alone.
    while (sized != GZIP_FAIL && batch.size() > 1 && wire > budget) {
      // Drop proportionally to the overshoot rather than one frame at a time: at 12 frames a
      // frame-at-a-time walk is up to 11 compressor passes for one POST.
      size_t keep = (batch.size() * budget) / wire;
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
      if (!queueUploadFrame(g_upload_held, obd_result_vector.front())) {
        --upload_heap_dropped;
        // The source stays in main RAM, so this rejected copy is not a discarded frame.
        uploadNotSent("heap");
        ok = false;
        break;
      }
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
    attempted = true;
    ++g_upload_posts; // FLUSHSTAT's counts (#110): this batch was tried...
    if (!logNetworkAt(batch, send_utc)) { ok = false; break; }
    ++g_upload_accepted; // ...accepted, and its frames go
    g_upload_frames_ok += (int)n;
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
// The file holds only earlier wakes' frames, none of RAM's: set in setup(), cleared once RAM holds them.
bool g_backlog_unread = false;
void loadUploadBacklog();

// At the count cap the oldest frame still goes. Count-cap and heap drops are aggregated until an accepted upload or
// backlog save.
void logUploadCapped() {
  if (upload_capped) {
    logEvent("UPLOAD discarded n=%lu reason=cap", upload_capped);
    upload_capped = 0;
  }
  if (upload_heap_dropped) {
    logEvent("UPLOAD discarded n=%lu reason=heap free=%lu maxblock=%lu psram=%lu", upload_heap_dropped,
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(), (unsigned long)ESP.getFreePsram());
    upload_heap_dropped = 0;
  }
}

bool uploadBacklogPending() { return LittleFS.exists(UPLOAD_BACKLOG_PATH); }
// The file holds frames no RAM copy does, waiting for the modem to register.
bool uploadBacklogUnread() { return g_backlog_unread && uploadBacklogPending(); }

// Called from enterDeepSleep AFTER the flush attempt, and before an OTA reboot. An unread file holds only older
// frames, so this wake's are appended: a wake without coverage neither reads nor rewrites it. Past BACKLOG_MAX_BYTES
// or the flash budget the file is compacted instead (read, then rewritten). A file RAM already holds (loaded, or
// written by this wake) is overwritten.
// The file takes at most what LittleFS has free above BACKLOG_FS_RESERVE (a file being overwritten still counts as
// used: littlefs keeps its blocks until the new one is synced), and a rewrite at most BACKLOG_COMPACT_BYTES. Past that
// the oldest frames are not saved (reason=fs, or reason=size), so the newest, the trip-end marker among them, are the
// ones kept. A write that fails ends the save, and the file's size is checked after close, since File buffers and a
// failed final flush is not reported. Lost frames re-arm the marker debt if the marker is among them.
bool saveUploadBacklog() {
  logUploadCapped();
  if (g_upload_held.empty() && obd_result_vector.empty()) {
    if (!g_backlog_unread) LittleFS.remove(UPLOAD_BACKLOG_PATH); // a copy RAM held (loaded, or discarded), now sent
    return true; // an unread file is left as it is
  }
  const unsigned long t0 = millis();
  auto freeBytes = [] {
    const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    return total > used ? total - used : 0;
  };
  auto fsBudget = [](size_t free_bytes) { return free_bytes > BACKLOG_FS_RESERVE ? free_bytes - BACKLOG_FS_RESERVE : 0; };
  auto lineBytes = [](const OBDVector& frame) { return encodeUploadFrame(frame).dump().size() + 2; }; // println's CR LF
  bool append = false, torn = false;
  size_t base = 0; // where this save's lines start: the old size, plus the newline ending a torn last line
  if (uploadBacklogUnread()) {
    if (File tail = LittleFS.open(UPLOAD_BACKLOG_PATH, "r")) {
      base = tail.size();
      torn = base && tail.seek(base - 1) && tail.read() != '\n'; // a cut line must not swallow the next frame
      tail.close();
    }
    size_t bytes = torn ? 1 : 0;
    for (const auto* frames : { &g_upload_held, &obd_result_vector })
      for (const auto& frame : *frames) bytes += lineBytes(frame);
    append = base && base + bytes <= BACKLOG_MAX_BYTES && bytes <= fsBudget(freeBytes());
    if (!append) {
      logEvent("BACKLOG compacted reason=%s on_flash=%u add=%u", base + bytes > BACKLOG_MAX_BYTES ? "max" : "fs",
        (unsigned)base, (unsigned)bytes);
      obd_result_vector.insert(obd_result_vector.begin(), g_upload_held.begin(), g_upload_held.end()); // file, held, RAM
      std::vector<OBDVector>().swap(g_upload_held);
      loadUploadBacklog();
      logUploadCapped();
      base = 0;
    }
  }
  const size_t held_count = g_upload_held.size();
  const size_t frame_count = held_count + obd_result_vector.size();
  auto frameAt = [&](size_t i) -> const OBDVector& {
    return i < held_count ? g_upload_held[i] : obd_result_vector[i - held_count];
  };
  auto lost = [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) uploadFrameLost(frameAt(i));
  };
  size_t first = 0;
  if (!append) {
    const size_t free_bytes = freeBytes(), fs_budget = fsBudget(free_bytes);
    const size_t budget = fs_budget < BACKLOG_COMPACT_BYTES ? fs_budget : BACKLOG_COMPACT_BYTES;
    size_t bytes = 0;
    first = frame_count;
    while (first) {
      const size_t line = lineBytes(frameAt(first - 1));
      if (bytes + line > budget) break;
      bytes += line;
      --first;
    }
    if (first) {
      lost(0, first);
      logEvent("UPLOAD discarded n=%u reason=%s free=%u", (unsigned)first, budget < fs_budget ? "size" : "fs",
        (unsigned)free_bytes);
    }
    if (first == frame_count) return true;
  }
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, append ? "a" : "w");
  if (!f) { Serial.printf("Backlog: open for write failed\n"); return false; }
  if (!append) g_backlog_unread = false;
  if (append && torn && f.print("\n")) ++base;
  size_t written = first;
  for (; written < frame_count; ++written)
    if (!f.println(encodeUploadFrame(frameAt(written)).dump().c_str())) break;
  f.close();
  size_t on_flash = 0;
  if (File check = LittleFS.open(UPLOAD_BACKLOG_PATH, "r")) { on_flash = check.size(); check.close(); }
  size_t kept = first;
  for (size_t at = base; kept < written && at + lineBytes(frameAt(kept)) <= on_flash; ++kept) at += lineBytes(frameAt(kept));
  if (kept < frame_count) {
    lost(kept, frame_count);
    logEvent("BACKLOG write failed at %u of %u", (unsigned)(kept - first), (unsigned)(frame_count - first));
  }
  Serial.printf("Backlog: %d unsent frame(s) persisted to flash\n", (int)(kept - first));
  if (append) {
    logEvent("BACKLOG appended %u bytes=%u ms=%lu", (unsigned)kept, (unsigned)on_flash, millis() - t0);
    // On flash now, behind the older frames: a second save must not append them again.
    std::vector<OBDVector>().swap(g_upload_held);
    std::vector<OBDVector>().swap(obd_result_vector);
  } else {
    logEvent("BACKLOG saved %d unsent frame(s) ms=%lu", (int)(kept - first), millis() - t0);
  }
  return true;
}

// Called once the modem has registered, just before a cellular upload (the loop's, or the pre-sleep flush), and by a
// compacting save; a wake that never registers leaves the file unread. The file's frames are older than the RAM
// buffer's, so they go in front; queueUploadFrame drops the oldest past MAX_UPLOAD_BUFFER (counted in upload_capped).
// Then the file is deleted - from here the frames are RAM-owned and re-persisted at the next sleep if still unsent.
void loadUploadBacklog() {
  if (!LittleFS.exists(UPLOAD_BACKLOG_PATH)) return;
  g_backlog_unread = false; // read now, or unreadable: not read again this wake, even if the remove below fails
  File f = LittleFS.open(UPLOAD_BACKLOG_PATH, "r");
  if (!f) return;
  const unsigned long t0 = millis();
  g_upload_held.clear();
  std::vector<OBDVector> pending;
  pending.swap(obd_result_vector);
  size_t restored = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    try {
      json tlm = json::parse(line.c_str());
      OBDVector frame = decodeUploadFrame(tlm);
      if (!frame.empty()) {
        ++restored;
        queueUploadFrame(obd_result_vector, frame);
      }
    }
    catch (const json::exception&) { /* skip a corrupt line (e.g. power lost mid-write) */ }
  }
  f.close();
  LittleFS.remove(UPLOAD_BACKLOG_PATH);
  while (!pending.empty()) {
    queueUploadFrame(obd_result_vector, pending.front());
    OBDVector().swap(pending.front());
    pending.erase(pending.begin());
  }
  g_geo_queued_seeded = false;
  if (restored) {
    Serial.printf("Backlog: restored %d frame(s) from flash for upload\n", (int)restored);
    logEvent("BACKLOG restored %d frame(s) from flash ms=%lu", (int)restored, millis() - t0);
  }
}
#endif

#endif
