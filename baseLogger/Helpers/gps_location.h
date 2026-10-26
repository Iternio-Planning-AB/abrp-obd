#pragma once
/*
  GPS/GNSS helpers - position + GNSS time handling.
  Contains: the GNSS->RTC clock seed (setClockFromGps), the parked-location freeze that
  captures the last good driving fix (freezeParkedLocation), and addGpsData() which attaches
  position/heading to a telemetry frame (frozen fix when parked, live fix when driving).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/
#include "FreematicsGPS.h"   // TinyGPS::distance_between

/*
  Set the RTC from the GNSS date/time fields (the u-blox reports these in UTC). Returns true when a
  fix with a valid date was applied. This is the primary time source: outdoors the GNSS fix provides
  UTC directly, so no NTP round-trip is needed.
*/
static GPS_DATA geo_accepted{};
static uint32_t geo_accepted_utc = 0;

uint32_t gpsUtc(const GPS_DATA& fix) {
  int year = fix.date % 100 + 2000;
  int month = fix.date / 100 % 100;
  int day = fix.date / 10000;
  int hour = fix.time / 1000000;
  int minute = fix.time / 10000 % 100;
  int second = fix.time / 100 % 100;
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (month < 1 || month > 12 || day < 1 || day > days[month - 1] + (month == 2 && leap) ||
      hour > 23 || minute > 59 || second > 59 || year < 2001) return 0;
  time_t epoch = utcToEpoch(year, month, day, hour, minute, second);
  return geoValidUtc((double)epoch, 4294967295.0) ? (uint32_t)epoch : 0;
}

bool pollGpsData() {
  bool accepted = sys.gpsGetData(&gd);
#if ENABLE_LOGGING
  const GPSReadDiagnostics& read = sys.gpsReadDiagnostics();
  if (read.reanchored)   // no coordinates: the flash log is private, but the distance is enough
    logEvent("GPS reanchor dist=%lum rejects=%u", (unsigned long)read.reanchor_dist_m, (unsigned)read.reanchor_rejects);
  if (read.confirmed) logEvent("GPS first fix confirmed held=%u", (unsigned)read.confirm_held);
  else if (read.confirm_restarted && read.confirm_held == 2)   // once a wake: no agreeing second fix in time
    logEvent("GPS first fix unconfirmed, holding the next");
#endif
  if (accepted) {
    geo_accepted = gd ? *gd : GPS_DATA{};
    geo_accepted_utc = gd && geoValidLocation(gd->lat, gd->lng) && gd->lat != 0 && gd->lng != 0 ? gpsUtc(*gd) : 0;
  }
  return accepted;
}

uint32_t gpsLocationUtc() {
  return gd && gd->date == geo_accepted.date && gd->time == geo_accepted.time &&
         gd->lat == geo_accepted.lat && gd->lng == geo_accepted.lng ? geo_accepted_utc : 0;
}

bool setClockFromGps() {
  if (!gd || gd->date == 0) return false; // date stays 0 until the GNSS locks - no fix yet
  if (gd->date != geo_accepted.date || gd->time != geo_accepted.time) return false;
  uint32_t epoch = gpsUtc(geo_accepted);
  if (!epoch) return false;
  setClock(CLK_GPS, epoch);
  return true;
}

/*
  Is the GNSS reporting real movement? gd->speed is in KNOTS (see GPS_DATA), so *1.852 -> km/h.
  This is deliberately INDEPENDENT of the OBD: the vehicle-speed PID goes quiet whenever the car's ECUs sleep
  or when a profile doesn't provide speed, and the position logic must not depend on it (that dependency is
  exactly what made a moving car keep reporting its old parking spot).
*/
static bool gpsHasFix() { return gd && gd->lat != 0 && gd->lng != 0; }

/*
  HOW OLD IS THE FIX, really.

  gd->ts cannot answer this on the co-processor link path: FreematicsPlus.cpp stamps it with millis()
  when a $GNIFO reply is PARSED, and the co-processor re-echoes its last known fix on every request.
  So ts refreshes on every poll whether or not the GNSS produced anything new, and every staleness
  bound built on it silently never fires - which is what let a latched "moving" verdict survive a
  garage indefinitely.

  gd->date and gd->time come from the sentence itself and only advance when the receiver produces a
  genuinely new fix, so a CHANGE in that pair is the only honest evidence of one. This tracks when
  the pair last changed; the age is measured from there. Drive into a garage and the pair freezes,
  so the age grows without bound, which is exactly the signal that was missing.

  gps_fix_seq increments per new fix and gives parkAccumSample() a dedupe key that actually
  distinguishes fixes - its old ts-based guard could never fire on this path either.
*/
static uint32_t gps_fix_date = 0;
static uint32_t gps_fix_time = 0;
static uint32_t gps_fix_seq = 0;
static unsigned long gps_fix_changed_ms = 0;

// Must be called once per poll cycle EVEN WITHOUT A FIX - that is the case whose age has to grow.
static void gpsNoteFix() {
  if (!gd) return;
  if (gd->date != gps_fix_date || gd->time != gps_fix_time) {
    gps_fix_date = gd->date;
    gps_fix_time = gd->time;
    gps_fix_changed_ms = millis();
    gps_fix_seq++;
  }
}

// Age of the newest genuinely-new fix. A wake that never saw one answers "infinitely old", so every
// caller treats "GNSS never worked" the same as "GNSS went stale" - unknown is not evidence.
static unsigned long gpsFixAgeMs() {
  if (gps_fix_seq == 0) return ULONG_MAX;
  return millis() - gps_fix_changed_ms;
}

static bool gpsIsMoving() {
  return gpsHasFix() && (gd->speed * 1.852f) >= (float)GPS_MOVING_MIN_KMH;
}

/*
  Stationary-fix accumulator, used to freeze an AVERAGED parking position instead of one instantaneous sample.
  FIELD EVIDENCE (8 PARKED_LOC events, 5 of them the same parking spot): single-sample freezes scattered up to
  22.4 m apart, individual samples up to 14.9 m from their own mean - while fix quality was good throughout
  (HDOP 0.47-0.69, 12 satellites). So the error was not poor reception, it was using ONE sample: the device sits
  still for ~5 minutes before latching parked (~60 samples at the 5s poll cadence) and threw all but the last
  one away. Averaging independent samples shrinks that random error by roughly sqrt(N).
  Only samples taken while the GNSS says we are STOPPED are accumulated, and any real movement clears the
  accumulator - so it always describes the final resting place, never the approach to it.
*/
static double park_acc_lat = 0.0;
static double park_acc_lon = 0.0;
static uint16_t park_acc_n = 0;
static uint32_t park_acc_utc = 0;
static bool park_acc_unknown_utc = false;
RTC_DATA_ATTR uint32_t frozen_geo_utc = 0;
RTC_DATA_ATTR double frozen_geo_lat = 0, frozen_geo_lon = 0;

void resetFrozenGpsProvenance(bool deep_sleep) {
  if (!deep_sleep) frozen_geo_utc = 0;
}

uint32_t frozenGpsUtc() {
  return frozen_lat == frozen_geo_lat && frozen_lon == frozen_geo_lon ? frozen_geo_utc : 0;
}
static const uint16_t PARK_ACC_MAX = 240;   // ~20 min at the 5s cadence; caps drift in the running mean

// Consecutive samples reporting movement. Used ONLY to debounce the is_parked latch, never the coordinates:
// a lone jittery speed sample from a stationary receiver would otherwise unlatch parked for a full
// ACTIVE_TIMER, and because is_charging and the max-awake backstop are both parked-gated, that would suppress
// charge detection for 5 minutes. Two consecutive samples (~10s) is trivially satisfied by a car pulling away.
static uint8_t gps_moving_streak = 0;
static const uint8_t GPS_MOVING_CONFIRM = 2;

/*
  Per-poll GNSS bookkeeping: maintain the stationary parking average and the movement streak.
  IDEMPOTENT PER FIX. Two reasons: the poll cycle now calls this early (the OBD-alarm gate needs a
  fresh movement verdict BEFORE it decides whether polling is allowed) while addGpsData() still
  calls it on the way past, and a poll cadence faster than the GNSS 1Hz update would otherwise feed
  the same fix into the average several times and over-weight it.

  Keyed on gps_fix_seq, NOT gd->ts. ts was supposed to advance once per received fix and does not on
  the co-processor link path - it is re-stamped every time a reply is parsed, so this guard could
  never fire there and the average WAS being over-weighted by repeats of the same fix. gps_fix_seq
  only advances when the sentence's own date/time changes, which is the real thing.
*/
static uint32_t park_last_ts = 0;
static float park_anchor_lat = 0, park_anchor_lon = 0;   // first sample of a parked window

static void resetParkAccum() {
  park_acc_lat = park_acc_lon = 0.0;
  park_acc_n = 0;
  park_acc_utc = 0;
  park_acc_unknown_utc = false;
}

static void setFrozenLocation(double lat, double lon, uint32_t utc) {
  frozen_lat = lat;
  frozen_lon = lon;
  frozen_valid = true;
  frozen_geo_utc = utc;
  frozen_geo_lat = frozen_lat;
  frozen_geo_lon = frozen_lon;
}

// The mean of the stationary samples, stamped with its newest contributor's time (0 if any was unknown).
static void freezeAtParkMean() {
  setFrozenLocation(park_acc_lat / (double)park_acc_n, park_acc_lon / (double)park_acc_n,
    park_acc_unknown_utc ? 0 : park_acc_utc);
}

// Parked, with a full good cluster far from the frozen spot (or none yet): the car moved without the GNSS
// seeing it move. Accepted risk: a GNSS spoofer can move the parked spot; the alarm gate does not use it.
static void relocateParkedLocation() {
  if (!is_currently_parked || park_acc_n < PARK_ACC_MIN) return;
  float dist = frozen_valid ? TinyGPS::distance_between(frozen_lat, frozen_lon,
    park_acc_lat / park_acc_n, park_acc_lon / park_acc_n) : INFINITY;
  if (!(dist > PARK_RELOCATE_MIN_M)) return;
#if ENABLE_LOGGING
  const bool first = !frozen_valid;
#endif
  freezeAtParkMean();
#if ENABLE_LOGGING
  char moved[16] = "new";  // no frozen spot yet (cold boot): there is no distance to report
  if (!first) snprintf(moved, sizeof(moved), "%.0fm", dist);
  logEvent("PARKED_LOC relocated %s lat=%.6f lon=%.6f avg_of=%u hdop=%d",
    moved, frozen_lat, frozen_lon, (unsigned)park_acc_n, gd->hdop);
#endif
  resetParkAccum();
}

static void parkAccumSample() {
  gpsNoteFix(); // before the fix check: a wake with no fix still has to age
  if (!gpsHasFix()) return;
  if (gps_fix_seq == park_last_ts) return;   // already processed this fix
  park_last_ts = gps_fix_seq;
  if (gpsIsMoving()) {            // moving -> this is the approach, not the parking spot
    if (gps_moving_streak < 255) gps_moving_streak++;
    resetParkAccum();
    return;
  }
  gps_moving_streak = 0;
  if (is_currently_parked) {
    // Relocation evidence only: a good fix inside one tight cluster.
    if (gd->hdop == 0 || gd->hdop > PARK_RELOCATE_MAX_HDOP) return;
    if (park_acc_n >= PARK_ACC_MAX || (park_acc_n &&
        TinyGPS::distance_between(park_anchor_lat, park_anchor_lon, gd->lat, gd->lng) > PARK_RELOCATE_MAX_SPREAD_M))
      resetParkAccum();
    if (!park_acc_n) {
      park_anchor_lat = gd->lat;
      park_anchor_lon = gd->lng;
    }
  }
  if (park_acc_n >= PARK_ACC_MAX) return;
  park_acc_lat += gd->lat;
  park_acc_lon += gd->lng;
  park_acc_n++;
  uint32_t epoch = gpsLocationUtc();
  if (!epoch) park_acc_unknown_utc = true;
  if (epoch > park_acc_utc) park_acc_utc = epoch;
  relocateParkedLocation();
}

/* Debounced "the GNSS is sure we are moving" - drives the is_parked latch and the awake leash. */
static bool gpsMovingConfirmed() { return gps_moving_streak >= GPS_MOVING_CONFIRM; }

/*
  Same verdict, but only while a RECENT fix still vouches for it. Deliberately a separate function:
  gpsMovingConfirmed() above is LATCHED - the streak resets only when a fresh STATIONARY fix arrives,
  so once fixes stop coming (garage, tunnel) it stays true indefinitely.

  This is now measured with gpsFixAgeMs(), not gd->ts. The previous version's comment claimed ts was
  "device millis() stamped when the fix was decoded, so this is a true staleness bound" - it is not,
  on the path this device actually uses. ts is stamped when a $GNIFO reply is parsed and the
  co-processor re-echoes its last fix on every request, so the bound never fired and every caller
  believed it had staleness protection it did not have.

  Two things that cost, both found in the field:
    - the alarm gate's garage-then-charge exposure (ALARM_GNSS_MAX_AGE_MS), where latched movement
      would permit polling on a locked, armed car;
    - the is_parked latch, which the old comment here waved off as "fails open, and the latch has an
      independent time-based re-latch". It does not: the GNSS unlatch branch refreshes
      g_last_speed_ms on every cycle, so the 5-minute re-latch can never expire. A car driven into a
      garage and plugged in therefore reported is_parked=0 and, since is_charging is gated on the
      latch, charging_state NOT_CHARGING at -52 A - for as long as the wake lasted.

  No fix at all, and a GNSS that was never powered this wake, both resolve to "not moving": unknown
  is not evidence of movement.
*/
static bool gpsMovingFresh(unsigned long max_age_ms) {
  if (!gd || !gpsMovingConfirmed()) return false;
  return gpsFixAgeMs() <= max_age_ms;
}

// Capture the parking position on the is_parked 0->1 transition (from changeState() and the time-based
// re-latch). Prefers the mean of the stationary samples collected since the car stopped moving; falls back to
// the instantaneous fix when too few samples exist (e.g. a very short stop). Never overwrites a good frozen
// fix with garbage (keeps the previous one if there is no fix at all).
void freezeParkedLocation() {
  if (park_acc_n >= PARK_ACC_MIN) {
    freezeAtParkMean();
#if ENABLE_LOGGING
    logEvent("PARKED_LOC frozen lat=%.6f lon=%.6f avg_of=%u hdop=%d",
      frozen_lat, frozen_lon, (unsigned)park_acc_n, gd ? gd->hdop : 0);
#endif
  } else if (gpsHasFix()) {
    setFrozenLocation(gd->lat, gd->lng, gpsLocationUtc());
#if ENABLE_LOGGING
    logEvent("PARKED_LOC frozen lat=%.6f lon=%.6f single (only %u stationary sample(s)) hdop=%d",
      gd->lat, gd->lng, (unsigned)park_acc_n, gd->hdop);
#endif
  } else {
#if ENABLE_LOGGING
    logEvent("PARKED_LOC kept previous (no valid fix this drive)");
#endif
  }
  resetParkAccum();   // start a fresh average for the next stop
}

static const char* gps_location_source = "omitted";
static const char* gpsLocationSource() { return gps_location_source; }

void addGpsData(OBDVector& vec) {
  gps_location_source = "omitted";
  scrubGeoMetadata(vec);
  // Keep refining the parking average whenever we hold a live fix, so the freeze has something to average.
  if (g_gps_powered) parkAccumSample();

  // Parked: reuse the FROZEN fix - a GNSS that just powered up on a parked wake can't re-stabilize in the few
  // seconds we're awake, so its fresh reading is worse than the averaged one we captured while stopped (and on
  // a stationary sample wake we don't even power the GNSS).
  if (is_currently_parked && !drive_unconfirmed) {  // UNCONFIRMED (#6) publishes the live fix like DRIVING
    // ...EXCEPT when the GNSS itself says we are moving. is_currently_parked stays latched until a poll returns
    // vehicle speed > 0, so on a motion wake - or any drive where the car's speed PID is unavailable - this
    // branch would keep publishing the LAST PARKING SPOT while the car is actually under way, teleporting the
    // vehicle backwards on the map. A live fix that shows movement is always the truth, so prefer it. Reporting
    // nothing is also better than reporting a stale position, hence no frozen fallback while moving.
    if (gpsIsMoving()) {
      gps_location_source = "gnss";
      vec.emplace_back("lon", gd->lng);
      vec.emplace_back("lat", gd->lat);
      vec.emplace_back("__geo_loc_utc", gpsLocationUtc());
      if (last_gd.date != gd->date || last_gd.time != gd->time) {
        setClockFromGps();
        last_gd = *gd;
      }
      return;
    }
    if (frozen_valid) {
      gps_location_source = "frozen";
      vec.emplace_back("lon", frozen_lon);
      vec.emplace_back("lat", frozen_lat);
      vec.emplace_back("__geo_loc_utc", frozenGpsUtc());
    } else if (g_gps_powered && gpsHasFix()) {
      // Cold-boot fallback: no frozen fix yet, but we did power GNSS this wake and it has one - use it.
      gps_location_source = "gnss";
      vec.emplace_back("lon", gd->lng);
      vec.emplace_back("lat", gd->lat);
      vec.emplace_back("__geo_loc_utc", gpsLocationUtc());
    }
    return;
  }

  if (!gd) return; // GPS re-init can fail after a wake (e.g. in a garage), leaving gd null - don't deref it
  if (gd->lat != 0 && gd->lng != 0 && ( last_gd.lat != gd->lat || last_gd.lng != gd->lng )) {
#if PRINT
    Serial.printf("Adding coords to vec\n");
#endif
    gps_location_source = "gnss";
    vec.emplace_back("lon", gd->lng);
    vec.emplace_back("lat", gd->lat);
    vec.emplace_back("__geo_loc_utc", gpsLocationUtc());
  }
  // Heading = GNSS course-over-ground (gd->heading, already parsed from RMC/VTG). Emit ONLY when actually
  // MOVING: at standstill COG is undefined noise, so we'd otherwise feed ABRP a spinning map marker. Gating
  // on GNSS speed (same source as the heading, so they're consistent) means we include it for essentially
  // the whole drive - even slow sections - and omit it only when stopped. When omitted, ABRP holds the last
  // heading we sent (its normal behavior), which locks the last good direction in place. API path only: the
  // BLE serializer strips heading (the phone reports its own) - see bleTlmString. Requires a live fix, so a
  // no-GPS wake (parked returns above; garage = lat/lng 0) naturally sends nothing.
  if (gd->lat != 0 && gd->lng != 0 && gd->speed * 1.852f >= HEADING_MIN_SPEED_KMH && gd->heading != GPS_HEADING_INVALID) {
    vec.emplace_back("heading", (double)gd->heading);
    vec.emplace_back("__geo_head_utc", gd->heading == geo_accepted.heading ? gpsLocationUtc() : 0);
  }
  if (last_gd.date != gd->date || last_gd.time != gd->time) { // refresh clock whenever the GNSS time advances
    setClockFromGps();
    last_gd = *gd;
  }
}
