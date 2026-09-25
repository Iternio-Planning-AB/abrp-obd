#pragma once
/*
  ECDH key agreement for the ABD8 TOFU provisioning step (BLE_SECURITY_PLAN.md §4).

  WHY THIS EXISTS. The original TOFU step had the phone mint 32 random bytes and WRITE them to the
  dongle as {"provision":"<b64>"} - a verb since DELETED, this being the reason. That was the shared
  secret travelling in clear over a link with no MITM protection (LE Just Works gives none even with
  bonding on). A passive sniffer present for that ONE write owned the dongle permanently: TOFU is
  one-shot, so the secret it captured is the secret that stays, and only serial AUTHCLEAR resets it.
  With that secret it can reach ABD2 (arbitrary CAN injection via profile init_commands), OTA and the
  log dump.

  WHAT IT FIXES, AND WHAT IT DOES NOT. Both sides derive the secret from an ephemeral ECDH exchange,
  so nothing secret is ever transmitted and a PASSIVE sniffer learns nothing. It does NOT stop an
  ACTIVE man-in-the-middle inside the provisioning window - unauthenticated DH cannot. That is a
  deliberate trade: passive sniffing is cheap, silent and needs no timing, while a MITM has to be
  co-present and actively transmitting at the moment of a one-off office provisioning. Closing the
  MITM hole needs an out-of-band value (a passcode/QR printed on the unit, driving SPAKE2+), which is
  a manufacturing decision, not a firmware one.

  UPGRADE PATH, left deliberately open: make the dongle key STATIC (store d in NVS) and publish its
  fingerprint in the ABD5 info JSON. Two phones could then compare fingerprints to DETECT a MITM
  after the fact, and a printed fingerprint would authenticate it outright. Forward secrecy is not
  the point here - the whole purpose is to derive a LONG-TERM stored secret - so an ephemeral key
  buys nothing that a static one loses.

  WIRE FORMAT (now the only provisioning route - the {"provision":...} verb it was staged alongside
  has been removed, so there is no cleartext path left to fall back to):

    app    → {"kex":"<b64 65B uncompressed P-256 point>"}
    dongle → {"kex":"<b64 65B uncompressed P-256 point>","id":"<12 hex dongle id>"}
    both     secret = HKDF-SHA256(IKM = ECDH shared X, salt = dongle id, info = "ABRP-OBD-KEX-v1")

  The derived secret lands in the "auth"/"secret" NVS slot the provision verb used to write, so
  everything downstream - challenge/proof, the QR keyring, AUTHCLEAR revocation - is untouched, and
  a dongle provisioned by the old verb keeps working without a re-provision.

  POINTS ARE UNCOMPRESSED ON PURPOSE. 33-byte compressed points would halve the payload, but
  mbedtls_ecp_point_read_binary does not implement decompression for short Weierstrass curves and
  answers MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE. 65 bytes is what actually parses. It is also why
  g_auth_cmd in auth_gate.h had to grow past 96 bytes: {"kex":"<88 b64 chars>"} is 98.

  P-256 rather than X25519 because SECP256R1 is ALREADY linked for the OTA manifest's ECDSA
  verification (see ota_update.h / jws_verify.h), so this costs no extra curve code. Only
  mbedtls_ecp_* is used - not mbedtls_ecdh_* - for the same reason: those symbols are known-present.

  Extracted in the style of jws_verify.h: #included into the sketch translation unit, not a
  standalone compilation unit.
*/

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

/* Uncompressed P-256 point: 0x04 || X(32) || Y(32). */
static const size_t AUTH_KEX_POINT_LEN = 65;
/* base64 of 65 bytes is 88 chars; +1 for the NUL the JSON assembly needs. */
static const size_t AUTH_KEX_POINT_B64_LEN = 89;

static const char AUTH_KEX_INFO[] = "ABRP-OBD-KEX-v1";

/*
  RNG shim for mbedtls_ecp_gen_keypair / mbedtls_ecp_mul.

  Deliberately built on esp_random() in 4-byte draws, exactly like authSendNonce's nonce fill,
  instead of esp_fill_random: the two live in different headers across IDF versions and this file is
  pulled into the sketch TU, where a wrong include is a build break for a function we can trivially
  express with what auth_gate.h already calls.

  esp_random() is only guaranteed to be a true RNG while the RF subsystem is running. That holds
  here by construction - this runs from authGateService() servicing a command that arrived over an
  ACTIVE BLE connection, so the radio is up. Do not lift this into pre-radio init without calling
  bootloader_random_enable() first: a predictable private key here silently forfeits the whole point.
*/
static int authKexRng(void* ctx, unsigned char* out, size_t len) {
  (void)ctx;
  size_t i = 0;
  while (i < len) {
    uint32_t r = esp_random();
    size_t take = (len - i) < 4 ? (len - i) : 4;
    memcpy(out + i, &r, take);
    i += take;
  }
  return 0;
}

/*
  HKDF-SHA256 with L = 32, written out from mbedtls_md_hmac rather than calling mbedtls_hkdf.
  mbedtls_md_hmac is already linked (the proof path uses it); MBEDTLS_HKDF_C is a config option this
  build has no reason to guarantee. Two HMACs is the whole algorithm at this output length:
    PRK = HMAC(salt, IKM)                    extract
    OKM = HMAC(PRK, info || 0x01)            expand, single block
*/
static bool authKexHkdf32(const uint8_t* ikm, size_t ikm_len,
                          const char* salt,
                          uint8_t out[32]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;

  uint8_t prk[32];
  if (mbedtls_md_hmac(md, (const uint8_t*)salt, strlen(salt), ikm, ikm_len, prk) != 0) {
    return false;
  }

  uint8_t info_block[sizeof(AUTH_KEX_INFO)]; // info bytes (without NUL) + the 0x01 counter
  size_t info_len = sizeof(AUTH_KEX_INFO) - 1;
  memcpy(info_block, AUTH_KEX_INFO, info_len);
  info_block[info_len] = 0x01;

  bool ok = mbedtls_md_hmac(md, prk, sizeof(prk), info_block, info_len + 1, out) == 0;
  memset(prk, 0, sizeof(prk));
  return ok;
}

/*
  One ECDH round: consume the app's public point, derive the shared secret, and hand back our own
  public point (base64) for the reply.

  peer_b64      the app's point, base64, as it arrived in the "kex" field
  dongle_id     HKDF salt - the caller passes authDongleId(). Taken as a parameter rather than
                called here so this header does not depend on auth_gate.h's declaration order.
  out_secret    the 32-byte secret to store in NVS - identical in role to a provisioned one
  out_pub_b64   our public point, base64, NUL-terminated, for the {"kex":...} reply

  Returns false on any malformed input or crypto failure, having zeroed out_secret. The caller must
  treat false as "do not store anything" - a half-derived secret would lock the dongle to a key the
  phone does not hold, recoverable only by AUTHCLEAR.
*/
static bool authKexDerive(const std::string& peer_b64,
                          const char* dongle_id,
                          uint8_t out_secret[32],
                          char* out_pub_b64,
                          size_t out_pub_b64_size) {
  memset(out_secret, 0, 32);
  if (out_pub_b64_size < AUTH_KEX_POINT_B64_LEN) return false;

  uint8_t peer_raw[AUTH_KEX_POINT_LEN];
  size_t peer_len = 0;
  if (mbedtls_base64_decode(peer_raw, sizeof(peer_raw), &peer_len,
                            (const uint8_t*)peer_b64.c_str(), peer_b64.size()) != 0 ||
      peer_len != AUTH_KEX_POINT_LEN) {
    return false;
  }

  mbedtls_ecp_group grp;
  mbedtls_ecp_point peer_point, shared_point;
  mbedtls_mpi d;
  mbedtls_ecp_point own_point;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&peer_point);
  mbedtls_ecp_point_init(&shared_point);
  mbedtls_ecp_point_init(&own_point);
  mbedtls_mpi_init(&d);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_ecp_point_read_binary(&grp, &peer_point, peer_raw, peer_len) != 0) break;

    /*
      NOT optional. Without it a crafted off-curve or small-order point turns the multiply below
      into an invalid-curve attack that leaks our private scalar. ota_update.h checks its pubkey the
      same way for the same reason.
    */
    if (mbedtls_ecp_check_pubkey(&grp, &peer_point) != 0) break;

    if (mbedtls_ecp_gen_keypair(&grp, &d, &own_point, authKexRng, NULL) != 0) break;
    if (mbedtls_ecp_mul(&grp, &shared_point, &d, &peer_point, authKexRng, NULL) != 0) break;

    /*
      The shared value is the X coordinate. mbedTLS 2.x names the field `X` (this build is 2.x - the
      surrounding code calls mbedtls_sha256_ret, which 3.x removed); mbedTLS 3.x renamed it
      private_X, so this line is what breaks first on a core upgrade.
    */
    uint8_t shared_x[32];
    if (mbedtls_mpi_write_binary(&shared_point.X, shared_x, sizeof(shared_x)) != 0) break;

    /*
      Salt is the dongle id, so the same ECDH output can never derive the same secret for two
      different units, and info pins this derivation to this protocol version.
    */
    bool derived = authKexHkdf32(shared_x, sizeof(shared_x), dongle_id, out_secret);
    memset(shared_x, 0, sizeof(shared_x));
    if (!derived) break;

    uint8_t own_raw[AUTH_KEX_POINT_LEN];
    size_t own_len = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &own_point, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &own_len, own_raw, sizeof(own_raw)) != 0 ||
        own_len != AUTH_KEX_POINT_LEN) {
      break;
    }
    size_t b64_len = 0;
    if (mbedtls_base64_encode((uint8_t*)out_pub_b64, out_pub_b64_size, &b64_len,
                              own_raw, own_len) != 0) {
      break;
    }
    out_pub_b64[b64_len] = 0;
    ok = true;
  } while (0);

  /* Wipe the private scalar before it goes back to the heap, pass or fail. */
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&own_point);
  mbedtls_ecp_point_free(&shared_point);
  mbedtls_ecp_point_free(&peer_point);
  mbedtls_ecp_group_free(&grp);
  if (!ok) memset(out_secret, 0, 32);
  return ok;
}
