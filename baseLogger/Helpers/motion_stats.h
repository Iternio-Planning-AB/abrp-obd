#pragma once
/*
  Per-minute accelerometer summary for the diagnostic MOTION line: aggregation only, no I/O, so a host test can
  drive it. #included by logger.ino into the single sketch translation unit (and by tests/log_diagnostics_checks.cpp).
*/
#include <cstdint>
#include <cstdio>

static const unsigned long MOTION_STATS_MS = 60000UL; // diagnostic cadence, not a tunable
static const unsigned long MOTION_GAP_CAP_MS = 2000UL; // above a slow poll loop; a blocking upload is not observed

struct MotionStats {
  unsigned long start_ms = 0;
  unsigned long last_ms = 0;
  uint16_t n = 0;          // detectMovement windows
  uint32_t total_ms = 0;   // wall time the windows stand for (gap since the previous window, capped)
  uint32_t active_ms = 0;  // ...of which the window saw motion. Not a count: a moving window ends on its first sample
  float peak = 0;        // g, largest window peak
  float sum = 0;         // g, sum of window peaks
  float axmax = 0;       // g, largest single-axis excursion: what the 0.1 g motion threshold compares
  bool obd = false;      // the car answered a poll this minute
};

// One detectMovement window. `sampled` is false for a bypassed window (OTA, log dump, UNINITIALIZED),
// which is neither counted nor allowed to emit. Returns true with `out` holding the finished minute.
bool motionStatsWindow(MotionStats& m, bool sampled, bool active, uint32_t window_ms, float g, float axmax,
                       unsigned long now, MotionStats& out) {
  if (!sampled) return false;
  bool rolled = false;
  if (m.n && now - m.start_ms >= MOTION_STATS_MS) {
    out = m;
    m = MotionStats{};
    rolled = true;
  }
  uint32_t dt = window_ms ? window_ms : 1; // a lone 0 ms moving window still counts
  if (m.n) dt = now - m.last_ms < MOTION_GAP_CAP_MS ? now - m.last_ms : MOTION_GAP_CAP_MS;
  if (!m.n) m.start_ms = now;
  m.last_ms = now;
  m.n++;
  m.total_ms += dt;
  if (active) m.active_ms += dt;
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
    m.total_ms ? (unsigned)((m.active_ms * 100ULL + m.total_ms / 2) / m.total_ms) : 0U, (unsigned)m.n, (m.last_ms - m.start_ms) / 1000UL,
    m.peak, m.n ? m.sum / m.n : 0.0f, m.axmax, m.obd ? 1 : 0, parked ? 1 : 0, chg ? 1 : 0);
}
