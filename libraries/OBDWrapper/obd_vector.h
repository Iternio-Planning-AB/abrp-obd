#ifndef OBD_VECTOR_H
#define OBD_VECTOR_H

/*
  The telemetry frame type, shared by the OBD, network and upload code.

  A frame's pairs and keys prefer PSRAM. A queued frame is about 1 KB of pairs, under the 4 KB malloc threshold that
  sends a buffer to PSRAM on its own, so in internal heap the upload queue competed with Bluedroid and the modem: in
  the field (#75) 120 queued frames offline left a 2292 B largest internal block, with 4 MB of PSRAM free. With both
  here, a queued frame costs no internal heap. Falls back to internal heap when PSRAM is absent or full. PSRAM writes
  are safe on this rev 1 chip only with the PSRAM-safe runtime (#64).
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

// The key too: one past the string's own 15 characters (est_battery_range) would otherwise put a heap block per
// queued frame in internal heap. Converts to and from std::string, so code reading a key needs no change; its
// operators are found only through a Key argument.
struct Key : std::basic_string<char, std::char_traits<char>, obd_alloc::PsramPreferred<char>> {
  using Base = std::basic_string<char, std::char_traits<char>, obd_alloc::PsramPreferred<char>>;
  using Base::Base;
  Key() = default;
  Key(const char* s) : Base(s) {}
  Key(const std::string& s) : Base(s.data(), s.size()) {}
  operator std::string() const { return std::string(data(), size()); }
};
inline bool operator==(const Key& a, const Key& b) { return a.compare(0, a.size(), b.data(), b.size()) == 0; }
inline bool operator==(const Key& a, const std::string& b) { return a.compare(0, a.size(), b.data(), b.size()) == 0; }
inline bool operator==(const std::string& a, const Key& b) { return b == a; }
inline bool operator==(const Key& a, const char* b) { return a.compare(b) == 0; }
inline bool operator==(const char* a, const Key& b) { return b.compare(a) == 0; }
inline bool operator!=(const Key& a, const Key& b) { return !(a == b); }
inline bool operator!=(const Key& a, const std::string& b) { return !(a == b); }
inline bool operator!=(const std::string& a, const Key& b) { return !(b == a); }
inline bool operator!=(const Key& a, const char* b) { return !(a == b); }
inline bool operator!=(const char* a, const Key& b) { return !(b == a); }
inline bool operator<(const Key& a, const Key& b) { return a.compare(b) < 0; }
}

using OBDKey = obd_alloc::Key;

using OBDPair = std::pair<OBDKey, double>;
using OBDVector = std::vector<OBDPair, obd_alloc::PsramPreferred<OBDPair>>;

#endif
