#pragma once
/*
  Activity state machine helpers (BLE / ACTIVE / CHARGING / STANDBY / UNINITIALIZED).
  Contains: the per-state upload + poll cadences (getTransmissionDelay / getPollingDelay),
  the state transition handler (changeState), and the movement/BLE-driven state selector
  incl. the max-awake backstop and deep-sleep hand-off (handleActivityState). The
  ActivityState enum itself stays in logger.ino (globals section).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
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
    if (!is_currently_parked) freezeParkedLocation(); // 0->1: capture the stabilized last-driving fix
    is_currently_parked = true;
  }
  transmission_delay = getTransmissionDelay(new_state);
  polling_delay = getPollingDelay(new_state);
}

#if OBD_ALARM_GATE
bool alarmEnforced(); // alarm_gate.h is included after this file
#endif
// Open the post-drive window (POST_DRIVE_WINDOW_MINUTES) if the rail is high now, once per wake. Only after an
// awake period on the parked cadence: a latched, capped or recovery cadence was chosen for a reason.
bool pdwOpenWindow() {
  if (g_pdw_ladder_done) return false;
  g_pdw_ladder_done = true;
#if OBD_ALARM_GATE
  if (alarmEnforced()) return false;
#endif
  if (g_sample_poke || recently_charging || charging_cap_latched || g_next_wake_minutes != WAKE_PARKED_MINUTES)
    return false;
#if ENABLE_IO36_RAIL && ENABLE_SLEEP && ENABLE_MEMS
  const float rail = readIO36Rail();
#else
  const float rail = 0.0f;
#endif
  // Timed from the last motion, not from now: the ladder's own idle minutes count toward the window.
  const uint64_t now_us = esp_rtc_get_time_us();
  const uint64_t idle_us = (uint64_t)(millis() - last_active) * 1000ULL;
  pdw_open_us = idle_us < now_us ? now_us - idle_us : 0;
  pdw_polls = 0;
  pdw_open = rail >= RAIL_AWAKE_VOLTS;
  if (!pdwActive(now_us)) pdw_open = 0;
#if ENABLE_LOGGING
  logEvent("POSTDRIVE window %s wake=%s rail=%.2f idle=%lus", pdw_open ? "opened" : "closed", g_wake_reason, rail,
    (unsigned long)(idle_us / 1000000ULL));
#endif
  return pdw_open;
}
// End of the awake ladder: an open window comes back on the charging cadence.
void pdwArmAtLadderEnd() {
  if (!pdwOpenWindow()) return;
  g_next_wake_minutes = WAKE_CHARGING_MINUTES;
  g_cadence_reason = "pdw";
}

/*
  Use movement and BLE connection to derive what activity state should be set
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
    if (!is_currently_parked) g_last_speed_ms = millis();
  } else {
    active = detectMovement(500);
  }
#if ENABLE_MEMS && ENABLE_LOGGING
  noteMotionWindow(!g_ota_active && !g_logdump_active && current_state != UNINITIALIZED, active);
#endif
  auto now = millis();

  // Safety net: if the console latched maintenance_mode but RESUME was never sent (e.g. the puller dropped
  // the connection or the script was Ctrl-C'd), auto-release after an idle period so the device can sleep
  // again instead of staying awake indefinitely and draining the battery.
  if (maintenance_mode && (now - maintenance_since) > MAINTENANCE_IDLE_MS) {
    maintenance_mode = false;
    Serial.printf("maintenance_mode auto-released after %lus idle\n", MAINTENANCE_IDLE_MS / 1000);
  }

  // Max-awake backstop (glitch guard, mirrors the charging cap). The awake window is held by the motion+speed
  // leash; if it stays awake past MAX_AWAKE_MINUTES while the car is PARKED (accel noise refreshing the leash,
  // a wedged state, or a connected-but-idle phone), force deep sleep so the 12V can't drain. Gated on
  // is_currently_parked so a genuinely long DRIVE (speed keeps us unlatched) is NEVER cut off; a moving car
  // re-wakes via WOM within a second. millis() == time awake this wake (a deep-sleep reboot resets it).
#if ENABLE_SLEEP && ENABLE_MEMS
  // !g_ota_active / !g_logdump_active: this fires on time-awake-since-boot, not idle time, so without
  // them a transfer started late in a long awake window (parked at a charger, or sitting in the car
  // after a >55min drive) would be force-slept mid-flash / mid-dump. It restricts nothing - a driving
  // car is already exempt via is_currently_parked - and both transfers carry their own inactivity
  // timeouts (plus the OTA session cap) to bound the exposure.
  if (!maintenance_mode && !g_ota_active && !g_logdump_active && current_state != UNINITIALIZED &&
      is_currently_parked && millis() > (unsigned long)MAX_AWAKE_MINUTES * 60UL * 1000UL) {
    Serial.printf("MAX_AWAKE backstop: awake %lus while parked -> forcing deep sleep\n", millis() / 1000UL);
#if ENABLE_LOGGING
    logEvent("AWAKE_BACKSTOP awake=%lus parked -> force deep sleep (ceiling=%dmin)",
      millis() / 1000UL, MAX_AWAKE_MINUTES);
#endif
    pdwArmAtLadderEnd();
    enterDeepSleep(true, "backstop");
  }
#endif

  switch (current_state) {
  case BLE:
    // Sticky for the session: the instant the phone passes the ABD8 proof it IS the transport, and the
    // handoff bookkeeping in changeState() above may act on its departure. Latched rather than read live
    // because by the time we leave this state the phone is already gone and bleRelaying() is false again.
    if (bleRelaying()) g_ble_session_relayed = true;
#if BLE_CONNECTED_HOLDS_AWAKE
    // A live BLE session = a person actively using the app at the car. Hold the leash so pairing/Save/
    // charge-watching isn't cut mid-session (field log: the motion leash slept the device under an
    // actively-streaming session at ~3min; the phone saw reason=0x16).
    //
    // RELAYING, not merely connected. With the whole GATT table encryption- and authorization-gated, a
    // peer that never authorizes can read and write nothing - so letting it refresh this leash only
    // lets a stranger (or a phone whose user declined the pairing dialog) hold the car awake for the
    // full MAX_AWAKE_MINUTES backstop for no benefit. Keying on bleRelaying() drops that to
    // ACTIVE_TIMER + CHARGING_TIMER (~10 min) and then sleeps normally.
    //
    // Deliberately NOT the vestibule idea of dropping such a connection: the dongle cannot tell a
    // camper from a user mid-pairing-decision, and dropping the link makes the app's poller reconnect
    // and re-raise the prompt every cycle. Sleeping instead stops advertising entirely, so the worst
    // case becomes one prompt per wake rather than one every poll tick.
    //
    // The connect transitions below stamp last_active, so a new session always gets a full ACTIVE_TIMER
    // to complete its handshake - without that, a phone connecting when the leash was nearly expired
    // could fall out of this state mid-handshake, and CHARGING deliberately never revives on a
    // connection alone.
    if (bleRelaying()) {
      last_active = now;
    }
#endif
    if (!bleConnected()) {
      changeState(ACTIVE);
    } else if (now - last_active > ACTIVE_TIMER) {
      changeState(CHARGING);
    }
    break;

  case ACTIVE:
    if (bleConnected()) {
      // Stamp the leash on the way in so the new session gets a full ACTIVE_TIMER to authorize before
      // the relaying check above starts governing it (see the BLE case).
      last_active = now;
      changeState(BLE);
    } else if (now - last_active > ACTIVE_TIMER) { //Inactive for 5 minutes
      changeState(CHARGING);
    }
    break;
  case CHARGING:
    // Removed the "connected -> BLE" revive: a connection ALONE must not pull us back to the awake state (that
    // was the stay-awake-while-connected behaviour). Real motion still revives; otherwise progress to sleep.
    if (active) {
      changeState(ACTIVE);
    } else if (now - last_active > CHARGING_TIMER) {
      changeState(STANDBY);
    }
    break;
  case STANDBY:
    // Removed the "connected -> BLE" revive (see CHARGING). Motion revives; otherwise sleep, dropping BLE.
    if (active) {
      changeState(ACTIVE);
    } else {
#if ENABLE_SLEEP && ENABLE_MEMS
      // No movement or connection: drop to deep sleep until a WOM (door/motion) event or periodic timer.
      // Suppressed during a serial maintenance session so a USB dump isn't cut off mid-stream. modem_on=true:
      // the modem was powered this wake, so flush buffered frames and power it down on the way out.
      if (!maintenance_mode) {
        pdwArmAtLadderEnd();
        enterDeepSleep(true); // never returns - the chip reboots into setup() on wake
      }
#endif
    }
    break;
  case UNINITIALIZED:
    if (bleConnected() && config_initialized) {
      last_active = now; // same handshake grace as the ACTIVE -> BLE transition above
      changeState(BLE);
    } else if (config_initialized) {
      // Factory (or BLE-pushed) profile is loaded -> run standalone over cellular; a BLE connect promotes to
      // the BLE transport state. (Was #if !ENABLE_BLE, which left the device idle until a phone pushed a
      // profile - now cellular and BLE coexist.) Every wake here has passed the ATRV gate (asleep cars skip
      // earlier), so ACTIVE is correct; the gate, not a separate state, decides whether a parked car is polled.
      changeState(ACTIVE);
    } else {
      if (!config_message_printed) {
        Serial.printf("Awaiting initialization\n");
        config_message_printed = true;
      }
    }
    break;
  default:
    //This cannot be reached
    break;
  }
}
