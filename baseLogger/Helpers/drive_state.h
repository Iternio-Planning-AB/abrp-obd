#pragma once
/*
  Drive region of the state machine (#23, step 1 #24): is the car PARKED or DRIVING. Behaviour is unchanged
  from the code this replaced; tests/run_sm_checks.py proves it against the pinned commit, down to the log
  sequence and the clock reads around it.

  The region owns the park/drive latch, the time of the last movement hold, and the park-marker debt. They
  are still stored in the old globals - is_currently_parked, g_last_speed_ms and park_marker_owed, defined in
  logger.ino, read directly by their many readers - until #27 retires the names. The functions below are
  their only writers (a runner check enforces it).

  The transitions are pure functions of the region and the clock; the glue logs first, then reads the
  clock and stores, in the order the firmware always has.

    moment        from     condition                                   to        effects
    speed/gnss    any      a speed key > 0.0, or gpsMovingFresh        DRIVING   hold, leash; LEASH line if it was PARKED
    poll end      DRIVING  now - last hold > ACTIVE_TIMER              PARKED    freeze, LEASH re-latch line
    idle          DRIVING  activity state -> CHARGING or STANDBY       PARKED    freeze
    session hold  DRIVING  an OTA update or log dump is running        DRIVING   hold
    buffered      DRIVING  a frame entered the upload buffer           DRIVING   owe the PARK marker
    marker        any      the pre-sleep PARK marker was queued        -         debt paid

  Still elsewhere: the awake ladder and its last_active leash (state_machine.h, #29), the charge latch
  (#25), the wake gates and RTC boot (#26). UNCONFIRMED and the DRIVE transition log line come with the #6
  driving rules (step 2).

  #included by logger.ino after motion_stats.h - it is NOT a standalone compilation unit - and compiled by
  the host runners (heading, sm). Uses is_currently_parked, g_last_speed_ms and last_active, which
  logger.ino defines above its helper includes.
*/

enum DriveState { DRIVE_PARKED, DRIVE_DRIVING };
struct DriveRegion {
  DriveState state;
  unsigned long last_hold_ms;  // millis() of the last speed, GNSS or session hold
};

inline DriveRegion driveOnMoving(DriveRegion r, unsigned long now_ms) {
  r.state = DRIVE_DRIVING;
  r.last_hold_ms = now_ms;
  return r;
}
inline DriveRegion driveOnPollEnd(DriveRegion r, unsigned long now_ms) {
  if (r.state == DRIVE_DRIVING && now_ms - r.last_hold_ms > (unsigned long)ACTIVE_TIMER) r.state = DRIVE_PARKED;
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
void freezeParkedLocation();   // Helpers/gps_location.h

inline DriveRegion driveRegion() { return {is_currently_parked ? DRIVE_PARKED : DRIVE_DRIVING, g_last_speed_ms}; }
inline void driveStore(const DriveRegion& r) {
  is_currently_parked = r.state == DRIVE_PARKED;
  g_last_speed_ms = r.last_hold_ms;
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

// gpsMovingFresh() this poll; kmh is the GNSS speed, for the log line only.
inline void driveGnss(double kmh) {
  if (is_currently_parked) {
    Serial.printf("LEASH: GNSS %.1f km/h -> driving (unlatch parked, hold awake)\n", kmh);
#if ENABLE_LOGGING
    logEvent("LEASH driving gnss=%.1fkm/h -> unlatch parked (no OBD speed needed)", kmh);
#endif
  }
  last_active = millis();
  driveStore(driveOnMoving(driveRegion(), millis()));
}

// After the speed and GNSS checks of every poll: the time-based re-latch.
inline void drivePollEnd() {
  const DriveRegion region = driveRegion(), next = driveOnPollEnd(region, millis());
  if (next.state == region.state) return;
  freezeParkedLocation();
  driveStore(next);
#if ENABLE_LOGGING
  logEvent("LEASH no speed for %lus -> re-latch parked (time-based; BLE-hold safe)",
    (unsigned long)(ACTIVE_TIMER / 1000UL));
#endif
}

// changeState() to CHARGING or STANDBY.
inline void driveIdle() {
  const DriveRegion region = driveRegion();
  if (region.state == DRIVE_DRIVING) freezeParkedLocation();
  driveStore(driveOnIdle(region));
}

// An OTA update or log dump is running: polling is suspended, so unobserved time must not end a drive.
inline void driveSessionHold() { driveStore(driveOnSessionHold(driveRegion(), millis())); }

inline void driveFrameBuffered() {
  if (!is_currently_parked) park_marker_owed = true;
}
inline void driveMarkerQueued() { park_marker_owed = false; }
