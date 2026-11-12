#pragma once
/*
  The next sleep (#29, step 4b-ii of #23): its cadence (g_next_wake_minutes and g_cadence_reason), the reason the SLEEP
  enter line names, and loop()'s own sleeps: the weak-rail cold boot's window (the car switched on, or nobody came), a
  sample poke's sleep after its one poll, and the net-recover reboot. The ladder's sleeps are state_machine.h's, the
  gate's skip is wake_gate.h's and the crash streak's is setup()'s; each arms its cadence here.

  The only writer of the cadence: CADENCE_STORAGE in tests/run_sm_checks.py checks it. The storage and CadenceReason
  stay in logger.ino until #27, beside declarations for the headers included before this one. The OTA/dump block
  stays in enterDeepSleep: a refused sleep returns, and the glue runs on.

  Included into logger.ino's single translation unit after auth_gate.h (authResetWindowOpen); shares its globals and
  config.h flags. One #pragma once and no project include: the state-machine harness inlines it.
*/

// The texts the SLEEP enter and DC_AWAKE off lines print; CADENCE_TIER has none, its minutes name it.
const char* cadenceReasonName(CadenceReason why) {
  switch (why) {
  case CADENCE_PDW: return "pdw";
  case CADENCE_PDW_RECHECK: return "pdw-recheck";
  case CADENCE_FOLLOWUP: return "followup";
  case CADENCE_PARKED_HELD: return "parked-held";
  case CADENCE_PARKED_CAP: return "parked-cap";
  case CADENCE_PARKED_UNCORROBORATED: return "parked-uncorroborated";
  case CADENCE_TIER: break;
  }
  return nullptr;
}

// The cadence decision, armed whole: the energy region's poll and ladder end, and the gate's skip.
void cadenceArm(int minutes, CadenceReason why) {
  g_next_wake_minutes = minutes;
  g_cadence_reason = why;
}

// The SLEEP enter line's reason=: a sleep that is not a cadence tier passes its own (the recovery reboot's 1 min
// would otherwise read as dc-charge); else the cadence names itself where its minutes are ambiguous (a 3-minute pdw
// or followup wake is not a charge; parked-held, parked-cap and parked-uncorroborated are a charge held slow); else
// the tier the armed minutes name. Read back from what was armed, so the line cannot disagree with the timer.
const char* sleepReasonText(const char* reason, CadenceReason why, int minutes) {
  const char* cadence = cadenceReasonName(why);
  return reason ? reason
    : cadence ? cadence
    : (minutes == WAKE_DC_CHARGING_MINUTES) ? "dc-charge"
    : (minutes == WAKE_CHARGING_MINUTES) ? "charging" : "parked";
}

// A sample poke sleeps after its one poll, unless the console or the DC hold (dc_held) keeps it.
bool samplePokeSleeps(bool poke, bool maintenance, bool dc_held) { return poke && !maintenance && !dc_held; }

#if ENABLE_SLEEP && ENABLE_MEMS
// A sleep that is not a cadence tier: its own minutes, the cadence reason left as armed.
void sleepFor(int minutes, bool modem_on, const char* reason) {
  g_next_wake_minutes = minutes;
  enterDeepSleep(modem_on, reason);
}
#endif

#if ATRV_GATE_ENFORCE && ENABLE_SLEEP && ENABLE_MEMS
// After a poll cycle: the weak-rail cold boot's two sleeps, modem off (it was never powered this wake). A serial
// session (maintenance_mode) holds both.
// First the recovery: a cycle's rail at RAIL_AWAKE_VOLTS says the car switched on, so the DC-DC is charging the 12V and
// the lockout protects nothing, but the veto would keep every cycle off the car for as long as a phone holds the
// window open (#104). RAIL_AWAKE_VOLTS, not IO36_LOCKOUT_HIGH: a bench supply around 11 V is exactly the wake the BLE
// window exists for. A connected phone holds it off only inside the key-reset window: the reset needs a power-on boot,
// so sleeping under a phone mid-reset would refuse it for good (see the static_assert on WEAK_RAIL_COLD_BOOT_AWAKE_MS).
// It runs on every cycle the expiry does, before the expiry can take the parked sleep.
void loopWeakRailSleep() {
#if ENABLE_IO36_RAIL
  if (g_weak_rail_cold_boot && !maintenance_mode && g_last_io36 >= RAIL_AWAKE_VOLTS &&
      !(bleConnected() && authResetWindowOpen())) {
#if ENABLE_LOGGING
    logEvent("WEAK-RAIL cold boot ended: rail=%.2f (car awake) -> deep sleep %dmin", g_last_io36,
      WEAK_RAIL_RECOVERED_WAKE_MINUTES);
#endif
    sleepFor(WEAK_RAIL_RECOVERED_WAKE_MINUTES, false, "weak-rail-recovered");
  }
#endif
  // The window this wake exists for has expired with nobody connected (the BLE state refreshes last_active): sleep on
  // the parked cadence, as the skip path would have at once; a person had a couple of minutes to reach the dongle.
  if (g_weak_rail_cold_boot && !maintenance_mode && !bleConnected() && millis() > WEAK_RAIL_COLD_BOOT_AWAKE_MS) {
#if ENABLE_LOGGING
    logEvent("WEAK-RAIL cold-boot BLE window expired (nobody connected) -> deep sleep %dmin",
      WAKE_PARKED_MINUTES);
#endif
    sleepFor(WAKE_PARKED_MINUTES, false, nullptr);
  }
}

// After a poll cycle and dcPromotePoke(): a sample poke (a parked car's charge sample, not a drive) has its one
// reading. Run the Test C before/after experiment, did the poll wake the car: wait for any ECU power-up, re-read the
// true rail and log it beside the pre-poll rail. Then sleep at once instead of idling down the ladder.
void loopSamplePokeSleep(const EnergyPoll& ep, int obd_count) {
  if (!samplePokeSleeps(g_sample_poke, maintenance_mode, dcAwakeHeld(current_state))) return;
#if ENABLE_IO36_RAIL
  delay(5000);                       // give ECUs time to power up IF the poll woke the car
  float rail_after = readIO36Rail(); // local only - do NOT overwrite g_last_io36, or the SLEEP-enter log
                                     // and cadence decision (top-of-cycle rail) would disagree with it
#else
  float rail_after = 0.0f;
#endif
#if ENABLE_LOGGING
  // SAMPLE_POLL: did the one-shot read of an awake/charging car return real data, and did the rail move?
  // is_charging here lets us confirm the current-sign verdict tracks the rail/SoC; the adjacent telemetry
  // row carries the full soc/power/current values for this same poll.
  // prog/rel are what make the two charge signals separable in a field log: prog=1 is_charging=0 is the
  // exact case this exists for (counters moving, current sample says otherwise), and rel counts down the
  // unconfirmed-release window, so a cadence that relaxed can be traced to which signal gave up when.
  // No running tallies: logEvent keeps 119 characters, and the dump counts these lines and pids=0 itself.
  // Fields: DONGLE_CONTEXT §9.
  const uint64_t pdw_now_us = esp_rtc_get_time_us();
  const unsigned pdw_left = pdwActive(pdw_now_us)
    ? (unsigned)((POST_DRIVE_WINDOW_US - (pdw_now_us - pdw_open_us) + 59999999ULL) / 60000000ULL) : 0;
  logEvent("SAMPLE_POLL pids=%d is_charging=%d prog=%d rel=%u taper=%d stall=%lu rail=%.2f->%.2f hold=%d sil=%u fu=%u pdw=%u",
    obd_count, ep.is_charging ? 1 : 0, ep.charge_progress ? 1 : 0, (unsigned)charge_release_polls,
    g_taper_confirm ? 1 : 0, (unsigned long)taper_confirm_wakes, g_rail_before_poke, rail_after,
    ep.high_rail_silent ? 1 : 0, (unsigned)charge_silent_polls, (unsigned)charge_followup, pdw_left);
#endif
  enterDeepSleep(true); // modem was powered this wake; flush + sleep
}
#endif

#if ENABLE_NETWORK && ENABLE_SLEEP && ENABLE_MEMS
// Escalation after a modem restart: a wedged modem has survived 3 consecutive power-cycles in the field - the only
// recovery that has EVER worked is a fresh ESP32 boot. Take it deliberately through the normal sleep path: the flush
// gets one last try, the BACKLOG persists every frame, and WOM re-wakes within ~a second while driving (timer backstop
// 1 min when parked). Bounded by net_reboot_count (RTC) so a genuine dead-coverage drive reboots at most twice, then
// just buffers.
void netRecoverSleep() {
  if (g_recovers_since_ok < 2 || net_reboot_count >= 2) return;
  net_reboot_count++;
#if ENABLE_LOGGING
  logEvent("NET recover exhausted -> reboot via deep sleep (#%lu, %d frames to backlog)",
    (unsigned long)net_reboot_count, (int)obd_result_vector.size());
#endif
  pdwOpenWindow(); // the reboot ends this awake period
  sleepFor(1, true, "net-recover"); // fresh boot restores the backlog and re-inits everything
}
#endif
