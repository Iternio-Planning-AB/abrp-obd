#pragma once
/*
  Drive region of the state machine (#23, step 1 #24, step 2 #6): is the car PARKED, DRIVING, or looks like
  it is driving and is UNCONFIRMED. Speed-PID drives behave as they always did; tests/run_sm_checks.py proves
  that against the pinned commit, down to the log sequence and the clock reads around it, and its `rules`
  group covers the step-2 rules.

  The region owns the park/drive latch, the UNCONFIRMED flag, the time of the last movement hold, and the
  park-marker debt. They are still stored in the old globals - is_currently_parked, drive_unconfirmed,
  g_last_speed_ms and park_marker_owed, defined in logger.ino, read directly by their many readers - until
  #27 retires the names. UNCONFIRMED is parked to every one of those readers: is_currently_parked stays true,
  so the backstop, is_charging and the pre-sleep marker gate need no change; the frame just omits is_parked
  (no drivingState), the live location replaces the frozen spot, and no PARK marker is owed. The functions
  below are the only writers (a runner check enforces it).

  The transitions are pure functions of the region, the evidence and the clock; the glue gathers the evidence,
  logs first, then reads the clock and stores, in the order the firmware always has. The OBD-alarm gate takes
  no input from here.

    moment        from                  condition                                       to           effects
    speed         any                   a speed key > 0.0                               DRIVING      hold, leash; LEASH line if it was parked
    gnss          PARKED, UNCONFIRMED   gpsMovingFresh, car on, motion within           DRIVING      hold, leash, LEASH line
                                        DRIVE_MOTION_RECENT_MS, no charge latch
    gnss          PARKED, UNCONFIRMED   gpsMovingFresh, any of those missing            same         one LEASH refused line per streak
    gnss          DRIVING               gpsMovingFresh                                  DRIVING      hold, leash (no new gate)
    accel         PARKED                car on, sustained motion, no charge latch       UNCONFIRMED  hold = now, LEASH line
    accel         PARKED, UNCONFIRMED   ... and discharge > DRIVE_DISCHARGE_KW          DRIVING      hold, LEASH line
    accel         DRIVING               car on, sustained motion, no charge latch       DRIVING      hold (continuation, no log)
    accel         UNCONFIRMED           charge latch set, or the car off                PARKED       LEASH line, no freeze
    poll end      UNCONFIRMED           hold = last motion; now - hold > ACTIVE_TIMER   PARKED       LEASH line, no freeze
    poll end      DRIVING               now - last hold > ACTIVE_TIMER                  PARKED       freeze, LEASH re-latch line
    idle          DRIVING               activity state -> CHARGING or STANDBY           PARKED       freeze
    idle          UNCONFIRMED           activity state -> CHARGING or STANDBY           PARKED       LEASH line, no freeze
    session hold  DRIVING               an OTA update or log dump is running            DRIVING      hold
    buffered      DRIVING               a frame entered the upload buffer               DRIVING      owe the PARK marker
    marker        any                   the pre-sleep PARK marker was queued            -            debt paid
    restored      any                   a reset but deep sleep; the crash copy owes     -            debt owed (#49)

  Still elsewhere: the awake ladder and its last_active leash (state_machine.h, #29), the charge latch
  (#25), the wake gates and RTC boot (#26).

  #included by logger.ino after motion_stats.h - it is NOT a standalone compilation unit - and compiled by
  the host runners (heading, sm). Uses is_currently_parked, drive_unconfirmed, g_last_speed_ms, last_active,
  g_last_io36, g_last_motion_ms, g_last_obd_answer_ms and recently_charging, which logger.ino defines above
  its helper includes, and g_drive_motion (mems.h).
*/

enum DriveState { DRIVE_PARKED, DRIVE_UNCONFIRMED, DRIVE_DRIVING };
struct DriveRegion {
  DriveState state;
  unsigned long last_hold_ms;  // millis() of the last speed, GNSS or session hold; UNCONFIRMED: of the last motion
};
struct DriveEvidence {
  bool car_on;            // the 12 V rail is up, or the car answered an OBD poll within ACTIVE_TIMER and the
                          // rail still reads a car (>= IO36_LOCKOUT_LOW)
  bool motion_recent;     // a detectMovement hit within DRIVE_MOTION_RECENT_MS
  bool motion_sustained;  // the rolling window shows motion on enough of its sampled time
  bool discharge;         // the battery is discharging above DRIVE_DISCHARGE_KW
  bool charge_latched;    // the charge latch (recently_charging) is set
};
// rail_read: the build reads the rail (ENABLE_IO36_RAIL); without it g_last_io36 stays 0 and only the answer counts.
inline bool driveCarOn(bool rail_read, float rail, bool obd_recent) {
  if (!rail_read) return obd_recent;
  return rail >= RAIL_AWAKE_VOLTS || (obd_recent && rail >= IO36_LOCKOUT_LOW);
}
// Why a fresh moving GNSS fix may not start a drive, or nullptr when it may. A drive already under way just
// continues on it: no new gate.
inline const char* driveGnssRefusal(const DriveRegion& r, const DriveEvidence& e) {
  if (r.state == DRIVE_DRIVING) return nullptr;
  if (e.charge_latched) return "charging";
  if (!e.car_on) return "car-off";
  if (!e.motion_recent) return "motion";
  return nullptr;
}

inline DriveRegion driveOnMoving(DriveRegion r, unsigned long now_ms) {
  r.state = DRIVE_DRIVING;
  r.last_hold_ms = now_ms;
  return r;
}
inline DriveRegion driveOnAccel(DriveRegion r, const DriveEvidence& e, unsigned long now_ms) {
  if (r.state == DRIVE_UNCONFIRMED && (e.charge_latched || !e.car_on)) {
    r.state = DRIVE_PARKED;
    return r;
  }
  if (e.charge_latched || !e.car_on || !e.motion_sustained) return r;
  // A drive under way is held by the motion alone (cruising draws well under the start limit); starting one needs the discharge.
  if (r.state == DRIVE_DRIVING || e.discharge) return driveOnMoving(r, now_ms);
  if (r.state == DRIVE_PARKED) {
    r.state = DRIVE_UNCONFIRMED;
    r.last_hold_ms = now_ms;
  }
  return r;
}
// Each sampled motion window refreshes the UNCONFIRMED hold; g_last_motion_ms is the newest of them.
inline DriveRegion driveOnMotionHold(DriveRegion r, unsigned long last_motion_ms) {
  if (r.state == DRIVE_UNCONFIRMED && last_motion_ms && (long)(last_motion_ms - r.last_hold_ms) > 0)
    r.last_hold_ms = last_motion_ms;
  return r;
}
inline DriveRegion driveOnPollEnd(DriveRegion r, unsigned long now_ms) {
  if (r.state != DRIVE_PARKED && now_ms - r.last_hold_ms > (unsigned long)ACTIVE_TIMER) r.state = DRIVE_PARKED;
  return r;
}
inline DriveRegion driveOnIdle(DriveRegion r) {
  r.state = DRIVE_PARKED;
  return r;
}
inline DriveRegion driveOnSessionHold(DriveRegion r, unsigned long now_ms) {
  if (r.state == DRIVE_DRIVING) r.last_hold_ms = now_ms;
  return r;
}

// ---- glue: storage, effects and the log lines ----

extern bool park_marker_owed;  // RTC, defined in logger.ino; the heading fixture defines it after this header
#if ENABLE_MEMS
extern DriveMotion g_drive_motion;  // mems.h
#endif
void freezeParkedLocation();   // Helpers/gps_location.h

inline DriveRegion driveRegion() {
  return {!is_currently_parked ? DRIVE_DRIVING : drive_unconfirmed ? DRIVE_UNCONFIRMED : DRIVE_PARKED, g_last_speed_ms};
}
inline void driveStore(const DriveRegion& r) {
  if (driveRegion().state != r.state) g_drive_gnss_refused = false;
  is_currently_parked = r.state != DRIVE_DRIVING;
  drive_unconfirmed = r.state == DRIVE_UNCONFIRMED;
  g_last_speed_ms = r.last_hold_ms;
}

// The evidence as of now, with the numbers the log lines quote. Discharge reads the frame's power (kW, positive
// = discharge) or else current (A) x voltage (V); a missing or out-of-range value cannot confirm anything.
struct DriveSample {
  DriveEvidence evidence;
  unsigned act_pct;
  double kw;
  bool obd_recent;
};
inline bool driveFrameValue(const OBDVector& frame, const char* key, double lo, double hi, double* out) {
  for (const auto& res : frame) {
    if (res.first != key) continue;
    if (!(res.second > lo && res.second < hi)) return false;
    *out = res.second;
    return true;
  }
  return false;
}
inline DriveSample driveSample(const OBDVector* frame) {
  const unsigned long now = millis();
  DriveSample s = {};
  s.obd_recent = g_last_obd_answer_ms && now - g_last_obd_answer_ms < (unsigned long)ACTIVE_TIMER;
  s.evidence.car_on = driveCarOn(ENABLE_IO36_RAIL, g_last_io36, s.obd_recent);
  s.evidence.motion_recent = g_last_motion_ms && now - g_last_motion_ms <= (unsigned long)DRIVE_MOTION_RECENT_MS;
#if ENABLE_MEMS
  s.evidence.motion_sustained = motionSustained(g_drive_motion, now, &s.act_pct);
#endif
  s.evidence.charge_latched = recently_charging;
  double amps = 0, volts = 0;
  bool known = false;
  if (frame) {
    known = driveFrameValue(*frame, "power", -1000.0, 1000.0, &s.kw);
    if (!known && driveFrameValue(*frame, "current", -2000.0, 2000.0, &amps) &&
        driveFrameValue(*frame, "voltage", 0.0, 1000.0, &volts)) {
      s.kw = amps * volts / 1000.0;
      known = true;
    }
  }
  s.evidence.discharge = known && s.kw > (double)DRIVE_DISCHARGE_KW;
  return s;
}

// A speed key read > 0.0 this poll (the caller checks, and feeds the alarm gate after).
inline void driveSpeed(double kmh) {
  if (is_currently_parked) {
    Serial.printf("LEASH: speed=%.1f -> driving (unlatch parked, hold awake)\n", kmh);
#if ENABLE_LOGGING
    logEvent("LEASH driving speed=%.1f -> unlatch parked + hold awake (smooth-road guard)", kmh);
#endif
  }
  last_active = millis();
  driveStore(driveOnMoving(driveRegion(), millis()));
}

// gpsMovingFresh() this poll; kmh is the GNSS speed, for the log lines only. Starting a drive needs the evidence.
inline void driveGnss(double kmh) {
  if (const char* refused = driveGnssRefusal(driveRegion(), driveSample(nullptr).evidence)) {
    if (!g_drive_gnss_refused) {
      g_drive_gnss_refused = true;
#if ENABLE_LOGGING
      logEvent("LEASH gnss=%.1fkm/h refused reason=%s", kmh, refused);
#endif
    }
    return;
  }
  if (is_currently_parked) {
    Serial.printf("LEASH: GNSS %.1f km/h -> driving (unlatch parked, hold awake)\n", kmh);
#if ENABLE_LOGGING
    logEvent("LEASH driving gnss=%.1fkm/h -> unlatch parked (no OBD speed needed)", kmh);
#endif
  }
  last_active = millis();
  driveStore(driveOnMoving(driveRegion(), millis()));
}

// Every poll, after the speed and GNSS checks: sustained accelerometer motion on a car that is on.
inline void driveAccel(const OBDVector& frame) {
  const DriveSample s = driveSample(&frame);
  const DriveRegion region = driveRegion(), next = driveOnAccel(region, s.evidence, millis());
  if (next.state != region.state) {
#if ENABLE_LOGGING
    if (next.state == DRIVE_DRIVING)
      logEvent("LEASH driving accel act=%u%% power=%.1fkW -> unlatch parked", s.act_pct, s.kw);
    else if (next.state == DRIVE_UNCONFIRMED)
      logEvent("LEASH unconfirmed accel act=%u%% rail=%.2f obd=%d -> live location, no drivingState", s.act_pct,
        g_last_io36, s.obd_recent ? 1 : 0);
    else
      logEvent("LEASH unconfirmed %s -> parked", s.evidence.charge_latched ? "charging" : "car-off");
#endif
  } else if (next.last_hold_ms == region.last_hold_ms) {
    return;
  }
  if (next.state == DRIVE_DRIVING) last_active = millis();
  driveStore(next);
}

// After the speed, GNSS and accelerometer checks of every poll: the time-based re-latch.
inline void drivePollEnd() {
  const DriveRegion before = driveRegion(), region = driveOnMotionHold(before, g_last_motion_ms);
  if (region.last_hold_ms != before.last_hold_ms) driveStore(region);
  const DriveRegion next = driveOnPollEnd(region, millis());
  if (next.state == region.state) return;
  if (region.state == DRIVE_DRIVING) freezeParkedLocation();
  driveStore(next);
#if ENABLE_LOGGING
  if (region.state == DRIVE_DRIVING)
    logEvent("LEASH no speed for %lus -> re-latch parked (time-based; BLE-hold safe)",
      (unsigned long)(ACTIVE_TIMER / 1000UL));
  else
    logEvent("LEASH unconfirmed no motion for %lus -> parked", (unsigned long)(ACTIVE_TIMER / 1000UL));
#endif
}

// changeState() to CHARGING or STANDBY.
inline void driveIdle() {
  const DriveRegion region = driveRegion();
  if (region.state == DRIVE_DRIVING) freezeParkedLocation();
#if ENABLE_LOGGING
  else if (region.state == DRIVE_UNCONFIRMED) logEvent("LEASH unconfirmed idle -> parked");
#endif
  driveStore(driveOnIdle(region));
}

// An OTA update or log dump is running: polling is suspended, so unobserved time must not end a drive.
inline void driveSessionHold() { driveStore(driveOnSessionHold(driveRegion(), millis())); }

inline void driveFrameBuffered() {
  if (!is_currently_parked) park_marker_owed = true;
}
inline void driveMarkerQueued() { park_marker_owed = false; }
// setup(): .rtc.data was reloaded, and wdt_streak.h's copy says the PARK may not have been sent. Queue loss uses this re-arm too.
inline void driveMarkerRestored() { park_marker_owed = true; }
