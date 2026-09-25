#pragma once
/*
  gzip compression for the telemetry POST body (Content-Encoding: gzip).

  Why this exists: the SIM7070's HTTP session caps a request body at AT+SHCONF BODYLEN=1024 - its hard
  maximum, not a tunable - so the number of telemetry points that fit in one POST is set by body BYTES,
  not by anything we choose. Telemetry JSON repeats every key name in every point, which is exactly what
  deflate eats: measured on realistic varying samples, 1000 bytes holds 5 points raw and 21 gzipped. That
  is the difference between one POST per minute of driving and five.

  Deflate comes from the ESP32 BOOT ROM (rom/miniz.h), so it costs no flash. The catch is that the ROM
  build has no allocator wired up - tdefl_compress_mem_to_heap() and _to_output() are declared with a
  warning attribute saying so - which is why this uses the streaming API instead: WE own the
  tdefl_compressor and WE own the output buffer, and miniz never allocates.

  That struct is ~300 KB (32 KB dictionary, 64 KB hash, 64 KB next-links, ~83 KB output staging), far too
  much for the ESP32's internal DRAM next to Bluedroid and mbedTLS. It lives in PSRAM, which this board
  has (BOARD_HAS_PSRAM, ESP-WROVER), and is allocated once and kept: a per-POST alloc/free of that size
  would be a fragmentation source in its own right, and the whole point is to be gentler on the heap than
  the payload we are replacing.

  The ROM gives raw deflate or a zlib wrapper; it does not produce a gzip container, so the 10-byte header
  and the CRC32 + ISIZE trailer are written here. gzip specifically, not deflate: gzip is what the v2
  endpoint was verified to accept, and "Content-Encoding: deflate" is ambiguous in the wild (raw vs zlib)
  in a way that is not worth discovering on a cellular link.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

#include <esp_heap_caps.h>
#include "esp32/rom/miniz.h"
#include "esp32/rom/crc.h"

// Compressor state, allocated on first use and never freed. nullptr means PSRAM was unavailable or the
// allocation failed, in which case gzipBody() reports failure and the caller sends the body uncompressed.
static tdefl_compressor* g_gzip_deflator = nullptr;
static bool g_gzip_alloc_failed = false;

// miniz hands compressed bytes here as it produces them; we append to the caller's string.
static mz_bool gzipPutBuf(const void* pBuf, int len, void* pUser) {
  auto* out = static_cast<std::string*>(pUser);
  out->append(static_cast<const char*>(pBuf), (size_t)len);
  return MZ_TRUE;
}

/*
  Compress `in` into a complete gzip member in `out`. Returns false and leaves `out` untouched if the
  compressor is unavailable or deflate fails - callers must fall back to the uncompressed body rather
  than treat this as fatal, since an upload that goes up bigger is strictly better than no upload.

  Deliberately NOT bounded by a "did it get smaller" check: this is only ever called on a body that is
  about to be measured against the modem's cap by the caller, and a body that grew is caught there.
*/
inline bool gzipBody(const std::string& in, std::string& out) {
  if (g_gzip_deflator == nullptr) {
    if (g_gzip_alloc_failed) {
      return false; // asked once, answered once - don't retry a ~300 KB alloc on every POST
    }
    g_gzip_deflator =
      (tdefl_compressor*)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (g_gzip_deflator == nullptr) {
      g_gzip_alloc_failed = true;
      Serial.printf("gzip: %u bytes of PSRAM unavailable - sending bodies uncompressed\n",
        (unsigned)sizeof(tdefl_compressor));
#if ENABLE_LOGGING
      logEvent("GZIP unavailable (%u B PSRAM alloc failed) - uncompressed bodies",
        (unsigned)sizeof(tdefl_compressor));
#endif
      return false;
    }
  }

  std::string deflated;
  // A body that compresses badly still fits comfortably; reserving the input size avoids the repeated
  // reallocation an append-driven grow would otherwise do inside the put-buf callback.
  deflated.reserve(in.size());

  // Raw deflate (no TDEFL_WRITE_ZLIB_HEADER) - the gzip container is written below. 128 probes is
  // miniz's default effort; the bodies here are a few KB, so the CPU cost is microseconds and there is
  // nothing to gain from trading ratio for speed on a link where every byte costs a fifth of a point.
  if (tdefl_init(g_gzip_deflator, gzipPutBuf, &deflated, TDEFL_DEFAULT_MAX_PROBES) != TDEFL_STATUS_OKAY) {
    return false;
  }
  if (tdefl_compress_buffer(g_gzip_deflator, in.data(), in.size(), TDEFL_FINISH) != TDEFL_STATUS_DONE) {
    return false;
  }

  const uint32_t crc = crc32_le(0, (const uint8_t*)in.data(), (uint32_t)in.size());
  const uint32_t isize = (uint32_t)in.size(); // gzip stores the uncompressed length mod 2^32

  out.clear();
  out.reserve(deflated.size() + 18);
  // RFC 1952 header: magic, CM=deflate, no FLGs, no mtime (we have a clock but it tells the server
  // nothing it does not already have from the points themselves), XFL=0, OS=255 "unknown".
  static const char GZIP_HEADER[10] = { '\x1f', '\x8b', 8, 0, 0, 0, 0, 0, 0, '\xff' };
  out.append(GZIP_HEADER, sizeof(GZIP_HEADER));
  out.append(deflated);
  for (int i = 0; i < 4; i++) out.push_back((char)((crc >> (8 * i)) & 0xff));   // CRC32, little-endian
  for (int i = 0; i < 4; i++) out.push_back((char)((isize >> (8 * i)) & 0xff)); // ISIZE, little-endian
  return true;
}
