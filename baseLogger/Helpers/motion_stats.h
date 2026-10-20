#pragma once
/*
  Per-minute accelerometer summary for the diagnostic MOTION line: aggregation only, no I/O, so a host test can
  drive it. #included by logger.ino into the single sketch translation unit (and by tests/log_diagnostics_checks.cpp).
*/
#include <cstdint>
#include <cstdio>

static const unsigned long MOTION_STATS_MS = 60000UL; // diagnostic cadence, not a tunable

struct MotionStats {
  unsigned long start_ms = 0;
  unsigned long last_ms = 0;
  uint16_t n = 0;        // detectMovement windows
  uint16_t active = 0;   // ...that saw motion
  float peak = 0;        // g, largest window peak
  float sum = 0;         // g, sum of window peaks
  float axmax = 0;       // g, largest single-axis excursion: what the 0.1 g motion threshold compares
  bool obd = false;      // the car answered a poll this minute
};

// One detectMovement window. `sampled` is false for a bypassed window (OTA, log dump, UNINITIALIZED),
// which is neither counted nor allowed to emit. Returns true with `out` holding the finished minute.
bool motionStatsWindow(MotionStats& m, bool sampled, bool active, float g, float axmax, unsigned long now,
                       MotionStats& out) {
  if (!sampled) return false;
  bool rolled = false;
  if (m.n && now - m.start_ms >= MOTION_STATS_MS) {
    out = m;
    m = MotionStats{};
    rolled = true;
  }
  if (!m.n) m.start_ms = now;
  m.last_ms = now;
  m.n++;
  m.active += active;
  m.sum += g;
  if (g > m.peak) m.peak = g;
  if (axmax > m.axmax) m.axmax = axmax;
  return rolled;
}

// The partial minute before sleep; false when there is nothing to report.
bool motionStatsTake(MotionStats& m, MotionStats& out) {
  if (!m.n) return false;
  out = m;
  m = MotionStats{};
  return true;
}

// s = seconds the windows span: a blocking upload or dump between windows stretches a "minute".
void motionStatsLine(char* buf, size_t len, const MotionStats& m, bool parked, bool chg) {
  snprintf(buf, len, "MOTION act=%u n=%u s=%lu peak=%.3f mean=%.3f axmax=%.3f obd=%d parked=%d chg=%d",
    m.n ? (unsigned)((m.active * 100UL + m.n / 2) / m.n) : 0U, (unsigned)m.n, (m.last_ms - m.start_ms) / 1000UL,
    m.peak, m.n ? m.sum / m.n : 0.0f, m.axmax, m.obd ? 1 : 0, parked ? 1 : 0, chg ? 1 : 0);
}
