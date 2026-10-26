#ifndef OBD_VECTOR_H
#define OBD_VECTOR_H

/*
  The telemetry frame type, shared by the OBD, network and upload code.

  A frame's pairs prefer PSRAM. A queued frame is about 1 KB of pairs, under the 4 KB malloc threshold that sends a
  buffer to PSRAM on its own, so in internal heap the upload queue competed with Bluedroid and the modem: in the field
  (#75) 120 queued frames offline left a 2292 B largest internal block, with 4 MB of PSRAM free. Keys up to 15
  characters stay inside the std::string, so a queued frame costs no internal heap. Falls back to internal heap when
  PSRAM is absent or full. PSRAM writes are safe on this rev 1 chip only with the PSRAM-safe runtime (#64).
*/

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// In its own namespace so argument-dependent lookup on a frame finds nothing extra in the global one.
namespace obd_alloc {
template <typename T>
struct PsramPreferred {
  using value_type = T;
  PsramPreferred() = default;
  template <typename U> PsramPreferred(const PsramPreferred<U>&) {}
  T* allocate(std::size_t n) {
    if (n > SIZE_MAX / sizeof(T)) throw std::bad_alloc();
    void* p = nullptr;
#ifdef ESP_PLATFORM
    p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!p) p = std::malloc(n * sizeof(T));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) { std::free(p); } // free() takes heap_caps_malloc's blocks too
};
template <typename T, typename U>
bool operator==(const PsramPreferred<T>&, const PsramPreferred<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const PsramPreferred<T>&, const PsramPreferred<U>&) { return false; }
}

using OBDPair = std::pair<std::string, double>;
using OBDVector = std::vector<OBDPair, obd_alloc::PsramPreferred<OBDPair>>;

#endif
