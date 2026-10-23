#pragma once
/*
  Firmware OTA over BLE (ENABLE_OTA) - staged dual-slot update with native rollback.
  See ota-update-architecture-v4.md for the full design rationale.

  WHAT THIS FILE CONTAINS
    - the BLE wire protocol: control opcodes on ABD5, 3-byte-header firmware packets on ABD6, and the
      20-byte CRC16-CCITT ACK/status frames we notify back (crc16Ccitt, otaSendAck, otaSendStatus);
    - the two Bluedroid-task hooks (otaCtrlHook / otaDataHook) which ONLY parse + copy + set flags;
    - the loop-task service (otaService) which does every flash operation, sends every ACK, and enforces
      every timeout/power guard;
    - staging + commit via native esp_ota_ops, with a read-back SHA-256 verify before the boot pointer moves;
    - the trial-boot handling: self-test then mark-valid, or let the bootloader roll back (otaIsTrialBoot /
      otaHandlePendingVerify), plus NVS rollback detection so the app can be told an update was reverted.

  SAFETY MODEL (why this cannot get stuck or fire accidentally)
    - The update state is entered ONLY by a START command on ABD5 from a connected + AUTHENTICATED phone.
      Nothing else - no timer, no boot path, no sensor - can enter it.
    - All session state is RAM-only. Any reset (WDT, brownout, power cut, panic) clears it completely, so the
      device can never boot into "updating". The staged image is in the INACTIVE slot and is never bootable
      until esp_ota_set_boot_partition(), so an interrupted transfer is simply discarded.
    - Three independent exits guarantee liveness: an inactivity timeout, an absolute session cap, and a
      bounded resume window after a BLE drop. Whichever fires first aborts cleanly and releases every hold.

  TASK DISCIPLINE (this is load-bearing - see the config-push PANIC history)
    The BLE hooks run on the Bluedroid task and must never touch flash or block: esp_ota_begin() erases the
    target and blocks for seconds, which would stall the BLE stack and drop the phone mid-transfer. So the
    hooks only stage a pending command / assemble a sector, and otaService() on the loop task performs the
    work. The 4KB sector buffer is static, never a task stack local (it would be half of an 8KB stack).
*/

// ENABLE_BLE is required as well: BLE is the only transport, and every ACK/status path goes through the
// wrapper. Without it the stubs at the bottom apply and the feature is simply absent.
#if ENABLE_OTA && ENABLE_BLE

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include "ota_pubkey.h"

/*
  SIGNATURE SCHEME (the dongle's trust boundary). The app sends version + size + sha256 + signature in START;
  we recompute the hash from the STAGED FLASH and verify a signature over this domain-separated blob:

      blob   = "ABRPOTA1" | uint32le(version) | uint32le(size) | sha256(image)      (48 bytes)
      digest = SHA-256(blob)
      sig    = ECDSA-P256(digest), raw r||s, 64 bytes

  Signing the VERSION and SIZE alongside the image hash - not just the image hash - is deliberate: it stops a
  replay in which a genuinely-signed OLD image is presented with an inflated version number to slip past the
  anti-downgrade check. The hash that goes into the blob is the one WE computed by reading the staged image
  back, never the one the app claimed, so a tampered image cannot match a valid signature.
  tools/ota_sign.py produces exactly this; keep the two in step if either changes.
*/
#define OTA_SIG_MAGIC "ABRPOTA1"
#define OTA_SIG_LEN   64

/* ---------------------------------------------------------------------------------------------------
   Wire protocol (must match the app's transfer module)
   --------------------------------------------------------------------------------------------------- */

// Control opcodes: uint16 little-endian in bytes [0..1] of an ABD5 write.
//   START: [0..1]=0x0001 [2..5]=image_size [6..9]=version [10..41]=sha256 (optional) [42..105]=signature
//          (reserved for the phase-2 ECDSA check; parsed and ignored today).
#define OTA_CMD_START 0x0001
#define OTA_CMD_END   0x0002
#define OTA_CMD_ABORT 0x0003
#define OTA_CMD_INFO  0x0004

// Sector-ACK status codes, bytes [2..3] of an ACK frame.
#define OTA_ST_OK      0x0000
#define OTA_ST_VERIFY  0x0001  // hash/verify failure
#define OTA_ST_SECTOR  0x0002  // sector or packet out of sequence -> resend this sector from packet 0
#define OTA_ST_LENGTH  0x0003  // payload would overflow the sector

// Status frames use a sentinel in bytes [0..1] that can never be a real sector index (indices stop at
// image_size/4096, ~420 for the current image), so the app can tell ACKs and status apart unambiguously.
#define OTA_FRAME_STATUS 0xFFFE

// Phases reported in a status frame, byte [2].
#define OTA_PH_IDLE      0x00
#define OTA_PH_ARMED     0x01  // ready to receive; detail = sector to (re)start from (the resume anchor)
#define OTA_PH_VERIFYING 0x03
#define OTA_PH_COMMITTED 0x04  // boot pointer moved, rebooting now
#define OTA_PH_ERROR     0x05  // detail = OTA_ERR_*
#define OTA_PH_ABORTED   0x06  // detail = OTA_ERR_*
#define OTA_PH_REJECTED  0x07  // detail = OTA_ERR_*
#define OTA_PH_TRIAL     0x08  // running a pending-verify trial boot
#define OTA_PH_ROLLEDBACK 0x09 // the previous attempt was reverted by the bootloader

// Reject / abort / error reasons (detail field). Distinct values so the app can show a real cause.
#define OTA_ERR_NOAUTH      1
#define OTA_ERR_BUSY        2
#define OTA_ERR_NOTPARKED   3
#define OTA_ERR_WEAKPOWER   4
#define OTA_ERR_BADSIZE     5
#define OTA_ERR_DOWNGRADE   6
#define OTA_ERR_TRIALBOOT   7  // running image still pending-verify; a new OTA cannot begin yet
#define OTA_ERR_LOWHEAP     8
#define OTA_ERR_BEGINFAIL   9
#define OTA_ERR_NOPARTITION 10
#define OTA_ERR_WRITEFAIL   11
#define OTA_ERR_SIZEMISMATCH 12
#define OTA_ERR_ENDFAIL     13
#define OTA_ERR_HASHFAIL    14
#define OTA_ERR_COMMITFAIL  15
#define OTA_ERR_INACTIVITY  16
#define OTA_ERR_SESSIONCAP  17
#define OTA_ERR_LINKLOST    18
#define OTA_ERR_INTERNAL    19
#define OTA_ERR_NOSIG       20 // a signature is required but the app didn't send one
#define OTA_ERR_SIGFAIL     21 // signature present but does NOT verify against the embedded public key
#define OTA_ERR_NOKEY       22 // signature required but no public key is installed (run tools/ota_keygen.py)

/* ---------------------------------------------------------------------------------------------------
   Session state - ALL RAM-only, so any reset clears it (see the safety model above)
   --------------------------------------------------------------------------------------------------- */

enum OtaState : uint8_t {
  OTA_S_IDLE = 0,     // no session
  OTA_S_RECEIVING,    // armed + streaming sectors
  OTA_S_RESUMEWAIT    // BLE dropped mid-transfer; handle kept alive for OTA_RESUME_GRACE_MS
};
static OtaState ota_state = OTA_S_IDLE;

// The 4KB assembly buffer. Static by design: 4KB on the 8KB Bluedroid or loop stack would be fatal.
// Reused after the transfer as the read-back buffer for the SHA-256 verify (the transfer is over by then).
static uint8_t ota_sector_buf[OTA_SECTOR_SIZE];

// Written by the Bluedroid task, read by the loop task. The stop-and-wait protocol means only one side
// owns the buffer at a time: the client cannot send sector N+1 until we ACK sector N, and we only ACK
// after the flash write completes. ota_sector_ready is the handoff.
static volatile bool     ota_sector_ready = false;
static volatile uint16_t ota_sector_len = 0;
static volatile bool     ota_sector_is_last = false;

// Sector assembly cursor (Bluedroid task).
static uint16_t ota_cur_sector = 0;   // next sector index expected == the resume anchor
static uint8_t  ota_next_packet = 0;  // next packet_seq expected within the current sector
static uint16_t ota_sector_off = 0;   // bytes assembled into ota_sector_buf so far
static bool     ota_sector_err = false; // sticky: this sector must be resent from packet 0

// A NACK is raised by the Bluedroid task and sent by the loop task (keeps all notifies on one task).
static volatile bool     ota_nack_pending = false;
static volatile uint16_t ota_nack_sector = 0;
static volatile uint16_t ota_nack_status = 0;

// One pending control command, staged by the Bluedroid task for the loop task to execute.
static volatile bool ota_cmd_present = false;
static uint16_t ota_cmd_opcode = 0;
static uint32_t ota_cmd_size = 0;
static uint32_t ota_cmd_version = 0;
static uint8_t  ota_cmd_sha[32];
static bool     ota_cmd_has_sha = false;
static uint8_t  ota_cmd_sig[OTA_SIG_LEN];
static bool     ota_cmd_has_sig = false;

// The staging session.
static esp_ota_handle_t      ota_handle = 0;
static const esp_partition_t* ota_part = nullptr;
static uint32_t ota_img_size = 0;
static uint32_t ota_img_version = 0;
static uint8_t  ota_img_sha[32];
static bool     ota_img_has_sha = false;
static uint8_t  ota_img_sig[OTA_SIG_LEN];
static bool     ota_img_has_sig = false;
static uint32_t ota_bytes_written = 0;
static uint16_t ota_total_sectors = 0;
static uint8_t  ota_progress_decile = 0;   // milestone logging only - never log per sector (flash contention)

static unsigned long ota_last_rx_ms = 0;
static unsigned long ota_session_start_ms = 0;
static unsigned long ota_disconnect_ms = 0;
static unsigned long ota_last_power_check_ms = 0;
static uint8_t       ota_weak_rail_strikes = 0;

// Set when this boot is a pending-verify trial boot, and when the previous attempt was rolled back.
static bool ota_rolled_back = false;

/* ---------------------------------------------------------------------------------------------------
   Frame helpers
   --------------------------------------------------------------------------------------------------- */

// CRC16-CCITT, poly 0x1021, init 0x0000 - the variant used by the Espressif esp_ble_ota profile, so the
// app's crc16Ccitt must match these parameters exactly.
static uint16_t crc16Ccitt(const uint8_t* d, size_t n) {
  uint16_t crc = 0x0000;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static inline uint32_t otaLe32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Sector ACK: [0..1] sector echoed, [2..3] status, [4..5] next expected sector (the resume anchor the app
// should restart from), [18..19] CRC16 over bytes [0..17].
static void otaSendAck(uint16_t sector, uint16_t status) {
  uint8_t f[20] = { 0 };
  f[0] = (uint8_t)(sector & 0xFF);       f[1] = (uint8_t)(sector >> 8);
  f[2] = (uint8_t)(status & 0xFF);       f[3] = (uint8_t)(status >> 8);
  f[4] = (uint8_t)(ota_cur_sector & 0xFF); f[5] = (uint8_t)(ota_cur_sector >> 8);
  uint16_t c = crc16Ccitt(f, 18);
  f[18] = (uint8_t)(c & 0xFF);           f[19] = (uint8_t)(c >> 8);
  ble.notifyOta(f, sizeof(f));
}

// Status frame: [0..1] = OTA_FRAME_STATUS sentinel, [2] phase, [4..5] detail, [6..9] extra, [18..19] CRC16.
static void otaSendStatus(uint8_t phase, uint16_t detail, uint32_t extra) {
  uint8_t f[20] = { 0 };
  f[0] = (uint8_t)(OTA_FRAME_STATUS & 0xFF); f[1] = (uint8_t)(OTA_FRAME_STATUS >> 8);
  f[2] = phase;
  f[4] = (uint8_t)(detail & 0xFF);           f[5] = (uint8_t)(detail >> 8);
  f[6] = (uint8_t)(extra & 0xFF);            f[7] = (uint8_t)((extra >> 8) & 0xFF);
  f[8] = (uint8_t)((extra >> 16) & 0xFF);    f[9] = (uint8_t)((extra >> 24) & 0xFF);
  uint16_t c = crc16Ccitt(f, 18);
  f[18] = (uint8_t)(c & 0xFF);               f[19] = (uint8_t)(c >> 8);
  ble.notifyOta(f, sizeof(f));
}

/* ---------------------------------------------------------------------------------------------------
   NVS state (namespace "ota") - version tracking + rollback detection.
   Deliberately NVS and not RTC RAM: esp_restart() CLEARS .rtc.bss, so every RTC_DATA_ATTR variable is
   wiped by the commit reboot. Its own namespace, separate from the BLE wrapper's "BLE" namespace.
     current : the confirmed-running firmware version (anti-downgrade floor)
     attempt : version whose boot pointer we set but which has not confirmed itself yet (0 = none)
   --------------------------------------------------------------------------------------------------- */

static uint32_t otaNvsGet(const char* key, uint32_t def) {
  Preferences p;
  if (!p.begin("ota", true)) return def;
  uint32_t v = p.getULong(key, def);
  p.end();
  return v;
}

static void otaNvsPut(const char* key, uint32_t val) {
  Preferences p;
  if (!p.begin("ota", false)) return;
  p.putULong(key, val);
  p.end();
}

/* Version floor for anti-downgrade: whichever is higher, the compiled-in version or the stored one. */
static uint32_t otaCurrentVersion() {
  uint32_t stored = otaNvsGet("current", 0);
  return stored > (uint32_t)OTA_FW_VERSION ? stored : (uint32_t)OTA_FW_VERSION;
}

/* ---------------------------------------------------------------------------------------------------
   Power gate. Mirrors the ATRV gate's THREE-BAND rail logic (config.h OTA_RAIL_HEALTHY) rather than a
   single voltage floor - that is what keeps bench testing possible, since USB reads ~5.6V:
     rail <  IO36_LOCKOUT_LOW (8.0) -> bench / no car        -> ALLOW
     rail >= OTA_RAIL_HEALTHY (12.2) -> healthy car battery   -> ALLOW
     in between                      -> weak-but-alive battery -> REFUSE (a flash-write current spike on a
                                        sagging 12V can trip the brownout detector mid-write)
   Reads the rail FRESH rather than using g_last_io36: the cached value is refreshed by the poll cycle, and
   polling is suspended during an update, so the cache goes stale exactly when we need it.
   --------------------------------------------------------------------------------------------------- */
static bool otaPowerOk() {
#if ENABLE_IO36_RAIL && ENABLE_SLEEP && ENABLE_MEMS
  float rail = readIO36Rail();
  if (rail < IO36_LOCKOUT_LOW) return true;      // bench / no car
  return rail >= OTA_RAIL_HEALTHY;               // healthy car, else weak band
#else
  return true;                                   // no rail sense compiled in - nothing to gate on
#endif
}

/* ---------------------------------------------------------------------------------------------------
   Session teardown
   --------------------------------------------------------------------------------------------------- */

// Release everything and return to IDLE. Safe to call from any state. `reason` 0 = a normal, quiet reset.
static void otaReset(uint8_t phase, uint16_t reason) {
  if (ota_handle) {
    esp_ota_abort(ota_handle);   // releases the staging handle; the inactive slot is simply left dirty
    ota_handle = 0;
  }
  if (reason) {
    otaSendStatus(phase, reason, ota_bytes_written);
#if ENABLE_LOGGING
    logEvent("OTA %s reason=%u after %lu/%lu B (sector %u)",
      phase == OTA_PH_REJECTED ? "REJECTED" : phase == OTA_PH_ERROR ? "ERROR" : "ABORTED",
      (unsigned)reason, (unsigned long)ota_bytes_written, (unsigned long)ota_img_size,
      (unsigned)ota_cur_sector);
#endif
  }
  ota_state = OTA_S_IDLE;
  g_ota_active = false;          // releases the sleep/poll/console holds (see the guard sites)
  ota_part = nullptr;
  ota_img_size = ota_img_version = ota_bytes_written = 0;
  ota_img_has_sha = false;
  ota_img_has_sig = false;
  ota_total_sectors = 0;
  ota_progress_decile = 0;
  ota_cur_sector = 0;
  ota_next_packet = 0;
  ota_sector_off = 0;
  ota_sector_err = false;
  ota_sector_ready = false;
  ota_sector_len = 0;
  ota_sector_is_last = false;
  ota_weak_rail_strikes = 0;
  ota_cmd_present = false;
  ota_nack_pending = false;
}

/* Reject a START without disturbing any session that may already be running. */
static void otaReject(uint16_t reason) {
  otaSendStatus(OTA_PH_REJECTED, reason, 0);
#if ENABLE_LOGGING
  logEvent("OTA START rejected reason=%u", (unsigned)reason);
#endif
}

/* ---------------------------------------------------------------------------------------------------
   Bluedroid-task hooks. Parse + copy + set flags ONLY: no flash, no logging, no blocking.
   --------------------------------------------------------------------------------------------------- */

void otaCtrlHook(const uint8_t* d, size_t n) {
  if (!d || n < 2) return;
  if (ota_cmd_present) return;   // one command in flight; the app waits for our reply before sending more
  uint16_t op = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
  ota_cmd_opcode = op;
  ota_cmd_size = 0;
  ota_cmd_version = 0;
  ota_cmd_has_sha = false;
  ota_cmd_has_sig = false;
  if (op == OTA_CMD_START && n >= 10) {
    ota_cmd_size = otaLe32(d + 2);
    ota_cmd_version = otaLe32(d + 6);
    if (n >= 42) {               // sha256 present (optional, so an app that omits it still works)
      memcpy(ota_cmd_sha, d + 10, 32);
      ota_cmd_has_sha = true;
    }
    if (n >= 42 + OTA_SIG_LEN) { // signature present
      memcpy(ota_cmd_sig, d + 42, OTA_SIG_LEN);
      ota_cmd_has_sig = true;
    }
  }
  ota_cmd_present = true;
}

void otaDataHook(const uint8_t* d, size_t n) {
  // Not in a transfer: ignore. A stray write can therefore never start anything or corrupt state.
  if (!g_ota_active || ota_state != OTA_S_RECEIVING) return;
  if (!d || n < 3) return;
  // Previous sector still awaiting its flash write - under stop-and-wait the client shouldn't be sending.
  if (ota_sector_ready) return;

  uint16_t sec = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
  uint8_t  seq = d[2];
  size_t   payload = n - 3;
  ota_last_rx_ms = millis();

  // 0xFFFF is the reference protocol's "this is the final sector" marker; we also accept the real index for
  // the final sector, so either app convention works.
  bool marker_last = (sec == 0xFFFF);
  uint16_t eff = marker_last ? ota_cur_sector : sec;
  bool is_last = marker_last || (ota_total_sectors && eff == (uint16_t)(ota_total_sectors - 1));
  bool last_pkt = (seq == 0xFF);

  // Sticky sector error: swallow everything (including the stale end-of-sector marker) until the client
  // restarts this sector from packet 0. Without this, a mid-sector loss would silently corrupt the image.
  if (ota_sector_err) {
    // Accept seq 0 as the restart - or the end marker, but ONLY for the final sector, whose sole packet
    // can legitimately carry seq 0xFF (image_size % 4096 < payload size). Requiring seq==0 there would
    // ignore every retry and deadlock the transfer until the inactivity timeout. The is_last restriction
    // matters: for a NORMAL sector, the burst's own trailing 0xFF packet (arriving right after the loss
    // that set the sticky error) would otherwise be taken as a fresh restart, assemble a one-packet
    // "sector", advance the sequential esp_ota_write cursor by a few hundred bytes, and doom the whole
    // transfer to SIZEMISMATCH at END after minutes of streaming.
    if (eff == ota_cur_sector && (seq == 0 || (last_pkt && is_last))) {
      ota_sector_err = false;
      ota_sector_off = 0;
      ota_next_packet = 0;
    } else {
      return;
    }
  }

  if (eff != ota_cur_sector) {                       // wrong sector
    ota_sector_err = true; ota_sector_off = 0; ota_next_packet = 0;
    ota_nack_sector = sec; ota_nack_status = OTA_ST_SECTOR; ota_nack_pending = true;
    return;
  }
  if (!last_pkt && seq != ota_next_packet) {          // wrong packet within the sector
    ota_sector_err = true; ota_sector_off = 0; ota_next_packet = 0;
    ota_nack_sector = sec; ota_nack_status = OTA_ST_SECTOR; ota_nack_pending = true;
    return;
  }
  if (ota_sector_off + payload > OTA_SECTOR_SIZE) {   // would overflow the sector buffer
    ota_sector_err = true; ota_sector_off = 0; ota_next_packet = 0;
    ota_nack_sector = sec; ota_nack_status = OTA_ST_LENGTH; ota_nack_pending = true;
    return;
  }

  memcpy(ota_sector_buf + ota_sector_off, d + 3, payload);
  ota_sector_off += payload;
  if (!last_pkt) ota_next_packet++;

  if (last_pkt) {                                    // sector complete -> hand off to the loop task
    ota_sector_len = ota_sector_off;
    ota_sector_is_last = is_last;
    ota_sector_ready = true;                         // publish LAST: the loop task keys off this flag
  }
}

/* ---------------------------------------------------------------------------------------------------
   Loop-task work
   --------------------------------------------------------------------------------------------------- */

/* Read the staged image back out of flash and hash it. Cannot be done in RAM (the image is far larger than
   the heap), so this is the read-back verify that gates the commit. Feeds the watchdog: hashing ~1.7MB
   takes seconds. Reuses the sector buffer - the transfer is finished by now. */
static bool otaComputeStagedSha(uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return false; }
  uint32_t off = 0;
  bool ok = true;
  while (off < ota_img_size) {
    size_t chunk = (size_t)((ota_img_size - off) < OTA_SECTOR_SIZE ? (ota_img_size - off) : OTA_SECTOR_SIZE);
    if (esp_partition_read(ota_part, off, ota_sector_buf, chunk) != ESP_OK) { ok = false; break; }
    if (mbedtls_sha256_update_ret(&ctx, ota_sector_buf, chunk) != 0) { ok = false; break; }
    off += chunk;
    if ((off & 0xFFFF) == 0) esp_task_wdt_reset();   // feed every 64KB
  }
  if (ok && mbedtls_sha256_finish_ret(&ctx, out) != 0) ok = false;
  mbedtls_sha256_free(&ctx);
  return ok;
}

/* Is a real public key installed? (The generated placeholder is all zeros - see Helpers/ota_pubkey.h.) */
static bool otaHavePubkey() {
  for (size_t i = 0; i < OTA_PUBKEY_LEN; i++) if (OTA_PUBKEY[i] != 0x00) return true;
  return false;
}

/*
  Verify the ECDSA-P256 signature over the domain-separated blob described at the top of this file. img_sha is
  the hash WE computed from the staged flash, so a tampered image can never satisfy a valid signature.
*/
static bool otaVerifySignature(const uint8_t img_sha[32]) {
  // Build blob = magic | version | size | img_sha, then hash it.
  uint8_t blob[8 + 4 + 4 + 32];
  memcpy(blob, OTA_SIG_MAGIC, 8);
  blob[8]  = (uint8_t)(ota_img_version & 0xFF);
  blob[9]  = (uint8_t)((ota_img_version >> 8) & 0xFF);
  blob[10] = (uint8_t)((ota_img_version >> 16) & 0xFF);
  blob[11] = (uint8_t)((ota_img_version >> 24) & 0xFF);
  blob[12] = (uint8_t)(ota_img_size & 0xFF);
  blob[13] = (uint8_t)((ota_img_size >> 8) & 0xFF);
  blob[14] = (uint8_t)((ota_img_size >> 16) & 0xFF);
  blob[15] = (uint8_t)((ota_img_size >> 24) & 0xFF);
  memcpy(blob + 16, img_sha, 32);

  uint8_t digest[32];
  if (mbedtls_sha256_ret(blob, sizeof(blob), digest, 0) != 0) return false;

  mbedtls_ecdsa_context ctx;   // mbedtls 2.28: typedef of mbedtls_ecp_keypair, grp/Q are public fields
  mbedtls_ecdsa_init(&ctx);
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
    if (mbedtls_ecp_point_read_binary(&ctx.grp, &ctx.Q, OTA_PUBKEY, OTA_PUBKEY_LEN) != 0) break;
    if (mbedtls_ecp_check_pubkey(&ctx.grp, &ctx.Q) != 0) break;
    if (mbedtls_mpi_read_binary(&r, ota_img_sig, 32) != 0) break;
    if (mbedtls_mpi_read_binary(&s, ota_img_sig + 32, 32) != 0) break;
    ok = (mbedtls_ecdsa_verify(&ctx.grp, digest, sizeof(digest), &ctx.Q, &r, &s) == 0);
  } while (0);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  mbedtls_ecdsa_free(&ctx);
  return ok;
}

/* END: close staging, verify, move the boot pointer, record the attempt, reboot. */
static void otaFinalize() {
  if (ota_bytes_written != ota_img_size) {
    otaReset(OTA_PH_ERROR, OTA_ERR_SIZEMISMATCH);
    return;
  }
  otaSendStatus(OTA_PH_VERIFYING, 0, ota_bytes_written);
  esp_task_wdt_reset();

  esp_err_t e = esp_ota_end(ota_handle);   // validates image format/header; frees the handle either way
  ota_handle = 0;
  if (e != ESP_OK) { otaReset(OTA_PH_ERROR, OTA_ERR_ENDFAIL); return; }

  // Hash the staged image back out of flash ONCE, then use that digest for both checks below.
  uint8_t img_sha[32];
  if (ota_img_has_sha || OTA_REQUIRE_SIGNATURE || ota_img_has_sig) {
    esp_task_wdt_reset();
    if (!otaComputeStagedSha(img_sha)) { otaReset(OTA_PH_ERROR, OTA_ERR_HASHFAIL); return; }
    esp_task_wdt_reset();
  }
  // Integrity: does the staged image match the hash the app declared? (Early, clearer failure than a bad
  // signature; skipped only if the app didn't send a hash.)
  if (ota_img_has_sha && memcmp(img_sha, ota_img_sha, 32) != 0) {
    otaReset(OTA_PH_ERROR, OTA_ERR_HASHFAIL);
    return;
  }
  // AUTHENTICITY - the actual trust boundary. Nothing reaches the boot pointer without a signature that
  // verifies against the public key compiled into this firmware.
#if OTA_REQUIRE_SIGNATURE
  if (!ota_img_has_sig) { otaReset(OTA_PH_ERROR, OTA_ERR_NOSIG); return; }
  if (!otaVerifySignature(img_sha)) {
    Serial.printf("OTA: SIGNATURE VERIFICATION FAILED - refusing to commit\n");
    otaReset(OTA_PH_ERROR, OTA_ERR_SIGFAIL);
    return;
  }
  Serial.printf("OTA: signature verified (ECDSA-P256)\n");
#else
  // Signing disabled at build time (bench transfer testing). Still verify a signature if one was supplied, so
  // a mismatch is caught rather than ignored - but do not require one.
  if (ota_img_has_sig && !otaVerifySignature(img_sha)) {
    otaReset(OTA_PH_ERROR, OTA_ERR_SIGFAIL);
    return;
  }
#endif
  // Re-check power immediately before committing: a brownout AFTER the commit but BEFORE the trial boot
  // marks itself valid would make the bootloader roll back a perfectly good image.
  if (!otaPowerOk()) { otaReset(OTA_PH_ABORTED, OTA_ERR_WEAKPOWER); return; }

  esp_task_wdt_reset();
  if (esp_ota_set_boot_partition(ota_part) != ESP_OK) {
    otaReset(OTA_PH_ERROR, OTA_ERR_COMMITFAIL);
    return;
  }
  // Record the attempt so that, if the bootloader reverts, the OLD firmware can detect it on the next boot
  // and report ROLLED_BACK to the app (see otaHandlePendingVerify).
  otaNvsPut("attempt", ota_img_version);

  otaSendStatus(OTA_PH_COMMITTED, 0, ota_img_version);
  if (g_config_pending) dropUnsavedPush("ota"); // a push the transfer held back dies with the reboot
#if ENABLE_LOGGING
  logEvent("OTA committed v%lu (%lu B) -> reboot into trial boot",
    (unsigned long)ota_img_version, (unsigned long)ota_img_size);
  datalogger.end();      // flush + close the log segment; the reboot wipes RAM
#endif
  Serial.printf("OTA: committed, rebooting into new firmware\n");
  Serial.flush();
  delay(600);            // let the COMMITTED notify actually go out over the air before we reset
  wdtStreakEnd();
  esp_restart();         // never returns
}

/* START (or a resume of an interrupted session with identical parameters). */
static void otaHandleStart() {
  // Resume path: same image, session still alive inside the grace window -> continue where we left off
  // instead of re-erasing and restarting a multi-minute transfer.
  // Re-authentication is required even to resume: a BLE disconnect clears the auth flag, so the app must
  // re-run its handshake (it already does on every connect). Without this, anyone who connected during the
  // resume window could continue someone else's in-flight transfer.
  if (ota_state == OTA_S_RESUMEWAIT && ota_handle && ble.get_authenticated() &&
      ota_cmd_size == ota_img_size && ota_cmd_version == ota_img_version &&
      ota_cmd_has_sha == ota_img_has_sha &&
      (!ota_img_has_sha || memcmp(ota_cmd_sha, ota_img_sha, 32) == 0)) {
    ota_state = OTA_S_RECEIVING;
    ota_sector_err = false;
    ota_sector_off = 0;
    ota_next_packet = 0;
    ota_sector_ready = false;
    ota_last_rx_ms = millis();
#if ENABLE_LOGGING
    logEvent("OTA resumed at sector %u/%u (%lu B already staged)",
      (unsigned)ota_cur_sector, (unsigned)ota_total_sectors, (unsigned long)ota_bytes_written);
#endif
    otaSendStatus(OTA_PH_ARMED, ota_cur_sector, ota_bytes_written);
    return;
  }

  // --- preconditions, each with its own reject reason so the app can explain the refusal ---
  if (!ble.get_authenticated())                 { otaReject(OTA_ERR_NOAUTH);    return; }
  if (g_ota_active || ota_state != OTA_S_IDLE)  { otaReject(OTA_ERR_BUSY);      return; }
#if OTA_REQUIRE_SIGNATURE
  // Check the signing prerequisites HERE rather than at commit: refusing up front costs nothing, whereas
  // discovering it after erasing 1.7MB and streaming for minutes wastes the user's time and airtime.
  if (!otaHavePubkey()) {
    Serial.printf("OTA: no signing public key installed - run tools/ota_keygen.py (or build with "
                  "-DOTA_REQUIRE_SIGNATURE=0 for an unsigned bench test)\n");
    otaReject(OTA_ERR_NOKEY); return;
  }
  if (!ota_cmd_has_sig)                         { otaReject(OTA_ERR_NOSIG);     return; }
#endif
  // A new OTA cannot begin while the running image is itself unconfirmed (esp_ota_begin would fail with
  // ESP_ERR_OTA_ROLLBACK_INVALID_STATE). Normally impossible: the trial boot marks valid early in setup().
  {
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
      otaReject(OTA_ERR_TRIALBOOT); return;
    }
  }
  // NOTE: deliberately NOT gated on is_currently_parked. An update is always manually initiated by the user
  // in the app, so driving is a legitimate time to do it - the transfer is safe either way (a failure only
  // ever leaves the INACTIVE slot dirty, and mid-drive the rail is at alternator voltage, i.e. the healthiest
  // power the device ever sees). The only cost is that live telemetry pauses for the duration, which the app
  // already communicates. OTA_ERR_NOTPARKED is kept in the protocol for compatibility but is never sent.
  if (!otaPowerOk())                            { otaReject(OTA_ERR_WEAKPOWER); return; }
  if (ota_cmd_size < OTA_MIN_IMAGE_BYTES ||
      ota_cmd_size > OTA_MAX_IMAGE_BYTES)       { otaReject(OTA_ERR_BADSIZE);   return; }
  // Anti-downgrade. Equal is allowed on purpose so re-flashing the same build (bench testing, recovery)
  // works; only a genuinely older version is refused. version 0 = "app didn't say", so no check possible.
  if (ota_cmd_version != 0 && ota_cmd_version < otaCurrentVersion()) {
    otaReject(OTA_ERR_DOWNGRADE); return;
  }
  if (ESP.getMaxAllocHeap() < OTA_MIN_MAXBLOCK) { otaReject(OTA_ERR_LOWHEAP);   return; }

  const esp_partition_t* tgt = esp_ota_get_next_update_partition(NULL);
  if (!tgt)                                     { otaReject(OTA_ERR_NOPARTITION); return; }
  if (ota_cmd_size > tgt->size)                 { otaReject(OTA_ERR_BADSIZE);     return; }

  // Take the holds BEFORE the blocking erase: esp_ota_begin() erases the staging area and blocks for
  // seconds, and we must not be deep-slept or polled in the middle of it.
  g_ota_active = true;
  ota_state = OTA_S_RECEIVING;
  ota_part = tgt;
  ota_img_size = ota_cmd_size;
  ota_img_version = ota_cmd_version;
  ota_img_has_sha = ota_cmd_has_sha;
  if (ota_cmd_has_sha) memcpy(ota_img_sha, ota_cmd_sha, 32);
  ota_img_has_sig = ota_cmd_has_sig;
  if (ota_cmd_has_sig) memcpy(ota_img_sig, ota_cmd_sig, OTA_SIG_LEN);
  ota_bytes_written = 0;
  ota_total_sectors = (uint16_t)((ota_img_size + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE);
  ota_cur_sector = 0;
  ota_next_packet = 0;
  ota_sector_off = 0;
  ota_sector_err = false;
  ota_sector_ready = false;
  ota_progress_decile = 0;
  ota_weak_rail_strikes = 0;
  ota_session_start_ms = ota_last_rx_ms = millis();

#if ENABLE_LOGGING
  logEvent("OTA START v%lu %lu B -> %s (%u sectors) free=%lu maxblock=%lu",
    (unsigned long)ota_img_version, (unsigned long)ota_img_size, ota_part->label,
    (unsigned)ota_total_sectors, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
#endif
  Serial.printf("OTA: staging %lu B to %s\n", (unsigned long)ota_img_size, ota_part->label);

  esp_task_wdt_reset();
  // Sequential-writes mode: erase lazily, one block at a time inside each esp_ota_write, instead of
  // bulk-erasing the image range here. The bulk erase (~1.7MB, several seconds of blocked flash cache)
  // starved the BLE link past its supervision timeout - the phone dropped the connection (0x08) during
  // "staging" on every FRESH start, before ARMED was ever sent (resumes were immune: no erase). Per-block
  // erases stay far under the supervision window and begin() returns immediately, so ARMED goes out right
  // away. The mode's one requirement - strictly sequential writes - is exactly what the stop-and-wait
  // sector protocol already guarantees, and the size-vs-slot fit is still enforced by the BADSIZE gate
  // above (esp_ota_begin no longer knows the size in this mode).
  esp_err_t e = esp_ota_begin(ota_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  esp_task_wdt_reset();
  if (e != ESP_OK) {
    ota_handle = 0;
    otaReset(OTA_PH_REJECTED, OTA_ERR_BEGINFAIL);
    return;
  }
  otaSendStatus(OTA_PH_ARMED, 0, 0);   // detail 0 = start from sector 0
}

/* Write one completed sector and ACK it. This is the only place firmware bytes reach flash. */
static void otaWriteReadySector() {
  esp_task_wdt_reset();
  esp_err_t e = esp_ota_write(ota_handle, ota_sector_buf, ota_sector_len);
  if (e != ESP_OK) {
    otaReset(OTA_PH_ERROR, OTA_ERR_WRITEFAIL);
    return;
  }
  ota_bytes_written += ota_sector_len;
  bool was_last = ota_sector_is_last;
  uint16_t acked = ota_cur_sector;

  ota_cur_sector++;
  ota_next_packet = 0;
  ota_sector_off = 0;
  ota_sector_ready = false;   // clear BEFORE the ACK: the client may send the next sector immediately after
  otaSendAck(acked, OTA_ST_OK);

#if ENABLE_LOGGING
  // Milestone logging only. One flash log line per sector would be ~420 LittleFS writes competing with
  // esp_ota_write on the same flash chip (and could trigger a 256KB segment rotation mid-transfer).
  if (ota_img_size) {
    uint8_t decile = (uint8_t)((uint64_t)ota_bytes_written * 10ULL / ota_img_size);
    if (decile > ota_progress_decile) {
      ota_progress_decile = decile;
      logEvent("OTA %u%% (%lu/%lu B, sector %u/%u)", (unsigned)(decile * 10),
        (unsigned long)ota_bytes_written, (unsigned long)ota_img_size,
        (unsigned)ota_cur_sector, (unsigned)ota_total_sectors);
    }
  }
#endif
  (void)was_last;   // the app confirms completion with END; we never commit on our own initiative
}

/* Liveness + safety guards. Every path here ends in a clean release of all holds. */
static void otaCheckGuards() {
  unsigned long now = millis();

  // BLE dropped mid-transfer: keep the session (and the erased/written staging) alive briefly so a
  // reconnect can resume from the anchor instead of restarting the whole upload.
  if (!ble.connected()) {
    if (ota_state == OTA_S_RECEIVING) {
      ota_state = OTA_S_RESUMEWAIT;
      ota_disconnect_ms = now;
#if ENABLE_LOGGING
      logEvent("OTA link dropped at sector %u/%u - holding %lus for resume",
        (unsigned)ota_cur_sector, (unsigned)ota_total_sectors, OTA_RESUME_GRACE_MS / 1000UL);
#endif
    }
    if (now - ota_disconnect_ms > OTA_RESUME_GRACE_MS) otaReset(OTA_PH_ABORTED, OTA_ERR_LINKLOST);
    return;   // the inactivity timer is meaningless while there is no link
  }
  if (ota_state == OTA_S_RESUMEWAIT) {
    // Reconnected but the app hasn't re-sent START yet; still bounded by the same grace window.
    if (now - ota_disconnect_ms > OTA_RESUME_GRACE_MS) otaReset(OTA_PH_ABORTED, OTA_ERR_LINKLOST);
    return;
  }

  if (now - ota_last_rx_ms > OTA_INACTIVITY_MS)          { otaReset(OTA_PH_ABORTED, OTA_ERR_INACTIVITY); return; }
  if (now - ota_session_start_ms > OTA_SESSION_MAX_MS)   { otaReset(OTA_PH_ABORTED, OTA_ERR_SESSIONCAP); return; }

  // Power, rate-limited and with hysteresis so a transient dip can't kill a multi-minute transfer.
  if (now - ota_last_power_check_ms > 1000UL) {
    ota_last_power_check_ms = now;
    if (!otaPowerOk()) {
      if (++ota_weak_rail_strikes >= 5) { otaReset(OTA_PH_ABORTED, OTA_ERR_WEAKPOWER); return; }
    } else {
      ota_weak_rail_strikes = 0;
    }
  }
}

/*
  The OTA service. Called from loop() BEFORE the early returns and before the poll block, so a session is
  serviced on every iteration. Cheap no-op when idle (one flag test).
*/
void otaService() {
  if (ota_nack_pending) {
    ota_nack_pending = false;
    otaSendAck(ota_nack_sector, ota_nack_status);
  }

  if (ota_cmd_present) {
    uint16_t op = ota_cmd_opcode;
    ota_cmd_present = false;
    switch (op) {
    case OTA_CMD_START: otaHandleStart(); break;
    case OTA_CMD_END:
      if (g_ota_active && ota_handle) otaFinalize();     // never returns on success (reboots)
      else otaSendStatus(OTA_PH_ERROR, OTA_ERR_INTERNAL, 0);
      break;
    case OTA_CMD_ABORT:
      if (g_ota_active) otaReset(OTA_PH_ABORTED, OTA_ERR_INTERNAL);
      else otaSendStatus(OTA_PH_IDLE, 0, otaCurrentVersion());
      break;
    case OTA_CMD_INFO:
      otaSendStatus(ota_rolled_back ? OTA_PH_ROLLEDBACK : (g_ota_active ? OTA_PH_ARMED : OTA_PH_IDLE),
        (uint16_t)ota_cur_sector, otaCurrentVersion());
      break;
    default: break;
    }
  }

  if (!g_ota_active) return;

  if (ota_sector_ready && ota_handle) otaWriteReadySector();
  otaCheckGuards();
}

/* ---------------------------------------------------------------------------------------------------
   Trial boot / rollback handling
   --------------------------------------------------------------------------------------------------- */

/*
  Is this boot an unconfirmed trial boot? Cheap and callable at the very top of setup(), before Serial - used
  to skip the cold-boot-only 2 s serial-attach delay. (It no longer spares the clock: a trial boot zeroes it
  like a cold boot, because the time esp_restart() leaves behind was seen ~28 years off - #13.) Cached, because
  the answer changes once we mark the image valid.
*/
bool otaIsTrialBoot() {
  static int8_t cached = -1;
  if (cached < 0) {
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    cached = (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
              st == ESP_OTA_IMG_PENDING_VERIFY) ? 1 : 0;
  }
  return cached == 1;
}

/*
  Self-test for the trial boot. Deliberately checks only that CORE HARDWARE INITIALISED - never connectivity.
  Requiring a network registration or a paired phone would roll back a perfectly good update just because the
  car was parked in a garage. The dominant protection against genuinely broken firmware is the bootloader
  itself: a trial boot that crashes, panics or hangs never reaches mark-valid, so it is reverted automatically.
  This adds the "boots, but its hardware is dead" case on top of that.
*/
static bool otaSelfTest() {
  bool ok = true;
#if ENABLE_LOGGING
  if (LittleFS.totalBytes() == 0) ok = false;    // filesystem did not mount
#endif
#if ENABLE_MEMS
  if (mems == 0) ok = false;                     // motion sensor absent -> sleep/wake logic would be broken
#endif
#if ENABLE_NETWORK
  if (!gzipSelfTest()) ok = false;               // misbuilt compressor -> every upload body would fail
#endif
  return ok;
}

/*
  Resolve the pending-verify state. MUST be called early in setup() - after the log/FS, sensor and OBD init
  it inspects, but BEFORE the ATRV gate, because the gate can deep-sleep this boot (weak rail) and a reset
  without mark-valid makes the bootloader discard the new firmware. Also detects a completed rollback so the
  app can be told the update was reverted.
*/
void otaHandlePendingVerify() {
  uint32_t attempt = otaNvsGet("attempt", 0);

  if (otaIsTrialBoot()) {
    bool ok = otaSelfTest();
    if (ok) {
      esp_ota_mark_app_valid_cancel_rollback();
      otaNvsPut("current", (uint32_t)OTA_FW_VERSION);
      otaNvsPut("attempt", 0);
      g_wake_reason = "OTA_BOOT";
      Serial.printf("OTA trial boot: self-test OK, firmware v%d marked valid\n", OTA_FW_VERSION);
#if ENABLE_LOGGING
      logEvent("OTA trial boot OK -> marked valid (fw=%d, attempted=%lu)",
        OTA_FW_VERSION, (unsigned long)attempt);
#endif
      // A mismatch here means the RELEASE is wrong, not the dongle: the booted image was built with a
      // different OTA_FW_VERSION than the manifest/START declared, i.e. the repo binary is a stale
      // build signed under a new version number. Loud on purpose - from the app's side it looks like
      // a silent rollback ("installed v5, dongle reports v4"), which has misled debugging twice.
      if (attempt != 0 && attempt != (uint32_t)OTA_FW_VERSION) {
        Serial.printf("OTA WARNING: manifest declared v%lu but this image was built as v%d - stale release binary?\n",
          (unsigned long)attempt, OTA_FW_VERSION);
#if ENABLE_LOGGING
        logEvent("OTA VERSION MISMATCH: declared v%lu, image built as v%d - release binary is a stale build",
          (unsigned long)attempt, OTA_FW_VERSION);
#endif
      }
    } else {
      Serial.printf("OTA trial boot: self-test FAILED -> rolling back\n");
#if ENABLE_LOGGING
      logEvent("OTA trial boot self-test FAILED -> rollback to previous firmware");
      datalogger.end();
#endif
      Serial.flush();
      delay(200);
      esp_ota_mark_app_invalid_rollback_and_reboot();   // never returns
    }
    return;
  }

  // Not a trial boot, but an attempt was recorded: the bootloader reverted it (the trial boot crashed,
  // hung, or failed its self-test). Surface that to the app instead of silently pretending nothing happened.
  if (attempt != 0) {
    ota_rolled_back = true;
    otaNvsPut("attempt", 0);
    Serial.printf("OTA: previous update to v%lu was ROLLED BACK (running v%d)\n",
      (unsigned long)attempt, OTA_FW_VERSION);
#if ENABLE_LOGGING
    logEvent("OTA ROLLED BACK: attempted v%lu did not confirm; running v%d",
      (unsigned long)attempt, OTA_FW_VERSION);
#endif
  }
}

/* Readable ABD5 payload so the app can see what's installed without starting a transfer. Refreshed when
   idle; during a transfer a read returns the latest ACK frame instead (a notify carries the value). */
void otaPublishInfo() {
  char buf[224];
  // fw = the RUNNING image's version - what "Installed on dongle" must mean. The anti-downgrade
  // floor (max of every version validly booted, NVS-persisted) is a different fact and is published
  // separately as "floor": after a bench serial-flash to a LOWER version the two legitimately
  // diverge, and reporting the floor as "installed" showed a version that was not running at all.
  // "alarm" = the RAW stored OBD-alarm flag (-1 never written / 0 / 1), so the app's save-screen
  // toggle initialises from the dongle's stored value instead of silently flipping a deliberate
  // "No" back to the protective default (IGNITION_SENSE_ARCHITECTURE.md §10.3/§10.4).
  snprintf(buf, sizeof(buf),
    "{\"fw\":%lu,\"floor\":%lu,\"build\":\"%s\",\"signed\":%d,\"state\":\"%s\",\"rolled_back\":%d,\"sector\":%u,\"sectors\":%u,\"alarm\":%d}",
    (unsigned long)OTA_FW_VERSION, (unsigned long)otaCurrentVersion(), FW_BUILD_TAG,
    OTA_REQUIRE_SIGNATURE ? 1 : 0,
    ota_state == OTA_S_IDLE ? "idle" : ota_state == OTA_S_RECEIVING ? "receiving" : "resume_wait",
    ota_rolled_back ? 1 : 0, (unsigned)ota_cur_sector, (unsigned)ota_total_sectors,
#if OBD_ALARM_GATE
    g_alarm_flag
#else
    0
#endif
  );
  ble.setOtaInfo(std::string(buf));
}

/* Bench/serial-only escape hatch: clear the persisted anti-downgrade floor (+ stale attempt marker)
   so a freshly serial-flashed lower OTA_FW_VERSION reports and gates from a clean slate. Deliberately
   reachable ONLY from the console (physical access) - never over BLE - so it cannot be used to weaken
   the OTA anti-rollback from a phone. */
void otaBenchReset() {
  if (g_ota_active) {
    Serial.printf("OTARESET refused: firmware update in progress\n");
    return;
  }
  otaNvsPut("current", 0);
  otaNvsPut("attempt", 0);
  ota_rolled_back = false;
  Serial.printf("OTA bench reset: version floor cleared, running fw v%d is now authoritative\n",
    (int)OTA_FW_VERSION);
  otaPublishInfo();
}

#else  /* !ENABLE_OTA - no-op stubs so the call sites in logger.ino need no #if guards */

void otaService() {}
bool otaIsTrialBoot() { return false; }
void otaHandlePendingVerify() {}
void otaPublishInfo() {}
void otaBenchReset() {}

#endif
