#pragma once
/*
  Wake gate of the state machine (#23, step 4a #26): setup()'s ATRV gate, which decides from the rail, the ATRV read
  and the wake cause whether this wake polls the car or sleeps with the modem never powered. gateDecide is the pure
  decision; wakeGateRun applies it, with the OBD-alarm veto between the decision and the poke. Only the functions here
  assign the gate's persisted state (skips_count, polls_count, heartbeat_asleep_streak, last_poll_epoch,
  g_weak_rail_cold_boot; tests/run_sm_checks.py enforces it); its storage stays in logger.ino and sleep_power.h until
  #27. Not owned: the poke-lifetime flags g_sample_poke, g_taper_confirm and g_rail_before_poke, which the poll and the
  DC hold also write (#27), and the next-wake cadence, which the skip arms through sleep_paths.h (#29). One #pragma
  once and no project include, as the harness inlines it. Included after alarm_gate.h: it uses sleep_power.h's
  statics and enterDeepSleep, logEvent and g_alarm_flag.
*/

struct GateInputs {
  float rail;  // TRUE rail (IO36) - the trustworthy battery signal
  bool awake, is_motion, is_cold, heartbeat_due, recently_charging;
  uint32_t taper_confirm_wakes;
  bool charge_end_polled;
};
struct GateDecision {
  bool weak_rail, rail_awake, taper_bridge, give_up_taper, charge_end_poll, wants_poke, should_poll;
};

// The poll decision before the alarm veto. The charge state is read before energyOnRailAwake() resets the taper
// budget and the charge-end read: that runs only on a rail-awake wake, where the taper branch does not.
GateDecision gateDecide(const GateInputs& in) {
  GateDecision d = {};
  // Low-rail lockout: rail in [LOW, HIGH) = weak-but-alive battery -> suppress ALL polling so we don't push
  // a recoverable 12V over the edge. rail < LOW = bench/no-car (allowed to run, so USB DUMP works); rail >=
  // HIGH = healthy. Uses the TRUE rail because ATRV is unreliable (diverges up to 3.6V).
  d.weak_rail = (in.rail >= IO36_LOCKOUT_LOW && in.rail < IO36_LOCKOUT_HIGH);

  // RAIL-AWAKE: the true rail is high -> the car's DC-DC is on (charging / remote climate) -> it's genuinely
  // awake and WILL answer OBD with real data. This is the main parked-period data source. We sample ONCE
  // then sleep (g_sample_poke); the next wake re-checks the rail, so a charge is followed at the wake
  // cadence WITHOUT holding the car awake - we sleep between samples, so the rail can truthfully fall when
  // the charge ends (a free exit, no rail-drop masking). A confirmed DC charge is the exception: it stays awake
  // (DC_CHARGING_AWAKE_SECONDS) and every poll re-reads the rail that ends it.
  d.rail_awake = in.rail >= RAIL_AWAKE_VOLTS;

  // TAPER BRIDGE: the last poll saw charging but the rail has since dropped into the taper band (below
  // RAIL_AWAKE yet >= RAIL_TAPER_FLOOR). Rail alone can't tell a taper from a parked car here, so spend a
  // confirm-poll to read the current sign. Bounded by taper_confirm_wakes. If the rail is below the floor
  // (weak/collapsed) or the budget is spent, give up: it's not a recoverable taper -> clear the latch.
  if (in.recently_charging && !d.rail_awake && !d.weak_rail) {
    if (in.rail >= RAIL_TAPER_FLOOR && in.taper_confirm_wakes < (uint32_t)TAPER_CONFIRM_MAX_WAKES) {
      d.taper_bridge = true;             // confirm-poll; the loop updates the budget from SoC progress
    } else {
      // Rail fell out of the taper band: read the car once more so the charge end and final SoC reach ABRP
      // (CHARGE_END_POLL_FLOOR). A spent stall budget gets no extra read - those polls already reported.
      d.charge_end_poll = !in.charge_end_polled && in.rail < RAIL_TAPER_FLOOR && in.rail >= CHARGE_END_POLL_FLOOR;
      d.give_up_taper = true;
    }
  }

  // Decide whether to poll. A plain TIMER wake on a PARKED car (rail ~12.6) does NOT poll - polling a parked
  // Scenic returns nothing useful and only costs a voltage sag. We poll only for a real reason, never on a
  // weak rail:  cold boot | motion + awake (a drive) | rail high (charging/climate) | taper bridge (confirm
  // a tapering charge whose rail dropped) | charge end | optional parked heartbeat. ATRV's 8V threshold is just a
  // collapse detector.
  d.wants_poke = d.rail_awake || d.taper_bridge || d.charge_end_poll || in.heartbeat_due;
  d.should_poll = !d.weak_rail && (in.is_cold || (in.is_motion && in.awake) || d.wants_poke);
  return d;
}

// The ATRVGATE line's action, from the decision and the poll and poke that followed the alarm veto.
const char* gateActionText(const GateDecision& d, bool should_poll, bool sample_poke) {
  return !should_poll ? (d.weak_rail ? "skip(weak-rail)" : "skip")
       : !sample_poke ? "poll"
       : d.rail_awake ? "poll(rail-awake)"
       : d.taper_bridge ? "poll(taper-confirm)"
       : d.charge_end_poll ? "poll(charge-end)" : "poll(heartbeat)";
}

#if ENABLE_ATRV_GATE
// Sleep gate. Read the OBD pin-16 voltage (ATRV) - a LOCAL read, NO CAN traffic - to learn whether the car
// is awake WITHOUT poking the bus (poking it is what wakes a sleeping car). Run from setup() BEFORE the modem and
// OBD profile are brought up: if the car is asleep and no heartbeat is due, we go straight back to deep
// sleep having powered NOTHING this wake (no modem, no poll) - the cheapest possible skip. A long-asleep
// car still gets ONE deliberate reading per HEARTBEAT_POLL_MINUTES. The clock comes from the RTC, which
// keeps running across deep sleep (last set from NITZ), so the skip events still carry a valid utc.
// OBSERVE mode (ATRV_GATE_ENFORCE 0) logs the verdict but changes nothing.
void wakeGateRun(esp_sleep_wakeup_cause_t wake_cause) {
  g_last_voltage = obd.getVoltage();
  float v = g_last_voltage;
  float rail = g_last_io36;                           // TRUE rail (IO36) - the trustworthy battery signal
  bool awake = v >= ATRV_AWAKE_VOLTS;                 // ATRV verdict: a COLLAPSE detector only (see gateDecide)
  bool is_motion = (wake_cause == ESP_SLEEP_WAKEUP_EXT0);
  bool is_cold   = (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  time_t now = rtc.getEpoch();
  bool clock_ok = now > 1000000000UL;
  // since_poll is only meaningful once we have a clock AND a baseline stamp; -1 = "not established yet"
  // (this is what printed the raw epoch before - last_poll_epoch was stuck at 0 all run).
  long since_poll = (clock_ok && last_poll_epoch != 0) ? (long)(now - last_poll_epoch) : -1;
  // Backed-off heartbeat interval: base doubled once per consecutive asleep poke, capped. A car parked for
  // days is poked rarely instead of every HEARTBEAT_POLL_MINUTES forever (see heartbeat_asleep_streak).
  // Optional parked time-heartbeat (default OFF - see ENABLE_PARKED_HEARTBEAT). Proven to return nothing
  // useful on this car, so rail-rise + motion are the real triggers; this is kept only for A/B.
#if ENABLE_PARKED_HEARTBEAT
  long hb_interval = (long)HEARTBEAT_POLL_MINUTES * 60;
  for (uint32_t i = 0; i < heartbeat_asleep_streak && hb_interval < (long)HEARTBEAT_POLL_MAX_MINUTES * 60; i++)
    hb_interval *= 2;
  if (hb_interval > (long)HEARTBEAT_POLL_MAX_MINUTES * 60) hb_interval = (long)HEARTBEAT_POLL_MAX_MINUTES * 60;
  bool heartbeat_due = since_poll >= 0 && since_poll >= hb_interval;
#else
  long hb_interval = 0;
  bool heartbeat_due = false;
#endif

  // From here through the rail-before-poke stamp: the decision and its effects (run_energy_checks.py slices it).
  const GateDecision d = gateDecide(GateInputs{rail, awake, is_motion, is_cold, heartbeat_due, recently_charging,
                                               taper_confirm_wakes, charge_end_polled});
  const bool weak_rail = d.weak_rail, rail_awake = d.rail_awake, taper_bridge = d.taper_bridge,
             charge_end_poll = d.charge_end_poll;
  if (rail_awake) energyOnRailAwake();
  if (d.give_up_taper) energyGiveUpTaper(rail, is_motion, charge_end_poll);
  g_taper_confirm = taper_bridge;      // tell the loop this poll is a taper confirm (for the SoC-stall test)
  if (is_motion) energyOnMotionWake();
  bool should_poll = d.should_poll;
#if OBD_ALARM_GATE
  // ALARM GATE (§8.2): on a flagged vehicle a STATIONARY poke - rail-awake charge sample, taper
  // confirm, heartbeat - is exactly the parked-and-armed scenario, and polling it trips the OBD
  // alarm every cadence tick. Suppress the poke and sleep on the normal skip path (modem never
  // powered). Motion and cold-boot wakes still proceed: they only keep the device AWAKE - the
  // actual bus requests are gated per-cycle and per-PID by the §8.3 permit in loop().
  if (alarmEnforced() && should_poll && !is_motion && !is_cold) {
    should_poll = false;
#if ENABLE_LOGGING
    logEvent("ALARMGATE stationary poke suppressed (flag=%d rail=%.2f) - parked+armed scenario",
      g_alarm_flag, rail);
#endif
  }
#endif
  // A "sample poke" = a stationary one-shot read (rail-awake / taper bridge / charge end / heartbeat): poll
  // once, log, sleep, re-check next wake. NOT a drive (motion streams) and NOT a cold boot.
  g_sample_poke = should_poll && d.wants_poke && !is_motion && !is_cold;
  if (g_sample_poke) g_rail_before_poke = rail; // rail BEFORE the poke, vs rail_after in the fast-path
  if (should_poll) polls_count++; else skips_count++;
  const char* verdict = awake ? "AWAKE" : "ASLEEP";   // ATRV-vs-8V (collapse/bench detector)
  const char* action = gateActionText(d, should_poll, g_sample_poke);
#if ENABLE_LOGGING
  if (taper_bridge)
    logEvent("TAPER_CONFIRM attempt=%lu/%d rail=%.2f (recently_charging) -> confirm-poll",
      (unsigned long)taper_confirm_wakes, TAPER_CONFIRM_MAX_WAKES, rail);
  if (charge_end_poll)
    logEvent("CHARGE_END rail=%.2f < taper floor %.2f -> final poll, latch cleared", rail, RAIL_TAPER_FLOOR);
#endif
  Serial.printf("ATRVGATE: atrv=%.2f rail=%.2f -> %s, %s %s\n",
    v, rail, verdict, action, ATRV_GATE_ENFORCE ? "[ENFORCE]" : "[observe]");
#if ENABLE_LOGGING
  // rail_awake and weak_rail follow from rail; logEvent keeps 119 characters. rc= is the live latch, after a
  // give-up cleared it.
  logEvent("ATRVGATE atrv=%.2f rail=%.2f %s %s since_poll=%lds skips=%lu polls=%lu rc=%d %s",
    v, rail, verdict, action, since_poll, (unsigned long)skips_count, (unsigned long)polls_count,
    recently_charging ? 1 : 0, ATRV_GATE_ENFORCE ? "ENFORCE" : "observe");
#endif
  // NOTE: last_poll_epoch is stamped AFTER NITZ (gateStampPoll), not here - on a cold boot the clock isn't valid
  // yet at this point, which is exactly why the heartbeat never fired before.

#if ATRV_GATE_ENFORCE && ENABLE_SLEEP && ENABLE_MEMS
  // Don't poll -> sleep without powering the modem. A COLD BOOT never takes this path, weak rail or not.
  //
  // It used to: the reasoning was that a bench supply reads below LOW, so a cold boot in the weak band had
  // to be a real weak car. That assumption is false in practice - a bench supply or a sagging feed sits
  // squarely in the band (measured 10.95 against the 11.00 threshold), and a genuinely weak car still has
  // an owner standing at it who just plugged the dongle in. Either way the device went silent on BLE for
  // the whole parked cadence, which is where the log-dump, the update and the connection-key reset all
  // live - the reset especially, since it is only accepted within seconds of this very boot.
  //
  // The poll is still refused: should_poll is false, and because loop() never reads it, every bus and GNSS
  // site checks g_weak_rail_cold_boot itself (see its definition; #104). So no OBD request is made, GNSS
  // stays off and the modem is never powered on this path (the loop's offline reconnect skips it too, or an
  // unanswering modem would be powered on). Only the radio comes up, for a bounded window.
  if (!should_poll && weak_rail && wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    g_weak_rail_cold_boot = true;
#if ENABLE_LOGGING
    logEvent("ATRVGATE weak-rail COLD BOOT (rail=%.2f) - no poll, no modem, BLE up for %lus",
      rail, (unsigned long)(WEAK_RAIL_COLD_BOOT_AWAKE_MS / 1000UL));
#endif
    Serial.printf("ATRVGATE: weak rail on a power-on -> staying awake for BLE (no poll, no modem)\n");
  }
  if (!should_poll && !g_weak_rail_cold_boot &&
      (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED || weak_rail)) {
    Serial.printf("ATRVGATE: not polling -> deep sleep (%s)\n", weak_rail ? "weak rail lockout" : "asleep");
#if ENABLE_LOGGING
    logEvent("ATRVGATE skip -> deep sleep (%s, modem never powered)", weak_rail ? "WEAK RAIL" : "asleep");
#endif
    cadenceArm(WAKE_PARKED_MINUTES, CADENCE_TIER); // skip = parked: long cadence
    energyOnSkip();
    enterDeepSleep(false); // modem_on=false: never powered it this wake - never returns
  }
#endif
}

// Heartbeat baseline (THE fix for the heartbeat never firing). setup() calls this only on a wake that's going to
// poll (a skip slept back in the gate), once the clock is valid (NITZ ran). Stamp last_poll_epoch so the heartbeat
// interval measures from this poll. Previously this was attempted IN the gate gated on clock_ok, but the gate runs
// BEFORE NITZ, so on a cold boot clock_ok was false, the stamp never happened, last_poll_epoch stayed 0, and
// heartbeat_due could never become true.
void gateStampPoll() {
  if (clockValid()) last_poll_epoch = rtc.getEpoch();
}
#endif

// Heartbeat back-off bookkeeping, per poll: any successful read means the car is responsive -> reset. A heartbeat
// poke that still finds nothing -> lengthen the interval (see heartbeat_asleep_streak / the gate).
void gateOnPollResult(int obd_count) {
  if (obd_count > 0) heartbeat_asleep_streak = 0;
  else if (g_sample_poke && heartbeat_asleep_streak < 16) heartbeat_asleep_streak++;
}
