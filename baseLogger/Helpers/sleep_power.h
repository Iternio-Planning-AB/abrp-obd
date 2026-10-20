#pragma once
/*
  Power management / deep-sleep helpers.
  Contains: the WOM wake pin + RTC-persisted wake/sleep counters, the true-12V-rail reader
  (readIO36Rail), the reset-reason decoder (resetReasonStr), and enterDeepSleep() - the full
  power-down path (pre-sleep upload flush + backlog save, BLE teardown, modem/GPS power-off,
  Wake-on-Motion arming, adaptive wake timer). softResetMEMS lives in Helpers/mems.h.

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

/*
  Deep-sleep low-power mode (see ENABLE_SLEEP in config.h).
  Requires the ICM-42627 (ENABLE_MEMS) and its INT1 pin wired to GPIO39.
*/
#if ENABLE_SLEEP && ENABLE_MEMS

// ICM-42627 INT1 is hardwired to GPIO39 on the Freematics ONE+. GPIO39 is
// input-only, RTC-capable and has no internal pull resistors; the WOM INT is
// push-pull active-HIGH (LOW at rest, HIGH on a motion event), so no pull is needed.
#define WOM_INT_PIN GPIO_NUM_39

// Survive the deep-sleep reboot so we can log how many wake cycles happened.
RTC_DATA_ATTR static uint32_t bootCount  = 0;
RTC_DATA_ATTR static uint32_t sleepCount = 0;
// Epoch of the last time we actually polled the car (woke/read it). Survives deep sleep (cleared on power
// loss -> 0 -> first wake counts as "due"). Used by the ATRV enforce gate to still grab one reading per
// HEARTBEAT_POLL_MINUTES even while the car is asleep.
RTC_DATA_ATTR static time_t last_poll_epoch = 0;
// Consecutive heartbeat pokes that found the car still asleep (no PIDs). Doubles the effective heartbeat
// interval per step (capped at HEARTBEAT_POLL_MAX_MINUTES); reset to 0 the moment a poll returns data.
RTC_DATA_ATTR static uint32_t heartbeat_asleep_streak = 0;

#if ENABLE_IO36_RAIL
// Read the TRUE 12V rail via the IO36_BatDec divider (averaged, eFuse-calibrated mV -> rail volts). See
// ENABLE_IO36_RAIL. Returns 0 if the pin reads nothing. Logged beside ATRV to answer the brownout question.
float readIO36Rail() {
  uint32_t sum = 0;
  for (int i = 0; i < IO36_RAIL_SAMPLES; i++) sum += analogReadMilliVolts(IO36_RAIL_PIN);
  float pin_mv = (float)sum / IO36_RAIL_SAMPLES;
  float raw = (pin_mv * IO36_RAIL_DIVIDER) / 1000.0f;   // uncalibrated rail volts
  return IO36_RAIL_CAL_A * raw + IO36_RAIL_CAL_B;        // calibrated true battery volts (multimeter fit)
}
#endif

// Decode esp_reset_reason() to a short string for the log (Test B). POWERON/BROWNOUT on a wake we expected
// to be a clean deep-sleep timer/motion wake = an uncommanded reset.
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON:  return "POWERON";
  case ESP_RST_BROWNOUT: return "BROWNOUT";
  case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
  case ESP_RST_SW:       return "SW";
  case ESP_RST_PANIC:    return "PANIC";
  case ESP_RST_INT_WDT:  return "INT_WDT";
  case ESP_RST_TASK_WDT: return "TASK_WDT";
  case ESP_RST_WDT:      return "WDT";
  case ESP_RST_EXT:      return "EXT";
  default:               return "OTHER";
  }
}

/*
  Power down every main peripheral, arm Wake-on-Motion on the ICM-42627 and enter
  ESP32 deep sleep (~10mA, controller + sensor only). Never returns: a WOM event on
  GPIO39 (door open / motion) wakes the chip, which reboots and runs setup() again.
*/
// modem_on: true on a normal sleep (the modem was powered this wake -> flush buffered frames + power it
// down). false on a cheap ATRV-gate SKIP, where we decided the car is asleep BEFORE ever powering the modem
// - so there's nothing to flush and we must NOT call powerOff() (it would PWRKEY-toggle an already-off modem
// back ON). GPS is powered before the gate either way, so it's always shut down here.
void enterDeepSleep(bool modem_on) {
  // HARD BLOCK while a firmware update is transferring. This single guard covers EVERY sleep path in the
  // firmware (the ATRV gate skip, the max-awake backstop, the STANDBY transition, the WDT-streak escalation,
  // the sample-poke fast path and the modem-recovery reboot), which is why it lives at the very top of the
  // one real primitive rather than being duplicated at each call site. It must precede every side effect
  // below - the parked-marker frame, the pre-sleep upload flush, saveUploadBacklog(), the BLE teardown,
  // datalogger.end(), the modem power-off and the WOM arming - because a sleep here would abandon a
  // half-staged image, drop the phone, and (on the boot after an already-committed update) make the
  // bootloader roll a perfectly good firmware back. Every caller simply continues if we return.
  // g_logdump_active gets the same hard block: sleeping mid-dump tears BLE down next to a stationary
  // car (no motion, no WOM), so the app could not even reconnect to retry until the next timer wake.
  // The dump's 30s inactivity guard bounds how long this can defer a sleep.
  if (g_ota_active || g_logdump_active) return;
  sleepCount++;
  Serial.printf("Entering deep sleep (sleep #%lu). WOM threshold %.4fg (%d LSB), wake on GPIO%d HIGH.\n",
    sleepCount, WOM_THRESHOLD_G, WOM_THRESHOLD_LSB, (int)WOM_INT_PIN);

#if ENABLE_NETWORK
  if (modem_on) {
  // Append a final parked marker. The latch flips to parked in changeState() on the way to STANDBY,
  // but we sleep at the top of loop() before another frame can be polled/sent - and the periodic flush
  // may have just cleared the buffer with is_parked:0 frames - so the API would otherwise never learn the
  // trip ended. A fresh frame is truthful (prior buffered frames keep their real is_parked:0; this adds the
  // closing is_parked:1 with no contradictory speed) and tiny (~35 bytes, once per trip). Needs a valid utc,
  // which ABRP requires anyway.
  // Gated on the latch: the modem-recovery reboot sleeps MID-DRIVE (WOM re-wakes immediately), and an
  // unconditional marker there would push a false is_parked:1 into the trip. And only when owed (see
  // park_marker_owed): a parked heartbeat wake ended no trip and has nothing to mark.
  if (rtc.getEpoch() > 1e9 && is_currently_parked && park_marker_owed) {
    OBDVector parked_frame;
    parked_frame.emplace_back("is_parked", 1.0);
    parked_frame.emplace_back("utc", rtc.getEpoch());
    obd_result_vector.push_back(parked_frame);
    park_marker_owed = false;
  }

  // Flush before deep sleep wipes RAM. handleActivityState() can trigger sleep before loop()'s normal send
  // block runs, so frames buffered during CHARGING would otherwise be lost. Best-effort, radios still on.
  //
  // CRITICAL: never block the sleep hunting for signal. Only attempt the upload if the link is ALREADY up
  // (WiFi associated). In a dead spot - e.g. an underground garage - connected() is false, so we skip it
  // entirely instead of stalling for tens of seconds and draining the battery; the frames are safe in
  // LittleFS and go up on the next connected wake. (The POST itself is also bounded by the wrapper timeout.)
  if (!obd_result_vector.empty() && upload_fail_count < MAX_UPLOAD_FAILS_PER_WAKE) {
    // up= is seconds since NET init=up, sampled before connect(): parked flushes ~25 s in failed while ~50 s
    // succeeded. The open trace (see HTTPClientSIM::open) says which step failed and whether the bearer was up.
    if (!network.connected()) {
      Serial.printf("Pre-sleep flush skipped: not registered - %d frame(s) kept in flash\n",
        obd_result_vector.size());
#if ENABLE_LOGGING
      logEvent("FLUSH skipped: not registered - %d kept", (int)obd_result_vector.size());
#endif
    } else {
      int up_s = (int)(network.upForMs() / 1000);
      if (!network.connect()) {
        std::string why = network.lastOpenTrace();
        Serial.printf("Pre-sleep flush skipped: open failed %s - %d frame(s) kept in flash\n", why.c_str(),
          obd_result_vector.size());
#if ENABLE_LOGGING
        logEvent("FLUSH skipped: open failed %s up=%ds - %d kept", why.c_str(), up_s,
          (int)obd_result_vector.size());
        logEvent("FLUSH diag %s", network.netDiag().c_str()); // separates weak signal from slow TLS
#endif
      } else {
        int n = (int)obd_result_vector.size();
        Serial.printf("Flushing %d buffered frame(s) before sleep\n", n);
        bool ok = uploadBuffered(true); // drain the whole buffer in capped batches - last chance before RAM is wiped
#if ENABLE_LOGGING
        logEvent("FLUSH %s http=%d sent=%d left=%d up=%ds %s", ok ? "ok" : "partial", g_last_http,
          n - (int)obd_result_vector.size(), (int)obd_result_vector.size(), up_s,
          network.lastOpenTrace().c_str());
#endif
        network.disconnect();
      }
    }
  } else if (!obd_result_vector.empty()) {
    Serial.printf("Pre-sleep flush skipped: upload backed off - %d frame(s) kept in flash\n",
      obd_result_vector.size());
#if ENABLE_LOGGING
    logEvent("FLUSH skipped: backed off - %d frame(s) kept", (int)obd_result_vector.size());
#endif
  }
  } // if (modem_on)

#if ENABLE_LOGGING
  // Persist whatever the flush could NOT send (dead bearer / no coverage) before RAM is wiped - previously
  // those frames were silently lost. Restored + uploaded on the next polling wake (see loadUploadBacklog).
  saveUploadBacklog();
#endif
#endif

  bleDeinitForSleep(); // tear BLE down BEFORE the log flush + peripheral power-down (no-op if BLE wasn't up)

#if ENABLE_LOGGING
  // Last line before the radios/peripherals go dark and RAM is wiped. Pairs with the next wake's WAKE line;
  // the gap between them is genuine deep sleep (controller + sensor only). Documents the power-down intent
  // so the timeline shows the modem+GPS were commanded off - confirm the actual draw with an ammeter.
#if ENABLE_MEMS && ENABLE_LOGGING
  flushMotionStats();
#endif
#if ENABLE_PERIODIC_WAKE
  // Log the chosen cadence + why, so the adaptive behavior is auditable: timer tightens during a charge and
  // relaxes when parked. (g_next_wake_minutes == WAKE_CHARGING_MINUTES => we observed an active charge.)
  // Three tiers now, so the reason has to name which: dc-charge (1 min), charging (3), parked (30).
  // Reading the cadence back out like this keeps one source of truth - the value that was actually
  // armed - rather than a second copy of the decision that could disagree with it.
  logEvent("SLEEP enter -> %s+GPS off, timer=%dmin reason=%s rail=%.2f (WOM %.3fg)",
    modem_on ? "modem" : "(modem already off)", g_next_wake_minutes,
    (g_next_wake_minutes == WAKE_DC_CHARGING_MINUTES) ? "dc-charge"
      : (g_next_wake_minutes == WAKE_CHARGING_MINUTES) ? "charging" : "parked",
    g_last_io36, WOM_THRESHOLD_G);
#else
  logEvent("SLEEP enter -> %s+GPS off, deep sleep (WOM %.3fg only)",
    modem_on ? "modem" : "(modem already off)", WOM_THRESHOLD_G);
#endif
  datalogger.end(); // flush + close the log segment so no buffered rows are lost across the sleep reboot
#endif

  // Power off the radios/peripherals so only the controller and sensor draw current. CRITICAL: this powers
  // the CELLULAR MODEM down (AT+CPOWD) as well - it was previously left running through deep sleep, which
  // drained the car's 12V battery overnight (see POWER_MANAGEMENT.md). WiFi build: turns the radio off.
  // Skipped on a gate SKIP (modem_on=false): the modem was never powered this wake, and powerOff() would
  // PWRKEY-toggle an already-off modem back ON.
#if ENABLE_NETWORK
  if (modem_on) network.powerOff();
#endif
#if ENABLE_GPS
  if (g_gps_powered) sys.gpsEnd(true); // only if we actually powered GNSS this wake (skip/charging never did)
  // BELT-AND-BRACES, and it must stay the LAST co-processor command before sleep. GNSS power is latched in
  // the CO-PROCESSOR (gpsBegin sends ATGPSON), which the ESP32's deep sleep and reboot do not touch, while
  // g_gps_powered is plain RAM that any reset clears. So a WDT/panic/brownout between gpsBegin() and the
  // next clean sleep leaves GNSS powered with the flag false - and every following gate-skip wake (which
  // never calls gpsBegin) skips the line above, so it stays powered for 30-min sleep after 30-min sleep.
  // ATGPSOFF is idempotent, so sending it unconditionally costs one ~100ms command and closes that hole.
  // Raw command rather than gpsEnd(): without FLAG_GNSS_USE_LINK, gpsEnd takes its other branch and tears
  // down a task/UART driver that was never created this boot.
  { char gb[16]; if (sys.link) sys.link->sendCommand("ATGPSOFF\r", gb, sizeof(gb), 100); }
#endif

  // Arm Wake-on-Motion: latched, push-pull, active-HIGH INT1 with the calibrated
  // WOM thresholds. configureWOM() also reads INT_STATUS2 to clear any stale latched
  // interrupt that would otherwise wake the chip immediately.
  configureWOM(*static_cast<ICM_42627*>(mems));

  // Tell the ESP32 to wake when GPIO39 goes HIGH (the WOM interrupt).
  esp_sleep_enable_ext0_wakeup(WOM_INT_PIN, 1);

#if ENABLE_PERIODIC_WAKE
  // Adaptive timer: g_next_wake_minutes was chosen this wake (Feature B) - short while charging, long while
  // parked. 64-bit math (minutes -> us) so the value can't overflow a 32-bit int.
  esp_sleep_enable_timer_wakeup((uint64_t)g_next_wake_minutes * 60ULL * 1000000ULL);
  Serial.printf("Wake timer armed: %d min\n", g_next_wake_minutes);
#endif

  clean_sleep_marker = true; // Test B: prove the NEXT boot came through our own sleep path, not a brownout
  Serial.flush();
  delay(100);
  clock_sleep_enter_epoch = rtc.getEpoch(); // last: the wake measures the sleep from here (clockCompensateSleep)
  esp_deep_sleep_start();
  // Never reached.
}
#endif
