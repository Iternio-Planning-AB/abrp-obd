#pragma once
/*
  Energy region of the state machine (#23, step 3 #25): the charge latch and its release, the taper bridge,
  the DC session, the fast-cadence budget and cap, corroboration, the follow-up and the post-drive window.

  The storage keeps its names and its definitions in logger.ino until #27; only the functions in this file
  assign it (tests/run_sm_checks.py enforces it). The next-wake cadence (g_next_wake_minutes, g_cadence_reason)
  is not this region's yet: pdwArmAtLadderEnd sets it here, as the gate and the poll do, until step 4b owns it.

  Included into logger.ino's single translation unit before sleep_power.h, so it relies on the logEvent
  declaration above the includes and declares what it calls from later files. One #pragma once and no
  project include: the state-machine harness inlines it.
*/
#if ENABLE_IO36_RAIL && ENABLE_SLEEP && ENABLE_MEMS
float readIO36Rail();  // Helpers/sleep_power.h
#endif
#if OBD_ALARM_GATE
bool alarmEnforced();  // Helpers/alarm_gate.h
#endif

void resetChargeCorroboration() {
  charge_uncorroborated_s = 0;
  charge_corr_last_us = 0;
  charge_corr_soc_hw = -1.0;
  charge_corr_kwh_hw = -1.0;
  charge_corr_soe_hw = -1.0;
  dc_session = false;  // every path that drops the old charge's state drops its DC latch
}
static const uint64_t POST_DRIVE_WINDOW_US = (uint64_t)POST_DRIVE_WINDOW_MINUTES * 60000000ULL;
bool pdwActive(uint64_t now_us) {
  return pdw_open && now_us >= pdw_open_us && now_us - pdw_open_us < POST_DRIVE_WINDOW_US &&
    pdw_polls < (uint8_t)POST_DRIVE_MAX_POLLS;
}
// A pause inside one session keeps the charge's budget, cap and corroboration; only this old is it dropped.
bool chargeStateStale(uint64_t now_us) {
  return now_us < charge_last_evidence_us ||
    now_us - charge_last_evidence_us >= (uint64_t)CHARGE_STALE_RESET_MINUTES * 60000000ULL;
}
// Other images can lay RTC memory out differently, so only a deep-sleep wake trusts these.
void chargeWakeStateSanitize(bool deep_sleep) {
  if (deep_sleep) return;
  charge_followup = pdw_open = pdw_polls = 0;
  pdw_open_us = charge_last_evidence_us = 0;
  dc_session = false;
}

// Open the post-drive window (POST_DRIVE_WINDOW_MINUTES) if the rail is high now, once per wake. Only after an
// awake period on the parked cadence: a latched, capped or recovery cadence was chosen for a reason.
// At a healthy but low rail within the window of the last motion, open it from now for the re-check (#22).
bool pdwOpenWindow() {
  if (g_pdw_ladder_done) return false;
  g_pdw_ladder_done = true;
  g_pdw_recheck = false;
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
#if ENABLE_IO36_RAIL && ENABLE_SLEEP && ENABLE_MEMS
  // The DC-DC already off at park: a plug-in after the sleep shows no rail rise to WOM. Only a real reading in the
  // healthy band (never a weak 12 V), and only near motion: not after the max-awake backstop or a long BLE session.
  if (POST_DRIVE_RECHECK_MINUTES < WAKE_PARKED_MINUTES && rail >= IO36_LOCKOUT_HIGH && rail < RAIL_AWAKE_VOLTS &&
      idle_us < POST_DRIVE_WINDOW_US) {
    pdw_open_us = now_us;
    pdw_open = 1;
    g_pdw_recheck = true;
  }
#endif
#if ENABLE_LOGGING
  logEvent("POSTDRIVE window %s wake=%s rail=%.2f idle=%lus",
    g_pdw_recheck ? "recheck" : pdw_open ? "opened" : "closed", g_wake_reason, rail,
    (unsigned long)(idle_us / 1000000ULL));
#endif
  return pdw_open;
}
// End of the awake ladder: an open window comes back on the charging cadence, a re-check's once a little later.
void pdwArmAtLadderEnd() {
  if (!pdwOpenWindow()) return;
  g_next_wake_minutes = g_pdw_recheck ? POST_DRIVE_RECHECK_MINUTES : WAKE_CHARGING_MINUTES;
  g_cadence_reason = g_pdw_recheck ? "pdw-recheck" : "pdw";
}
