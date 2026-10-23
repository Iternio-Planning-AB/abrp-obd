#pragma once
/*
  BLE helpers - everything for the phone/app link (BLEWrapper session management).
  Contains: the config-push callback + pending-config hand-off to the loop task
  (configCallback), the BLEWrapper instance, the connection probe (bleConnected), the
  cross-task BLE event log queue writer (bleLogCb), per-wake bring-up/teardown
  (bleInitForWake / bleDeinitForSleep, with no-op stubs when BLE is disabled), and the
  persisted BLE-pushed vehicle profile loader (loadPersistedProfile).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

// Helpers/alarm_gate.h is included AFTER this file in the sketch translation unit (it needs
// gps_location.h). Forward-declare the three names this file uses; same pattern as the
// otaBenchReset/profileSigSelfTest declarations in logging.h.
void alarmFlagHook(int v);
bool alarmFlagged();
bool alarmEnforced();
extern bool g_obd_needs_init;
extern bool g_no_profile;
void dropUnsavedPush(const char* why); // defined with the persisted profile below
bool forgetProfile(const char* why);   // Helpers/auth_gate.h (after network_upload.h); AUTHCLEAR calls it too

/*
  Callbackfunction for BLEWrapper. To be called when config has been completely transferred over BLE
*/

#if ENABLE_BLE
// Hand-off from the Bluedroid task to the loop task. The BLE stack invokes configCallback on ITS OWN task;
// applying the profile there (setVehicleCommands + obd.initialize) raced the loop task mid-readPIDs() on the
// shared OBD UART and the live vehicle_commands json -> echoed/garbage responses (buffer "2291C8", ATRV 0.00),
// out-of-bounds parses, and PANIC/WDT reboots that killed the BLE session. The callback now only validates and
// STASHES the config; loop() applies it between poll cycles, where nothing else touches the OBD link.
// The string is handed over by swap under g_config_mux: the loop's apply runs for seconds (obd.initialize), and
// a second push completing meanwhile must neither tear the string the loop is reading nor become the file the
// first push is persisted as. Only the swap and the flag sit inside the lock - nothing allocates or logs there.
std::string g_pending_config_str;
volatile bool g_config_pending = false;
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;

bool configCallback(json newConfig) {
  try {
    // Validate shape only - no hardware access on the BLE task. Must carry a profile, token and hash.
    (void)newConfig.at("configJson");
    (void)newConfig.at("tlm_push_token").get<std::string>();
    (void)newConfig.at("hash").get<unsigned long>();
    std::string stash = newConfig.dump();
    const int bytes = (int)stash.size();
    portENTER_CRITICAL(&g_config_mux);
    g_pending_config_str.swap(stash);
    g_config_pending = true;
    portEXIT_CRITICAL(&g_config_mux);
    Serial.printf("BLE config stashed (%d bytes) - applied by loop task between polls\n", bytes);
    return true;
  }
  catch (const json::exception& err) {
    Serial.printf("BLE config rejected: %s\n", err.what());
    return false;
  }
}

// Initialization of BLEWrapper, handles all BLE communication with ABRP app

BLEWrapper ble{
  configCallback,
  preferences, // Persistent storage
  rtc // Sync clock
};

#endif

/*
  Returns true only when BLE is enabled and a device is connected.
  Lets the activity state machine compile and behave correctly when BLE is disabled (e.g. WiFi-only).
*/
bool bleConnected() {
#if ENABLE_BLE
  return ble.connected();
#else
  return false;
#endif
}

// TRUE only when a connected phone has ALSO completed the AUTH handshake on ABD2 - i.e. the app is
// actually taking the telemetry and relaying it to the cloud. This, not bleConnected(), is what the
// cellular gates must key on: with the GATT table encryption-gated, a phone that DECLINED the pairing
// dialog (or any stranger) stays connected but can read nothing - and keying cellular off mere presence
// made the dongle buffer silently while such a phone camped (data flowed NOWHERE live; field-found).
// authenticated is session-scoped: set by AUTH OK, cleared on disconnect/deinit - and AUTH can only
// arrive over a paired link, so this is exactly "the phone is a working transport".
bool bleRelaying() {
#if ENABLE_BLE
  return ble.connected() && ble.get_authenticated();
#else
  return false;
#endif
}

#if ENABLE_BLE
#if ENABLE_LOGGING
// Runs in the Bluedroid callback task - and, for the events init() emits, the setup task: only ENQUEUE the
// event here (no flash access - that would race the loop task's datalogger). loop() drains bleLogQueue and
// writes these to the DUMP.
void bleLogCb(const char* m) {
  if (!bleLogQueue) return;
  char tmp[100];
  strncpy(tmp, m, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  xQueueSend(bleLogQueue, tmp, 0); // non-blocking; drop if full rather than stall the BLE task
}
#endif

// Bring BLE up for a person-present wake and record it so enterDeepSleep() tears it back down. Logs free
// INTERNAL heap before/after - this is the Bluedroid + cellular coexistence check (want the "after" > 100 KB).
void bleInitForWake() {
  uint32_t heap_before = ESP.getFreeHeap();
#if ENABLE_LOGGING
  // Wired BEFORE init() on purpose: init() itself logs the pairing/bond state, and a callback set
  // afterwards would send that line to Serial only - invisible in a DUMP, which is where field
  // diagnostics actually get read. Also means the hook exists before any BLE task can fire.
  if (!bleLogQueue) bleLogQueue = xQueueCreate(48, 100); // deep enough for a full profile push burst while driving
                                                         // (32 dropped the tail of a push in the field - two EVT lines lost)
  ble.set_log_callback(bleLogCb);
#endif
  // Bonding policy must be set BEFORE init(), which is what programs SMP and prunes the bond table.
  ble.set_bonding(BLE_ENABLE_BONDING, BLE_MAX_BONDS);
  // Whether a profile VALUE may arrive as a raw package, or only inside a verified JWS.
  ble.set_profile_require_signature(PROFILE_REQUIRE_SIGNATURE);
  // OBD-alarm flag writes (ABD2 "obd_alarm" - the save-screen toggle). The hook only stashes; the
  // NVS persist runs on the loop task (alarmGateService). See the forward declaration at the top.
  ble.set_alarm_flag_hook(alarmFlagHook);
  ble.init();
  ble.set_force_repush(BLE_FORCE_REPUSH); // DIAGNOSTIC: force full profile re-push on connect (see config.h)
  g_ble_up = true;
  uint32_t heap_after = ESP.getFreeHeap();
  Serial.printf("BLE up: heap %lu -> %lu (Bluedroid cost %ld B)\n",
    (unsigned long)heap_before, (unsigned long)heap_after, (long)heap_before - (long)heap_after);
#if ENABLE_LOGGING
  logEvent("BLE init heap_before=%lu heap_after=%lu cost=%ld",
    (unsigned long)heap_before, (unsigned long)heap_after, (long)heap_before - (long)heap_after);
#endif
}

// Tear BLE fully down before deep sleep so the Bluedroid controller stops clocking the radio domain (a
// left-initialized controller keeps deep-sleep current high + holds tens of KB of heap - undoes the battery
// work). No-op if BLE wasn't up this wake (a charge-sample poke or a gate skip). Called from enterDeepSleep()
// BEFORE datalogger.end() so this EVT is captured.
void bleDeinitForSleep() {
  if (!g_ble_up) return;
  uint32_t heap_before = ESP.getFreeHeap();
  ble.deinit();
  g_ble_up = false;
  if (g_config_pending) dropUnsavedPush("sleep"); // no callback can arrive now; RAM goes with the sleep
  Serial.printf("BLE down before sleep (heap %lu -> %lu freed)\n",
    (unsigned long)heap_before, (unsigned long)ESP.getFreeHeap());
#if ENABLE_LOGGING
  logEvent("BLE deinit heap %lu -> %lu (radio domain released for sleep)",
    (unsigned long)heap_before, (unsigned long)ESP.getFreeHeap());
#endif
}
#else
void bleInitForWake() {}      // no-op stubs so call sites need no #if guards
void bleDeinitForSleep() {}
#endif

#if ENABLE_BLE && ENABLE_LOGGING
/*
  Persisted BLE-pushed vehicle profile. The push handshake persisted only the HASH (NVS) while the profile
  itself lived in RAM - so every sleep/reboot silently reverted to the factory initScenic() profile while the
  fast path (HASH OK) told the app its pushed profile was still active. Masked so far because the app's Scenic
  profile ~= initScenic; a genuinely different profile would have polled the wrong PIDs on every standalone
  wake. Fix: the loop's config-apply block writes every accepted push (configJson + tlm_push_token) to this
  file and only then commits the push's hash (BLEWrapper::commitHash), so NVS never holds a hash whose profile
  is not on flash; boot loads the file in preference to the factory profile. A push that cannot be written,
  or is still waiting for the loop when RAM goes, takes the file down too (dropUnsavedPush).
*/
static const char* PROFILE_PATH = "/vehicle_profile.json";

// A full push that will never reach the file: it failed to apply or write, or sleep / the OTA reboot wipes RAM
// before the loop applied it. Its hash was never committed, but the file still holds the previous push, which may
// carry another token. Drop the hash AND the file: no later wake may run a profile or token older than the last
// accepted push. The app re-pushes on its next connect; until then the dongle waits as an unpaired one does.
void dropUnsavedPush(const char* why) {
  ble.invalidateHash(); // first: a reset between the two must not leave a hash with no file
  LittleFS.remove(PROFILE_PATH);
  logEvent("PROFILE push not saved (%s) - profile removed, hash invalidated (app will re-push)", why);
}

bool loadPersistedProfile() {
  if (!LittleFS.exists(PROFILE_PATH)) return false;
  File f = LittleFS.open(PROFILE_PATH, "r");
  if (!f) return false;
  String s = f.readString();
  f.close();
  try {
    json newConfig = json::parse(s.c_str());
    json config = newConfig["configJson"];
    config.erase("utc"); // clock side-channel captured during the push, not a profile field
    tlm_push_token = newConfig.at("tlm_push_token").get<std::string>();
    obd.setVehicleCommands(config);
    // On an alarm-flagged vehicle, boot-time initialize() (ATZ + the profile's init commands) is
    // bus-adjacent traffic with no permit yet - defer it to the first permitted poll cycle (§8.4:
    // the OBD link is not brought up until a permit exists). Unflagged: unchanged, init here.
    if (alarmEnforced()) {
      g_obd_needs_init = true;
      Serial.printf("BLE-pushed vehicle profile restored (%d bytes) - OBD init deferred (alarm gate)\n",
        (int)s.length());
      logEvent("PROFILE restored from flash (%d bytes) - OBD init deferred until permit", (int)s.length());
      return true;
    }
    obd.initialize();
    Serial.printf("BLE-pushed vehicle profile restored from flash (%d bytes)\n", (int)s.length());
    logEvent("PROFILE restored from flash (%d bytes) - pushed profile active", (int)s.length());
    return true;
  }
  catch (const json::exception& err) {
    // Unreadable (e.g. power lost mid-write): factory fallback AND invalidate the stored hash, so the fast
    // path can't claim a profile we aren't running - the app re-pushes on the next connect.
    Serial.printf("Persisted profile unreadable (%s) - factory fallback, hash invalidated\n", err.what());
    LittleFS.remove(PROFILE_PATH);
    ble.invalidateHash();
    logEvent("PROFILE restore FAILED - factory fallback, hash invalidated");
    return false;
  }
}
#elif ENABLE_BLE
void dropUnsavedPush(const char*) { ble.invalidateHash(); } // nothing on flash to remove
#endif

