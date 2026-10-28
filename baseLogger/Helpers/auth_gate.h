#pragma once
/*
  Phase-2 auth gate (ABD8): nonce + HMAC-SHA256 challenge/response, replacing the sniffable
  {"AUTH":"<tlm_push_token>"} bearer write as the thing that proves a phone is AUTHORIZED (pairing/
  encryption from phase E proves only that the link is private). See BLE_SECURITY_PLAN.md §4.

  Protocol (all JSON on ABD8; replies are notified AND land in the readable value):
    app → {"kex":"<b64 65B P-256 point>"}
                                      TOFU via ECDH - the ONLY provisioning route. Both sides derive
                                      the secret, so nothing secret crosses the air. Accepted only
                                      while no secret is stored →
                                      {"kexpub":"<our point>","id":"<dongle id>"}.
                                      See auth_kex.h for the derivation and its threat model.

                                      The unprovisioned greet still advertises "kex":1, and it is
                                      NOT vestigial capability negotiation: this firmware answers an
                                      unknown verb with SILENCE, and the app's handshake has no
                                      timeout by design, so a dongle running something older than
                                      the kex branch would hang the app forever. The flag lets the
                                      app fail with "reflash it" instead. Eight bytes for a legible
                                      failure on the one fleet that reflashes constantly.
                                      Reply field is "kexpub", not "kex", so the flag (a number) and
                                      the point (a string) never share a key.
    app → {"authreset":1}             Wipe the stored key - the in-app AUTHCLEAR. Accepted ONLY
                                      inside the power-on window, i.e. after the dongle was
                                      physically unplugged and replugged, never after a deep-sleep
                                      wake → {"auth":"reset"}, else {"auth":"locked"}.
                                      A nonce frame carries "reset":1 while that window is open, so
                                      an app holding no key can offer this instead of dead-ending.
                                      See authResetWindowOpen() for why uptime alone is not enough.
    app → {"challenge":1}             → {"nonce":"<b64 16B>","id":"<dongle id>","proto":[MIN,MAX]}
                                      (single-use, TTL; "proto" is the BLE protocol range, see
                                       Helpers/ble_protocol.h)
                                      (an unprovisioned dongle answers
                                       {"auth":"unprovisioned","id":"..."} - the app's TOFU cue)
  Every greet carries "id", the STABLE dongle identity the app keys its keyring by - see
  authDongleId() for why neither the BLE device id nor the vehicle id can serve that role.
    app → {"proof":"<b64 32B>","proto":v}
                                      HMAC-SHA256(secret, nonce) → {"auth":"ok","proto":v} / {"auth":"fail"}
                                      "proto" in the proof is the version the app chose, sent only if the
                                      nonce frame offered a range; the ok reply names the session's version
                                      only if the proof carried one.

  ENFORCEMENT IS UNCONDITIONAL. A valid proof is the ONLY thing that grants a session; the legacy
  AUTH write on ABD2 unlocks the profile push and nothing else. There was an audit mode that let the
  token grant a session, so an app without this handshake lost nothing - it provided no access
  control whatsoever and no such app exists, so it is gone along with the AUTH_ENFORCE switch.

  Task discipline (load-bearing, same as OTA/log dump): authGateHook runs on the Bluedroid task and only
  copies bytes + sets a flag; authGateService() on the loop task does ALL NVS and HMAC work. The secret
  lives in the "auth" NVS namespace - which is exactly what the AUTHCLEAR serial command wipes, returning
  the dongle to TOFU. AUTHCLEAR is the REVOCATION mechanism, not merely a rescue: wipe the secret,
  reconnect with the phone that should keep access, and it provisions a BRAND-NEW secret - so every
  other phone still holding the old one is locked out. That is why the app always mints a fresh secret
  on "unprovisioned" and never re-pushes one it already holds; re-pushing would make revocation a
  no-op. It stays serial-only (no BLE-reachable reset) so revocation needs physical possession, and
  there is no factory time window: dongles are provisioned in the office.

  Extracted in the style of log_dump.h: #included by logger.ino into the sketch translation unit (uses
  ble, logEvent, json) - NOT a standalone compilation unit.
*/

#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include "auth_kex.h"

static const uint32_t AUTH_NONCE_TTL_MS = 30000; // challenge must be answered within this window
// How long after a POWER-ON reset an over-the-air key reset is accepted. See authResetWindowOpen().
//
// FIVE minutes, not the two it started at. Two was sized for "replug, open the app, confirm", but
// the real path is longer: a phone that has never seen this dongle has to walk the whole setup
// wizard - intro steps, scan, connect, profile - before the handshake that offers the reset even
// runs. Measured over that, two minutes expired first, every time, and the user got a refusal for
// doing exactly the right thing.
//
// The security this rests on is the UNPLUG, not the length of the window: it takes physical
// possession of the dongle to open one at all. Going from 2 to 5 minutes widens only the slice in
// which an attacker who is already standing at the car, at the moment its owner replugs the dongle,
// could race them - and anyone in that position has the OBD port in reach anyway.
static const uint32_t AUTH_RESET_WINDOW_MS = 300000;
static const size_t   AUTH_SECRET_LEN = 32;
static const size_t   AUTH_NONCE_LEN = 16;

/* Bluedroid-task hook: copy + flag, nothing else (same contract as logDumpCtrlHook). */
static volatile bool g_auth_cmd_present = false;
/*
  Largest command is {"kex":"<88 chars b64>"} = 98 B. The hook DROPS anything >= sizeof(g_auth_cmd)
  SILENTLY, so an undersized buffer here presents as a kex the dongle simply never answers - which,
  with no timeout on the app side, is an unexplained hang rather than an error. 160 leaves headroom
  for a future verb without another silent cliff.
*/
static uint8_t g_auth_cmd[160];
static volatile size_t g_auth_cmd_len = 0;

void authGateHook(const uint8_t* d, size_t n) {
  if (!d || n == 0 || n >= sizeof(g_auth_cmd) || g_auth_cmd_present) return;
  memcpy(g_auth_cmd, d, n);
  g_auth_cmd_len = n;
  g_auth_cmd_present = true;
}

/* Live challenge state - single client, so a new challenge simply replaces the old one. */
static uint8_t g_auth_nonce[AUTH_NONCE_LEN];
static unsigned long g_auth_nonce_ms = 0;
static bool g_auth_nonce_valid = false;

/* Edge tracker for the greet-on-subscribe below (see authGateService). */
static bool g_auth_prev_subscribed = false;

#if ENABLE_BLE
static void authNotifyJson(const char* s) {
  ble.notifyAuth((const uint8_t*)s, strlen(s));
}

/*
  Stable per-unit dongle identity, sent with every greet so the app can key its keyring by THE DONGLE.
  It cannot key by BLE device id: the same dongle presents a different one to every phone (iOS hands
  out a per-central UUID, Android an address), so that key is unshareable - and it must not key by
  vehicle either, because the secret belongs to the dongle, not the car. Pushing a new vehicle profile
  changes nothing about who may talk to the dongle, and moving the dongle to another car must not
  invalidate anyone's access.
  Source is the eFuse base MAC - the same immutable value the BLE address derives from (see the deinit
  note in blewrapper.cpp), so it survives reflashing, NVS erase, AUTHCLEAR and deep sleep.
*/
static const char* authDongleId() {
  static char id[13] = { 0 };
  if (id[0] == 0) {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(id, sizeof(id), "%04X%08X",
      (uint16_t)(mac >> 32), (uint32_t)(mac & 0xFFFFFFFF));
  }
  return id;
}

/* Read the stored secret. False (without touching out) when the dongle is unprovisioned. */
static bool authLoadSecret(uint8_t out[AUTH_SECRET_LEN]) {
  Preferences p;
  if (!p.begin("auth", true)) return false; // read-only begin fails if the namespace doesn't exist yet
  size_t n = p.getBytes("secret", out, AUTH_SECRET_LEN);
  p.end();
  return n == AUTH_SECRET_LEN;
}

/* Decode a base64 JSON string field into exactly want_len bytes. */
static bool authB64Decode(const std::string& b64, uint8_t* out, size_t want_len) {
  size_t olen = 0;
  if (mbedtls_base64_decode(out, want_len, &olen, (const uint8_t*)b64.c_str(), b64.size()) != 0) {
    return false;
  }
  return olen == want_len;
}

/* Mint a fresh single-use nonce and notify it. Shared by the greet path and the challenge command. */
/*
  Is the over-the-air key reset open right now?

  TWO conditions, and the reset reason is the load-bearing one. This dongle deep-sleeps and every
  motion wake REBOOTS it (see sleep_power.h: "a WOM event on GPIO39 (door open / motion) wakes the
  chip, which reboots and runs setup() again"). So a window gated on uptime alone would reopen every
  single time somebody opens the car door - a recurring two-minute hole, in a car park, for anyone in
  BLE range. They could wipe the key, TOFU-provision themselves, and inherit CAN injection through
  the profile's init_commands along with OTA and the log dump.

  ESP_RST_POWERON only fires when the dongle actually lost power, which on a unit living in the OBD
  port means somebody physically unplugged it. That is the "physical access" this is supposed to
  require, and a deep-sleep wake (ESP_RST_DEEPSLEEP) is explicitly not it.

  BROWNOUT is deliberately excluded too: it is an uncommanded reset that happens in-car on a weak
  rail, so accepting it would hand back a slice of the same hole.
*/
static bool authResetWindowOpen() {
  return esp_reset_reason() == ESP_RST_POWERON && millis() < AUTH_RESET_WINDOW_MS;
}

static void authSendNonce() {
  for (size_t i = 0; i < AUTH_NONCE_LEN; i += 4) {
    uint32_t r = esp_random();
    memcpy(g_auth_nonce + i, &r, 4);
  }
  g_auth_nonce_ms = millis();
  g_auth_nonce_valid = true;
  uint8_t b64[AUTH_NONCE_LEN * 2 + 4];
  size_t olen = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &olen, g_auth_nonce, AUTH_NONCE_LEN);
  char msg[128];
  // "reset":1 says the power-on window is open, so an app that cannot answer this nonce can offer
  // the user a key reset instead of a dead end. Sent only while it is TRUE, so its absence is a
  // definite "not available" rather than "old firmware" - the app must not offer what would be
  // refused.
  // "proto":[MIN,MAX] is the range of BLE protocol versions this firmware speaks (Helpers/ble_protocol.h), in every
  // nonce frame, so every proof answers a frame that offered it. It is the ONE thing negotiation adds here, ever: this
  // frame goes out before either side knows the other's version, so it must stay JSON every app parses, and its
  // headroom is small. Anything a version needs beyond the range is exchanged after the grant.
  // Sizes: the id is 12 characters and the nonce 24, so the frame is 56 B, 66 B with "reset":1, and the range makes it
  // 70 B / 80 B; at worst ([255,255]) 84 B, held under the 93 B notify of the MTU 96 floor below. The greet waits for
  // that MTU; the {"challenge":1} reply relies on the app's own MTU >= 96 check, as it always has.
  char range[24];
  bleProtoRangeField(range, sizeof(range));
  snprintf(msg, sizeof(msg), "{\"nonce\":\"%.*s\",\"id\":\"%s\"%s%s}",
    (int)olen, (const char*)b64, authDongleId(), authResetWindowOpen() ? ",\"reset\":1" : "", range);
  authNotifyJson(msg);
}
static_assert(sizeof("{\"nonce\":\"\",\"id\":\"\",\"reset\":1,\"proto\":[255,255]}") - 1 + (AUTH_NONCE_LEN + 2) / 3 * 4 + 12 < 93,
  "the worst-case nonce frame must fit one notify at the MTU 96 floor");

/* Loop-task service: consume one command per call. Cheap flag test when idle. */
// AUTHCLEAR and the in-app key reset: the dongle forgets the vehicle it was paired to, so a re-paired dongle cannot
// keep reporting to the previous account, and the next owner cannot read the previous one's history. Polling stops as
// on an unpaired dongle until the next full push. The OBD-alarm flag stays: it belongs to the car.
// False means the log, the profile file or the backlog file is still on flash: the caller keeps the key and the reset
// can be repeated, as it can after a power cut part-way. The log goes first, so a log that would not erase leaves
// everything else as it was. The reason is then the first line of the fresh log, after the header and FW build line.
// Loop task only. The files are deleted, not scrubbed: a raw read of the flash chip can still recover them.
bool forgetProfile(const char* why) {
#if ENABLE_LOGGING
  const char* ended_dump = g_logdump_active ? ", ended dump" : "";
  const char* ended_ota = g_ota_active ? ", ended ota" : "";
#endif
#if ENABLE_BLE && ENABLE_LOGGING
  logDumpEnd("forget"); // its next window request gets err:ended instead of bytes of the fresh log
#endif
#if ENABLE_OTA && ENABLE_BLE
  if (g_ota_active) otaReset(OTA_PH_ABORTED, OTA_ERR_NOAUTH); // the session it belonged to just lost its grant
#endif
#if ENABLE_LOGGING
  if (bleLogQueue) xQueueReset(bleLogQueue); // queued connect/write lines belong to the erased log
#if ENABLE_MEMS
  flushMotionStats(); // the partial minute too
#endif
  const int erased = datalogger.clearAll();
  if (erased < 0) {
    logEvent("PROFILE forget FAILED (%s) - log not erased, nothing cleared", why);
    return false;
  }
#endif
  std::string dropped;
  portENTER_CRITICAL(&g_config_mux);
  dropped.swap(g_pending_config_str);
  g_config_pending = false;
  portEXIT_CRITICAL(&g_config_mux);
  ble.forgetStoredPush(); // before the file: a reset between the two must not leave a hash with no file
#if ENABLE_LOGGING
  LittleFS.remove(PROFILE_PATH);
#endif
#if ENABLE_NETWORK && ENABLE_LOGGING
  const unsigned long discarded = obd_result_vector.size() + g_upload_held.size();
  LittleFS.remove(UPLOAD_BACKLOG_PATH);
#endif
#if ENABLE_LOGGING
  // Either file left behind is restored at a later wake: the previous owner's token, or their frames under the next.
  if (LittleFS.exists(PROFILE_PATH)
#if ENABLE_NETWORK
      || LittleFS.exists(UPLOAD_BACKLOG_PATH)
#endif
  ) {
    logEvent("PROFILE forget FAILED (%s) - profile or backlog file not removed", why);
    return false;
  }
#endif
#if ENABLE_NETWORK
  g_upload_held.clear(); // held frames are this vehicle's too, and the next save writes them first
  upload_capped = 0;
  resetGeoAccepted(false);
  g_geo_queued = {};
#endif
  obd_result_vector.clear();
  tlm_push_token.clear();
  g_no_profile = true;
  frozen_valid = false; // RTC survives AUTHCLEAR: no position of the previous vehicle carries into the new log
  frozen_lat = frozen_lon = 0.0;
  resetFrozenGpsProvenance(false);
  obd_fail_logged = 0;              // the erased log named these failures: the fresh one names them again
  last_good_220105_reply[0] = '\0'; // the previous car's raw reply
#if ENABLE_LOGGING
  logEvent("PROFILE forgotten (%s) - log erased (%d segments), profile and push token cleared%s%s", why, erased,
    ended_dump, ended_ota);
#endif
#if ENABLE_NETWORK && ENABLE_LOGGING
  if (discarded) logEvent("UPLOAD discarded n=%lu reason=forget", discarded);
#endif
  return true;
}

void authGateService() {
  // GREET on the rising edge of the ABD8 subscription: the dongle speaks FIRST. The app's monitor
  // registration returns before its CCCD write lands, so any app-initiated opening command can outrun
  // its own subscription and the reply notifies into an unarmed pipe (measured: provisioning succeeded
  // only on the third connect on iOS). The CCCD write is the one event that marks the pipe armed, and
  // WE receive it - so the greet lands exactly when the app can hear it, no timers, no retries.
  // Honest across sessions because onDisconnect clears the CCCD flag (see clearAuthSubscription).
  // MTU is part of the trigger, not a separate check. A notify carries at most MTU-3 bytes and is
  // never fragmented, so at Android's unnegotiated default of 23 the 20-byte ceiling silently DROPS
  // both greet payloads (~43 B and ~56 B once they carry the dongle id) - measured: iOS
  // negotiates 247 before the greet and provisioned fine, Android greeted at MTU 23, lost it, and
  // never answered. Folding the MTU into the edge condition means the greet simply DEFERS while the
  // pipe is too narrow and fires the moment MTU rises - no extra state, no timer, no retry. Same
  // floor the log dump uses for the same reason, raised to 96 here because the id-bearing nonce
  // frame is ~56 B and MTU-3 must clear it with room to spare.
  bool auth_subscribed = ble.connected() && ble.authSubscribed() && ble.mtu() >= 96;
  if (auth_subscribed && !g_auth_prev_subscribed) {
    uint8_t secret[AUTH_SECRET_LEN];
    if (authLoadSecret(secret)) {
      logEvent("AUTH2 greet: nonce sent id=%s", authDongleId());
      authSendNonce();
    } else {
      logEvent("AUTH2 greet: unprovisioned (awaiting TOFU) id=%s", authDongleId());
      char msg[96];
      // "kex":1 tells the app this firmware can do the ECDH TOFU, so it never has to probe.
      snprintf(msg, sizeof(msg), "{\"auth\":\"unprovisioned\",\"id\":\"%s\",\"kex\":1}",
        authDongleId());
      authNotifyJson(msg);
    }
  }
  g_auth_prev_subscribed = auth_subscribed;
  // Invalidate the nonce when the CONNECTION ends, NOT when the CCCD goes quiet. Tying it to the
  // subscription looked tidier and caused a real failure: the app removes its ABD8 subscription when
  // its handshake settles (including on timeout), which clears the CCCD - so a proof that was already
  // in flight got judged against an invalidated nonce and logged "no/stale nonce". The TTL is what
  // bounds a nonce's life; an unsubscribe says nothing about whether the peer is still answering.
  if (!ble.connected()) g_auth_nonce_valid = false;

  if (!g_auth_cmd_present) return;

  json j = json::parse(std::string((const char*)g_auth_cmd, g_auth_cmd_len), nullptr, false);
  g_auth_cmd_present = false;
  if (j.is_discarded() || !j.is_object()) return;

  uint8_t secret[AUTH_SECRET_LEN];
  bool provisioned = authLoadSecret(secret);

  /*
    Over-the-air key reset, the in-app equivalent of the AUTHCLEAR serial command. Wipes the stored
    key so the next phone to connect provisions a brand-new one - which also locks out every phone
    still holding the old one, exactly like AUTHCLEAR.

    Accepted ONLY inside the power-on window (see authResetWindowOpen). The dongle runs on permanent
    12V from the OBD port, so a POWERON reset means somebody pulled it out and put it back: the
    physical-access requirement is the unplug, and the window is just how long they have to finish
    the job in the app. Outside it this is refused exactly like a rekey attempt.
  */
  if (j.contains("authreset")) {
    if (!authResetWindowOpen()) {
      logEvent("AUTH2 reset refused (no power-on window)");
      authNotifyJson("{\"auth\":\"locked\"}");
      return;
    }
    Preferences p;
    if (!p.begin("auth", false)) {
      logEvent("AUTH2 reset failed (NVS open)");
      authNotifyJson("{\"auth\":\"error\"}");
      return;
    }
    // The session grant dies with the key it was based on: whoever proved against the OLD key must
    // not keep telemetry/OTA/log-dump on the strength of a key that no longer exists.
    ble.set_authenticated(false);
    g_auth_nonce_valid = false;
    // The next owner pushes their own profile. This session can still finish a push it had under way.
    // No key is not a failure: a repeated reset, after a reply lost on a slow erase, still answers reset.
    if (!forgetProfile("authreset") || (p.isKey("secret") && !p.remove("secret"))) {
      p.end();
      logEvent("AUTH2 reset failed - key kept, repeat the reset"); // with no forget FAILED line before it: NVS
      authNotifyJson("{\"auth\":\"error\"}");
      return;
    }
    p.end();
    logEvent("AUTH2 key reset over BLE - dongle is unprovisioned again");
    authNotifyJson("{\"auth\":\"reset\"}");
    return;
  }

  /*
    ECDH TOFU - the ONLY way a secret ever enters this dongle (see auth_kex.h). One-shot: a
    provisioned dongle refuses, so nobody in radio range can rekey it out from under its owner, and
    AUTHCLEAR over serial stays the only way back.

    Reply MTU: the frame is ~118 B and a notify carries at most MTU-3 and is never fragmented, so a
    narrow pipe would drop it exactly like the greet used to. The greet's own gate only guarantees
    96, which is not enough here - hence the explicit check, reported rather than dropped so the app
    sees a reason instead of silence.
  */
  if (j.contains("kex") && j["kex"].is_string()) {
    if (provisioned) {
      logEvent("AUTH2 kex refused (already provisioned)");
      authNotifyJson("{\"auth\":\"locked\"}");
      return;
    }
    if (ble.mtu() < 128) {
      logEvent("AUTH2 kex refused (MTU %u too small for the reply)", (unsigned)ble.mtu());
      authNotifyJson("{\"auth\":\"mtu\"}");
      return;
    }
    uint8_t derived[AUTH_SECRET_LEN];
    char own_pub_b64[AUTH_KEX_POINT_B64_LEN];
    if (!authKexDerive(j["kex"].get<std::string>(), authDongleId(),
                       derived, own_pub_b64, sizeof(own_pub_b64))) {
      logEvent("AUTH2 kex failed (bad point or crypto error)");
      authNotifyJson("{\"auth\":\"badkex\"}");
      return;
    }
    Preferences p;
    if (!p.begin("auth", false) ||
        p.putBytes("secret", derived, AUTH_SECRET_LEN) != AUTH_SECRET_LEN) {
      p.end();
      memset(derived, 0, sizeof(derived));
      logEvent("AUTH2 kex NVS write failed");
      authNotifyJson("{\"auth\":\"error\"}");
      return;
    }
    p.end();
    memset(derived, 0, sizeof(derived));
    /*
      Reply AFTER the store commits. If the notify went first and the write then failed, the app
      would derive and keep a secret the dongle never held - unrecoverable without AUTHCLEAR, with
      no symptom beyond an unanswered nonce. Same ordering hazard the keyring writer hit app-side.
    */
    logEvent("AUTH2 provisioned (ECDH kex) - secret derived and stored");
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"kexpub\":\"%s\",\"id\":\"%s\"}", own_pub_b64, authDongleId());
    authNotifyJson(msg);
    return;
  }

  // The {"provision":"<b64 32B>"} verb is GONE. It took the shared secret as a plaintext BLE write,
  // which is precisely what the kex branch above exists to avoid, and it was reachable by any peer
  // that reached an unprovisioned dongle - so it was strictly worse than the path beside it and kept
  // only for apps that predate the kex. No such app is in the field. A dongle now has exactly one
  // way to acquire a secret: derive it, so the secret never exists on the air at all.

  // Explicit re-request. The normal opener is the greet above; this stays for an app that wants a
  // fresh nonce mid-session (e.g. right after provisioning, or after letting one expire).
  if (j.contains("challenge")) {
    if (!provisioned) {
      // The app's cue to provision (TOFU) - see the wizard flow. Carries "id" and "kex" for the
      // same reason the greet does: the app treats an unprovisioned reply WITHOUT an id as fatal
      // ("firmware too old for phase 2"), so the bare form this used to send made an explicit
      // {"challenge":1} against an unprovisioned dongle fail where the greet path succeeded.
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"auth\":\"unprovisioned\",\"id\":\"%s\",\"kex\":1}",
        authDongleId());
      authNotifyJson(msg);
      return;
    }
    authSendNonce();
    return;
  }

  if (j.contains("proof") && j["proof"].is_string()) {
    // consume the nonce FIRST, pass or fail - a proof may only ever be judged once per challenge,
    // otherwise a failed guess could be refined against the same nonce
    bool had_nonce = g_auth_nonce_valid; // captured BEFORE consuming, so the reason below is honest
    bool nonce_ok = had_nonce && (millis() - g_auth_nonce_ms) < AUTH_NONCE_TTL_MS;
    g_auth_nonce_valid = false;
    uint8_t incoming[AUTH_SECRET_LEN];
    if (!provisioned || !nonce_ok ||
        !authB64Decode(j["proof"].get<std::string>(), incoming, AUTH_SECRET_LEN)) {
      const char* why = !provisioned ? "unprovisioned"
                      : !had_nonce ? "no nonce issued"
                      : "nonce expired";
      // distinguishing these matters: "expired" means the proof was late (a slow loop task or a
      // sluggish app), "no nonce issued" means the proof answered a challenge we never sent
      logEvent("AUTH2 fail (%s)", nonce_ok ? "malformed proof" : why);
      authNotifyJson("{\"auth\":\"fail\"}");
      return;
    }
    uint8_t expected[AUTH_SECRET_LEN];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, secret, AUTH_SECRET_LEN, g_auth_nonce, AUTH_NONCE_LEN, expected) != 0) {
      logEvent("AUTH2 fail (hmac error)");
      authNotifyJson("{\"auth\":\"fail\"}");
      return;
    }
    // constant-time compare - a byte-by-byte early exit would leak match length through timing
    uint8_t diff = 0;
    for (size_t i = 0; i < AUTH_SECRET_LEN; i++) diff |= (uint8_t)(expected[i] ^ incoming[i]);
    if (diff != 0) {
      logEvent("AUTH2 fail (bad proof)");
      authNotifyJson("{\"auth\":\"fail\"}");
      return;
    }
    // The proof IS the session grant, and the grant carries its protocol version: bleProtoAccept chooses it (or keeps
    // it, for a proof on a live grant), allows or refuses setup, sets the grant and logs it (Helpers/ble_protocol.h).
    // The reply names the version only when the proof offered one, so an app that does not negotiate reads
    // {"auth":"ok"} as it always has; it is at most 23 B, inside any MTU.
    char reply[48];
    bleProtoAccept(j, reply, sizeof(reply));
    authNotifyJson(reply);
    return;
  }
}
#else
void authGateService() {} // no BLE, no ABD8: nothing to authorize
#endif
