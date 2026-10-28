#pragma once
/*
  OBD-alarm gate (IGNITION_SENSE_ARCHITECTURE.md §8, §10) - keeps an alarm-equipped vehicle's OBD
  port SILENT while the car might be armed. Field-proven premise (§2): the alarm triggers on active
  bus traffic, not on passive presence, so silence-while-armed is a complete fix.

  THE PERMIT (§8.3) is a pure instantaneous predicate - deliberately no latch, no grace period, no
  hold state. The obvious "hold while the rail stays energised" reproduces the exact failure this
  gate exists to prevent: an EV parked and plugged in keeps its DC-DC (and therefore the rail) up,
  so a latch would keep polling a locked, armed car. Equally deliberate: the accelerometer is NOT a
  permit input - stepping out, closing the door and locking all produce motion, so IMU-held permits
  extend through precisely the arming sequence. WOM stays a wake source only.

    permit = flag-off                       (gate inert - stock behaviour, the add-on contract)
           | setup-session window active    (a human just drove the wizard - see below)
           | rail energised AND in-transit  (driving; a towed car never energises the rail)

    in-transit = GNSS confirmed-moving, fix no older than ALARM_GNSS_MAX_AGE_MS
                                            (the only way to ENTER a driving permit)
               | fresh OBD road speed >= ALARM_SPEED_HOLD_KMH   (SUSTAINS one across a GNSS outage)

  The GNSS term is AGE-BOUNDED (gpsMovingFresh, not gpsMovingConfirmed). The movement streak only
  resets on a fresh STATIONARY fix, so where fixes stop arriving it latches true forever - and a
  latched "moving" plus a rail that comes back up when the owner plugs in is a permit to poll a
  locked, armed car in a garage. Staleness turns movement into non-evidence, so it must expire.

  THE SPEED-HOLD TERM exists because GNSS dies in tunnels and multi-storey car parks, and without it
  a flagged car went silent for the rest of the drive. It is deliberately a CONTINUATION term, never
  an entry term, and the asymmetry is the whole safety argument: road speed can only be learned by
  polling, and polling requires a permit, so the hold can only ever extend a permit GNSS already
  granted. It cannot bootstrap one. Consequence worth knowing: a drive that STARTS underground (GNSS
  never acquired) still polls nothing until the car reaches open sky - that case is not covered, and
  covering it would need an entry term that does not depend on already polling.
  Four things keep the hold from outliving the drive: the threshold is far above parking-manoeuvre
  speed, the hold is short (ALARM_SPEED_HOLD_MS), it lives INSIDE the rail AND - so switching the
  car off collapses the rail and denies immediately, without waiting for the hold to expire - and a
  SUSTAINED collapse (ALARM_RAIL_COLLAPSE_MS) additionally VOIDS the stored hold, so a charger
  bringing the rail back up minutes later cannot resurrect it. Denying and voiding are separate
  decisions on purpose: denying is instant and self-correcting, voiding is destructive (only a
  completed poll can re-arm the hold, and that needs a permit) and so demands confirmation.

  SETUP-SESSION WINDOW: the pairing/re-pairing wizard runs with the user stationary in the car, and
  its Save button requires a live PID read - which the driving term can never satisfy while parked.
  A completed FULL profile push (CLEAN -> SETUP OK, the config-apply block in loop()) is proof a
  human is actively driving the wizard at the car, so it grants a bounded permit. The background
  reconnect fast path (HASH OK) deliberately does NOT qualify: the phone auto-reconnects from a
  pocket, so "any authenticated BLE traffic" would re-create the 2 a.m. garage alarm.

  FLAG (§10): one copy, dongle-owned, NVS. Written only by the save screen via ABD2 key
  "obd_alarm" (authenticated writes only - the ABD2 NOAUTH gate runs first). Published read-only in
  the ABD5 info JSON so the app initialises its toggle from the stored value instead of silently
  flipping a deliberate "No" back to the default. Unset resolves to ALARMED (fail-safe, §10.3) -
  which is benign for a factory-fresh dongle because without a persisted profile it does not poll
  at all (nothing to ask, nowhere to send - see the no-factory-fallback change in setup()).

  TASK DISCIPLINE (same rule as OTA / log dump / auth gate): the ABD2 hook runs on the Bluedroid
  task and only stashes the value; alarmGateService() on the loop task does the NVS write and the
  ABD5 republish.

  Only the abrpobd lane is involved. The generic obdble lane never reaches any of this.
*/

#if OBD_ALARM_GATE

// Raw stored flag: -1 = never written, 0 = user said not alarmed, 1 = user said alarmed.
// The RESOLVED value (alarmFlagged) treats -1 as alarmed - see §10.3.
int g_alarm_flag = -1;
// Mailbox from the Bluedroid task (ABD2 "obd_alarm" write): -1 = empty, else 0/1 to persist.
static volatile int g_alarm_flag_pending = -1;
// Setup-session permit deadline (millis). 0 = never granted this wake. Wakes are minutes long and
// every wake is a fresh boot, so millis() wrap is not a concern here.
static unsigned long g_alarm_setup_permit_until = 0;
// Last time the phone was seen connected, stamped by alarmGateService() on the loop task. Backs the
// "is the human still here" half of the setup permit (see ALARM_PERMIT_BLE_GRACE_MS).
static unsigned long g_alarm_ble_seen_ms = 0;
// Speed-hold deadline (millis): set by alarmNoteSpeed() when a fresh OBD road speed clears
// ALARM_SPEED_HOLD_KMH. Sustains an already-granted driving permit through a GNSS outage.
static unsigned long g_alarm_speed_hold_until = 0;
// First millis() of the current run of sub-threshold rail readings; 0 = rail is currently healthy.
// Backs the sustained-collapse test that voids the speed hold (see ALARM_RAIL_COLLAPSE_MS).
static unsigned long g_alarm_rail_low_since = 0;
// No persisted vehicle profile -> nothing to poll with (set in setup(), cleared by a config apply).
bool g_no_profile = false;
// Profile loaded but obd.initialize() deferred until the first permitted cycle (§8.4: on a flagged
// car the OBD link is not brought up at all until a permit exists).
bool g_obd_needs_init = false;

void otaPublishInfo(); // Helpers/ota_update.h - included after this file in the sketch TU

/*
  Resolved flag: does this vehicle's OBD port trip its alarm, so bus traffic has to be rationed?

  Unset (-1) resolves to NO. That is a deliberate REVERSAL of §10.3, which resolved unknown to
  ALARMED on the argument that a false permit (transmitting while armed) is unrecoverable while a
  false deny is merely invisible - so buy false denies cheaply. That argument is still true in
  isolation; what changed is who bears it. Most cars do not have this behaviour, and defaulting them
  all to rationed polling costs every one of them the parked-period data the dongle exists to
  collect, to protect the few. So the restriction now applies only where a human has said this car
  needs it, and an unconfigured dongle behaves exactly like one with no gate at all.

  Consequence to keep in view: a user with an affected car who has not yet set the toggle gets one
  alarm before they know to set it. The app is what has to close that gap - that is a prompt/onboarding
  problem, not something this predicate can solve, and pretending otherwise by defaulting to silence
  is what made the feature unusable for everyone else.
*/
bool alarmFlagged() {
  return g_alarm_flag == 1;
}

/* THE question every behavioral site must ask. The stored per-vehicle flag IS the switch now - there
   is no compile-time bypass any more (OBD_ALARM_ENFORCE is gone). It stayed 0 while three
   enforcement-breaking bugs were open; the two that made an opted-in car unusable are fixed (the
   ABD2 re-AUTH misparse and the GNSS one-shot - see Appendix J root causes 1 and 3), and the rest
   are documented as affecting only a vehicle whose owner deliberately opted in.

   Bookkeeping paths (alarmGateLoad, alarmGateService, the ABD5 publish) deliberately do NOT ask
   this - they run either way, so the flag can be read back and cleared even on a flagged car. */
bool alarmEnforced() {
  return alarmFlagged();
}

/* Loop task, setup(): load the stored flag before the wake gate runs. Opened READ-WRITE on
   purpose: a read-only open of a namespace that has never been written logs a red
   "nvs_open failed: NOT_FOUND" from Preferences on every boot of every fresh dongle - RW creates
   the namespace silently the first time and is a plain open thereafter. */
void alarmGateLoad() {
  preferences.begin("alarm", false);
  g_alarm_flag = preferences.getInt("flag", -1);
  preferences.end();
  Serial.printf("ALARMGATE flag=%d (%s)\n", g_alarm_flag,
    g_alarm_flag == -1 ? "unset -> not alarmed (default: poll freely)"
                       : g_alarm_flag == 1 ? "ALARMED - bus traffic rationed" : "not alarmed");
#if ENABLE_LOGGING
  logEvent("ALARMGATE flag=%d (%s)", g_alarm_flag,
    g_alarm_flag == -1 ? "unset->off" : g_alarm_flag == 1 ? "ALARMED" : "off");
#endif
}

/* Bluedroid-task hook (wired via ble.set_alarm_flag_hook): stash only, no flash access here. */
void alarmFlagHook(int v) {
  g_alarm_flag_pending = v ? 1 : 0;
}

/* Loop task, called from the config-apply block on EVERY completed full push (including the
   identical-profile re-push - a re-pair with an unchanged profile is still a human at the car and
   its wizard still needs the live PID test). */
void alarmGrantSetupPermit() {
  g_alarm_setup_permit_until = millis() + (unsigned long)ALARM_SETUP_PERMIT_MINUTES * 60000UL;
#if ENABLE_LOGGING
  logEvent("ALARMGATE setup-session permit granted (%d min - full profile push completed)",
    ALARM_SETUP_PERMIT_MINUTES);
#endif
}

static bool alarmSetupPermitActive() {
  if (g_alarm_setup_permit_until == 0 || millis() >= g_alarm_setup_permit_until) return false;
  // ...and the human must still be here. Without this the permit survived the owner finishing the
  // wizard, switching off, locking and walking away - polling a locked, armed car for the rest of
  // the window. The grace absorbs the 0-3s reconnect flaps the field logs show, so a blip during
  // the wizard does not kill the session's live PID test.
// bleAuthenticated(), not bleConnected(): connected AND authenticated. Mere presence would let an
// unpaired phone camping nearby satisfy the "human is here" half of a live setup permit. Nor
// bleRelaying(): an app whose relay acks lapsed (#90) is still a person with the app at the car.
#if ENABLE_BLE
  if (bleAuthenticated()) { g_alarm_ble_seen_ms = millis(); return true; }
  return g_alarm_ble_seen_ms != 0 && (millis() - g_alarm_ble_seen_ms) <= ALARM_PERMIT_BLE_GRACE_MS;
#else
  return true;
#endif
}

/* Is a fresh high-speed OBD reading still vouching for us? Continuation only - see the header. */
static bool alarmSpeedHoldActive() {
  return g_alarm_speed_hold_until != 0 && millis() < g_alarm_speed_hold_until;
}

/*
  Feed a road speed (km/h) from a completed poll. Called from the poll cycle's existing speed scan.
  Only readings at or above the threshold extend the hold; slower ones are ignored rather than
  cancelling it, because a momentary stop in traffic inside a tunnel is still a live drive - the
  hold expiring on its own, or the rail collapsing, is what ends it.
*/
void alarmNoteSpeed(double kmh) {
  if (kmh >= (double)ALARM_SPEED_HOLD_KMH) g_alarm_speed_hold_until = millis() + ALARM_SPEED_HOLD_MS;
}

/*
  THE permit predicate (§8.3/§8.4) - pure function of its inputs, default-deny. Every transmit
  decision must route through this; never shortcut it on a wake path ("we woke on X therefore
  poll" is the scattered-gate failure §8.4 forbids).
*/
bool alarmGatePermit(float rail, bool moving) {
  if (!alarmEnforced()) return true;           // gate inert (unflagged, or enforcement bypassed)
  if (alarmSetupPermitActive()) return true;   // human actively at the car (wizard session)
  // Driving. Both in-transit sources sit INSIDE the rail AND, so a switched-off car is denied the
  // instant the rail collapses no matter what the speed hold still says. Voiding that stored hold is
  // a SEPARATE, destructive decision and deliberately does NOT happen here - see
  // alarmGateAllowsPoll(), which requires a sustained collapse. Keeping this function a pure
  // function of its inputs is what makes it testable and order-independent (§8.4).
  return rail >= RAIL_AWAKE_VOLTS && (moving || alarmSpeedHoldActive());
}

/* Fast per-PID guard for OBDWrapper (hard stop BETWEEN reads, §8.5 - readPIDs blocks for seconds,
   so a cycle-boundary check alone leaves polls in flight through the switch-off-and-lock window).
   Silent by design: called per PID; the per-cycle site below does the logging. */
/* The two live permit inputs, read once so the per-PID guard and the per-cycle site agree. */
static float alarmReadRail() {
#if ENABLE_IO36_RAIL
  // LIVE read, not the cached g_last_io36. The cache is refreshed once per poll cycle, so a guard
  // reading it re-evaluated byte-identical inputs for every PID and could never revoke mid-frame -
  // the "hard stop between reads" was a no-op. readIO36Rail() is a handful of ADC samples with no
  // delay and no bus traffic, so calling it per PID is cheap against the 1s-per-PID OBD timeout,
  // and it is what lets a collapsing rail (ignition off) actually cut a frame short.
  //
  // NO hysteresis on purpose, and the margin is thin - measured on the Scenic over 1016 field
  // samples: MOVING sits at 13.4-15.2V (mode 13.4, min 13.25, 1/636 below the 13.3 threshold),
  // switched-off sits at 12.5-13.2V. The two bands very nearly touch, so there is no room for a
  // release threshold below RAIL_AWAKE_VOLTS that would still detect ignition-off. Consequence: a
  // ~0.2%-per-read chance of cutting a frame short mid-drive, which costs a couple of PIDs on that
  // cycle and self-corrects on the next - the safe direction to err.
  // CALIBRATE PER VEHICLE before trusting the driving permit on a new alarmed car: confirm its
  // rail-while-driving and rail-while-off-and-locked actually separate at RAIL_AWAKE_VOLTS. If they
  // overlap, the rail term is not a usable permit input on that car.
  return readIO36Rail();
#else
  return 0.0f; // no true-rail sense -> the rail term fails -> deny (unknown => silence)
#endif
}

/* Movement, AGE-BOUNDED. Uses gpsMovingFresh, not gpsMovingConfirmed: the latter latches true once
   fixes stop arriving, which underground turns into a permanent "we are driving" (see
   ALARM_GNSS_MAX_AGE_MS). Long GNSS outages are meant to be carried by the speed hold instead. */
static bool alarmMovingNow() {
#if ENABLE_GPS
  return gpsMovingFresh(ALARM_GNSS_MAX_AGE_MS);
#else
  return false;
#endif
}

bool alarmPollGuard() {
  // Short-circuit BEFORE the ADC read. alarmGatePermit() would return true for an unflagged vehicle
  // anyway, but only after this function had already paid for a live rail read on every single PID.
  // Unflagged cars must cost exactly nothing - that is the add-on contract.
  if (!alarmEnforced()) return true;
  return alarmGatePermit(alarmReadRail(), alarmMovingNow());
}

/*
  Per-cycle gate for the poll site, WITH the diagnostics §10.3 demands: one line per wake plus one
  per verdict change. Transition-only logging has a hole - a wake where the gate never permits
  produces no transition, and "went quiet with an empty log" is exactly the shape that gets
  misattributed to the cellular stack for weeks.
*/
bool alarmGateAllowsPoll() {
  if (!alarmEnforced()) return true;  // gate inert: no ADC read, no state, no log noise

  float rail = alarmReadRail();
  bool moving = alarmMovingNow();

  // SUSTAINED-COLLAPSE test - the only place the stored speed hold is voided, and deliberately not
  // inside alarmGatePermit() (which stays pure). Denying on a single sub-threshold read is safe and
  // self-correcting; VOIDING is not, because the hold can only be re-armed by a completed poll,
  // which needs a permit - so one ADC dip mid-tunnel, with no GNSS to fall back on, would lock
  // polling out for the rest of the tunnel. Requiring the rail to stay down for
  // ALARM_RAIL_COLLAPSE_MS separates that dip from a real switch-off, which lasts tens of seconds.
  if (rail < RAIL_AWAKE_VOLTS) {
    if (g_alarm_rail_low_since == 0) g_alarm_rail_low_since = millis();
    if (millis() - g_alarm_rail_low_since >= ALARM_RAIL_COLLAPSE_MS) g_alarm_speed_hold_until = 0;
  } else {
    g_alarm_rail_low_since = 0;
  }

  bool permit = alarmGatePermit(rail, moving);
  static bool logged_this_wake = false;
  static bool last_permit = false;
  static unsigned long last_deny_log_ms = 0;
#if ENABLE_LOGGING
  /*
    Re-log periodically WHILE DENIED, not just on transitions.

    One line per wake plus transitions was the whole diagnostic for this gate, so a 60-minute
    all-deny drive produced a single line and no record of which term was failing as conditions
    changed - the exact "subsystem goes quiet with an empty log" shape the logging exists to prevent
    (Appendix J, diagnostics hole). It is also the only way to see root cause 4 in the field: a car
    whose driving rail sits at 13.33-13.47 V against a 13.3 V threshold gets a permanent rail-term
    deny that is indistinguishable from "not driving" without the per-term values over time.

    Only while denied. A permitted drive logs once and stays quiet.
  */
  bool deny_heartbeat = !permit && (millis() - last_deny_log_ms >= ALARM_DENY_RELOG_MS);
  if (!logged_this_wake || permit != last_permit || deny_heartbeat) {
    if (!permit) last_deny_log_ms = millis();
    // tx= is the audit trail. It counts CO-PROCESSOR COMMANDS, which is a superset of CAN frames:
    // most init/AT commands only configure the ELM locally and never reach the bus. So a nonzero
    // delta is not by itself proof of a leak - but the counter must NOT advance between two
    // consecutive DENY lines, and if it does the delta size says which path is responsible.
    logEvent("ALARMGATE %s flag=%d rail=%.2f moving=%d speed_hold=%d setup_permit=%d tx=%lu",
      permit ? "PERMIT" : "DENY", g_alarm_flag, rail, moving ? 1 : 0,
      alarmSpeedHoldActive() ? 1 : 0,
      alarmSetupPermitActive() ? 1 : 0,
      (unsigned long)obd.tx_count);
    logged_this_wake = true;
  }
#endif
  last_permit = permit;
  return permit;
}

/* Loop-task service: persist a pending flag write, then republish the ABD5 info JSON so the app's
   read-back sees the new value immediately. Cheap flag test when idle. */
void alarmGateService() {
  // Stamp phone presence FIRST, unconditionally - this runs every loop iteration and is what keeps
  // the setup permit alive (see alarmSetupPermitActive). It must not sit behind the early return
  // below, or the stamp would only update on the rare iterations that carry a pending flag write.
#if ENABLE_BLE
  if (bleAuthenticated()) g_alarm_ble_seen_ms = millis();
#endif
  if (g_alarm_flag_pending < 0) return;
  int v = g_alarm_flag_pending;
  g_alarm_flag_pending = -1;
  if (v == g_alarm_flag) return; // idempotent re-write (e.g. a retried save) - nothing to do
  preferences.begin("alarm", false);
  preferences.putInt("flag", v);
  preferences.end();
  g_alarm_flag = v;
  Serial.printf("ALARMGATE flag written: %d\n", v);
#if ENABLE_LOGGING
  logEvent("ALARMGATE flag=%d written via ABD2 (save screen)", v);
#endif
#if ENABLE_OTA && ENABLE_BLE
  otaPublishInfo(); // refresh the readable ABD5 JSON (carries "alarm") for the app's read-back
#endif
}

#else
// Stubs so call sites need no #if guards. alarmFlagged/alarmFlagHook are deliberately NOT inline:
// ble.h forward-declares them (non-inline) before this file is reached in the TU, and GCC rejects
// a function declared inline after a prior non-inline declaration/use.
inline void alarmGateLoad() {}
inline void alarmGateService() {}
inline void alarmGrantSetupPermit() {}
inline bool alarmGateAllowsPoll() { return true; }
inline bool alarmPollGuard() { return true; }
inline void alarmNoteSpeed(double) {}
bool alarmFlagged() { return false; }
bool alarmEnforced() { return false; }
void alarmFlagHook(int) {}
bool g_no_profile = false;
bool g_obd_needs_init = false;
#endif // OBD_ALARM_GATE
