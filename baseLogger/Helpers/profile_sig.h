#pragma once
/*
  Phase B transport + apply: receive a vehicle profile as a verbatim RS256 JWS, verify it, and only
  then let it become the running profile. Signature checking itself lives in jws_verify.h; this file
  is the chunked ABD2 transport, the buffer, and the hand-off into the EXISTING setup path.

  ADD-ON, NOT A REWIRE. The dongle still accepts the historical per-key profile packages exactly as
  before (PROFILE_REQUIRE_SIGNATURE 0, the shipping default), so an app that knows nothing about JWS
  keeps working byte-for-byte. A signed push is simply a second, better-authenticated way in. Flipping
  the flag to 1 later is what retires the unsigned path - audit-then-enforce staging, on the same
  reasoning the auth gate was staged on before it was made unconditional: a gate nobody has watched
  succeed yet should not be the only way in.

  TASK DISCIPLINE (load-bearing, same rule as OTA / log dump / auth gate). RSA-2048 verification is
  milliseconds of CPU and the apply path touches LittleFS, so NEITHER may run on the Bluedroid task.
  The ABD2 callback only memcpy's a chunk and bumps a counter; profileSigService() on the loop task
  does the verify, the decode and the apply.

  Only ABD2 is involved, which is abrpobd's own characteristic - the generic obdble lane (FFF0/18F0/
  FFE0) never reaches any of this.

  #included by logger.ino into the sketch translation unit - uses ble, logEvent, json.
*/

#include "jws_verify.h"

// A typical profile is ~1 KB of JSON (see "PROFILE restored from flash (1029 bytes)"), which is
// ~1.4 KB base64url plus a 342-char signature and a small header - call it 1.8 KB. 8 KB leaves room
// for a considerably richer vehicle without putting a multi-KB allocation on the heap at push time.
static const size_t PROFILE_JWS_MAX = 8192;
// Guard against a nonsense chunk count before any of it is trusted.
static const int PROFILE_JWS_MAX_CHUNKS = 128;

static char profile_jws_buf[PROFILE_JWS_MAX];
static size_t profile_jws_len = 0;
static int profile_jws_expected = 0;   // total chunks this transfer announced
static int profile_jws_next = 0;       // the index we require next - ordering is not assumed
static volatile bool profile_jws_ready = false;  // set by the hook, consumed by the service
static volatile bool profile_jws_failed = false; // transport-level failure, reported by the service
// Length of the last SUCCESSFULLY VERIFIED jws, kept so JWSTEST can re-run verification on a real
// signed artifact. The buffer is static and already allocated, so retaining its contents costs
// nothing; only this length is needed because the transport counters get reset on completion.
static size_t profile_jws_last_len = 0;

/*
  Bluedroid-task hook: copy the chunk and nothing else. Called from ABD2's handler once it has parsed
  a {"JWS":{...}} package. Returns false when the transfer is already broken, so the caller can answer
  the app immediately rather than letting it stream into a buffer that will be thrown away.
*/
static bool profileSigAppend(int index, int total, const char* data, size_t len) {
  if (total <= 0 || total > PROFILE_JWS_MAX_CHUNKS || index < 0 || index >= total) {
    profile_jws_failed = true;
    return false;
  }
  if (index == 0) {
    // a fresh transfer resets everything - a retry after a failure is therefore just "start again"
    profile_jws_len = 0;
    profile_jws_next = 0;
    profile_jws_expected = total;
    profile_jws_failed = false;
    profile_jws_ready = false;
  }
  if (total != profile_jws_expected || index != profile_jws_next) {
    // out of order or a changed total: refuse rather than assemble something that will fail to verify
    // for a reason nobody can distinguish from a bad signature
    profile_jws_failed = true;
    return false;
  }
  if (profile_jws_len + len > PROFILE_JWS_MAX) {
    profile_jws_failed = true;
    return false;
  }
  memcpy(profile_jws_buf + profile_jws_len, data, len);
  profile_jws_len += len;
  profile_jws_next++;
  if (profile_jws_next == profile_jws_expected) {
    profile_jws_ready = true; // the loop task verifies from here
  }
  return true;
}

/*
  Loop-task service: verify the assembled JWS, and on success decode ITS OWN payload into the config
  the existing setup path already consumes. Cheap flag test when idle.

  The profile that gets applied is the signed payload - deliberately NOT a reassembly of anything the
  app sent as separate values. That is the whole point: what runs is what was signed.
*/
void profileSigService() {
  if (profile_jws_failed) {
    profile_jws_failed = false;
    profile_jws_len = 0;
    profile_jws_next = 0;
    profile_jws_expected = 0;
    logEvent("PROFILE JWS transport error - chunk rejected");
    ble.notifySetup("JWS BAD");
    return;
  }
  if (!profile_jws_ready) return;
  profile_jws_ready = false;

  size_t payload_off = 0, payload_len = 0;
  unsigned long t0 = millis();
  bool ok = jwsVerifyRs256(profile_jws_buf, profile_jws_len, &payload_off, &payload_len);
  if (!ok) {
    logEvent("PROFILE JWS REJECTED (bad signature, %u B)", (unsigned)profile_jws_len);
    profile_jws_len = 0;
    profile_jws_next = 0;
    profile_jws_expected = 0;
    ble.notifySetup("JWS BAD");
    return;
  }

  // Signature proved: NOW it is safe to look at the payload. Decoded in place into a heap buffer
  // sized from the segment - base64 shrinks by 1/4, so the segment length is always a safe bound.
  uint8_t* decoded = (uint8_t*)malloc(payload_len + 1);
  if (!decoded) {
    logEvent("PROFILE JWS oom (%u B payload)", (unsigned)payload_len);
    ble.notifySetup("JWS BAD");
    return;
  }
  int dlen = jwsB64UrlDecode(profile_jws_buf + payload_off, payload_len, decoded, payload_len);
  if (dlen <= 0) {
    free(decoded);
    logEvent("PROFILE JWS payload undecodable");
    ble.notifySetup("JWS BAD");
    return;
  }
  decoded[dlen] = '\0';

  json profile = json::parse((const char*)decoded, nullptr, false);
  free(decoded);
  if (profile.is_discarded() || !profile.is_object()) {
    logEvent("PROFILE JWS payload not a JSON object");
    ble.notifySetup("JWS BAD");
    return;
  }

  // Feed the EXISTING config store, key by key, exactly as the per-key packages would have. Nothing
  // downstream changes: the completion check in ABD2's handler, completeSetup(), the loop-task apply,
  // the identical-profile ATZ skip and the LittleFS persist all behave as they always did. Only where
  // the values CAME FROM has changed - which is the one thing this phase is allowed to change.
  int keys = 0;
  for (auto it = profile.begin(); it != profile.end(); ++it) {
    ble.setJsonValue(it.key(), it.value());
    keys++;
  }
  logEvent("PROFILE JWS OK (%u B, %d keys, verify %lums)",
    (unsigned)profile_jws_len, keys, (unsigned long)(millis() - t0));
  profile_jws_last_len = profile_jws_len; // retained for JWSTEST (see profileSigSelfTest)
  profile_jws_len = 0;
  profile_jws_next = 0;
  profile_jws_expected = 0;
  ble.notifySetup("JWS OK");
}

/*
  Serial-only proof that verification actually DISCRIMINATES, not merely that it returns true.

  A happy-path push looks identical whether the signature is checked or blindly accepted, so that log
  alone proves nothing about security. This re-runs the verifier three times over the last profile
  this dongle really verified: intact, with one payload character altered, then intact again. The
  middle result is the one that matters - if a tampered artifact still verifies, the trust anchor is
  broken no matter how healthy the push looked.

  Read-only and physical-access only: it mutates a RAM byte and puts it straight back, applies
  nothing, and touches neither NVS nor the running profile.
*/
void profileSigSelfTest() {
  if (profile_jws_last_len == 0) {
    Serial.printf("JWSTEST: no verified profile in RAM yet - push one from the app first\n");
    return;
  }
  size_t off = 0, len = 0;
  bool intact = jwsVerifyRs256(profile_jws_buf, profile_jws_last_len, &off, &len);

  // Alter one character in the middle - inside the payload segment for any realistic profile, and
  // mapped between two characters that are both valid base64url so the rejection is a genuine
  // signature mismatch rather than a malformed-input reject. Never touch a '.' or the segmentation
  // itself would be what fails, which would prove something weaker.
  size_t at = profile_jws_last_len / 2;
  while (at < profile_jws_last_len && profile_jws_buf[at] == '.') at++;
  char saved = profile_jws_buf[at];
  profile_jws_buf[at] = (saved == 'A') ? 'B' : 'A';
  bool tampered = jwsVerifyRs256(profile_jws_buf, profile_jws_last_len, &off, &len);
  profile_jws_buf[at] = saved;

  bool restored = jwsVerifyRs256(profile_jws_buf, profile_jws_last_len, &off, &len);

  Serial.printf("JWSTEST (%u B): intact=%s  tampered=%s  restored=%s  -> %s\n",
    (unsigned)profile_jws_last_len,
    intact ? "ACCEPT" : "reject",
    tampered ? "ACCEPT" : "reject",
    restored ? "ACCEPT" : "reject",
    (intact && !tampered && restored) ? "PASS (signature is enforced)"
                                      : "FAIL - verification is not discriminating");
  logEvent("JWSTEST intact=%d tampered=%d restored=%d", intact ? 1 : 0, tampered ? 1 : 0,
    restored ? 1 : 0);
}
