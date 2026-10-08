#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

void check(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::string expected_input, injected_deflate;
bool alloc_failure = false, init_failure = false, deflate_failure = false;
unsigned allocations = 0;
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
void* heap_caps_malloc(size_t size, unsigned caps) {
  check(caps == MALLOC_CAP_SPIRAM, "compressor allocation remains PSRAM-only");
  ++allocations;
  return alloc_failure ? nullptr : std::malloc(size);
}
using mz_bool = int;
constexpr mz_bool MZ_TRUE = 1;
constexpr int TDEFL_DEFAULT_MAX_PROBES = 128, TDEFL_FINISH = 4;
constexpr int TDEFL_STATUS_OKAY = 0, TDEFL_STATUS_DONE = 1;
struct tdefl_compressor {
  mz_bool (*output)(const void*, int, void*) = nullptr;
  void* user = nullptr;
};
int tdefl_init(tdefl_compressor* compressor, mz_bool (*output)(const void*, int, void*), void* user, int flags) {
  check(flags == TDEFL_DEFAULT_MAX_PROBES, "gzip wrapper asks ROM for raw deflate without zlib header");
  compressor->output = output;
  compressor->user = user;
  return init_failure ? -1 : TDEFL_STATUS_OKAY;
}
int tdefl_compress_buffer(tdefl_compressor* compressor, const void* input, size_t length, int flush) {
  check(std::string(static_cast<const char*>(input), length) == expected_input, "exact binary input reaches deflator");
  check(flush == TDEFL_FINISH, "deflate member is finalized");
  if (deflate_failure) return -1;
  for (size_t begin = 0; begin < injected_deflate.size(); begin += 7) {
    const size_t count = std::min<size_t>(7, injected_deflate.size() - begin);
    check(compressor->output(injected_deflate.data() + begin, count, compressor->user) == MZ_TRUE,
          "production callback accepts fragmented binary deflate");
  }
  return TDEFL_STATUS_DONE;
}
uint32_t crc32_le(uint32_t initial, const uint8_t* input, uint32_t length) {
  uint32_t crc = ~initial;
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= input[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
struct SerialStub { template<typename... Args> void printf(const char*, Args...) {} } Serial;
#define ENABLE_LOGGING 0
#include "gzip_body.h"

std::string unhex(const std::string& encoded) {
  check(encoded.size() % 2 == 0, "fixture hex width");
  const std::string digits = "0123456789abcdef";
  std::string decoded;
  for (size_t i = 0; i < encoded.size(); i += 2) {
    const auto high = digits.find(encoded[i]), low = digits.find(encoded[i + 1]);
    check(high < 16 && low < 16, "fixture hex digits");
    decoded.push_back(static_cast<char>(high * 16 + low));
  }
  return decoded;
}
int main(int argc, char** argv) {
  check(argc >= 3, "input and raw-deflate fixtures supplied");
  expected_input = unhex(argv[1]);
  injected_deflate = unhex(argv[2]);
  const std::string mode = argc > 3 ? argv[3] : "success";
  alloc_failure = mode == "alloc";
  init_failure = mode == "init";
  deflate_failure = mode == "deflate";
  std::string out = "untouched";
  const bool ok = gzipBody(expected_input, out);
  if (mode == "success") {
    check(ok, "production gzip container succeeds");
    std::cout.write(out.data(), out.size());
  } else {
    check(!ok && out == "untouched", "failure leaves caller output unchanged");
    if (alloc_failure) {
      check(!gzipBody(expected_input, out) && allocations == 1, "failed PSRAM allocation is not retried");
    }
    std::cout << "PASS " << mode << '\n';
  }
  std::free(g_gzip_deflator);
}
