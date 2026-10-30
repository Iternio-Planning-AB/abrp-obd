#pragma once
/*
  Per-minute accelerometer summary for the diagnostic MOTION line, and the loop gap the WAKESTAT line reports (#110):
  aggregation only, no I/O, so a host test can drive it. #included by logger.ino into the single sketch translation
  unit (and by tests/log_diagnostics_checks.cpp).
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

// The longest gap between two visits to loop()'s service point this wake, for WAKESTAT (#110): a scheduler-gap proxy,
// not a measured BLE latency. The first visit only seeds, so setup() and the deep sleep before it never count. uint32
// arithmetic, so a gap across the 32-bit millis() wrap is still right. Its floor is detectMovement's 500 ms window
// while stationary; an in-loop upload burst or poll shows up too, matched offline by at= against the UPLOAD/OBD lines.
// enterDeepSleep() notes its own entry as one more visit when loop() ran, so a sleep from inside the loop body (the
// net-recover reboot after a modem stall) still counts the stretch before it. RAM only: every wake starts a new one.
struct LoopGap {
  uint32_t last_ms = 0;
  uint32_t max_ms = 0;  // longest gap
  uint32_t max_at = 0;  // millis() it ended at
  uint32_t n = 0;       // visits: loop()'s, and the sleep entry's
  bool seeded = false;
};
LoopGap g_loop_gap; // loop()'s gap tracker

void loopGapNote(LoopGap& g, uint32_t now) {
  if (g.seeded && now - g.last_ms > g.max_ms) {
    g.max_ms = now - g.last_ms;
    g.max_at = now;
  }
  g.seeded = true;
  g.last_ms = now;
  g.n++;
}

// After a console command (DUMP, CLKPROBE, GZTEST...), which blocks the loop for minutes by design: the next gap
// starts now, so a bench session never reads as a telemetry stall.
void loopGapReseed(LoopGap& g, uint32_t now) {
  g.seeded = true;
  g.last_ms = now;
}

// Rolling accumulator behind the drive rules' "sustained motion" (#6): a ring of 5 s buckets of sampled and
// active time, fed by every sampled detectMovement window (the same dt rule as MotionStats), judged over the
// last window_ms. Independent of ENABLE_LOGGING; a bucket from an earlier lap is skipped by its id.
static const unsigned long DRIVE_MOTION_BUCKET_MS = 5000UL;
// The +1: the current bucket is partly filled, so the window still has a full DRIVE_SUSTAINED_WINDOW_MS behind it.
static const unsigned DRIVE_MOTION_BUCKETS = DRIVE_SUSTAINED_WINDOW_MS / DRIVE_MOTION_BUCKET_MS + 1;
static_assert(DRIVE_SUSTAINED_WINDOW_MS % DRIVE_MOTION_BUCKET_MS == 0, "the window is a whole number of buckets");

struct DriveMotion {
  uint32_t id[DRIVE_MOTION_BUCKETS] = {};  // now / DRIVE_MOTION_BUCKET_MS of the bucket's contents
  uint32_t total_ms[DRIVE_MOTION_BUCKETS] = {};
  uint32_t active_ms[DRIVE_MOTION_BUCKETS] = {};
  unsigned long last_ms = 0;
  bool seen = false;
};

void motionAccumulate(DriveMotion& m, bool sampled, bool active, uint32_t window_ms, unsigned long now) {
  if (!sampled) return;
  uint32_t dt = window_ms ? window_ms : 1;
  if (m.seen) dt = now - m.last_ms < MOTION_GAP_CAP_MS ? now - m.last_ms : MOTION_GAP_CAP_MS;
  m.seen = true;
  m.last_ms = now;
  const uint32_t id = (uint32_t)(now / DRIVE_MOTION_BUCKET_MS);
  const unsigned slot = id % DRIVE_MOTION_BUCKETS;
  if (m.id[slot] != id) {
    m.id[slot] = id;
    m.total_ms[slot] = m.active_ms[slot] = 0;
  }
  m.total_ms[slot] += dt;
  if (active) m.active_ms[slot] += dt;
}

// Sustained: at least DRIVE_SUSTAINED_MIN_SAMPLED_MS sampled over the window, DRIVE_SUSTAINED_ACT_PCT of it active.
// act_out gets the active share, rounded, for the log line.
bool motionSustained(const DriveMotion& m, unsigned long now, unsigned* act_out = nullptr) {
  const uint32_t id = (uint32_t)(now / DRIVE_MOTION_BUCKET_MS);
  uint64_t sampled = 0, active = 0;
  for (unsigned slot = 0; slot < DRIVE_MOTION_BUCKETS; ++slot) {
    if (id - m.id[slot] >= DRIVE_MOTION_BUCKETS) continue;
    sampled += m.total_ms[slot];
    active += m.active_ms[slot];
  }
  if (act_out) *act_out = sampled ? (unsigned)((active * 100 + sampled / 2) / sampled) : 0U;
  return sampled >= DRIVE_SUSTAINED_MIN_SAMPLED_MS && active * 100 >= (uint64_t)DRIVE_SUSTAINED_ACT_PCT * sampled;
}
