#pragma once
/*
  Bench diagnostics, serial test injection, and persisted WiFi credentials.

  Included into logger.ino's single translation unit; shares its globals and config.h flags.
*/

/*
  Not used in "production", can be called when a total reset of the chip is to be done
  Erases memory that otherwise persists through flashing of hardware
*/
void resetChip() {
  nvs_flash_erase();
  nvs_flash_init();
  Serial.printf("Chip is now reset\n");
}

/*
  Allows for sending data from file. Used for testing
*/

#if FILE_TEST
void file_test() {
  if (Serial.available() > 0) {
    OBDVector test_vector{};
    char buffer[64];
    uint8_t idx = 0;
    char c;

    while (Serial.available() > 0 && idx < sizeof(buffer) - 1) {
      c = Serial.read();
      if (c == '\n' || c == '\r') {
        buffer[idx] = '\0';
        break;
      }
      buffer[idx++] = c;
    }

    uint32_t utc;
    float lat, lon;

    if (sscanf(buffer, "%u,%f,%f", &utc, &lat, &lon) == 3) {
      gd->time = utc;
      gd->lat = lat;
      gd->lng = lon;
    } else {
      Serial.println("Error parsing input");
    }
    addGpsData(test_vector);
    test_vector.emplace_back("utc", rtc.getEpoch());
    for (auto res : test_vector) {
      Serial.printf("Value: %-12s : %2f\n", res.first.c_str(), res.second);
    }
  }
}
#endif

/*
  WiFi credentials from NVS ("network" / ssid, password), seeded once from the build-time defaults.
*/

#if ENABLE_WIFI
void loadPreferences() {
  bool exists;
  preferences.begin("network");
  exists = preferences.isKey("ssid");
  if (exists) {
    wifi_ssid = preferences.getString("ssid").c_str();
    wifi_password = preferences.getString("password").c_str();
  } else if (!wifi_ssid.empty()) {
    // Seed NVS from the build-time default, but ONLY if there is one. The defaults in config.h are
    // empty (no credentials in the source tree), and seeding empty strings would be worse than
    // doing nothing: isKey("ssid") would then answer true forever, so this branch could never run
    // again and "never provisioned" would be indistinguishable from "provisioned with blanks".
    preferences.putString("ssid", wifi_ssid.c_str());
    preferences.putString("password", wifi_password.c_str());
  }
  preferences.end();
  Serial.printf(wifi_ssid.empty() ? "WIFI has no credentials - provision NVS \"network\"/ssid+password\n"
                                  : "WIFI loaded\n");
}
#endif

#if ENABLE_CAN_SNIFF
/*
  PASSIVE CAN-bus sniff (diagnostic). Puts the OBD adapter in ELM monitor-all mode and LISTENS only - it
  transmits nothing on the CAN bus (no Mode22/010D/ClearDTC), so unlike active polling it cannot wake or
  hold the car awake. Returns the number of CAN frames seen in the window; copies the first frame's bytes
  into `sample` (hex) as proof of real traffic. Bus alive -> car awake; bus silent -> car asleep.

  Must run BEFORE initScenic()/the first poll so the reading reflects the car's INDEPENDENT state (nothing
  we've done has touched the CAN bus yet - obd.begin/ATRV/GPS/modem don't transmit on CAN). initScenic()
  afterwards issues its own ATZ + init sequence, so it cleanly re-establishes the UDS polling state.
  ATSP7 = ISO15765 29-bit 500K, matching the Scenic profile; ATH1 keeps headers so frames are visible.
  init(vector) just sends adapter-config AT commands (no probe, no CAN traffic) - NOT init(PROTO_...),
  which probes with a 010D request (a transmission) and has the ATSP-under-#if-PRINT bug.
*/
int sniffCanBus(unsigned long window_ms, char* sample, size_t sample_len) {
  if (sample && sample_len) sample[0] = '\0';
#if OBD_ALARM_GATE
  // The alarm gate does not reach this diagnostic - it guards the poll path, not this one. Even the
  // "passive" branch sends adapter-config AT commands (ATZ/ATSP7/ATH1) to reach monitor mode, and
  // CAN_SNIFF_JOIN_PROBE deliberately fires a real UDS request. On a flagged car that is exactly the
  // traffic that sets the alarm off, so refuse rather than let a debug build trip a customer's horn.
  if (alarmEnforced()) {
    Serial.printf("CAN sniff refused: vehicle is alarm-flagged (see OBD_ALARM_GATE)\n");
#if ENABLE_LOGGING
    logEvent("SNIFF refused - alarm-flagged vehicle");
#endif
    return -1;
  }
#endif
#if CAN_SNIFF_JOIN_PROBE
  // DIAGNOSTIC (NOT passive): join the bus exactly like initScenic() and fire ONE real UDS request so the
  // CAN controller is definitely bus-active, then open the receive filter wide so monitor mode sees ALL
  // headers (not just our ECU's reply). The single request is the only transmission - run on an awake car.
  obd.init(std::vector<std::string>{
    "ATZ", "ATE0", "ATAL", "ATCP18", "ATSP7", "ATFCSD300000", "ATFCSM1",
    "ATSHDADBF1", "ATFCSH18DADBF1", "2291C8" }); // last entry = join probe (one UDS request)
  obd.setHeaderMask(0);   // ATCM 0 - all header bits don't-care
  obd.setHeaderFilter(0); // ATCF 0 - accept every header (see the whole bus)
#else
  // PASSIVE: listen-only, transmits nothing on the CAN bus (cannot wake/hold the car). ATSP7 = ISO15765
  // 29-bit 500K (matches initScenic); ATH1 keeps headers so frames are visible.
  obd.init(std::vector<std::string>{ "ATZ", "ATE0", "ATAL", "ATSP7", "ATH1" });
#endif
  obd.sniff(true);  // ATM1: monitor-all

  int frames = 0;
  byte buf[128];
  unsigned long start = millis();
  while (millis() - start < window_ms) {
    int b = obd.receiveData(buf, sizeof(buf));
    if (b > 0) {
      if (frames == 0 && sample && sample_len > 3) { // capture the first frame as hex proof
        size_t o = 0;
        for (int i = 0; i < b && o + 3 < sample_len; i++) {
          o += snprintf(sample + o, sample_len - o, "%02X", buf[i]);
          if (o + 1 < sample_len && i + 1 < b) sample[o++] = ' ';
        }
        sample[o] = '\0';
      }
      frames++;
    } else {
      delay(5); // nothing waiting on the link; yield briefly so the window isn't a busy-spin
    }
  }
  obd.sniff(false); // ATM0: stop monitoring
  return frames;
}
#endif
