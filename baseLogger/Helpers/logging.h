#pragma once
/*
  Flash logging + serial console helpers (ENABLE_LOGGING).
  Contains: the CSV schema (LOG_HEADER) and row builder (buildLogLine + stateName/fcol),
  the EVT event marker writer (logEvent), and the serial console (handleLogCommands:
  DUMP/STAT/CLEAR/RESUME, raw AT passthrough and the RADIO modem-config dump).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

static unsigned long gpsFixAgeMs();
static const char* gpsLocationSource();

#if ENABLE_LOGGING
// CSV column order for the data log. Legacy prefix, then optional GPS diagnostics. Must stay in sync with
// buildLogLine() below. lat/lon are the selected position; raw receiver coordinates are never logged.
const std::string LOG_HEADER =
  "millis,utc,boot,sleep,wake,state,batt12v,rail_v,free_heap,min_heap,max_block,free_psram,rssi,wifi,"
  "gps_fix,gps_sat,gps_hdop,gps_alt,obd_pids,up_http,up_status,reset,clean_sleep,note,"
  "soc,soh,voltage,current,power,kwh_charged,speed,ext_temp,is_parked,is_charging,lat,lon,"
  "gps_read,gps_reply_bytes,gps_field,gps_date,gps_time,gps_time_age_ms,gps_speed_kmh,gps_source,"
  "soe,batt_temp,batt_temp_min,batt_temp_max";

static const char* stateName(ActivityState s) {
  switch (s) {
  case BLE: return "BLE";
  case ACTIVE: return "ACTIVE";
  case CHARGING: return "CHARGING";
  case STANDBY: return "STANDBY";
  default: return "INIT";
  }
}

// Look up a telemetry key in the frame; returns its value as text, or "" if the frame doesn't have it
// (profiles differ, so blank cells are expected).
static std::string fcol(const OBDVector& f, const char* key) {
  for (const auto& r : f) {
    if (r.first == key) {
      char b[24];
      snprintf(b, sizeof(b), "%.3f", r.second);
      return std::string(b);
    }
  }
  return std::string();
}

static const char* gpsReadName(GPSReadResult result) {
  switch (result) {
  case GPSReadResult::Accepted: return "ok";
  case GPSReadResult::LinkUnavailable: return "link";
  case GPSReadResult::NoReply: return "no_reply";
  case GPSReadResult::MarkerMissing: return "marker";
  case GPSReadResult::RequiredFieldInvalid: return "field";
  case GPSReadResult::CoordinateRejected: return "coord";
  case GPSReadResult::DirectNoData: return "direct_none";
  case GPSReadResult::Unconfirmed: return "unconfirmed";
  default: return "none";
  }
}

// Assemble one CSV row: hardware diagnostics + the vehicle metrics from this reading. obd_count is the
// number of PIDs the OBD layer returned (0 = car didn't answer / asleep).
std::string buildLogLine(const OBDVector& frame, int obd_count) {
#if ENABLE_SLEEP && ENABLE_MEMS
  uint32_t bc = bootCount, sc = sleepCount;
#else
  uint32_t bc = 0, sc = 0;
#endif
  float batt12v = g_last_voltage; // cached ATRV (refreshed at the top of each poll cycle) - one read per cycle, not per row
#if ENABLE_WIFI
  bool wifi_up = (WiFi.status() == WL_CONNECTED);
  long rssi = wifi_up ? WiFi.RSSI() : 0;
#else
  bool wifi_up = false;
  long rssi = 0;
#endif
  int gps_fix = (gd && gd->lat != 0 && gd->lng != 0) ? 1 : 0;
  int gps_sat = gd ? gd->sat : 0;
  int gps_hdop = gd ? gd->hdop : 0;
  float gps_alt = gd ? gd->alt : 0;
  const char* note = (obd_count == 0) ? "no_obd" : "";

  char head[360];
  snprintf(head, sizeof(head),
    "%lu,%lu,%u,%u,%s,%s,%.2f,%.2f,%u,%u,%u,%u,%ld,%d,%d,%d,%d,%.1f,%d,%d,%s,%d,%d,%s",
    (unsigned long)millis(),
    (unsigned long)rtc.getEpoch(),
    bc, sc,
    g_wake_reason,
    stateName(current_state),
    batt12v,
    g_last_io36,           // rail_v: true 12V rail (IO36_BatDec) beside the adapter's batt12v (ATRV)
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getMinFreeHeap(),
    (unsigned)ESP.getMaxAllocHeap(),
    (unsigned)ESP.getFreePsram(),
    rssi,
    wifi_up ? 1 : 0,
    gps_fix, gps_sat, gps_hdop, gps_alt,
    obd_count,
    g_last_http,
    g_last_status.c_str(),
    (int)esp_reset_reason(),
    g_clean_sleep ? 1 : 0, // clean_sleep: 1 = came through our sleep path; 0 = uncommanded boot (trust rail_v only when 1)
    note);

  const GPSReadDiagnostics diagnostics = g_gps_powered ? sys.gpsReadDiagnostics() : GPSReadDiagnostics{};
  const unsigned long age = gpsFixAgeMs();
  char age_text[24] = {};
  if (age != ULONG_MAX) snprintf(age_text, sizeof(age_text), "%lu", age);
  char gps_columns[160];
  snprintf(gps_columns, sizeof(gps_columns), "%s,%u,%d,%lu,%lu,%s,%.2f,%s",
    gpsReadName(diagnostics.result), (unsigned)diagnostics.reply_bytes, (int)diagnostics.field,
    (unsigned long)(gd ? gd->date : 0), (unsigned long)(gd ? gd->time : 0), age_text,
    gd ? gd->speed * 1.852f : 0, gpsLocationSource());
  std::string line(head);
  const char* tcols[] = { "soc", "soh", "voltage", "current", "power", "kwh_charged",
                          "speed", "ext_temp", "is_parked", "is_charging", "lat", "lon" };
  for (auto c : tcols) {
    line += ",";
    line += fcol(frame, c);
  }
  line += ",";
  line += gps_columns;
  for (auto c : { "soe", "batt_temp", "batt_temp_min", "batt_temp_max" }) {
    line += ",";
    line += fcol(frame, c);
  }
  return line;
}

// Append a discrete EVENT marker to the flash log so the device's BEHAVIOR - not just its telemetry - can
// be reconstructed when the log is retrieved: when it woke (and why), every state change, whether OBD
// polling is keeping the car's ECUs awake, each connect/upload result, and when it powers the modem+GPS
// off and enters deep sleep. These lines start with "EVT," so they sort/grep apart from the numeric
// telemetry rows (filter them out for pure telemetry, or read them alone for the power/behaviour story).
// Format: EVT,<utc>,<millis>,<state>,<atrv>,<rail_io36>,<text>
void logEvent(const char* fmt, ...) {
  char msg[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char line[210];
  snprintf(line, sizeof(line), "EVT,%lu,%lu,%s,%.2f,%.2f,%s",
    (unsigned long)rtc.getEpoch(),
    (unsigned long)millis(),
    stateName(current_state),
    g_last_voltage, // cached ATRV (refreshed each poll cycle + at the gate) - avoids an AT round-trip per event
    g_last_io36,    // cached true-rail voltage (IO36_BatDec) - the brownout disambiguator, beside ATRV
    msg);
  datalogger.writeLine(line);
#if PRINT
  Serial.printf("[EVT] %s\n", msg);
#endif
}

// Defined in ota_update.h (real or ENABLE_OTA=0 stub), which logger.ino includes after this file -
// the forward declaration lets the console's OTARESET branch below compile.
void otaBenchReset();

// Same pattern: defined in profile_sig.h, which logger.ino also includes after this file, so the
// JWSTEST branch below needs the declaration up here.
void profileSigSelfTest();

// Non-blocking-ish serial console for retrieving the flash log: DUMP (stream all; DUMP n = newest n segments), STAT (usage), CLEAR.
void handleLogCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (!cmd.length()) return;

  // Any console interaction pins the device awake and pulls it out of a low-power state, so a USB dump
  // can't be cut off by the deep-sleep state machine (which over USB looks like a "restart" + lost WiFi).
  maintenance_mode = true;
  maintenance_since = millis();
  last_active = millis();
  if (current_state == CHARGING || current_state == STANDBY) {
    changeState(ACTIVE);
  }

  // Refuse the long-running / flash-mutating commands while a firmware update is in flight. DUMP, CLKPROBE
  // and the AT passthrough all UNSUBSCRIBE the task watchdog and then block the loop for minutes (a full log
  // stream, ~30-60s of modem probing, or up to 185s for AT+COPS=?), which would starve the OTA sector ACKs
  // until the app times out; CLKPROBE additionally tears down and rebuilds the modem's HTTP session, which the
  // OTA path is using. DUMP and CLEAR also close/rotate/erase log segments on the same flash chip that
  // esp_ota_write is using. STAT and RESUME are cheap and stay available.
  // A BLE log dump gets the same protection: serial DUMP would block the loop (starving the dump's window
  // replies) and CLEAR would delete the very segments the app is mid-way through reading.
  const bool is_dump = cmd.equalsIgnoreCase("DUMP") ||
                       (cmd.length() > 5 && cmd.substring(0, 5).equalsIgnoreCase("DUMP "));
  if ((g_ota_active || g_logdump_active) &&
      (is_dump || cmd.equalsIgnoreCase("CLEAR") ||
       cmd.equalsIgnoreCase("RADIO") || cmd.equalsIgnoreCase("CLKPROBE") || cmd.equalsIgnoreCase("TLSCHECK") ||
       cmd.substring(0, 2).equalsIgnoreCase("AT"))) {
    Serial.printf("BUSY: %s in progress - %s refused (try again when it finishes)\n",
      g_ota_active ? "firmware update" : "BLE log dump", cmd.c_str());
    return;
  }

  if (is_dump) {
    // toInt() is 0 for a typo, and 0 would mean everything: refuse it rather than start a 4-minute stream.
    const int newest = cmd.length() > 4 ? cmd.substring(5).toInt() : 0;
    if (cmd.length() > 4 && newest < 1) { Serial.printf("usage: DUMP [n>=1]\n"); return; }
    // Streaming a full log at 115200 baud can exceed WEDGE_WDT_TIMEOUT_S - unsubscribe for the (bench-only,
    // human-attended) dump so the wedge guard can't reset mid-transfer, then re-arm.
    esp_task_wdt_delete(NULL);
    datalogger.dump(Serial, newest);
    esp_task_wdt_add(NULL);
  }
  else if (cmd.equalsIgnoreCase("STAT")) datalogger.printStats(Serial);
  else if (cmd.equalsIgnoreCase("CLEAR")) datalogger.clearAll();
#if ENABLE_CELL
  // Raw modem passthrough: type any AT command in the serial monitor (e.g. AT+CMNB?, AT+CBANDCFG?,
  // AT+COPS=?) and get the modem's raw reply. THE tool for chasing per-unit NVM state without
  // reflashing. AT+COPS=? is a full network survey (every visible operator+RAT) and can take ~3min,
  // so it gets a long window; unsubscribe from the wedge WDT like DUMP does for the wait.
  else if (cmd.substring(0, 2).equalsIgnoreCase("AT")) {
    esp_task_wdt_delete(NULL);
    unsigned long window_ms = (cmd.indexOf("COPS=?") >= 0) ? 185000UL : 10000UL;
    Serial.printf(">> %s\n", cmd.c_str());
    Serial.printf("%s\n", network.sendRawAt(cmd.c_str(), window_ms).c_str());
    esp_task_wdt_add(NULL);
  }
  // Full read-only dump of the modem's persisted radio config (also auto-printed on the first
  // failed net init of a boot). Run on a good and a bad unit and diff.
  else if (cmd.equalsIgnoreCase("RADIO")) network.dumpRadioConfig();
  // Clock diagnostic: is an HTTP response Date header reachable on this modem (which would make
  // CLK_HTTP live and give a free correction per upload), and how far has the RTC drifted right
  // now? See clockProbe() in clock_sync.h. Blocks the loop for ~30-60s - drop the wedge WDT for it
  // the way DUMP does.
  else if (cmd.equalsIgnoreCase("CLKPROBE")) {
    esp_task_wdt_delete(NULL);
    clockProbe();
    esp_task_wdt_add(NULL);
  }
  // Does the server's current certificate chain to the bundled roots? A fresh verified open, no data
  // sent - the bench check around a server certificate change (DONGLE_CONTEXT.md §6a).
  else if (cmd.equalsIgnoreCase("TLSCHECK")) {
    network.disconnect(); // a reused session would prove nothing
    logEvent("TLSCHECK %s %s", network.connect() ? "ok" : "FAILED", network.lastOpenTrace().c_str());
  }
#endif
  // Bench-only: clear the persisted OTA anti-downgrade floor after version-number testing, so a
  // serial-flashed lower OTA_FW_VERSION reports and gates cleanly (see otaBenchReset in ota_update.h,
  // included after this file - hence the forward declaration).
  else if (cmd.equalsIgnoreCase("OTARESET")) otaBenchReset();
#if ENABLE_BLE
  // Forget every bonded phone. Bonds live in NVS and survive deep sleep, so a full bond table stays full
  // until something clears it - and a full table blocks NEW phones from pairing. Physical access only.
  else if (cmd.equalsIgnoreCase("BONDCLEAR")) {
    int removed = ble.clearAllBonds();
    // The phone still holds ITS half of the key. It will offer that stale LTK on the next connect, we no
    // longer have a match, and pairing fails (SEC failed reason=0x0f) - so a cleared bond is only really
    // cleared once the PHONE forgets us too: iOS Settings > Bluetooth > "Forget This Device", Android
    // "Unpair". Say so here, because this line is what a support session reads.
    Serial.printf("BONDCLEAR: removed %d bond(s) - each phone must ALSO forget/unpair this dongle,\n"
                  "           otherwise its stale key makes the next pairing fail\n", removed);
    logEvent("BONDCLEAR removed %d bond(s) - phones must forget/unpair too", removed);
  }
  // Wipe the auth secret, returning the dongle to trust-on-first-use so the app can provision a fresh
  // one. THE rescue path for a lost or mismatched secret: no toolchain, no reflash. Kept separate from
  // BONDCLEAR on purpose - a full bond table is routine and must not cost authorization.
  // Allowed at any time and it does NOT revoke the live session (the in-RAM unlock flag stands until
  // disconnect), so it cannot interfere with a transfer in flight.
  else if (cmd.equalsIgnoreCase("AUTHCLEAR")) {
    ble.clearAuthSecrets();
    // Say what this actually DOES: the next phone to connect provisions a brand-new secret and every
    // other phone holding the old one loses access. That is the point (revocation), but it also means
    // connect the phone that should keep access FIRST - whoever wins that race becomes the owner.
    Serial.printf("AUTHCLEAR: auth secret wiped - the NEXT phone to connect sets a new one\n"
                  "           (revokes every other phone; connect the keeper first)\n");
    logEvent("AUTHCLEAR secret wiped - next phone to connect provisions a new one (revocation)");
  }
#endif
  // Prove the profile-signature check discriminates: re-verify the last real signed profile intact,
  // tampered, and intact again (see profileSigSelfTest). A happy-path push cannot show this - it looks
  // the same whether the signature is enforced or blindly accepted.
  else if (cmd.equalsIgnoreCase("JWSTEST")) profileSigSelfTest();
  else if (cmd.equalsIgnoreCase("RESUME")) maintenance_mode = false; // hand back to normal duty (sleep allowed)
}
#endif
