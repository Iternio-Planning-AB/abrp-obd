#pragma once
/*
  gzip compression for the telemetry POST body (Content-Encoding: gzip).

  Why this exists: the SIM7070's HTTP session caps a request body at AT+SHCONF BODYLEN=1024 - its hard
  maximum, not a tunable - so the number of telemetry points that fit in one POST is set by body BYTES,
  not by anything we choose. Telemetry JSON repeats every key name in every point, which is exactly what
  deflate eats: measured on realistic varying samples, 1000 bytes holds 5 points raw and 21 gzipped. That
  is the difference between one POST per minute of driving and five.

  Deflate is zlib (libraries/zlib), compiled with -mfix-esp32-psram-cache-issue and with its ~29 KB of
  state in PSRAM. Not the boot-ROM miniz: ROM tdefl needs ~6.5 KB of stack, which overflowed the 8 KB loop
  task on the pre-sleep drain, and ROM code has no PSRAM cache workaround (issue #21). The libc memcpy zlib
  calls needs it too: linked without it (tools/pio_psram_libs.py), bodies over ~5 KB, whose buffers are
  past the 4 KB malloc threshold and so in PSRAM, came back with header byte 9 zeroed.

  Every member is inflated again and compared with its input before it is handed back (well under a
  millisecond), so a corrupt member costs a retry instead of a server 403.

  zlib gives raw deflate; the 10-byte header and the CRC32 + ISIZE trailer are written here. gzip
  specifically, not deflate: gzip is what the v2 endpoint was verified to accept, and
  "Content-Encoding: deflate" is ambiguous in the wild (raw vs zlib) in a way that is not worth
  discovering on a cellular link.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

#include <esp_heap_caps.h>
#define NO_GZIP // must match libraries/zlib/library.json
#include <zlib.h>

static_assert(ZLIB_VERNUM == 0x1320, "libraries/zlib is 1.3.2: re-verify SHA256SUMS and this file on upgrade");
static_assert(GZIP_FAILS_PER_WAKE <= 3, "verify-failure lines are bounded by the failure budget");

enum GzipOutcome : uint8_t {
  GZIP_OK,          // out holds the verified gzip member
  GZIP_UNAVAILABLE, // no compressor this wake (PSRAM, zlib mismatch): plain is the only format
  GZIP_OFF,         // switched off for the rest of this wake: plain only where it fits, otherwise keep frames
  GZIP_FAIL,        // this body failed: send neither it nor the plain body it was sized against
};

static const char GZIP_HEADER[10] = { '\x1f', '\x8b', 8, 0, 0, 0, 0, 0, 0, '\xff' };
static const uint32_t GZIP_BREADCRUMB = 0x475a4950;

// Set while deflate or inflate runs; still set at the next init means a reset inside the compressor. NOINIT:
// it survives a panic and deep sleep, so a crash wake that never uploads passes it on to the next one.
RTC_NOINIT_ATTR uint32_t g_gzip_in_deflate;

struct GzipState {
  z_stream deflater, inflater;
  bool tried, ready, off;
  size_t psram_bytes;
  unsigned calls, failures;
  unsigned long us_max;
  unsigned big;   // bodies with a buffer over the malloc threshold: in PSRAM, where a store can be lost
  size_t raw_max;
};
static GzipState g_gzip;

static voidpf gzipAlloc(voidpf, uInt items, uInt size) {
  void* p = heap_caps_malloc((size_t)items * size, MALLOC_CAP_SPIRAM);
  if (p) g_gzip.psram_bytes += (size_t)items * size;
  return p;
}
static void gzipFree(voidpf, voidpf p) { heap_caps_free(p); }

static void gzipOff(const char* why) {
  if (g_gzip.off) return;
  g_gzip.off = true;
#if ENABLE_LOGGING
  logEvent("GZIP off: %s", why);
#endif
}

// A gzip-category failure: a body that failed verification, or a server rejecting a gzip body.
void gzipNoteFailure() {
  if (++g_gzip.failures < GZIP_FAILS_PER_WAKE) return;
  char why[40];
  snprintf(why, sizeof(why), "%u gzip failures this wake", g_gzip.failures);
  gzipOff(why);
}

static bool gzipUnavailable(const char* why) {
#if ENABLE_LOGGING
  logEvent("GZIP unavailable (%s) - uncompressed bodies", why);
#endif
  Serial.printf("gzip: unavailable (%s) - sending bodies uncompressed\n", why);
  return false;
}

static bool gzipInit() {
  if (g_gzip.tried) return g_gzip.ready;
  g_gzip.tried = true;
  if (g_gzip_in_deflate == GZIP_BREADCRUMB) gzipOff("reset inside compressor on an earlier boot");
  g_gzip_in_deflate = 0;

  if (strcmp(zlibVersion(), ZLIB_VERSION) != 0) return gzipUnavailable("zlib version");
  for (z_stream* s : { &g_gzip.deflater, &g_gzip.inflater }) {
    s->zalloc = gzipAlloc;
    s->zfree = gzipFree;
    s->opaque = Z_NULL;
  }
  if (deflateInit2(&g_gzip.deflater, GZIP_LEVEL, Z_DEFLATED, -GZIP_WINDOW_BITS, GZIP_MEM_LEVEL,
        Z_DEFAULT_STRATEGY) != Z_OK) return gzipUnavailable("PSRAM deflate");
  if (inflateInit2(&g_gzip.inflater, -GZIP_WINDOW_BITS) != Z_OK) {
    deflateEnd(&g_gzip.deflater);
    return gzipUnavailable("PSRAM inflate");
  }
  g_gzip.ready = true;
#if ENABLE_LOGGING
  logEvent("GZIP ready zlib=%s psram=%uB", zlibVersion(), (unsigned)g_gzip.psram_bytes);
#endif
  return true;
}

// The server's check (inflates cleanly, CRC32 and ISIZE match) plus a byte compare with the input.
// Returns nullptr when the member is good, else the cause; out of memory is reported through no_memory.
static const char* gzipVerify(const std::string& in, const std::string& wire, char* why, size_t why_len,
                              bool& no_memory) {
  no_memory = false;
  if (wire.size() < 18 || memcmp(wire.data(), GZIP_HEADER, sizeof(GZIP_HEADER)) != 0) return "header";
  uint8_t* plain = (uint8_t*)heap_caps_malloc(in.size() + 1, MALLOC_CAP_SPIRAM);
  if (!plain) { no_memory = true; return "memory"; }
  z_stream& s = g_gzip.inflater;
  inflateReset(&s);
  s.next_in = (Bytef*)wire.data() + sizeof(GZIP_HEADER);
  s.avail_in = (uInt)(wire.size() - 18);
  s.next_out = plain;
  s.avail_out = (uInt)(in.size() + 1);
  const int rc = inflate(&s, Z_FINISH);
  const char* bad = nullptr;
  if (rc == Z_MEM_ERROR) { no_memory = true; bad = "memory"; }
  else if (rc != Z_STREAM_END) { snprintf(why, why_len, "z%d", rc); bad = why; }
  else if (s.total_out != in.size() || memcmp(plain, in.data(), in.size()) != 0) bad = "cmp";
  heap_caps_free(plain);
  if (bad) return bad;
  const uint8_t* t = (const uint8_t*)wire.data() + wire.size() - 8;
  const uint32_t crc = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
  const uint32_t isize = t[4] | t[5] << 8 | t[6] << 16 | (uint32_t)t[7] << 24;
  // avail_in != 0: the deflate stream ended early, so the server would read the trailer from the wrong place
  if (s.avail_in != 0 || crc != crc32(0, (const Bytef*)in.data(), (uInt)in.size()) || isize != (uint32_t)in.size())
    return "trailer";
  return nullptr;
}

// Raw deflate of `in`, framed as a gzip member in `wire`. Returns zlib's code: Z_STREAM_END is a member.
static int gzipDeflate(const std::string& in, std::string& wire) {
  z_stream& d = g_gzip.deflater;
  deflateReset(&d);
  const uLong bound = deflateBound(&d, (uLong)in.size());
  wire.assign(sizeof(GZIP_HEADER) + bound, '\0');
  memcpy(&wire[0], GZIP_HEADER, sizeof(GZIP_HEADER));
  d.next_in = (Bytef*)in.data();
  d.avail_in = (uInt)in.size();
  d.next_out = (Bytef*)&wire[sizeof(GZIP_HEADER)];
  d.avail_out = (uInt)bound;
  const int rc = deflate(&d, Z_FINISH);
  if (rc != Z_STREAM_END) return rc;
  wire.resize(sizeof(GZIP_HEADER) + d.total_out);
  const uint32_t crc = crc32(0, (const Bytef*)in.data(), (uInt)in.size());
  const uint32_t isize = (uint32_t)in.size(); // gzip stores the uncompressed length mod 2^32
  for (int i = 0; i < 4; i++) wire.push_back((char)((crc >> (8 * i)) & 0xff));   // CRC32, little-endian
  for (int i = 0; i < 4; i++) wire.push_back((char)((isize >> (8 * i)) & 0xff)); // ISIZE, little-endian
  return rc;
}

// A refused member's header bytes and address (0x3f8.. to 0x3fb.. is PSRAM): how the lost store in PR #33
// showed itself, header byte 9 zeroed in PSRAM and never in internal RAM. The header only: past it is body.
static void gzipDescribeRefusal(const std::string& wire, char* text, size_t len) {
  char head[2 * sizeof(GZIP_HEADER) + 1] = {};
  for (size_t i = 0; i < sizeof(GZIP_HEADER) && i < wire.size(); ++i)
    snprintf(head + 2 * i, 3, "%02x", (uint8_t)wire[i]);
  snprintf(text, len, "bytes=%s at=%08lx", head, (unsigned long)(uintptr_t)wire.data());
}

// Deflate, frame and verify `in` into `wire` under the breadcrumb. nullptr when verified, else the cause:
// "deflate" with zlib's code in `code`, "memory" with no_memory set, or the verifier's refusal.
static const char* gzipMember(const std::string& in, std::string& wire, char* code, size_t code_len, bool& no_memory) {
  no_memory = false;
  const char* why;
  g_gzip_in_deflate = GZIP_BREADCRUMB;
  const int rc = gzipDeflate(in, wire);
  if (rc != Z_STREAM_END) {
    snprintf(code, code_len, "%d", rc);
    why = "deflate";
  } else {
#ifdef GZIP_TEST_TAMPER
    GZIP_TEST_TAMPER(wire);
#endif
    why = gzipVerify(in, wire, code, code_len, no_memory);
  }
  g_gzip_in_deflate = 0;
  return why;
}

/*
  Compress `in` into a complete, verified gzip member in `out`; `out` is untouched unless this returns
  GZIP_OK. Deliberately NOT bounded by a "did it get smaller" check: the caller measures the body against
  the modem's cap, and a body that grew is caught there.
*/
inline GzipOutcome gzipBody(const std::string& in, std::string& out) {
  if (!gzipInit()) return GZIP_UNAVAILABLE;
  if (g_gzip.off) return GZIP_OFF;
  const unsigned long t0 = micros();
  std::string wire;
  char code[12];
  bool no_memory;
  const char* why = gzipMember(in, wire, code, sizeof(code), no_memory);
  ++g_gzip.calls;
  // malloc puts a buffer in PSRAM from CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes up; a string's is capacity + 1
  const size_t psram_from = CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
  if (in.capacity() + 1 >= psram_from || wire.capacity() + 1 >= psram_from) ++g_gzip.big;
  if (in.size() > g_gzip.raw_max) g_gzip.raw_max = in.size();
  if (no_memory) {
    // Not corruption, but nothing to vouch for the member: keep frames rather than drop any as oversized.
    gzipOff("no PSRAM to verify");
    return GZIP_OFF;
  }
  if (why) {
#if ENABLE_LOGGING
    if (!strcmp(why, "deflate")) logEvent("GZIP deflate fail rc=%s raw=%uB", code, (unsigned)in.size());
    else {
      char refusal[40];
      gzipDescribeRefusal(wire, refusal, sizeof(refusal));
      logEvent("GZIP verify fail raw=%uB wire=%uB why=%s %s", (unsigned)in.size(), (unsigned)wire.size(), why, refusal);
    }
#endif
    gzipNoteFailure();
    return GZIP_FAIL;
  }
  const unsigned long us = micros() - t0;
  if (us > g_gzip.us_max) g_gzip.us_max = us;
  out.swap(wire);
  return GZIP_OK;
}

// Telemetry-shaped JSON of exactly `bytes` bytes, varying with `seed`; synthetic values, no real data.
static std::string gzipSyntheticBody(size_t bytes, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 0x9e3779b9u;
  auto next = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
  std::string b = "[";
  unsigned soc = 500 + next() % 400, odo = 10000000 + next() % 80000000;
  for (unsigned i = 0; b.size() < bytes; ++i) {
    const unsigned t = (seed * 60 + i * 5) % 86400;
    char ts[24], point[640];
    snprintf(ts, sizeof(ts), "2026-10-22T%02u:%02u:%02uZ", t / 3600, t / 60 % 60, t % 60);
    soc -= next() % 2;
    odo += 50 + next() % 120;
    snprintf(point, sizeof(point),
      "%s{\"provider\":\"ABRP_OTA\",\"soc\":{\"time\":\"%s\",\"frac\":0.%03u},\"soh\":{\"time\":\"%s\",\"frac\":0.97},"
      "\"power\":{\"time\":\"%s\",\"w\":%d},\"voltage\":{\"time\":\"%s\",\"v\":%u.%u},"
      "\"current\":{\"time\":\"%s\",\"a\":%d.%u},\"batteryTemperature\":{\"time\":\"%s\",\"c\":22},"
      "\"externalTemperature\":{\"time\":\"%s\",\"c\":11},\"odometer\":{\"time\":\"%s\",\"m\":%u},"
      "\"speed\":{\"time\":\"%s\",\"ms\":%u.%02u},\"drivingState\":{\"time\":\"%s\",\"state\":\"DRIVE\"}}",
      i ? "," : "", ts, soc, ts, ts, (int)(next() % 100000) - 40000, ts, 370 + next() % 6, next() % 10, ts,
      (int)(next() % 260) - 100, next() % 10, ts, ts, ts, odo, ts, 8 + next() % 26, next() % 100, ts);
    b += point;
  }
  b.resize(bytes);
  return b;
}

/*
  OTA trial-boot check: a misbuilt compressor rolls the image back before it is marked valid. Only wrong
  output or a zlib mismatch fails it; no PSRAM passes, because uploads already go plain without it. A
  passing round trip also ends a plain wake left by a crash in the previous image's compressor.
  16 KB: well past the 4 KB malloc threshold, so the body and member are in PSRAM. A runtime linked without
  the PSRAM workaround failed 300 of 300 bodies of 12 KB on the bench, and none under 4 KB. Kept out of the
  wake summary, which counts upload bodies.
*/
bool gzipSelfTest() {
  if (strcmp(zlibVersion(), ZLIB_VERSION) != 0) return false;
  const std::string body = gzipSyntheticBody(16384, 1);
  if (!gzipInit()) return true;
  g_gzip.off = false;
  const unsigned calls = g_gzip.calls, big = g_gzip.big;
  const size_t raw_max = g_gzip.raw_max;
  const unsigned long us_max = g_gzip.us_max;
  std::string wire;
  const GzipOutcome outcome = gzipBody(body, wire);
  g_gzip.calls = calls;
  g_gzip.big = big;
  g_gzip.raw_max = raw_max;
  g_gzip.us_max = us_max;
  return outcome != GZIP_FAIL && (outcome != GZIP_OK || wire.size() < body.size() / 2);
}

/*
  Serial console GZTEST <bytes> [count]: the production deflate and verifier on `count` synthetic bodies,
  outside the failure budget and the flash log; prints pass/fail by cause and the first refusals. It holds
  the loop task, so BLE requests and sleep wait: it stops after 30 s or at any key.
*/
void gzipBenchTest(const char* args) {
  unsigned bytes = 0, count = 100;
  if (sscanf(args, "%u %u", &bytes, &count) < 1 || bytes < 2 || bytes > 32768 || count < 1 || count > 2000) {
    Serial.printf("usage: GZTEST <bytes 2..32768> [count 1..2000]\n");
    return;
  }
  if (!gzipInit()) { Serial.printf("GZTEST: no compressor\n"); return; }
  unsigned run = 0, pass = 0, header = 0, data = 0, trailer = 0, other = 0;
  unsigned long wire_bytes = 0, us_max = 0;
  const uint32_t seed0 = esp_random();
  const unsigned long t_start = millis();
  bool stopped = false;
  for (; run < count && !stopped; ++run) {
    esp_task_wdt_reset();
    const std::string body = gzipSyntheticBody(bytes, seed0 + run);
    std::string wire;
    char code[12];
    bool no_memory;
    const unsigned long t0 = micros();
    const char* why = gzipMember(body, wire, code, sizeof(code), no_memory);
    const unsigned long us = micros() - t0;
    if (us > us_max) us_max = us;
    stopped = Serial.available() > 0 || millis() - t_start > 30000;
    if (!why) { ++pass; wire_bytes += wire.size(); continue; }
    if (!strcmp(why, "header")) ++header;
    else if (!strcmp(why, "trailer")) ++trailer;
    else if (!strcmp(why, "cmp") || why[0] == 'z') ++data;
    else ++other;
    if (run - pass < 5) {
      char refusal[40];
      gzipDescribeRefusal(wire, refusal, sizeof(refusal));
      Serial.printf("GZTEST #%u why=%s %s\n", run, why, refusal);
    }
  }
  while (Serial.available() > 0) Serial.read(); // the key that stopped it is not a command
  Serial.printf("GZTEST bytes=%u n=%u pass=%u fail=%u (header=%u data=%u trailer=%u other=%u) wire_avg=%lu "
    "us_max=%lu ms=%lu%s\n", bytes, run, pass, run - pass, header, data, trailer, other,
    pass ? wire_bytes / pass : 0, us_max, millis() - t_start, stopped ? " stopped" : "");
}

#if ENABLE_LOGGING
// Before the pre-sleep backlog save. stk_min is the loop task's lowest free stack this boot, in bytes.
void gzipLogWakeSummary() {
  if (!g_gzip.calls) return;
  logEvent("GZIP n=%u big=%u fail=%u raw_max=%uB us_max=%lu stk_min=%u", g_gzip.calls, g_gzip.big,
    g_gzip.failures, (unsigned)g_gzip.raw_max, g_gzip.us_max, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#endif
