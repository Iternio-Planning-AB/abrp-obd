#pragma once
// Heap model for the upload queue checks: ESP.getFreeHeap()/getMaxAllocHeap()/getMaxAllocPsram() shrink by the
// storage the queue and the held frames really hold, measured independently of the production estimator.
// Include after obd_result_vector, g_upload_held and the ESPStub declaration (with all five methods).
//
// Placement follows the firmware: a frame's pairs and long keys go to PSRAM while the fixture has some
// (PsramPreferred), and the outer array goes there from the malloc threshold up. heap_fixture_psram is the largest
// PSRAM block with nothing queued; 0 is a board with no PSRAM, where everything lands internal.

#include <cstdint>

#ifndef CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096 // the Arduino core's sdkconfig: larger mallocs go to PSRAM
#endif

bool heap_fixture_enabled = false;
unsigned long heap_fixture_free = 1000000;
unsigned long heap_fixture_maxblock = 500000;
unsigned long heap_fixture_psram = 0;

void resetHeapFixture() {
  heap_fixture_enabled = false;
  heap_fixture_free = 1000000;
  heap_fixture_maxblock = 500000;
  heap_fixture_psram = 0;
}

void fixtureUploadVectorBytes(const std::vector<OBDVector>& frames, size_t& internal, size_t& psram) {
  const bool has_psram = heap_fixture_psram > 0;
  if (frames.capacity()) {
    const size_t outer = frames.capacity() * sizeof(OBDVector) + 16;
    (has_psram && outer >= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL ? psram : internal) += outer;
  }
  for (const auto& frame : frames) {
    if (frame.capacity()) (has_psram ? psram : internal) += frame.capacity() * sizeof(OBDVector::value_type) + 16;
    for (const auto& value : frame) {
      const auto data = reinterpret_cast<std::uintptr_t>(value.first.data());
      const auto object = reinterpret_cast<std::uintptr_t>(&value.first);
      if (!value.first.empty() && (data < object || data >= object + sizeof(std::string)))
        (has_psram ? psram : internal) += value.first.capacity() + 1 + 16;
    }
  }
}

size_t fixtureUploadBytes(bool in_psram = false) {
  size_t internal = 0, psram = 0;
  fixtureUploadVectorBytes(obd_result_vector, internal, psram);
  fixtureUploadVectorBytes(g_upload_held, internal, psram);
  return in_psram ? psram : internal;
}

unsigned long ESPStub::getFreeHeap() const {
  if (!heap_fixture_enabled) return 1000000;
  const size_t used = fixtureUploadBytes();
  return used >= heap_fixture_free ? 0 : heap_fixture_free - used;
}

unsigned long ESPStub::getMaxAllocHeap() const {
  if (!heap_fixture_enabled) return 500000;
  const size_t used = fixtureUploadBytes();
  return used >= heap_fixture_maxblock ? 0 : heap_fixture_maxblock - used;
}

unsigned long ESPStub::getPsramSize() const {
  if (!heap_fixture_enabled) return 4194304;
  return heap_fixture_psram ? 4194304 : 0;
}

// One free block in the fixture: free PSRAM and the largest block are the same.
unsigned long ESPStub::getFreePsram() const { return getMaxAllocPsram(); }

unsigned long ESPStub::getMaxAllocPsram() const {
  if (!heap_fixture_enabled) return 4000000;
  const size_t used = fixtureUploadBytes(true);
  return used >= heap_fixture_psram ? 0 : heap_fixture_psram - used;
}
