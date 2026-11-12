#pragma once
/*
  Energy region of the state machine (#23, step 3 #25): the charge latch and its release, the taper bridge, the DC
  session, the fast budget and cap, corroboration, the follow-up and the post-drive window. Their storage stays in
  logger.ino until #27; only the functions here assign it (tests/run_sm_checks.py enforces it). Not owned yet:
  g_next_wake_minutes and g_cadence_reason (step 4b-ii, #29; set here too), the poke-lifetime flags g_taper_confirm and
  g_rail_before_poke (#27), g_dc_logged and g_dc_refused_logged (state_machine.h). One #pragma once and no project
  include, as the harness inlines it. Included before sleep_power.h: it relies on logEvent's declaration and declares
  the later files' calls.
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

/*
  The gate's writes to the charge state, in setup() before any poll. The decision to poll stays in the gate until 4a.
*/
// A real high-rail sample resets the taper-bridge budget and re-arms the charge-end read.
void energyOnRailAwake() {
  taper_confirm_wakes = 0;
  charge_end_polled = false;
}
// The taper bridge gives up: the rail fell out of the taper band, or the stall budget is spent. The latch clears;
// a charge-end read (charge_end_poll, decided by the gate) is spent once until a high-rail sample re-arms it.
void energyGiveUpTaper(float rail, bool is_motion, bool charge_end_poll) {
  if (charge_end_poll) charge_end_polled = true;
  recently_charging = false;       // give up: rail too low, or too many SoC-stalled confirm-polls
  taper_confirm_wakes = 0;
  if (chargeStateStale(esp_rtc_get_time_us())) resetChargeCorroboration();
  if (!is_motion) {
    charge_followup = 1;
#if ENABLE_LOGGING
    logEvent("FOLLOWUP armed (latch cleared at rail=%.2f)", rail);
#endif
  }
}
// Somebody is at the car: the charge the follow-up was for is not the one that matters now.
void energyOnMotionWake() {
  if (charge_followup) {
    charge_followup = 0;
#if ENABLE_LOGGING
    logEvent("FOLLOWUP cleared reason=motion");
#endif
  }
}
// A charge that ended via a skip wake resets the fast-cadence budget and the cap latch - once stale: a
// 12 V top-up every few hours must not hand a stuck current a fresh budget each time.
void energyOnSkip() {
  if (chargeStateStale(esp_rtc_get_time_us())) {
    charging_fast_minutes = 0;
    charging_cap_latched = false;
    resetChargeCorroboration();
  }
}
// A held poke promoted by motion (dcPromotePoke) is a streaming wake: a charging sample counts only after
// CHARGE_STREAM_CONFIRM_SECONDS without a discharge sample, as on any streaming wake, so regen is not a charge.
void energyOnPromote() { g_last_discharge_ms = millis(); }
// The early DC release (drive_state.h's driveDcCharge, #128): its DC_RELEASE_POLLS vetted polls are the regen guard,
// so the releasing poll's charge sample is trusted at once, publishing is_charging and is_dcfc.
void energyOnDcRelease() { g_last_discharge_ms = millis() - (unsigned long)CHARGE_STREAM_CONFIRM_SECONDS * 1000UL; }

// What the SAMPLE_POLL line reads from a poll.
struct EnergyPoll {
  bool is_charging;       // the published current-sign verdict
  bool charge_progress;   // the car's own counters rose
  bool high_rail_silent;  // the latch was set and a sample poke at a high rail went unanswered
};
/*
  One loop() poll, after is_parked is appended: the charge verdict (is_charging and is_dcfc join the frame), the
  release latch and silent hold, the taper stall count, the DC session, the next-wake cadence, the fast budget and
  cap, corroboration, the post-drive window's close and the stale guard. The body keeps loop()'s indentation and
  text, clock reads and log lines in place, so it reads as a move against SM_PIN. It takes the frame and logs
  inside rather than returning a pure decision: splitting it waits for a step that needs it (#26, #22).
*/
EnergyPoll energyOnPoll(OBDVector& current_frame, int obd_count) {
    // Feature A - is_charging from the current PID's sign. Charging pushes current INTO the battery (negative,
    // ~-51A); discharge/idle is >= 0. Gate on parked so a drive's regen braking isn't mislabeled. Per-frame.
    // A streaming wake can still be a drive the latch has not caught yet, so there the sample is trusted only
    // after CHARGE_STREAM_CONFIRM_SECONDS without a discharge sample (config.h); until then the frame carries no
    // verdict, which the v2 mapping sends as no chargingState rather than NOT_CHARGING.
    bool is_charging = false;
    bool charging_current_valid = false;
    for (const auto& res : current_frame) {
      if (res.first == "current") {
        // Bounds-guard the cadence-critical read: a garbage current (e.g. the -2.9e8 seen in bad buffer
        // reads) is < CHARGING_CURRENT_THRESHOLD and would falsely flag charging -> trip fast cadence.
        // readPIDs already clamps to the profile's [-2000,2000], but don't depend on it here.
        float c = res.second;
        if (c > -2000.0f && c < 2000.0f) {
          charging_current_valid = true;
          if (c > CHARGE_DISCHARGE_AMPS) g_last_discharge_ms = millis();
          is_charging = is_currently_parked && (c < CHARGING_CURRENT_THRESHOLD);
        }
        break;
      }
    }
    const bool charge_signal_trusted = g_sample_poke ||
      millis() - g_last_discharge_ms >= (unsigned long)CHARGE_STREAM_CONFIRM_SECONDS * 1000UL;
    const bool unconfirmed_charging = is_charging && !charge_signal_trusted;
    if (!charge_signal_trusted) is_charging = false;
    if (!is_currently_parked || (charging_current_valid && charge_signal_trusted))
      current_frame.emplace_back("is_charging", is_charging ? 1.0 : 0.0);

    /*
      CHARGE PROGRESS - the second, independent charge signal (CHARGE_PROGRESS_* in config.h).

      is_charging above is ONE instantaneous current sample, and it is the only one this wake: readPIDs
      does a single read per PID with no retry or averaging, and a sample poke sleeps the moment the frame
      is built. The field case that forced this: a car charging at a measured 3.7kW returned +0.479A on
      that one sample, which relaxed the cadence 3min -> 30min and dropped the taper latch mid-charge.

      SoC and energy progress offer evidence independent of the instantaneous current sample. Estimated
      remaining energy (soe, Wh) needs a wider noise floor than a cumulative kwh_charged counter because
      the BMS can re-estimate it. This decides CADENCE only - published is_charging stays the current-sign verdict,
      because that is what the frame has always meant and changing it changes what the API is told.

      RATCHETED, not last-poll-vs-this-poll. The baseline moves up only when a value actually crosses the
      epsilon, so a coarsely-quantised soc (0.5% steps on some profiles) accumulates across polls instead of
      reading as zero change at every interval. A FALL re-anchors - that is the discharge / per-session-reset
      case, and it is also what stops a high baseline left over from an earlier charge from ever reading as
      progress later.
    */
    auto chargeCounterRose = [&](const char* key, double& baseline, double eps) {
      for (const auto& res : current_frame) {
        if (res.first != key) continue;
        const double v = res.second;
        if (!is_currently_parked || baseline < 0.0) baseline = v; // driving / first sample: anchor only
        else if (v > baseline + eps) { baseline = v; return true; }
        else if (v < baseline)       baseline = v;   // fell: re-anchor (discharge / counter reset)
        break;
      }
      return false;
    };
    // Anchor on EVERY poll, parked or not. A drive's regen raises kwh_charged, so leaving the baseline at a
    // pre-drive value would make the first parked poll after arriving read the regen as a charge.
    const bool soc_rose = chargeCounterRose("soc", prev_parked_soc, CHARGE_PROGRESS_SOC_PCT);
    const bool kwh_rose = chargeCounterRose("kwh_charged", prev_parked_kwh, CHARGE_PROGRESS_KWH);
    const bool soe_rose = chargeCounterRose("soe", prev_parked_soe, CHARGE_PROGRESS_SOE_WH);
    // Same parked gate as is_charging: SoC / energy changes during a drive can include regen, and neither
    // says anything about a charge session.
    const bool charge_progress = is_currently_parked && charge_signal_trusted && (soc_rose || kwh_rose || soe_rose);

    /*
      DC or AC? The driver of a fast-charging car is usually watching progress from somewhere else in
      the building, so a DC session earns a much denser cadence (WAKE_DC_CHARGING_MINUTES) than an AC
      one. This decides that, and also fills in is_dcfc for the uploaded frame - which is how the v2
      chargingState becomes CHARGING_DC instead of the blanket CHARGING_AC it reported before, since
      nothing in the firmware had ever produced is_dcfc and every consumer just read the default.

      THE CAR'S OWN WORD WINS. If the profile provides an is_dcfc PID it is already in current_frame
      by now, and it is better evidence than anything we can infer - so we read it and stop.

      Otherwise infer from POWER. Note the dongle deliberately does not report derived power (it
      reports only what the car provides), and that rule is untouched: this computes V*I to make a
      local DECISION, and publishes no number. 40 kW sits far above any AC session - three-phase 22 kW
      is the practical ceiling, and the -52 A at 381 V measured on this car is ~20 kW - so a frame
      over it is DC with room to spare. Both PIDs are needed; a profile missing either infers nothing
      and the session is treated as AC, which is the safe way to be wrong (a 3-minute cadence on a DC
      charge is a slower update, where 1 minute on an AC charge would be wasted wakes for hours).
    */
    bool is_dcfc = false;
    bool dcfc_from_car = false;
    if (is_charging) {
      double volts = 0, amps = 0;
      bool have_v = false, have_a = false;
      for (const auto& res : current_frame) {
        if (res.first == "is_dcfc") { is_dcfc = res.second != 0; dcfc_from_car = true; break; }
        if (res.first == "voltage") { volts = res.second; have_v = true; }
        else if (res.first == "current") { amps = res.second; have_a = true; }
      }
      if (!dcfc_from_car && have_v && have_a) {
        const double power_kw = fabs(volts * amps) / 1000.0;
        is_dcfc = power_kw >= (double)DC_CHARGING_MIN_KW;
#if ENABLE_LOGGING
        static int logged_dcfc = -1;  // on a change of verdict only: a held DC charge polls every 30 s
        if (logged_dcfc != (int)is_dcfc)
          logEvent("CHARGE %.1fkW (%.1fV x %.1fA) -> %s", power_kw, volts, amps, is_dcfc ? "DC" : "AC");
        logged_dcfc = is_dcfc;
#endif
      }
      // Only published while charging: is_dcfc says nothing on its own, and asserting a charge KIND
      // on a frame that is not charging would be a claim we have no basis for.
      if (!dcfc_from_car) current_frame.emplace_back("is_dcfc", is_dcfc ? 1.0 : 0.0);
    }
    // NOTE: power is deliberately NOT derived here (V*I/1000). Design decision: the dongle reports only what
    // the car/profile actually provides; derived metrics are the app/backend's job. The API's "missing:
    // power" note is informational - uploads are accepted regardless. If a profile later includes a real
    // power PID, it flows through like any other value.

    /*
      The charge verdict everything downstream of the frame runs on: the cadence, the taper latch, and the
      bridge budget. Either signal is enough - a negative current sample OR the car's counters moving.

      RELEASE HYSTERESIS (CHARGE_RELEASE_POLLS). recently_charging used to be a straight copy of is_charging,
      so it self-cleared on the FIRST poll that read current >= 0 - which is precisely the sample that turned
      out to be untrustworthy. One reading now only starts the release; it takes CHARGE_RELEASE_POLLS
      consecutive polls with no evidence at all to actually let go. Holding the latch keeps two things alive
      for that extra poll: the fast cadence (so the confirming poll happens in 3 minutes, not 30) and the
      gate's taper bridge (so a low-rail wake still spends a confirm-poll instead of skipping outright).
    */
    const bool was_recently_charging = recently_charging; // pre-poll latch, read before we overwrite it
    const bool charge_evidence = is_charging || charge_progress;
    // A silent poll (CHARGE_SILENT_HOLD_POLLS) is not evidence the charge ended, so it holds the release. An
    // unconfirmed charging sample restarts it, so only consecutive non-charging samples release. Neither can
    // set the latch.
    // Polled faster than its tier by a DC charge (g_dc_confirmed from the last poll), silence counts by time:
    // one poll per WAKE_DC_CHARGING_MINUTES of it, the tolerance a 1-minute DC wake has, not one per 30 s poll.
    const bool dc_silence = g_dc_confirmed && obd_count == 0;
    const unsigned long dc_tier_ms = (unsigned long)WAKE_DC_CHARGING_MINUTES * 60000UL;
    // Started back-dated by the silent polls already counted (RTC, earlier wakes), so the count never goes down.
    if (!dc_silence) g_dc_silent_since_ms = 0;
    else if (!g_dc_silent_since_ms) g_dc_silent_since_ms = millis() - (unsigned long)charge_silent_polls * dc_tier_ms;
    if (dc_silence) {
      const unsigned long tiers = (millis() - g_dc_silent_since_ms) / dc_tier_ms;
      charge_silent_polls = tiers < (unsigned long)CHARGE_SILENT_HOLD_POLLS ? (uint8_t)tiers : (uint8_t)CHARGE_SILENT_HOLD_POLLS;
    }
    const bool silent_hold = was_recently_charging && obd_count == 0 &&
      charge_silent_polls < (uint8_t)CHARGE_SILENT_HOLD_POLLS;
    // Silence with the DC-DC on is the car, not the charge ending: it never releases, so the rail drop still
    // gets its read. Past the hold it waits at the parked cadence (high_rail_held, below).
    const bool high_rail_unanswered = g_sample_poke && g_rail_before_poke >= RAIL_AWAKE_VOLTS && obd_count == 0;
    const bool high_rail_silent = was_recently_charging && high_rail_unanswered;
    const bool high_rail_held = high_rail_silent && !silent_hold;
    if (obd_count > 0) charge_silent_polls = 0;
    else if (silent_hold) charge_silent_polls++;
    if (charge_evidence || unconfirmed_charging) {
      charge_release_polls = 0;
    } else if (was_recently_charging && !silent_hold && !high_rail_silent && charge_release_polls < 255) {
      charge_release_polls++;
    }
    // Still "charging" for cadence purposes while the release is unconfirmed. Also the taper latch: the gate
    // reads this next wake to decide whether a low-rail wake is worth a confirm-poll.
    recently_charging = charge_evidence ||
      (was_recently_charging && charge_release_polls < (uint8_t)CHARGE_RELEASE_POLLS);
    if (charge_evidence) charge_last_evidence_us = esp_rtc_get_time_us();
    if (was_recently_charging && !recently_charging && g_sample_poke) {
      charge_followup = 1;
#if ENABLE_LOGGING
      logEvent("FOLLOWUP armed (latch released)");
#endif
    }

    if (charge_evidence) {
      // Taper-bridge budget is bound on SoC STALL, not confirm-count: a rising SoC resets it (a real taper
      // bridges forever); a flat/falling SoC on a confirm-poll burns it (stuck sensor, or the 100% balancing
      // tail -> give up). Only confirm-poll wakes (g_taper_confirm) accrue stalls.
      double soc = 0.0;
      for (const auto& res : current_frame) { if (res.first == "soc") { soc = res.second; break; } }
      if (soc > prev_charge_soc)      taper_confirm_wakes = 0;        // SoC progressed -> productive, reset
      else if (g_taper_confirm)       taper_confirm_wakes++;          // confirm-poll with no SoC gain -> stall
      if (soc > 0.0)                  prev_charge_soc = soc;          // remember THIS poll's SoC for next compare
    } else if (!recently_charging) {
      // Released, not merely held by a silent or unconfirmed poll: a held poll resetting these would let an
      // intermittently answering car defeat TAPER_CONFIRM_MAX_WAKES.
      taper_confirm_wakes = 0; // charge over -> reset the bridge budget
      prev_charge_soc = 0.0;   // ...and the SoC baseline, so the NEXT charge compares against its own start
    }

    // Feature B - pick the next-wake cadence. Dense while the charge latch is up, which is now either signal
    // plus the unconfirmed-release window above; it was the bare current sign, and a single bad sample of it
    // cost a 30-minute hole mid-charge. Still NOT keyed on rail: that would MISS the taper, whose rail has
    // dropped to ~13.2. Everything else relaxes to the long parked interval, and the cumulative cap still
    // stops a stuck signal - of either kind - from fast-waking forever.
    // On a progress-only or release-hold poll is_dcfc is false (it is only inferred from a current reading),
    // so those take the AC tier. That is the safe way to be wrong, for the same reason config.h gives: a
    // 3-minute cadence on a DC charge is a slower update, where 1 minute on an AC charge is hours of waste.
    bool cadence_charging = recently_charging;
    // Corroboration: does a counter that moves during any real charge (soc / kwh_charged) back the charge?
    bool has_charge_counter = false;
    bool corroborated = false;
    if (cadence_charging) {
      auto roseAboveHigh = [](double v, double& hw, double eps) {
        if (hw < 0.0)     { hw = v; return false; }   // first sample of this charge: anchor only
        if (v > hw + eps) { hw = v; return true; }
        return false;
      };
      for (const auto& res : current_frame) {
        if (res.first == "soc") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_soc_hw, CHARGE_PROGRESS_SOC_PCT);
        } else if (res.first == "kwh_charged") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_kwh_hw, CHARGE_PROGRESS_KWH);
        } else if (res.first == "soe") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_soe_hw, CHARGE_PROGRESS_SOE_WH);
        }
      }
      if (corroborated) {
#if ENABLE_LOGGING
        if (charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60)
          logEvent("CHARGE corroborated (soc/energy rose) -> charging cadence");
#endif
        charge_uncorroborated_s = 0;
      }
    }
    const bool uncorroborated = charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60;
    const uint64_t now_us = esp_rtc_get_time_us();
    // An answer ends the post-drive window: from here the charge evidence decides.
    if (pdw_open && (obd_count > 0 || !pdwActive(now_us))) {
#if ENABLE_LOGGING
      logEvent("POSTDRIVE window closed reason=%s polls=%u", obd_count > 0 ? "answered"
        : pdw_polls >= (uint8_t)POST_DRIVE_MAX_POLLS ? "spent" : "expired", (unsigned)pdw_polls);
#endif
      pdw_open = 0;
    }
    // A hold, or a release at a sample poke (a pause in the session), keeps the charge's budget, cap and
    // corroboration until its last evidence is stale; a stuck current must not get a fresh budget each time.
    // A streaming wake (a drive) always resets.
    const bool charge_state_stale = chargeStateStale(now_us);
    g_cadence_reason = nullptr;
    // A DC session lives inside one unbroken charge latch: a fresh latch, a drive or an answered poll with no
    // charge evidence (the charge end, or a DC plug swapped for AC) ends it.
    if (!was_recently_charging || !is_currently_parked ||
        (obd_count > 0 && !charge_evidence && !unconfirmed_charging)) dc_session = false;
    if (!is_dcfc || dcfc_from_car) g_dc_inferred_polls = 0;
    else if (g_dc_inferred_polls < 255) g_dc_inferred_polls++;
    g_dc_confirmed = false;
    const unsigned long fast_anchor = g_fast_anchor_ms;
    g_fast_anchor_ms = 0;  // only an unbroken run of fast-cadence polls carries it
    if (!cadence_charging) {
      // Not charging -> long cadence, and clear BOTH the budget and the cap latch (the charge genuinely
      // ended / rail dropped). Only here and in the high-rail hold below does the cap latch clear.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      if (!g_sample_poke || charge_state_stale) {
        charging_fast_minutes = 0;
        charging_cap_latched = false;
        resetChargeCorroboration();
      }
      // A silent car at a high rail may be charging without saying so. Look again soon, a bounded number of
      // times: inside the post-drive window (POST_DRIVE_*), else once after a charge (charge_followup).
      if (high_rail_unanswered && pdwActive(now_us)) {
        pdw_polls++;
        g_next_wake_minutes = WAKE_CHARGING_MINUTES;
        g_cadence_reason = "pdw";
      } else if (high_rail_unanswered && charge_followup) {
        charge_followup = 0;
        g_next_wake_minutes = WAKE_CHARGING_MINUTES;
        g_cadence_reason = "followup";
#if ENABLE_LOGGING
        logEvent("FOLLOWUP spent rail=%.2f -> %dmin", g_rail_before_poke, WAKE_CHARGING_MINUTES);
#endif
      }
    } else if (high_rail_held) {
      // Latched, silent past the hold, rail still high: wait at the parked cadence so the car can sleep
      // between polls. No budget accrues; once stale, a car whose rail never drops leaves no budget, cap or
      // high-water mark for the next charge.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-held";
      if (charge_state_stale) {
        charging_fast_minutes = 0;
        charging_cap_latched = false;
        resetChargeCorroboration();
        prev_charge_soc = 0.0;
      }
    } else if (charging_cap_latched ||
               charging_fast_minutes >= (uint32_t)WAKE_CHARGING_MAX_MINUTES) {
      // Charging, but we've spent the cumulative fast-cadence budget -> latch slow and STAY slow (don't
      // reset the budget here - that's what previously let it bounce straight back to fast).
      charging_cap_latched = true;
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-cap";
    } else if (uncorroborated) {
      // Current says charging, counters say not. Slow, not latched: a later counter rise resets the
      // count above and restores fast cadence.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-uncorroborated";
      charge_corr_last_us = now_us;
    } else {
      // Charging within budget -> dense sampling, denser still on DC. Once this charge has been DC (dc_session)
      // it stays DC through the taper under DC_CHARGING_MIN_KW and through the silent hold, with the rail at
      // DC_HOLD_RAIL_VOLTS; the car's own is_dcfc=0 ends it. g_dc_confirmed asks to stay awake (DC_CHARGING_AWAKE_SECONDS).
      if (is_dcfc && (dcfc_from_car || g_dc_inferred_polls >= 2)) dc_session = true;
      else if (dcfc_from_car) dc_session = false;
      g_dc_confirmed = is_dcfc ||
        (dc_session && g_last_io36 >= DC_HOLD_RAIL_VOLTS && (charge_evidence || silent_hold));
      g_next_wake_minutes = g_dc_confirmed ? WAKE_DC_CHARGING_MINUTES : WAKE_CHARGING_MINUTES;
      // A held poke keeps its other one-shot inputs (see dcPromotePoke), but is no longer a taper confirm:
      // its later polls must not spend the stall budget.
      if (g_dc_confirmed) g_taper_confirm = false;
      // Accumulate MINUTES, not wakes. The budget means "12 hours of fast cadence", and counting
      // wakes x WAKE_CHARGING_MINUTES only expressed that while there was a single fast tier - with a
      // 1-minute DC tier the same count would represent a third of the wall clock, so a long DC
      // session would latch back to the parked cadence three times sooner than intended.
      // A sample poke's poll counts its tier, held awake at DC_CHARGING_AWAKE_SECONDS too (the cap trips sooner).
      // A streaming wake (motion, a promoted poke) polls every few seconds, so after its first fast poll it counts
      // the time it spent: someone moving about the car must not cap a charge.
      if (g_sample_poke || !fast_anchor) {
        charging_fast_minutes += (uint32_t)g_next_wake_minutes;
        g_fast_anchor_ms = millis();
      } else {
        const unsigned long whole = (millis() - fast_anchor) / 60000UL;
        charging_fast_minutes += (uint32_t)whole;
        g_fast_anchor_ms = fast_anchor + whole * 60000UL;
      }
      // A silent poll counts too: no answer is not corroboration, so a car that rarely answers still reaches
      // CHARGE_CORROBORATE_MINUTES on wall-clock time. Only once a counter has answered this charge: a
      // profile without one could never corroborate, and silence alone must not slow a real charge.
      const bool counter_seen = charge_corr_soc_hw >= 0.0 || charge_corr_kwh_hw >= 0.0 || charge_corr_soe_hw >= 0.0;
      if ((has_charge_counter || (obd_count == 0 && counter_seen)) && !corroborated && charge_corr_last_us &&
          now_us > charge_corr_last_us) {
        // Capped at one fast interval, so time before this charge (or a long sleep) is never counted.
        uint64_t d = (now_us - charge_corr_last_us) / 1000000ULL;
        const uint64_t cap = (uint64_t)(WAKE_CHARGING_MINUTES + 1) * 60;
        if (d > cap) d = cap;
        const uint32_t before = charge_uncorroborated_s;
        charge_uncorroborated_s += (uint32_t)d;
#if ENABLE_LOGGING
        if (before < (uint32_t)CHARGE_CORROBORATE_MINUTES * 60 &&
            charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60)
          logEvent("CHARGE uncorroborated %lus (soc/energy not rising) -> parked cadence",
            (unsigned long)charge_uncorroborated_s);
#endif
      }
      charge_corr_last_us = now_us;
    }
    return EnergyPoll{is_charging, charge_progress, high_rail_silent};
}
