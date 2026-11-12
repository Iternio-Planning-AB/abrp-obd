#pragma once
/*
  The awake ladder (#29, step 4b of #23): ACTIVE -> CHARGING (the second idle stage) -> STANDBY -> deep sleep, the BLE
  state, the session holds, the max-awake backstop, the DC hold, and the poll/upload cadence of each state.
  ActivityState is declared in logger.ino.

  ladderStep() and backstopDue() are pure: they take the holds and the phone, each read once per call, and primitive
  observations. handleActivityState() is the glue: the motion window, the console's auto-release, the logging and the
  sleeps. Both ladder sleeps arm the post-drive window first (energy_state.h owns it). The OTA/dump block stays in
  enterDeepSleep: a refused sleep returns, and the ladder simply ticks again.

  The only writer of the ladder's state: LADDER_STORAGE in tests/run_sm_checks.py lists it and checks it. The storage
  stays in logger.ino until #27.

  Included into logger.ino's single translation unit; shares its globals and config.h flags. One #pragma once and no
  project include: the state-machine harness inlines it.
*/

// Upload cadence (ms between transmissions) for each activity state.
unsigned long getTransmissionDelay(ActivityState state) {
  unsigned long delay{};
  switch (state) {
  case BLE:
    // Zero because a relaying phone is the transport and the cellular upload is suppressed anyway
    // (!bleRelaying() in loop()), so this value never actually fires. It is NOT a licence to POST
    // continuously: the state is entered on mere connection, so before AUTH lands nothing suppresses the
    // upload and this zero would let it run every loop iteration. loop() therefore substitutes the ACTIVE
    // cadence while connected-but-unauthenticated - see upload_delay there.
    delay = 0;
    break;
  case ACTIVE:
    // 50s upload cadence while driving. The POLL stays at 5s (getPollingDelay below), so 50 s worth
    // of points - about ten - is buffered and shipped together. The points themselves are
    // unchanged: each carries the utc it was captured at, so the server still receives one sample per
    // 5s and the trip reconstructs identically. What changes is DELIVERY, and with it the cost: one
    // TLS session and a handful of POSTs per minute instead of a session every 5s.
    //
    // Was 5s, chosen so each point shipped the moment it was captured. The trade being made here is
    // freshness of the NEWEST point - up to 50s old on arrival rather than ~0-1s - which is what the
    // app's "last seen" and live route planning read. Everything already tolerates a gap of this size
    // (an ordinary cell reconnect is longer), but it is the thing to watch if live planning starts
    // looking stale mid-drive.
    //
    // Catch-up after an outage is still uploadBuffered's burst, not this cadence - see
    // UPLOAD_MAX_BATCHES_PER_CYCLE, which had to grow to stay ahead of the new steady-state fill rate.
    //
    // 50 s, not 60 s (#8): the upload fires on the first poll past this delay, so delay + one poll must stay
    // under the ~60 s at which the cellular path drops the reused session (SESSION_IDLE_MAX_MS). ~20 % more POSTs.
    delay = 50000;
    break;
  case CHARGING:
    // Its uploads come with its 60 s poll whatever this says, so each one reopens the session through
    // SESSION_IDLE_MAX_MS instead of reusing it.
    delay = 60000; // 1 minute
    break;
  case STANDBY:
    delay = 60000 * 25; //25 minutes
    break;
  default:
    delay = 0;
  }
  return delay;
}

// Poll at most every three seconds

unsigned long getPollingDelay(ActivityState state) {
  unsigned long delay{};
  switch (state) {
  case BLE:
    delay = 3000;
    break;
  case ACTIVE:
    delay = 5000;
    break;
  case CHARGING:
    delay = 60000; // 1 minute
    break;
  case STANDBY:
    delay = 60000 * 25; //25 minutes
    break;
  default:
    delay = 0;
  }
  return delay;
}

/*
  Update variables based on new state
*/

void changeState(ActivityState new_state) {
#if PRINT
  Serial.printf("Changing state from: %d to %d\n", current_state, new_state);
#endif
  // Leaving the BLE state = the phone just dropped. Stamp it so cellular holds off for BLE_UPLOAD_GRACE_MS and
  // doesn't start a TLS POST into the reconnect window (which collides with the handshake -> http=0). current_state
  // is still the OLD state here (before the assignment below).
  // Only for a session that ACTUALLY relayed (g_ble_session_relayed): both the hold-off and the handoff
  // describe handing the transport back from a phone that had it. An unauthenticated peer never had it, and
  // letting it stamp a hold-off on every connect/disconnect flap starves cellular for as long as it retries.
  if (current_state == BLE && new_state != BLE && g_ble_session_relayed) {
    g_ble_disconnect_ms = millis();
    g_ble_handoff_pending = true;
  }
  if (new_state == BLE) {
    g_ble_handoff_pending = false;  // phone came back: it relays again, no handoff needed
    g_ble_session_relayed = false;  // new session, and it has not authorized yet - earn it again
  }
  logging_message_printed = false;
  current_state = new_state;
#if ENABLE_LOGGING
  // Mark every activity-state transition. STANDBY/CHARGING here -> a deep sleep is imminent; HEARTBEAT/ACTIVE
  // -> the device is awake and will poll the car. The trail of these is the core of the behaviour timeline.
  logEvent("STATE -> %s", stateName(new_state));
#endif
  // Re-latch is_parked once the vehicle has been motionless long enough to leave ACTIVE
  // (-> CHARGING/STANDBY). Pairs with the speed > 0 unlatch in loop() so the parked state is
  // restored without depending on a deep-sleep reboot to reset the global.
  if (new_state == CHARGING || new_state == STANDBY) {
    driveIdle(); // 0->1: freezes the stabilized last-driving fix
  }
  transmission_delay = getTransmissionDelay(new_state);
  polling_delay = getPollingDelay(new_state);
}

#if OBD_ALARM_GATE
bool alarmEnforced(); // alarm_gate.h is included after this file
#endif

// DC_CHARGING_AWAKE_SECONDS. Without DC_EARLY_HOLD, motion within an ACTIVE_TIMER suspends the hold.
bool dcMotionRecent() { return g_last_motion_ms && millis() - g_last_motion_ms < (unsigned long)ACTIVE_TIMER; }
// Why a confirmed DC charge does not hold the wake awake right now, read live on every use; null when it does.
const char* dcAwakeVeto(ActivityState state) {
  if (state == BLE) return "ble";                           // the phone's session keeps its own cadence
  if (g_last_io36 < DC_HOLD_RAIL_VOLTS) return "rail";      // the DC-DC is not up
#if OBD_ALARM_GATE
  if (alarmEnforced()) return "alarm";                      // a flagged car is never held awake parked
#endif
#if !DC_EARLY_HOLD
  if (dcMotionRecent()) return "motion";
#endif
  return nullptr;
}
bool dcAwakeHeld(ActivityState state) { return g_dc_confirmed && !dcAwakeVeto(state); }
// A held charge polls and uploads every DC_CHARGING_AWAKE_SECONDS. Read at the poll and upload gates: the
// state delays are snapshots taken at changeState(), and the hold changes between transitions.
unsigned long dcAwakeDelay(ActivityState state, unsigned long state_delay) {
  return dcAwakeHeld(state) ? (unsigned long)DC_CHARGING_AWAKE_SECONDS * 1000UL : state_delay;
}
// Log the hold's edges: on every loop pass and after every poll, so a live veto is seen when it fires.
void dcAwakeTrack() {
  const bool held = dcAwakeHeld(current_state);
  const char* veto = dcAwakeVeto(current_state);
#if ENABLE_LOGGING
  if (held && !g_dc_logged) {
    logEvent("DC_AWAKE on rail=%.2f fast=%lumin", g_last_io36, (unsigned long)charging_fast_minutes);
  } else if (!held && g_dc_logged) {
    g_dc_refused_logged = true;
    const char* why = veto ? veto : g_cadence_reason ? g_cadence_reason : "not-dc";
#if ENABLE_MEMS
    if (veto && !strcmp(veto, "motion"))
      logEvent("DC_AWAKE off reason=motion rail=%.2f peak=%.3f axmax=%.3f", g_last_io36, motion_window_peak,
        motion_window_axmax);
    else
#endif
      logEvent("DC_AWAKE off reason=%s rail=%.2f", why, g_last_io36);
  } else if (!held && g_dc_confirmed && veto && !g_dc_refused_logged) {
    logEvent("DC_AWAKE refused reason=%s rail=%.2f", veto, g_last_io36);
    g_dc_refused_logged = true;
  }
#endif
  g_dc_logged = held;
}

// The session holds, read once per handleActivityState() call on the loop task, after the motion window: a transfer
// that starts during it counts in this pass.
struct AwakeHolds {
  bool ota;          // g_ota_active: a BLE firmware update is transferring
  bool dump;         // g_logdump_active: a BLE log dump is running
  bool maintenance;  // maintenance_mode: a serial console session (auto-released below; RESUME also clears it)
};
AwakeHolds awakeHolds() { return {g_ota_active, g_logdump_active, maintenance_mode}; }

// The phone, read once per call right before the rung, in the order the rung reads it: relaying, authenticated,
// connected.
struct AwakePhone {
  bool relaying;       // the phone is the transport
  bool authenticated;  // passed the ABD8 proof: a person with the app is at the car
  bool connected;
};
AwakePhone awakePhone() {
  AwakePhone p;
  p.relaying = bleRelaying();
  p.authenticated = bleAuthenticated();
  p.connected = bleConnected();
  return p;
}

// One tick's rung. stamp: refresh the leash (last_active) before the change; relayed: latch g_ble_session_relayed
// before it, which changeState() reads on leaving BLE.
struct LadderStep {
  // Prefixed: Arduino.h defines CHANGE.
  enum Move : uint8_t { MOVE_STAY, MOVE_CHANGE, MOVE_SLEEP, MOVE_AWAIT_CONFIG } move;
  ActivityState to;
  bool stamp, relayed;
  void change(ActivityState next) { move = MOVE_CHANGE, to = next; }
};

/*
  The rung for this tick. idle_ms: time since the leash, which also times CHARGING. dc_held: dcAwakeHeld() in this
  state. configured: a profile was loaded or none persisted (config_initialized).
*/
LadderStep ladderStep(ActivityState state, const AwakeHolds& h, const AwakePhone& phone, bool motion,
                      unsigned long idle_ms, bool dc_held, bool configured) {
  LadderStep s{LadderStep::MOVE_STAY, state, false, false};
  switch (state) {
  case BLE:
    // Sticky for the session: the instant the phone passes the ABD8 proof it IS the transport, and the
    // handoff bookkeeping in changeState() may act on its departure. Latched rather than read live
    // because by the time we leave this state the phone is already gone and bleRelaying() is false again.
    s.relayed = phone.relaying;
#if BLE_CONNECTED_HOLDS_AWAKE
    // A live BLE session = a person actively using the app at the car. Hold the leash so pairing/Save/
    // charge-watching isn't cut mid-session (field log: the motion leash slept the device under an
    // actively-streaming session at ~3min; the phone saw reason=0x16).
    //
    // AUTHENTICATED, not merely connected. With the whole GATT table encryption- and authorization-gated, a
    // peer that never authorizes can read and write nothing - so letting it refresh this leash only
    // lets a stranger (or a phone whose user declined the pairing dialog) hold the car awake for the
    // full MAX_AWAKE_MINUTES backstop for no benefit. Keying on bleAuthenticated() drops that to
    // ACTIVE_TIMER + CHARGING_TIMER (~10 min) and then sleeps normally. Not bleRelaying(): an app whose
    // relay acks lapsed (#90) is still a person at the car, and only the transport moves to cellular.
    //
    // Deliberately NOT the vestibule idea of dropping such a connection: the dongle cannot tell a
    // camper from a user mid-pairing-decision, and dropping the link makes the app's poller reconnect
    // and re-raise the prompt every cycle. Sleeping instead stops advertising entirely, so the worst
    // case becomes one prompt per wake rather than one every poll tick.
    //
    // The connect transitions below stamp the leash, so a new session always gets a full ACTIVE_TIMER
    // to complete its handshake - without that, a phone connecting when the leash was nearly expired
    // could fall out of this state mid-handshake, and CHARGING deliberately never revives on a
    // connection alone.
    s.stamp = phone.authenticated;
#endif
    if (!phone.connected) s.change(ACTIVE);
    else if ((s.stamp ? 0UL : idle_ms) > ACTIVE_TIMER) s.change(CHARGING);
    break;
  case ACTIVE:
    if (phone.connected) {
      // Stamp the leash on the way in so the new session gets a full ACTIVE_TIMER to authorize before
      // the relaying check above starts governing it (see the BLE case).
      s.stamp = true;
      s.change(BLE);
    } else if (idle_ms > ACTIVE_TIMER) {  // inactive for 5 minutes
      s.change(CHARGING);
    }
    break;
  case CHARGING:
    // A connection ALONE must not pull us back to the awake state; real motion revives, otherwise progress to sleep.
    // A confirmed DC charge holds here, awake (DC_CHARGING_AWAKE_SECONDS); WAKE_CHARGING_MAX_MINUTES bounds it.
    if (motion) s.change(ACTIVE);
    else if (idle_ms > CHARGING_TIMER && !dc_held) s.change(STANDBY);
    break;
  case STANDBY:
    // Motion revives; otherwise sleep, dropping BLE. Not during a serial maintenance session, so a USB dump isn't
    // cut off mid-stream.
    if (motion) s.change(ACTIVE);
    else if (!h.maintenance) s.move = LadderStep::MOVE_SLEEP;
    break;
  case UNINITIALIZED:
    // A loaded (or BLE-pushed) profile runs standalone over cellular; a BLE connect promotes to the BLE transport
    // state. Every wake here has passed the ATRV gate (asleep cars skip earlier), so ACTIVE is correct; the gate,
    // not a separate state, decides whether a parked car is polled.
    if (phone.connected && configured) {
      s.stamp = true;  // same handshake grace as the ACTIVE -> BLE transition
      s.change(BLE);
    } else if (configured) {
      s.change(ACTIVE);
    } else {
      s.move = LadderStep::MOVE_AWAIT_CONFIG;
    }
    break;
  default:
    break;
  }
  return s;
}

/*
  Max-awake backstop (glitch guard, mirrors the charging cap). The awake window is held by the motion+speed leash; if
  it stays awake past MAX_AWAKE_MINUTES while the car is PARKED (accel noise refreshing the leash, a wedged state, or a
  connected-but-idle phone), force deep sleep so the 12V can't drain. Gated on the parked latch so a genuinely long
  DRIVE (speed keeps us unlatched) is NEVER cut off; a moving car re-wakes via WOM within a second.
  parked_ms is the wake's parked time, every parked interval added up (driveParkedMs, #133): a drive does not count,
  and a brief DRIVING cannot restart it. awake_ms (millis()) past MAX_AWAKE_ABSOLUTE_MINUTES also trips it, which
  bounds a wake that flips between parked and DRIVING.
  Not during an OTA update or a log dump: parked_ms is not idle time, so a transfer started late in a long parked
  window (at a charger, or sitting in the car) would be force-slept mid-flash / mid-dump. It restricts nothing - a
  driving car is already exempt - and both transfers carry their own inactivity timeouts (plus the OTA session cap)
  to bound the exposure.
  A DC charge the hold would keep (DC_EARLY_HOLD, #128) is exempt, read as ACTIVE so the BLE state is too (its cadence
  stays the app's); WAKE_CHARGING_MAX_MINUTES bounds it.
*/
bool backstopDue(const AwakeHolds& h, ActivityState state, bool parked, bool dc_held, unsigned long parked_ms,
                 unsigned long awake_ms) {
  return !h.maintenance && !h.ota && !h.dump && state != UNINITIALIZED && parked && !(DC_EARLY_HOLD && dc_held) &&
         (parked_ms > (unsigned long)MAX_AWAKE_MINUTES * 60UL * 1000UL ||
          awake_ms > (unsigned long)MAX_AWAKE_ABSOLUTE_MINUTES * 60UL * 1000UL);
}

/*
  One loop pass of the ladder: use movement, the phone and the holds to set the activity state, or sleep.
*/
void handleActivityState() {
  // During a firmware update, skip the 500ms motion poll entirely and hold the awake leash directly. That
  // busy I2C loop runs on EVERY iteration, and since the OTA sector ACKs are emitted from this same task it
  // would cap the transfer at ~2 sectors/s (~8KB/s). Refreshing last_active by hand preserves what the call
  // is otherwise here for - keeping us in BLE/ACTIVE instead of ageing toward CHARGING -> STANDBY -> sleep -
  // and does not depend on BLE_CONNECTED_HOLDS_AWAKE being enabled.
  // The log dump gets the same bypass: window requests are serviced from this task too, so the 500ms
  // blocking motion poll added ~500ms of latency to every ~8KB window (measured ~800ms/window against
  // a ~300ms design before this exemption).
  bool active = false;
  if (g_ota_active || g_logdump_active) {
    last_active = millis();
    // An update can be started mid-drive (it's always user-initiated). Polling is suspended for its duration,
    // so no speed frames arrive and the driving leash would go stale: the time-based parked re-latch would
    // then fire on the first poll afterwards and mislabel a moving car as parked - freezing a bogus "parked
    // location" in the process. Hold the leash instead of letting unobserved time count against it.
    driveSessionHold();
  } else {
    active = detectMovement(500);
  }
  AwakeHolds h = awakeHolds();
#if ENABLE_MEMS
  const bool motion_sampled = !h.ota && !h.dump && current_state != UNINITIALIZED;
  noteDriveMotionWindow(motion_sampled, active);
#if ENABLE_LOGGING
  noteMotionWindow(motion_sampled, active);
#endif
#endif
  auto now = millis();

  // Safety net: if the console latched maintenance_mode but RESUME was never sent (e.g. the puller dropped
  // the connection or the script was Ctrl-C'd), auto-release after an idle period so the device can sleep
  // again instead of staying awake indefinitely and draining the battery.
  if (h.maintenance && (now - maintenance_since) > MAINTENANCE_IDLE_MS) {
    maintenance_mode = h.maintenance = false;
    Serial.printf("maintenance_mode auto-released after %lus idle\n", MAINTENANCE_IDLE_MS / 1000);
  }

#if ENABLE_SLEEP && ENABLE_MEMS
  if (backstopDue(h, current_state, is_currently_parked, dcAwakeHeld(ACTIVE), driveParkedMs(), millis())) {
    Serial.printf("MAX_AWAKE backstop: awake %lus while parked=%lus -> forcing deep sleep\n", millis() / 1000UL,
      driveParkedMs() / 1000UL);
#if ENABLE_LOGGING
    logEvent("AWAKE_BACKSTOP awake=%lus parked=%lus -> force deep sleep (ceiling=%dmin)",
      millis() / 1000UL, driveParkedMs() / 1000UL, MAX_AWAKE_MINUTES);
#endif
    pdwArmAtLadderEnd();
    enterDeepSleep(true, "backstop");
  }
#endif

  const AwakePhone phone = awakePhone();
  const LadderStep step = ladderStep(current_state, h, phone, active, now - last_active, dcAwakeHeld(current_state),
                                     config_initialized);
  if (step.relayed) g_ble_session_relayed = true;
  if (step.stamp) last_active = now;
  switch (step.move) {
  case LadderStep::MOVE_CHANGE:
    changeState(step.to);
    break;
  case LadderStep::MOVE_SLEEP:
#if ENABLE_SLEEP && ENABLE_MEMS
    // No movement or connection: drop to deep sleep until a WOM (door/motion) event or periodic timer.
    // modem_on=true: the modem was powered this wake, so flush buffered frames and power it down on the way out.
    pdwArmAtLadderEnd();
    enterDeepSleep(true); // never returns - the chip reboots into setup() on wake
#endif
    break;
  case LadderStep::MOVE_AWAIT_CONFIG:
    if (!config_message_printed) {
      Serial.printf("Awaiting initialization\n");
      config_message_printed = true;
    }
    break;
  case LadderStep::MOVE_STAY:
    break;
  }
}
