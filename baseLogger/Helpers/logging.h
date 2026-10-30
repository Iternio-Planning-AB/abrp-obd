#pragma once
/*
  Flash CSV/event logging and serial diagnostics (ENABLE_LOGGING).

  Included into logger.ino's single translation unit; shares its globals and config.h flags.
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

// The rest of `DUMP SINCE<text>`: one or more spaces, then a decimal epoch that fits 32 bits, as rtc.getEpoch().
bool parseSinceEpoch(const char* text, uint32_t& since) {
  if (*text != ' ') return false;
  while (*text == ' ') ++text;
  uint64_t value = 0;
  const char* digits = text;
  for (; *text >= '0' && *text <= '9'; ++text) {
    value = value * 10 + (uint64_t)(*text - '0');
    if (value > UINT32_MAX) return false;
  }
  if (text == digits || *text) return false;
  since = (uint32_t)value;
  return true;
}

// What follows "DUMP": nothing (everything), " <n>" (newest n) or " SINCE <utc>". False means print usage.
bool parseDumpArgs(const char* args, int& newest, bool& by_time, uint32_t& since) {
  newest = 0;
  by_time = false;
  if (!*args) return true;
  static const char word[] = "SINCE";
  by_time = true;
  for (int i = 0; i < 5; ++i) by_time = by_time && toupper((unsigned char)args[1 + i]) == word[i];
  if (by_time) return parseSinceEpoch(args + 6, since);
  // atol (as toInt) is 0 for a typo, and 0 would mean everything: refuse it rather than start a 4-minute stream.
  newest = (int)atol(args + 1);
  return newest >= 1;
}

// Defined in ota_update.h (real or ENABLE_OTA=0 stub), which logger.ino includes after this file -
// the forward declaration lets the console's OTARESET branch below compile.
void otaBenchReset();

// Same pattern: defined in profile_sig.h, which logger.ino also includes after this file, so the
// JWSTEST branch below needs the declaration up here.
void profileSigSelfTest();

void gzipBenchTest(const char* args); // gzip_body.h, included after this file

// Non-blocking-ish serial console for retrieving the flash log: DUMP (stream all; DUMP n = newest n segments;
// DUMP SINCE <utc> = from that time), STAT (usage), CLEAR. True when it read a command: loop() re-seeds the loop gap
// (WAKESTAT, #110) after one, so a DUMP's minutes are never mistaken for a stall.
bool handleLogCommands() {
  if (!Serial.available()) return false;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (!cmd.length()) return false;

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
      (is_dump || cmd.equalsIgnoreCase("CLEAR") || cmd.equalsIgnoreCase("AUTHCLEAR") || cmd.equalsIgnoreCase("PANIC") || cmd.equalsIgnoreCase("WDTHANG") ||
       cmd.equalsIgnoreCase("RADIO") || cmd.equalsIgnoreCase("CLKPROBE") || cmd.equalsIgnoreCase("TLSCHECK") ||
       cmd.substring(0, 6).equalsIgnoreCase("GZTEST") ||
       cmd.substring(0, 2).equalsIgnoreCase("AT"))) {
    Serial.printf("BUSY: %s in progress - %s refused (try again when it finishes)\n",
      g_ota_active ? "firmware update" : "BLE log dump", cmd.c_str());
    return true;
  }

  if (is_dump) {
    int newest;
    bool by_time;
    uint32_t since = 0;
    // The host capture script keys on "usage: DUMP" to fall back from SINCE to DUMP n on older firmware.
    if (!parseDumpArgs(cmd.c_str() + 4, newest, by_time, since)) { Serial.printf("usage: DUMP [n>=1 | SINCE <utc>]\n"); return true; }
    // Streaming a full log at 115200 baud can exceed WEDGE_WDT_TIMEOUT_S - unsubscribe for the (bench-only,
    // human-attended) dump so the wedge guard can't reset mid-transfer, then re-arm.
    esp_task_wdt_delete(NULL);
    if (by_time) {
      const unsigned long started = millis();
      const DataLogger::SinceStats stats = datalogger.dumpSince(Serial, since);
      logEvent("DUMP since=%lu start=%d cut=%d sent=%u ms=%lu", (unsigned long)since, stats.start, stats.cut,
               (unsigned)stats.sent, millis() - started);
    } else {
      datalogger.dump(Serial, newest);
    }
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
  // Refused during an update or a BLE log dump, like CLEAR (it erases the log too); a phone mid-update must
  // not be pushed into re-pairing and so win the "connect the keeper first" race below.
  else if (cmd.equalsIgnoreCase("AUTHCLEAR")) {
    ble.set_authenticated(false); // the open session proved against the wiped secret: it re-pairs like any other
    if (!forgetProfile("authclear")) {
      Serial.printf("AUTHCLEAR FAILED: the log, profile or backlog could not be erased - secret kept, try again\n");
      return true;
    }
    ble.clearAuthSecrets();
    // Say what this actually DOES: the next phone to connect provisions a brand-new secret and every
    // other phone holding the old one loses access. That is the point (revocation), but it also means
    // connect the phone that should keep access FIRST - whoever wins that race becomes the owner.
    // The vehicle profile, push token and flash log go too: polling and upload wait for that phone's full push.
    Serial.printf("AUTHCLEAR: auth secret, profile, push token and log wiped - the NEXT phone to connect sets a\n"
                  "           new secret and pushes the profile (revokes every other phone; connect the keeper first)\n");
    logEvent("AUTHCLEAR secret wiped - next phone to connect provisions a new one (revocation)");
  }
#endif
  // Prove the profile-signature check discriminates: re-verify the last real signed profile intact,
  // tampered, and intact again (see profileSigSelfTest). A happy-path push cannot show this - it looks
  // the same whether the signature is enforced or blindly accepted.
  else if (cmd.equalsIgnoreCase("JWSTEST")) profileSigSelfTest();
  // Bench check of the compressor and its verifier on synthetic bodies (gzip_body.h).
  else if (cmd.substring(0, 6).equalsIgnoreCase("GZTEST")) gzipBenchTest(cmd.c_str() + 6);
  else if (cmd.equalsIgnoreCase("RESUME")) maintenance_mode = false; // hand back to normal duty (sleep allowed)
  // Bench-only, physical access: the two resets the crash-loop guard counts (wdt_streak.h), to see on a dongle
  // that its count survives them. Two in a row escalate. PANIC aborts now; WDTHANG stops feeding the wedge
  // watchdog, which resets after WEDGE_WDT_TIMEOUT_S.
  else if (cmd.equalsIgnoreCase("PANIC") || cmd.equalsIgnoreCase("WDTHANG")) {
    const bool hang = cmd.equalsIgnoreCase("WDTHANG");
    logEvent("CONSOLE %s requested (streak=%lu)", hang ? "WDTHANG" : "PANIC", (unsigned long)wdt_reset_count);
    Serial.flush();
    if (!hang) abort();
    for (;;) delay(1000);
  }
  return true;
}
#endif
