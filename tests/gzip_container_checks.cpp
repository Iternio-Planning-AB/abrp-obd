#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

void check(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// PSRAM: every compressor allocation must ask for it, and the test can make the Nth one fail.
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096 // the Arduino core's sdkconfig: larger mallocs go to PSRAM
unsigned allocations = 0, fail_allocation = 0;
size_t largest_allocation = 0;
std::map<void*, size_t> live;
void* heap_caps_malloc(size_t size, unsigned caps) {
  check(caps == MALLOC_CAP_SPIRAM, "compressor memory is PSRAM-only");
  if (++allocations == fail_allocation) return nullptr;
  if (size > largest_allocation) largest_allocation = size;
  void* p = std::malloc(size ? size : 1);
  live[p] = size;
  return p;
}
void heap_caps_free(void* p) {
  if (!p) return;
  check(live.erase(p) == 1, "freed pointer came from heap_caps_malloc");
  std::free(p);
}

#define RTC_NOINIT_ATTR
unsigned long fake_micros = 0;
unsigned long micros() { return fake_micros += 1500; }
unsigned long millis() { return fake_micros / 1000; }
unsigned uxTaskGetStackHighWaterMark(void*) { return 5432; }
uint32_t esp_random() { return 12345; }
unsigned wdt_feeds = 0;
void esp_task_wdt_reset() { ++wdt_feeds; }
struct SerialStub {
  std::string out;
  int pending = 0; // bytes typed at the console
  int available() { return pending; }
  int read() { return pending > 0 ? (--pending, 'x') : -1; }
  void printf(const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    out += text;
  }
} Serial;

std::vector<std::string> events;
#define ENABLE_LOGGING 1
void logEvent(const char* fmt, ...) {
  char message[120];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  check(n > 0 && n < (int)sizeof(message), "GZIP event fits the 120-char log message");
  events.emplace_back(message);
}

std::string tamper_mode;
void tamper(std::string& wire) {
  if (tamper_mode == "data") wire[10] |= 0x06; // reserved block type: inflate refuses it
  else if (tamper_mode == "stored") wire[10 + (wire.size() - 18) / 2] ^= 0x10; // stored bytes inflate cleanly
  else if (tamper_mode == "crc") wire[wire.size() - 8] ^= 0x01;
  else if (tamper_mode == "isize") wire[wire.size() - 1] ^= 0x01;
  else if (tamper_mode == "header") wire[9] = 0; // the PSRAM store the mislinked runtime lost (PR #33)
}
#define GZIP_TEST_TAMPER(wire) tamper(wire)
#include "../baseLogger/config.h"
#include "gzip_body.h"

std::string unhex(const std::string& encoded) {
  check(encoded.size() % 2 == 0, "fixture hex width");
  std::string decoded;
  for (size_t i = 0; i < encoded.size(); i += 2) decoded.push_back((char)std::stoi(encoded.substr(i, 2), nullptr, 16));
  return decoded;
}
std::string hex(const std::string& bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) { out.push_back(digits[c >> 4]); out.push_back(digits[c & 15]); }
  return out;
}
bool logged(const std::string& prefix) {
  for (const auto& e : events) if (e.rfind(prefix, 0) == 0) return true;
  return false;
}
std::string body(unsigned points) {
  std::string b = "[";
  for (unsigned i = 0; i < points; ++i)
    b += std::string(i ? "," : "") + "{\"provider\":\"ABRP_OTA\",\"soc\":{\"time\":\"2026-01-01T00:00:0" +
         std::to_string(i % 10) + "Z\",\"frac\":0." + std::to_string(500 + i) + "}}";
  return b + "]";
}

// stdin: one hex input per line; stdout: the wire for each, as hex.
void batch() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::string out;
    check(gzipBody(unhex(line), out) == GZIP_OK, "compression succeeds");
    std::cout << hex(out) << '\n';
  }
  check(!logged("GZIP verify") && !logged("GZIP off"), "clean bodies never fail verification");
}

// A fresh wake: RAM state gone, RTC_NOINIT memory kept.
void boot() {
  if (g_gzip.ready) { deflateEnd(&g_gzip.deflater); inflateEnd(&g_gzip.inflater); }
  g_gzip = GzipState{};
  events.clear();
}

void allocFailure(unsigned which) {
  fail_allocation = which;
  std::string out = "untouched";
  check(gzipBody(body(3), out) == GZIP_UNAVAILABLE && out == "untouched", "failed PSRAM allocation sends plain, out untouched");
  check(live.empty(), "a half-initialised compressor releases everything it allocated");
  const unsigned asked = allocations;
  check(gzipBody(body(3), out) == GZIP_UNAVAILABLE && allocations == asked, "failed PSRAM allocation is not retried this wake");
  check(logged("GZIP unavailable") && !logged("GZIP ready"), "unavailability is logged once");
  check(gzipSelfTest(), "no PSRAM does not roll back an OTA image: uploads go plain without it");
}

// No memory to verify is not corruption, but the wake goes plain-where-it-fits rather than dropping frames.
void verifyMemory() {
  std::string out = "untouched";
  check(gzipBody(body(3), out) == GZIP_OK, "first member");
  fail_allocation = allocations + 1;
  out = "untouched";
  check(gzipBody(body(3), out) == GZIP_OFF && out == "untouched", "verify out of memory turns gzip off, out untouched");
  check(logged("GZIP off: no PSRAM to verify") && g_gzip_in_deflate == 0, "named in the log, breadcrumb cleared");
}

// Each refusal names its own cause; the second in a wake switches compression off.
void tampered() {
  check(gzipInit(), "compressor up");
  largest_allocation = 0; // from here only the verifier's copy of the body
  check(gzipSelfTest(), "OTA self-test round trip passes on a good build");
  // Under the threshold malloc stays internal, where a mislinked runtime corrupts nothing: the trial boot
  // would pass an image that loses PSRAM stores.
  check(largest_allocation > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, "OTA self-test body is PSRAM-sized");
  tamper_mode = "header";
  check(!gzipSelfTest(), "OTA self-test rolls back an image that loses a PSRAM store");
  std::string incompressible;
  for (uint32_t x = 2463534242u, i = 0; i < 2000; ++i) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    incompressible.push_back((char)(x >> 24));
  }
  const struct { const char* mode; std::string raw; const char* why; } cases[] = {
    {"data", body(40), "z-3"}, {"stored", incompressible, "cmp"}, {"crc", body(40), "trailer"}, {"isize", body(40), "trailer"},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (i % 2 == 0) boot();
    const auto& c = cases[i];
    std::string out;
    tamper_mode.clear();
    check(gzipBody(c.raw, out) == GZIP_OK, "baseline compression");
    const std::string good = out;
    if (std::string(c.mode) == "stored")
      check(good.size() > c.raw.size() + 18 && (good[10] & 0x06) == 0, "random bytes deflate as stored blocks");
    tamper_mode = c.mode;
    out = "untouched";
    check(gzipBody(c.raw, out) == GZIP_FAIL && out == "untouched", "a corrupted member is refused, out untouched");
    check(g_gzip_in_deflate == 0, "breadcrumb cleared after a refused member");
    std::string refused = good;
    tamper(refused);
    check(logged(std::string("GZIP verify fail raw=") + std::to_string(c.raw.size()) + "B wire=" +
                 std::to_string(good.size()) + "B why=" + c.why + " bytes=" + hex(refused.substr(0, 10)) + " at="),
          c.mode);
    check(logged("GZIP off: 2 gzip failures this wake") == (i % 2 == 1), "the second failure in a wake switches gzip off");
  }
  tamper_mode.clear();
  std::string out = "untouched";
  check(gzipBody(body(3), out) == GZIP_OFF && out == "untouched", "off for the rest of the wake");
}

void crashBreadcrumb() {
  g_gzip_in_deflate = GZIP_BREADCRUMB;
  boot(); // the crash wake: sleeps without uploading
  boot();
  std::string out;
  check(gzipBody(body(3), out) == GZIP_OFF, "a reset inside the compressor turns the next upload wake plain");
  check(g_gzip_in_deflate == 0 && logged("GZIP off: reset inside compressor on an earlier boot"), "named in the log, consumed");
  boot();
  check(gzipBody(body(3), out) == GZIP_OK, "only one upload wake goes plain");
  g_gzip_in_deflate = GZIP_BREADCRUMB;
  boot();
  check(gzipSelfTest() && gzipBody(body(3), out) == GZIP_OK, "a passing OTA self-test ends the plain wake");
  boot();
  check(gzipBody(body(3), out) == GZIP_OK && !logged("GZIP off"), "no breadcrumb, compression on");
}

void steady() {
  std::string out;
  check(gzipBody(body(12), out) == GZIP_OK, "first compression");
  const unsigned asked = allocations;
  check(logged(std::string("GZIP ready zlib=") + ZLIB_VERSION + " psram="), "readiness logged with zlib version");
  for (unsigned i = 0; i < 50; ++i) check(gzipBody(body(i % 13), out) == GZIP_OK, "repeated compression");
  check(allocations == asked + 50, "state is kept: only each verify's output buffer is allocated per call");
  check(live.size() == 6, "deflate state, window, prev, head and pending stay, and the inflate state; no window for Z_FINISH");
  gzipNoteFailure();
  check(!logged("GZIP off"), "one server gzip rejection does not switch compression off");
  check(gzipBody(body(90), out) == GZIP_OK, "a body over the PSRAM threshold");
  gzipLogWakeSummary();
  check(body(90).size() > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL && body(12).size() < CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
        "fixture sizes straddle the threshold");
  check(events.back() == "GZIP n=52 big=1 fail=1 raw_max=" + std::to_string(body(90).size()) +
        "B us_max=1500 stk_min=5432", "per-wake summary");
}

// GZTEST: the production deflate and verifier on synthetic bodies, outside the failure budget.
void bench() {
  gzipBenchTest(" 9000 3");
  check(Serial.out.find("GZTEST bytes=9000 n=3 pass=3 fail=0 (header=0 data=0 trailer=0 other=0)") != std::string::npos,
        "clean bodies pass");
  check(wdt_feeds >= 3, "the watchdog is fed per body");
  const struct { const char* mode; const char* tally; const char* line; } refusals[] = {
    {"crc", "(header=0 data=0 trailer=3 other=0)", "why=trailer bytes=1f8b08000000000000ff at="},
    {"header", "(header=3 data=0 trailer=0 other=0)", "why=header bytes=1f8b0800000000000000 at="},
    {"data", "(header=0 data=3 trailer=0 other=0)", "why=z-3 bytes=1f8b08000000000000ff at="},
  };
  for (const auto& r : refusals) {
    Serial.out.clear();
    tamper_mode = r.mode;
    gzipBenchTest(" 9000 3");
    check(Serial.out.find(std::string("pass=0 fail=3 ") + r.tally) != std::string::npos, r.mode);
    check(Serial.out.find(r.line) != std::string::npos, "a refusal shows its header bytes");
  }
  check(g_gzip.failures == 0 && !g_gzip.off && events.size() == 1, "no failure budget spent, nothing in the flash log");
  tamper_mode.clear();
  Serial.out.clear();
  Serial.pending = 1;
  gzipBenchTest(" 9000 50");
  check(Serial.out.find("n=1 ") != std::string::npos && Serial.out.find("stopped") != std::string::npos &&
        Serial.pending == 0, "a key stops the loop and is not left as a command");
  Serial.pending = 0;
  for (const char* bad : { "", " 1", " 9000 0", " 9000 2001", " 40000" }) {
    Serial.out.clear();
    gzipBenchTest(bad);
    check(Serial.out.rfind("usage: GZTEST", 0) == 0, bad);
  }
  std::string body = gzipSyntheticBody(9000, 7);
  check(body.size() == 9000 && body.rfind("[{\"provider\":", 0) == 0 && body == gzipSyntheticBody(9000, 7) &&
        body != gzipSyntheticBody(9000, 8), "synthetic bodies: exact size, telemetry-shaped, seeded");
}

int main(int argc, char** argv) {
  check(argc == 2, "mode supplied");
  const std::string mode = argv[1];
  if (mode == "batch") batch();
  else if (mode == "alloc-deflate") allocFailure(1);
  else if (mode == "alloc-inflate") allocFailure(6);
  else if (mode == "alloc-verify") verifyMemory();
  else if (mode == "tamper") tampered();
  else if (mode == "crash") crashBreadcrumb();
  else if (mode == "steady") steady();
  else if (mode == "bench") bench();
  else check(false, "known mode");
  for (const auto& e : events) check(e.size() < 120, "GZIP event under 120 chars");
  if (mode != "batch") std::cout << "PASS " << mode << '\n';
}
