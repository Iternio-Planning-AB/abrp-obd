#pragma once
/*
  Clock/time helpers - the unified RTC clock and every time-source parser.
  Contains: the source-priority clock setter (setClock: GPS authoritative, HTTP/NITZ/NTP
  fallbacks), validity check (clockValid), calendar->epoch conversion (utcToEpoch), and the
  parsers for the modem AT+CCLK clock (parseCclk) and HTTP Date headers (parseHttpDate).

  Extracted verbatim from logger.ino (definitions moved, logic unchanged). This file is
  #included by logger.ino into the single sketch translation unit - it is NOT a standalone
  compilation unit - so it shares logger.ino's globals and the config.h feature flags.
*/

// =====================================================================================================
// Unified clock. Priority: GPS is authoritative and wins while it has a live fix; coarser sources (HTTP
// Date now, NITZ later) only set the clock when it's still unset or GPS has gone stale (no live fix for
// GPS_STALE_MS - tunnel/garage). This gives instant time at boot from whatever arrives first and zero
// runtime drift once GPS locks, with no explicit "startup vs driving" phase to track.
// =====================================================================================================
unsigned long last_gps_sync = 0;            // millis() of the last live-GPS clock update (0 = never)
const unsigned long GPS_STALE_MS = 60000;   // GPS counts as stale this long after its last fix

bool clockValid() { return rtc.getEpoch() > 1000000000UL; } // > 2001-09-09 == a real time has been set

/*
  When the clock was last set from a real time source, as an EPOCH rather than a millis() stamp -
  millis() restarts on every deep-sleep wake, and the whole point of this is to survive those.
  RTC-persisted; 0 means never synced on this power-up.

  Why it exists. Deep-sleep timekeeping runs on the ESP32's internal 150 kHz RC oscillator
  (CONFIG_ESP32_RTC_CLK_SRC_INT_RC in the Arduino framework's sdkconfig), which is accurate to a
  percent or so and drifts with temperature. Measured on a parked car overnight: 30 s per hour FAST,
  ~8250 ppm. The dongle therefore believes more time has passed than has, stamps frames in the
  future, and the backend records a NEGATIVE registration delay - -201 s after a night in a garage,
  still growing.

  Nothing corrected it, for two reasons that had to be found separately. CLK_HTTP is dead on the
  cellular build: CellWrapper never overrides getDateHeader(), so it inherits the base class's empty
  string and setClock(CLK_HTTP, ...) is never reached. And every NTP call site was gated on
  !clockValid() - which is merely "epoch > 2001" and stays true across deep sleep forever - so NTP
  ran once on the first boot with coverage and never again. That left CLK_GPS as the only live
  correction, which is exactly the one a garage removes.

  CLK_HTTP stays WiFi-only, and this is settled, not pending: the SIM7070's "SH" HTTP application has
  no way to return response headers. CLKPROBE asked the modem directly - AT+SHCONF=? enumerates exactly
  URL / TIMEOUT / BODYLEN / HEADERLEN / POLLCNT / POLLINTMS / IPVER (HEADERLEN caps the REQUEST headers
  built with AT+SHAHEAD, nothing more), AT+SHREAD returns body bytes only, and every plausible spelling
  of a header read (AT+SHHEAD, AT+SHRHEAD, AT+SHRESHEAD, AT+SHRESP, AT+SHREADHEAD) answers ERROR both
  before a request and with a response buffered. Getting a Date header would mean abandoning the SH
  stack for raw TCP (AT+CAOPEN/CASEND/CARECV) and speaking HTTP by hand - a large change for a source
  no better than the AT+CCLK read we already do for free. Don't re-open it; run CLKPROBE if a future
  modem needs re-checking.
*/
RTC_DATA_ATTR time_t last_clock_sync_epoch = 0;

/* Has the clock gone long enough without a real source that it is worth spending an NTP round trip?
   Measured with the drifting clock itself, which is fine at this resolution: 0.83 % of an hour is
   30 s of error in the interval, and the decision only needs to be right to within minutes. */
bool clockStale() {
  if (last_clock_sync_epoch == 0) return true;      // never synced this power-up
  const time_t now = rtc.getEpoch();
  if (now < last_clock_sync_epoch) return true;     // went backwards: a correction landed, or garbage
  return (now - last_clock_sync_epoch) > CLOCK_RESYNC_SECONDS;
}

/* The one question every clock-acquisition site asks: is it worth spending a time source on this?
   Two different needs behind it - "we have no clock at all, so frames would be dropped for want of a
   utc" and "we have one but it has drifted" - and both want the same answer, so they share a
   predicate rather than each carrying half of the condition. */
bool clockNeedsSync() { return !clockValid() || clockStale(); }

// UTC calendar time -> Unix epoch seconds, with no timezone/library dependency (Howard Hinnant's
// days_from_civil). Correct regardless of the runtime TZ, unlike mktime().
time_t utcToEpoch(int year, int month, int day, int hour, int minute, int second) {
  int y = (month <= 2) ? year - 1 : year;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400L + hour * 3600L + minute * 60L + second;
}

// Apply a time update from a source, honoring the priority above. epoch is UTC seconds.
void setClock(ClockSource src, time_t epoch) {
  if ((unsigned long)epoch < 1000000000UL) return; // ignore garbage (pre-2001) times
  if (src == CLK_GPS) {
    rtc.setTime((uint32_t)epoch);  // GPS is authoritative - always wins
    last_gps_sync = millis();
    last_clock_sync_epoch = epoch;
    return;
  }
  bool gps_fresh = last_gps_sync && (millis() - last_gps_sync < GPS_STALE_MS);
  if (clockValid() && gps_fresh) return; // GPS is better and current - ignore the coarse source
  rtc.setTime((uint32_t)epoch);
  // Recorded only on an update that was ACCEPTED - the early return above is a rejected coarse
  // source, and counting it would restart the staleness clock without correcting anything.
  last_clock_sync_epoch = epoch;
#if PRINT
  // Honest source label. (An earlier build printed "NITZ" for any non-HTTP source, which made an NTP-set
  // clock look like working NITZ and cost a debugging detour - keep the labels exact.)
  Serial.printf("Clock set from %s: %s UTC\n",
    src == CLK_HTTP ? "HTTP Date" : src == CLK_NTP ? "NTP" : "NITZ",
    rtc.getTime("%Y-%m-%d %H:%M:%S").c_str());
#endif
}

// Parse a SIM modem AT+CCLK clock ("yy/MM/dd,hh:mm:ss±zz", tz in quarter-hours per 3GPP) to UTC epoch.
// Returns 0 if the modem clock isn't network-set yet (year < 2020, e.g. the 1980 default) or unparseable.
time_t parseCclk(const char* s) {
  if (!s || !*s) return 0;
  int yy, MM, dd, hh, mm, ss, tzq;
  if (sscanf(s, "%d/%d/%d,%d:%d:%d%d", &yy, &MM, &dd, &hh, &mm, &ss, &tzq) != 7) return 0;
  int year = yy + 2000;
  // Reject the modem's unset defaults: "80/.." -> 2080 and "04/.." -> 2004 both mean "clock not network-set".
  // Only a plausible current year is accepted.
  if (year < 2024 || year > 2070) return 0;
  time_t local = utcToEpoch(year, MM, dd, hh, mm, ss);
  return local - (time_t)tzq * 15 * 60;    // CCLK is local time; tz is quarter-hours -> back out to UTC
}

// Parse an RFC-1123 HTTP Date header ("Sun, 06 Nov 1994 08:49:37 GMT") to UTC epoch; 0 if unparseable.
time_t parseHttpDate(const char* s) {
  if (!s || !*s) return 0;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = { 0 };
  int day, year, hh, mm, ss;
  const char* p = strchr(s, ',');      // skip the optional "Sun," day-of-week prefix
  p = p ? p + 1 : s;
  if (sscanf(p, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
  const char* m = strstr(months, mon);
  if (!m) return 0;
  int month = (int)((m - months) / 3) + 1;
  return utcToEpoch(year, month, day, hh, mm, ss);
}

#if ENABLE_CELL
/*
  Bench-only clock diagnostic, run from the serial console as CLKPROBE. Writes nothing persistent
  except the clock itself (step 5, an ordinary NTP sync).

  The question it answers. setClock() accepts a CLK_HTTP source parsed from an HTTP response Date
  header, and network_upload.h calls network.getDateHeader() on every upload - but CellWrapper never
  overrides it, so on the cellular build that path is dead (see the comment on
  last_clock_sync_epoch). Making it live would give a free clock correction on every upload, which is
  strictly better than spending an NTP round trip once an hour. Whether it CAN be made live depends
  on something only the modem can tell us: the SIM7070 "SH" HTTP application exposes the status code
  and body length ("+SHREQ: <verb>,<code>,<len>") and the body (AT+SHREAD), and nothing in the
  command set we use returns the RESPONSE headers. Rather than guess from the AT manual, ask the
  modem: step 3 probes the plausible command names and step 4 runs a real GET and dumps every raw
  reply, so a Date line shows up if it is reachable at all.

  Read the output for one thing: does the string "Date:" appear ANYWHERE. If it does not, the SH
  stack is body-only, getDateHeader() is not implementable on this modem, and periodic NTP (step 5)
  is the correction path - which is what the firmware now does.

  Step 4 does disturb the modem's HTTP session (SHDISC/SHCONF/SHCONN). That is safe here because
  HTTPClientSIM::open() re-runs the whole SHCONF+SHCONN sequence and close() sends SHDISC on every
  single upload cycle, so the next upload rebuilds the session from scratch regardless of what state
  we leave it in. It blocks the loop for ~30-60s though, so the caller must drop the task watchdog.
*/
std::string clockProbeAt(const char* cmd, unsigned int timeout_ms = 10000) {
  std::string r = network.sendRawAt(cmd, timeout_ms);
  // Print the reply RAW - unlike dumpRadioConfig, which flattens CR/LF for diffing, a multi-line
  // header block is exactly what we are hunting for and flattening would hide its shape.
  Serial.printf(">>> %s\n%s\n", cmd, r.c_str());
  return r;
}

void clockProbe() {
  // Every plausible spelling of "give me the response header". All are expected to ERROR; one that
  // does not is the whole point of the exercise. Probed twice - once cold (test syntax, before any
  // request exists) and once with a response actually buffered, because a modem can reject a header
  // read that has nothing to read and accept the identical command a moment later.
  static const char* header_probe[] = {
    "AT+SHHEAD?", "AT+SHHEAD=?", "AT+SHRHEAD?", "AT+SHRHEAD=?",
    "AT+SHRESHEAD?", "AT+SHRESP?", "AT+SHREADHEAD?",
  };
  const int n_probe = sizeof(header_probe) / sizeof(header_probe[0]);

  Serial.printf("\n=== CLKPROBE: can an HTTP call give us the time? ===\n");

  // ---- [1] where the local clock stands, and how the new staleness logic sees it -------------
  const time_t rtc_now = rtc.getEpoch();
  Serial.printf("[1] rtc=%ld (%s UTC) valid=%d stale=%d last_sync=%ld age=%lds resync_after=%ds\n",
    (long)rtc_now, rtc.getTime("%Y-%m-%d %H:%M:%S").c_str(), clockValid() ? 1 : 0,
    clockStale() ? 1 : 0, (long)last_clock_sync_epoch,
    last_clock_sync_epoch ? (long)(rtc_now - last_clock_sync_epoch) : -1L, CLOCK_RESYNC_SECONDS);

  // ---- [2] the modem's own clock: is carrier NITZ pushing time on this SIM at all? -----------
  const std::string cclk = network.readClock();
  const time_t modem_epoch = parseCclk(cclk.c_str());
  Serial.printf("[2] AT+CCLK=\"%s\" parsed=%ld %s\n", cclk.c_str(), (long)modem_epoch,
    modem_epoch ? "" : "(not network-set - NITZ absent or modem default)");
  if (modem_epoch) Serial.printf("    modem vs rtc: %+lds\n", (long)(modem_epoch - rtc_now));
  clockProbeAt("AT+CLTS?", 3000);   // 1 = NITZ enabled in modem NVM
  clockProbeAt("AT+CPSI?", 3000);   // attached? a detached modem makes steps 4-5 meaningless

  // ---- [3] cold probe: which HTTP commands does this firmware's modem admit to having? -------
  clockProbeAt("AT+SHCONF=?", 3000); // the authoritative list of SHCONF parameters
  clockProbeAt("AT+SHREAD=?", 3000);
  for (int i = 0; i < n_probe; i++) clockProbeAt(header_probe[i], 3000);

  // ---- [4] a real GET, with every raw reply dumped so a Date line cannot hide -----------------
  clockProbeAt("AT+SHDISC", 5000);
  clockProbeAt("AT+SHCONF=\"URL\",\"http://api.iternio.com\"", 3000);
  clockProbeAt("AT+SHCONF=\"HEADERLEN\",350", 3000);
  clockProbeAt("AT+SHCONF=\"BODYLEN\",1024", 3000);
  clockProbeAt("AT+SHCONN", 30000);
  const std::string state = clockProbeAt("AT+SHSTATE?", 5000);
  if (strstr(state.c_str(), "+SHSTATE: 1")) {
    // 1 = GET. The reply is just "OK"; the real result arrives later as the unsolicited
    // "+SHREQ: \"GET\",<code>,<len>". We deliberately do NOT try to capture that here: sendRawAt()
    // calls xbPurge() before every command, so any URC that lands between two probe commands is
    // discarded (the first run of this probe printed a misleading "http=0 body_len=0" for exactly
    // that reason - the GET had actually succeeded). HTTPClientSIM::get() reads the URC properly by
    // keeping one accumulating sendCommand() buffer; here we only need to see the response BYTES.
    clockProbeAt("AT+SHREQ=\"/\",1", 20000);
    delay(3000);                               // let the round trip finish before reading
    // Blind read of the first 400 bytes - enough to show whether any header text is prepended to the
    // body, and short of sendRawAt()'s 512-byte reply buffer. An empty/ERROR reply here just means
    // the GET returned nothing readable; the header verdict comes from the probe list below.
    clockProbeAt("AT+SHREAD=0,400", 10000);
    // Warm probe - now there IS a buffered response for a header read to return.
    for (int i = 0; i < n_probe; i++) clockProbeAt(header_probe[i], 5000);
  } else {
    Serial.printf("[4] SHCONN did not reach SHSTATE:1 - skipping the GET\n");
  }
  clockProbeAt("AT+SHDISC", 5000);

  // ---- [5] what the fallback we actually ship reports, and the live skew it corrects ---------
  // Also the measurement the whole clock-drift investigation wants: how far off is this dongle
  // right now, after however long it has been running on the internal RC oscillator.
  const time_t before = rtc.getEpoch();
  if (network.syncNtp()) {
    const time_t t = parseCclk(network.readClock().c_str());
    if (t) {
      setClock(CLK_NTP, t);
      Serial.printf("[5] NTP ok: rtc was %+lds off (rtc=%ld ntp=%ld)\n",
        (long)(before - t), (long)before, (long)t);
      logEvent("CLKPROBE ntp skew %+lds", (long)(before - t));
    } else {
      Serial.printf("[5] NTP reported success but AT+CCLK still unparseable\n");
    }
  } else {
    Serial.printf("[5] NTP failed\n");
  }
  Serial.printf("=== CLKPROBE done - grep the output above for \"Date:\" ===\n");
}
#endif  // ENABLE_CELL
