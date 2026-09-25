#pragma once
/*
  Phase B: verify an RS256 JWS (BLE_SECURITY_PLAN.md §3) so a pushed vehicle profile is proved to come
  from the ABRP backend before any of it is parsed or applied.

  WHY THIS EXISTS - the oldest hole in the protocol, older than every other feature in the plan. A
  profile carries `init_commands` (raw AT, including ATSH to set arbitrary CAN headers and ATSP to pick
  the protocol) and `data_commands` as arbitrary hex. Anything that can write a profile can therefore
  inject arbitrary CAN frames with the dongle as transport. Encryption (phase E) and the HMAC gate
  (phase B2) both help - they stop a bystander and an unauthorized peer - but neither proves the
  profile's ORIGIN. Only a signature does: an authorized phone that has itself been fed a bad profile
  still cannot make this dongle run it.

  VERIFY BEFORE PARSE, ON THE VERBATIM BYTES. The signature covers the exact ASCII of
  "<header>.<payload>" as the backend emitted it. The app MUTATES the decoded profile after verifying
  (maxValue/minValue are coerced to Number in getObdBlePids.ts) and this firmware used to reassemble
  the pushed key/value packages into key-sorted nlohmann JSON - so neither of those forms is what was
  signed. A detached signature over any "canonical" re-serialization would break on number formatting
  alone. Hence: the JWS travels verbatim, is verified as received, and only then is its own payload
  decoded and used as the profile. Same discipline as OTA, which hashes the bytes it actually stored.

  Uses mbedTLS (already linked - ota_update.h uses its SHA-256 and ECDSA). RSASSA-PKCS1-v1_5 + SHA-256
  over the signing input; a 2048-bit verify costs a few ms on this chip, once per profile push.

  NO serial escape hatch here, deliberately, and the dependency graph is why: unlike the auth gate,
  this gate does NOT stand in front of OTA (otaService checks get_authenticated, never the profile).
  So if verification ever breaks - a rotated key, a backend format change - the dongle is still
  reachable and a firmware fix is still deliverable over the air. The failure is recoverable without
  physical access, so it does not need an override the way an OTA-gating check would.
*/

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

/*
  The ABRP profile-signing public key, byte-for-byte the one the APP already verifies against
  (src/api/vehicles/getObdBlePids.ts). Both ends must trust the same key or a profile the app accepts
  would be refused here. If it is ever rotated, both sides change together and the firmware side ships
  over OTA first - a dongle on the old key still accepts old-key profiles, so there is no flag day.
*/
static const char JWS_PUBLIC_KEY_PEM[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAn35J/7l6oL9u7lWqtRB1\n"
  "Zehvlsm8hx2cBJn9+sHWxxqVARbSCUK1++Dv0EJf1LZNkM81BD0/FLVbG2bg591D\n"
  "6o469Qdwrm7/FuMtooL445YEA5T+0UlIgrxwgroo7qfML23VmpvhzV0lO/KG6v4P\n"
  "YrwVvYD0HnlZnj6fOZUpuzw5FnDYBPqZxmjYYM1Pgqf/vOYFlICCv3jMb4EL0hHP\n"
  "xAO9OxupfLr3FtOw1dW3Te6dBA9c1bBoKRrnnZuKeTuDmpIRmA7FnVpcTUYsdXNa\n"
  "eA1T/KFRhMRGoHdE51+vuICwp6wPBEP/aRsuUaKBVN4ePnKPOmfCz44dGLlvr9cR\n"
  "zQIDAQAB\n"
  "-----END PUBLIC KEY-----\n";

static const size_t JWS_RSA_SIG_LEN = 256; // 2048-bit RSASSA-PKCS1-v1_5

/*
  base64URL -> bytes. JWS uses the URL-safe alphabet WITHOUT padding, which mbedtls_base64_decode does
  not accept, so translate '-'/'_' and re-pad. Returns the byte count, or -1 on malformed input / when
  the result would not fit.
*/
static int jwsB64UrlDecode(const char* in, size_t in_len, uint8_t* out, size_t out_cap) {
  if (!in || !out) return -1;
  // 4 base64 chars carry 3 bytes; refuse anything that cannot fit before allocating work
  size_t padded_len = in_len + 4;
  char* tmp = (char*)malloc(padded_len + 1);
  if (!tmp) return -1;
  size_t n = 0;
  for (size_t i = 0; i < in_len; i++) {
    char c = in[i];
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
    tmp[n++] = c;
  }
  while (n % 4 != 0) tmp[n++] = '=';
  tmp[n] = '\0';
  size_t olen = 0;
  int rc = mbedtls_base64_decode(out, out_cap, &olen, (const uint8_t*)tmp, n);
  free(tmp);
  if (rc != 0) return -1;
  return (int)olen;
}

/*
  Verify a compact JWS and hand back the location of its (still base64url-encoded) payload.

  jws/jws_len : the compact serialization exactly as received - "<header>.<payload>.<signature>".
  payload_off/payload_len : set on success to the payload segment inside `jws` (not decoded here, so
                            the caller decodes only after the signature has been proved).

  Returns true only when the algorithm is RS256 AND the signature verifies. Everything about the
  payload is untrusted until that happens.
*/
static bool jwsVerifyRs256(const char* jws, size_t jws_len, size_t* payload_off, size_t* payload_len) {
  if (!jws || jws_len == 0) return false;

  // locate the two separators - a compact JWS has exactly three segments
  size_t first = 0, second = 0;
  bool have_first = false, have_second = false;
  for (size_t i = 0; i < jws_len; i++) {
    if (jws[i] != '.') continue;
    if (!have_first) { first = i; have_first = true; }
    else if (!have_second) { second = i; have_second = true; }
    else return false; // a fourth segment is not a compact JWS
  }
  if (!have_first || !have_second || second <= first + 1 || first == 0) return false;
  if (second + 1 >= jws_len) return false; // empty signature

  // Header must declare RS256. Checked because "alg" is attacker-controlled input until verified:
  // accepting whatever it names is the classic JWS confusion bug (alg:none, or an HMAC alg verified
  // against the public key as if it were a shared secret).
  uint8_t header[128];
  int hlen = jwsB64UrlDecode(jws, first, header, sizeof(header) - 1);
  if (hlen <= 0) return false;
  header[hlen] = '\0';
  if (strstr((const char*)header, "\"RS256\"") == NULL) return false;

  uint8_t sig[JWS_RSA_SIG_LEN];
  int slen = jwsB64UrlDecode(jws + second + 1, jws_len - second - 1, sig, sizeof(sig));
  if (slen != (int)JWS_RSA_SIG_LEN) return false;

  // The signing input is the ASCII "<header>.<payload>" - the bytes as they arrived, never a
  // re-serialization (see the file header).
  uint8_t digest[32];
  if (mbedtls_sha256_ret((const uint8_t*)jws, second, digest, 0) != 0) return false;

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = false;
  // the +1 counts the NUL: mbedtls_pk_parse_public_key requires it for PEM input
  if (mbedtls_pk_parse_public_key(&pk, (const uint8_t*)JWS_PUBLIC_KEY_PEM,
                                  sizeof(JWS_PUBLIC_KEY_PEM)) == 0) {
    if (mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
      ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, JWS_RSA_SIG_LEN) == 0;
    }
  }
  mbedtls_pk_free(&pk);

  if (ok) {
    *payload_off = first + 1;
    *payload_len = second - first - 1;
  }
  return ok;
}
