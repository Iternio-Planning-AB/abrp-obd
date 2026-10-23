#include "cellwrapper.h"
#include <algorithm>
#include <sstream>

#ifndef PRINT
#define PRINT 0
#endif

CellWrapper::CellWrapper(FreematicsESP32 sys, std::string apn) : NetworkWrapper(sys), apn{ apn }, net{} { };

bool CellWrapper::init(const bool retry/* = false */) {
  // 45s registration window. REGRESSION FIX: I had cut this to 15s, but a COLD power-cycle needs longer to
  // scan + camp (a healthy unit did ~9s, a slower one needed >15s) - and setting CNMP during init triggers a fresh
  // scan, restarting that clock. 15s cut off legitimate cold attaches, so boot came up offline and only
  // recovered after the modem had camped in the background across a later run. 45s lets a normal cold camp
  // finish. If it still fails (true no-signal/garage), the loop's reconnect() probe completes it the moment
  // the modem registers - so we never lose a registration, we just stop blocking.
  return init(retry, 45000);
}

bool CellWrapper::init(const bool retry, unsigned int registration_timeout_ms) {
  do {
    m_initialized = false;
    m_init_stage = "begin";
    Serial.printf("net begin\n");
    if (!net.begin(&sys)) {
      Serial.printf("No net connected\n");
      continue;
    }
    Serial.printf("net begun\n");
    m_init_stage = "sim";
    if (!net.checkSIM()) {
      Serial.printf("No SIM CARD\n");
      continue;
    }
    Serial.printf("sim checked\n");
    // Force GSM+LTE network mode, but ONLY if it isn't already 51. FIELD FINDING (two identical SIM7070G
    // units): a failing unit sat at CNMP=2 (automatic) with CSQ=99 / CPSI "NO SERVICE" - the auto scan never
    // camped - while a working unit was at CNMP=51 (GSM+LTE) and registered in ~9s. CRITICAL: setting CNMP
    // re-triggers a network scan, which DROPS an existing camp and restarts the slow cold attach - so blindly
    // re-applying 51 every boot would sabotage reconnection. Read first; write once (it persists in NVM, so a
    // mis-provisioned unit self-corrects one time and every later boot skips it). (38 = LTE-only; 51 keeps 2G
    // fallback for roaming dead-zones.)
    {
      char d[64] = { 0 };
      const char* oe[] = { "OK", "ERROR" };
      sys.xbPurge(); sys.xbWrite("AT+CNMP?\r"); sys.xbReceive(d, sizeof(d), 1000, oe, 2);
      if (!strstr(d, "+CNMP: 51")) {
        Serial.printf("Network mode not 51 - setting GSM+LTE (triggers one re-scan)\n");
        char e[64] = { 0 };
        sys.xbPurge(); sys.xbWrite("AT+CNMP=51\r"); sys.xbReceive(e, sizeof(e), 3000, oe, 2);
      }
    }
    // Narrow the RAT/band scan to what this deployment actually uses - THE lever for cold-attach
    // time. FIELD DATA (2026-07-17): with the factory CMNB=3 + all-bands config, a cold power-on
    // does a blind scan of 18 CAT-M + 16 NB-IoT bands x a full roaming PLMN search - ~3 MINUTES to
    // camp on the weak-RX unit - yet both units have only ever camped LTE CAT-M1 (Band 20 locally).
    //   CMNB=1                       -> CAT-M only (halves the scan space; NB-IoT never used)
    //   CBANDCFG="CAT-M",1,3,8,20,28 -> the European LTE-M bands (5 instead of 18)
    // Both persist in modem NVM and re-trigger a scan when WRITTEN, so use the same read-first,
    // write-once guard as CNMP above: a matching value is never rewritten, and a camped/scanning
    // modem is never disturbed on later boots. Revisit the band list outside Europe (US CAT-M
    // would need 2,4,12,13).
    {
      char d[160] = { 0 };
      const char* oe[] = { "OK", "ERROR" };
      sys.xbPurge(); sys.xbWrite("AT+CMNB?\r"); sys.xbReceive(d, sizeof(d), 1000, oe, 2);
      if (!strstr(d, "+CMNB: 1")) {
        Serial.printf("Scan mode not CAT-M-only - setting CMNB=1 (one-time write, persists)\n");
        memset(d, 0, sizeof(d));
        sys.xbPurge(); sys.xbWrite("AT+CMNB=1\r"); sys.xbReceive(d, sizeof(d), 3000, oe, 2);
      }
      static const char CATM_BANDS[] = "1,3,8,20,28";
      char want[48];
      snprintf(want, sizeof(want), "+CBANDCFG: \"CAT-M\",%s", CATM_BANDS);
      memset(d, 0, sizeof(d));
      sys.xbPurge(); sys.xbWrite("AT+CBANDCFG?\r"); sys.xbReceive(d, sizeof(d), 1000, oe, 2);
      if (!strstr(d, want)) {
        Serial.printf("CAT-M band list not narrowed - setting %s (one-time write, persists)\n", CATM_BANDS);
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "AT+CBANDCFG=\"CAT-M\",%s\r", CATM_BANDS);
        memset(d, 0, sizeof(d));
        sys.xbPurge(); sys.xbWrite(cmd); sys.xbReceive(d, sizeof(d), 3000, oe, 2);
      }
    }
    // DIAGNOSTIC: identify the ACTUAL modem + its network mode before doing anything else. Our HTTP layer
    // uses SIM7070 "SH" commands (SHCONN/SHBOD/SHREQ/SHDISC). If this board is a SIM7670 (LTE Cat-1) as the
    // hardware spec states, that is the WRONG command family - the SIM7670 uses AT+HTTPINIT/HTTPACTION/
    // HTTPREAD, and LTE-M/LPWA commands (PSM/eDRX/CMNB) don't apply and can misconfigure it. This prints the
    // model so we can confirm the silicon and use the correct command set instead of guessing.
    {
      char d[256] = { 0 };
      sys.xbPurge(); sys.xbWrite("ATI\r");               sys.xbReceive(d, sizeof(d), 1000); Serial.printf("DIAG ATI: %s\n", d);
      memset(d, 0, sizeof(d)); sys.xbPurge(); sys.xbWrite("AT+SIMCOMATI\r"); sys.xbReceive(d, sizeof(d), 2000); Serial.printf("DIAG SIMCOMATI: %s\n", d);
      memset(d, 0, sizeof(d)); sys.xbPurge(); sys.xbWrite("AT+CNMP?\r");      sys.xbReceive(d, sizeof(d), 1000); Serial.printf("DIAG CNMP: %s\n", d);
    }
    m_initialized = true;
    if (!finishInit(registration_timeout_ms)) {
      Serial.printf("Failed to connect to server using apn %s\n", apn.c_str());
      // Full persistent-radio-config dump on the FIRST failure of this boot (read-only). Includes
      // the live CSQ/CPSI/CEREG/COPS diagnostics plus every NVM-backed setting that survives a
      // power pull (CMNB/CBANDCFG/PSM/eDRX/...) - the settings that can make two "identical" units
      // behave differently at cold attach. Diff a failing unit's dump against a working one's.
      {
        static bool dumped_this_boot = false;
        if (!dumped_this_boot) {
          dumped_this_boot = true;
          dumpRadioConfig();
        }
      }
      continue;
    }
    return true;
  } while (retry);
  m_ready = false;
  return false;
}

bool CellWrapper::finishInit(unsigned int registration_timeout_ms) {
  m_init_stage = "registration";
  if (!net.setup(apn.c_str(), registration_timeout_ms)) {
    m_ready = false;
    return false;
  }
  Serial.printf("apn setup\n");
  // Bind the APN to PDP context 0 for the SH HTTP data path. The Freematics open() runs AT+CNACT but
  // NOT AT+CNCFG, so without this the context activates with no usable data binding and SH requests die
  // with a bare "ERROR". (Confirmed against the standalone modem test, which does CNCFG and succeeds.)
  {
    char cmd[80], b[96] = { 0 };
    snprintf(cmd, sizeof(cmd), "AT+CNCFG=0,0,\"%s\"\r", apn.c_str());
    sys.xbPurge();
    sys.xbWrite(cmd);
    sys.xbReceive(b, sizeof(b), 3000);
  }
  // Enable NITZ (network time) so AT+CCLK reads a real date; persisted in the modem's NVM (see readClock).
  {
    char b[64] = { 0 };
    sys.xbPurge();
    sys.xbWrite("AT+CTZU=1\r");
    sys.xbReceive(b, sizeof(b), 1000);
  }
  // Disable LTE-M power-saving (PSM + eDRX). Per Freematics' own notes, the SIM7070G on Cat-M1 can enter a
  // PSM/eDRX "deep sleep" lockup when the tower forces it to sleep or changes bands: the modem's internal
  // stack freezes, the ESP32 keeps pushing bytes, and only a real reset recovers it. That is exactly the
  // http=0-at-strong-signal freeze seen mid-drive (and the community's "fails 49/50 at perfect signal").
  // Turning PSM + eDRX off keeps the radio always-on so the tower can't park it in the locking state - a
  // preventive fix that complements the CFUN radio-cycle recovery in restart().
  {
    char b[64] = { 0 };
    const char* okerr[] = { "OK", "ERROR" };
    sys.xbPurge(); sys.xbWrite("AT+CPSMS=0\r");  sys.xbReceive(b, sizeof(b), 2000, okerr, 2); // PSM off
    memset(b, 0, sizeof(b));
    sys.xbPurge(); sys.xbWrite("AT+CEDRXS=0\r"); sys.xbReceive(b, sizeof(b), 2000, okerr, 2); // eDRX off
  }
  if (!m_ready) m_ready_ms = millis();
  m_ready = true;
  m_dead_rf_since = 0; // radio proved alive - re-arm the dead-RF revival ladder
  m_revive_stage = 0;
  return true;
}


bool CellWrapper::connect() {
  // Keep-alive: reuse a live SH session across uploads instead of tearing it down every cycle. This mirrors
  // the reference Freematics TeleClientHTTP::transmit (which only reopens when state() != HTTP_CONNECTED) and
  // WiFiWrapper::connect (which returns true when https is already connected). The caller now disconnects
  // ONLY after a failed upload, so a healthy session is reused (no per-cycle TLS handshake); a dead session
  // fails once, gets closed, and the next connect() opens a fresh one.
  m_idle_closed_s = -1;
  if (net.state() == HTTP_CONNECTED && m_idle_max_ms && millis() - m_last_use_ms > m_idle_max_ms) {
    m_idle_closed_s = (long)((millis() - m_last_use_ms) / 1000);
    net.close(); // a long-idle session is dead server- or NAT-side; reusing it costs an ~8 s http=0
  }
  m_reused = net.state() == HTTP_CONNECTED;
  if (m_reused) return true;
  bool ok = net.open(url.c_str(), 443);
  if (ok) m_last_use_ms = millis();
  return ok;
}

void CellWrapper::disconnect() {
  net.close();
}

void CellWrapper::restart() {
  // Radio cycle (AT+CFUN=0 -> AT+CFUN=1). A frozen SH/TLS stack - http=0 while signal is STRONG (csq good,
  // vbat healthy), the LTE-M PSM/band-change lockup - is NOT cleared by a soft re-init: field data showed
  // net.close()+init() twice had no effect and only a full modem power-cycle (deep-sleep reboot) recovered
  // it. AT+CFUN=0/1 is the fast "warm reset" in between: it detaches, FLUSHES the modem's network stack +
  // frozen socket, and re-attaches in ~3s WITHOUT powering the modem down - so it keeps the GPS fix, avoids
  // the CPOWD roaming re-registration cost, and never touches the ESP32/BLE. Then re-run registration +
  // APN/CNCFG and let the next connect() open a fresh session. (Escalation if this proves insufficient:
  // AT+CRESET, a modem firmware reboot. The deep-sleep reboot remains the last-resort backstop.)
  // Soft re-init: close the SH session and re-run modem setup (registration check + APN/CNCFG). Deliberately
  // NO AT+CFUN re-attach here - it backfired: repeated re-attaches on a ROAMING SIM pushed the visited network
  // into a registration-reject BACKOFF, so the modem then could not attach for a long stretch even at strong
  // signal ("Failed to connect" on fresh boots). A genuinely wedged modem that a soft re-init cannot clear is
  // handled by the bounded deep-sleep reboot, which power-cycles the modem via CPOWD (the reliable reset).
  Serial.printf("Restarting cellular connection (soft re-init)\n");
  m_ready = false;
  net.close();
  init();
}

void CellWrapper::reconnect() {
  // Loop calls this on a cadence while offline. FIELD FIX (dump windows 1783776631/1783777890/1783783523):
  // this used to call restart() = a full CPOWD power-cycle + re-init that BLOCKED the poll loop ~40s per
  // attempt - capture density fell to a measured 41% (6s polls interleaved with 41-45s gaps), and each
  // power-cycle killed the registration attempt in progress, so drives that started outside coverage never
  // attached for the whole drive even after driving back into coverage.
  //
  // The modem needs no restarting to attach: once powered with CFUN=1 (boot init did that), it keeps trying
  // to register on its own in the background. So this is now a CHEAP NON-BLOCKING probe (~0.1-1s):
  //   - modem answers, not registered  -> return immediately; background attach continues, poll loop unhurt
  //   - modem answers, registered      -> finish APN/CNCFG binding without begin()/PWRKEY or radio config;
  //                                       one registration recheck only -> m_ready -> uploads resume
  //   - modem doesn't answer at all    -> it's off/dead (boot init failed at power-on) -> full init() to
  //                                       power it on; this is the one blocking path and only fires when
  //                                       the modem is genuinely down, not every retry interval
  // restart()'s power-cycle is deliberately NOT used here - it stays reserved for the http=0 stale-bearer
  // recovery (a REGISTERED modem with a wedged data stack), where it is field-proven necessary.
  if (m_ready) return;
  char b[96] = { 0 };
  const char* okerr[] = { "OK", "ERROR" };
  // Probe EPS registration first (CEREG = LTE-M/CAT-M1, the RAT this modem actually attaches on when roaming),
  // then fall back to GPRS (CGREG). Checking only CGREG would report a fully-registered LTE modem as "not
  // registered" - the same false negative that made setup() fail while NITZ/NTP worked.
  sys.xbPurge();
  sys.xbWrite("AT+CEREG?\r");
  sys.xbReceive(b, sizeof(b), 1000, okerr, 2);
  bool answered = strstr(b, "+CEREG:") != nullptr;
  char* p = strstr(b, "+CEREG: 0,");
  if (p && p[10] >= '0' && p[10] <= '5') m_registration = p[10];
  bool registered = p && ( *( p + 10 ) == '1' || *( p + 10 ) == '5' );
  if (!registered) {
    memset(b, 0, sizeof(b));
    sys.xbPurge();
    sys.xbWrite("AT+CGREG?\r");
    sys.xbReceive(b, sizeof(b), 1000, okerr, 2);
    answered = answered || strstr(b, "+CGREG:") != nullptr;
    p = strstr(b, "+CGREG: 0,");
    registered = p && ( *( p + 10 ) == '1' || *( p + 10 ) == '5' );
  }
  if (!answered) {
    Serial.printf("Cell reconnect: modem not answering - full init\n");
    init();
    return;
  }
  if (!registered) {
    // Still hunting for the network. Normally zero-cost: let the modem keep scanning in the
    // background. BUT check for the dead-RF signature first (see reviveDeadRadio) - a modem that
    // boots with its radio dead reports "searching" forever and will never recover on its own.
    reviveDeadRadio();
    return;
  }
  Serial.printf("Cell reconnect: registered - completing data setup\n");
  if (!m_initialized) {
    init();
    return;
  }
  finishInit(0); // setup(apn, 0) makes one registration pass, not an unbounded search
}

void CellWrapper::reviveDeadRadio() {
  // FIELD SIGNATURE (unit ...705073): from a cold power-on this modem sometimes boots with the
  // radio effectively DEAD - AT-responsive, CFUN=1, CEREG says "searching" - but AT+CSQ stays
  // 99,99 (not one dBm ever measured) indefinitely, while an identical unit beside it camps in 9s.
  // The RADIO dumps of both units are byte-identical, so it is NOT an NVM setting; only a modem
  // restart revives it (the old firmware "fixed" it by accident: its infinite retry loop kept
  // kicking the modem with PWRKEY pulses + re-inits until the radio came alive).
  //
  // Ladder, one bounded step per DEAD_RF_MS of continuous dead-RF observation:
  //   stage 1: AT+CFUN=0 -> AT+CFUN=1 (radio cycle, ~seconds)
  //   stage 2: AT+CFUN=1,1            (full modem reboot, then re-init)
  //   stage 3: CPOWD power-cycle + re-init (the big hammer)
  // ONLY the csq=99 signature escalates. A modem that SEES signal but hasn't registered is left
  // strictly alone - cycling a legitimately-searching roaming modem provokes network reject
  // backoff (learned the hard way). One ladder pass per boot; stages reset on a successful init.
  //
  // THRESHOLD: csq stays 99 during a perfectly healthy scan (CSQ only measures once camped), and
  // the field-measured healthy cold scan on the weak-RX unit is ~3 MINUTES (camped at t=181s,
  // 2026-07-17 soak; its RX reads ~8dB below the sibling unit's). The threshold must sit safely
  // beyond the slowest healthy scan or stage 1 would restart a scan that was about to finish -
  // 5 minutes gives ~2 minutes of margin over the worst observed case.
  const unsigned long DEAD_RF_MS = 300000;
  const char* okerr[] = { "OK", "ERROR" };
  char b[64] = { 0 };
  sys.xbPurge();
  sys.xbWrite("AT+CSQ\r");
  sys.xbReceive(b, sizeof(b), 1000, okerr, 2);
  char* p = strstr(b, "+CSQ:");
  int csq = p ? atoi(p + 5) : 99;
  if (csq != 99) {
    m_dead_rf_since = 0; // radio measures signal - it's alive and legitimately searching
    return;
  }
  if (!m_dead_rf_since) {
    m_dead_rf_since = millis();
    return;
  }
  if (millis() - m_dead_rf_since < DEAD_RF_MS) return;
  m_dead_rf_since = 0; // restart the observation window after each step
  switch (m_revive_stage) {
    case 0:
      m_revive_stage = 1;
      Serial.printf("REVIVE stage 1: radio dead (csq=99 for %lus) - CFUN radio cycle\n", DEAD_RF_MS / 1000UL);
      memset(b, 0, sizeof(b));
      sys.xbPurge(); sys.xbWrite("AT+CFUN=0\r"); sys.xbReceive(b, sizeof(b), 5000, okerr, 2);
      delay(2000);
      memset(b, 0, sizeof(b));
      sys.xbPurge(); sys.xbWrite("AT+CFUN=1\r"); sys.xbReceive(b, sizeof(b), 5000, okerr, 2);
      break;
    case 1:
      m_revive_stage = 2;
      Serial.printf("REVIVE stage 2: still dead - modem reboot (AT+CFUN=1,1)\n");
      sys.xbPurge();
      sys.xbWrite("AT+CFUN=1,1\r");
      delay(10000); // modem reboots; UART is gone for several seconds
      init();       // full re-init (begin + registration window + APN)
      break;
    case 2:
      m_revive_stage = 3;
      Serial.printf("REVIVE stage 3: still dead - CPOWD power-cycle\n");
      powerOff();
      delay(2000);
      init();
      break;
    default:
      // Ladder exhausted this boot - a genuinely dead radio (hardware/antenna) can't be fixed
      // from software; keep probing quietly so a late recovery is still picked up.
      break;
  }
}

void CellWrapper::powerOff() {
  // Power the modem fully down before deep sleep so it stops drawing (it was left ON -> overnight battery
  // drain). SIM7070 uses AT+CPOWD=1 (NOT AT+CPOF, which HTTPClientSIM::end() wrongly sends). Fall back to a
  // long PWRKEY pulse if the modem doesn't acknowledge. The next boot's init() powers it back on.
  // Ack detection, two defects fixed. The FALLBACK SEMANTICS BELOW ARE DELIBERATELY UNCHANGED:
  //   1. expected-match array: without one, xbReceive cannot return early and burns the whole 3000ms on
  //      every sleep - violating the rule this file already states for syncNtp ("every xbReceive() MUST pass
  //      expected-match strings"). It also maximised the window for defect 2.
  //   2. buffer size: xbReceive discards the OLDEST line once it passes bufsize-16 (48 for char[64]), and
  //      "NORMAL POWER DOWN" is emitted FIRST - so unsolicited output arriving in those 3s could push the
  //      ack out of the buffer, strstr would miss it, and the PWRKEY fallback would fire on a modem that had
  //      already powered down, switching it back ON to draw through the whole sleep. A NITZ push alone
  //      (*PSUTTZ + DST, ~50 bytes, present in this unit's field logs) is enough to do it.
  // ONLY "POWER DOWN" is matched: adding "OK"/"ERROR" would return early on an OK that some firmwares emit
  // BEFORE the ack, i.e. report a SUCCESSFUL power-down as a failure and PWRKEY the modem mid-shutdown.
  char b[192] = { 0 };
  const char* expect_pd[] = { "POWER DOWN" };
  sys.xbPurge();
  sys.xbWrite("AT+CPOWD=1\r");
  int r = sys.xbReceive(b, sizeof(b), 3000, expect_pd, 1); // xbReceive returns matched index+1, so 1 = ack
  // Bench evidence for the sleep-floor work: which branch ran and what the modem actually said. The fallback
  // stays UNCONDITIONAL on a missing ack - it is what protects a powered-but-HUNG modem, which this firmware
  // has field history of (the reconnect "modem not answering" branch, the PSM lockup note above,
  // net_reboot_count). Making it conditional trades one failure mode for another and needs its own session.
  Serial.printf("CPOWD r=%d fallback=%d [%s]\n", r, r != 1 ? 1 : 0, b);
  if (r != 1) {
    sys.xbTogglePower(2510); // ~2.5s PWRKEY hold = hardware power-off
  }
  m_initialized = false;
  m_ready = false;
}

HttpResponse CellWrapper::getEpoch() {
  auto response = net.getEpoch(api_page.c_str());
  // Serial.printf("Response: %d, %s\n", response.returnValue, response.buffer);
  return HttpResponse{ response.returnValue, std::string(response.buffer) };
}

bool CellWrapper::set_headers(const HeaderVector headers) {
  return net.setHeaders(headers);
}

void CellWrapper::set_api_page(const char* new_api_page) {
  api_page = new_api_page;
}

bool CellWrapper::syncNtp() {
  // Sync the modem RTC from NTP over the data bearer - robust where carrier NITZ isn't delivered (the
  // roaming Hologram SIM never received the NITZ push; AT+CCLK stayed at the 1980 default).
  // UPDATE (field, 2026-08): NITZ pushes ARE now arriving on this unit - the serial logs show *PSUTTZ + DST
  // URCs and a "Clock set from NITZ" event on most boots - so NTP is a FALLBACK here, not the only source.
  // Those unsolicited pushes matter beyond the clock: they are the ~50 bytes that can overrun a small
  // xbReceive buffer, which is why powerOff() now uses char[192] plus an expected-match array.
  // Activate PDP
  // context 0, point NTP at it (tz 0 -> CCLK reads UTC), then execute and wait for the "+CNTP: <code>" URC.
  //
  // TIMING FIX: every xbReceive() MUST pass expected-match strings. xbReceive only returns early when the
  // buffer matches an expected string - with none it blocks the ENTIRE timeout on every call, which made
  // each sync cost a fixed ~19s (5+1+1+12) regardless of outcome. With the matches below, each step returns
  // the moment the modem answers: a healthy sync completes in ~1s (bench-verified 843-1108ms).
  unsigned long t0 = millis();
  char b[160] = { 0 };
  const char* okerr[] = { "OK", "ERROR" };
  sys.xbPurge();
  sys.xbWrite("AT+CNACT=0,1\r");                  // ensure PDP context 0 (data) is up; "ERROR" if already
  sys.xbReceive(b, sizeof(b), 5000, okerr, 2);    //   active - fine either way, NTP runs on the live context
  memset(b, 0, sizeof(b));
  sys.xbPurge();
  sys.xbWrite("AT+CNTPCID=0\r");                  // run NTP over context 0
  sys.xbReceive(b, sizeof(b), 2000, okerr, 2);
  memset(b, 0, sizeof(b));
  sys.xbPurge();
  sys.xbWrite("AT+CNTP=\"pool.ntp.org\",0\r");    // server, timezone 0 -> modem RTC stored as UTC
  sys.xbReceive(b, sizeof(b), 2000, okerr, 2);
  memset(b, 0, sizeof(b));
  const char* cntp[] = { "+CNTP:" };              // result URC arrives AFTER the server round-trip
  sys.xbPurge();
  sys.xbWrite("AT+CNTP\r");                        // execute the sync
  sys.xbReceive(b, sizeof(b), 15000, cntp, 1);
  bool ok = strstr(b, "+CNTP: 1") != nullptr;     // 1=ok; 61=net 62=dns 63=conn 66=timeout (SIM7070)
  Serial.printf("NTP sync %s in %lums (%s)\n", ok ? "ok" : "FAILED",
    (unsigned long)(millis() - t0), b);
  return ok;
}

std::string CellWrapper::readClock() {
  // Raw AT+CCLK? query; returns the quoted time payload, e.g. 80/01/06,00:31:48+02 (or "" if unavailable).
  char buf[160] = { 0 };
  sys.xbPurge();
  sys.xbWrite("AT+CCLK?\r");
  sys.xbReceive(buf, sizeof(buf), 1000);
  char* p = strstr(buf, "+CCLK:");
  if (!p) return std::string();
  p = strchr(p, '"');
  if (!p) return std::string();
  ++p;
  char* end = strchr(p, '"');
  if (!end) return std::string();
  return std::string(p, end - p);
}

std::string CellWrapper::sendRawAt(const char* cmd, unsigned int timeout_ms) {
  // Bench passthrough: no parsing, no state - return exactly what the modem said. OK/ERROR end the
  // wait early; long operations (AT+COPS=? network survey ~1-3min) just get a big timeout from the
  // caller. Purge first so a stale tail from a previous exchange can't masquerade as the reply.
  char buf[512] = { 0 };
  std::string line = std::string(cmd) + "\r";
  const char* okerr[] = { "OK", "ERROR" };
  sys.xbPurge();
  sys.xbWrite(line.c_str());
  sys.xbReceive(buf, sizeof(buf), timeout_ms, okerr, 2);
  return std::string(buf);
}

void CellWrapper::dumpRadioConfig() {
  // READ-ONLY radio-state dump. Purpose: two "identical" SIM7070G units behave differently at cold
  // attach (one camps in ~9s, one never camps), which points at NVM-persisted modem settings - the
  // kind that survive a power pull and that past experiments (standalone modem test scripts, older
  // firmware) may have written. Everything queried here persists in modem NVM or shows live radio
  // state; NOTHING here writes. Diff the output of a failing unit against a working unit.
  static const char* queries[] = {
    "AT+CNMP?",     // network mode: 2 auto, 13 GSM-only, 38 LTE-only, 51 GSM+LTE
    "AT+CMNB?",     // LTE scan mode: 1 CAT-M, 2 NB-IoT, 3 both - NB-only can NEVER camp on LTE-M
    "AT+CBANDCFG?", // per-RAT band lists - a narrowed list left by an old experiment kills camping
    "AT+CPSMS?",    // power saving mode (PSM)
    "AT+CEDRXS?",   // eDRX
    "AT+CSCLK?",    // UART autosleep
    "AT+COPS?",     // operator selection: "+COPS: 1,..." = MANUAL mode pinned to a stored operator
    "AT+CGDCONT?",  // PDP contexts + APNs
    "AT+CNCFG?",    // app-PDN APN binding
    "AT+CFUN?",     // functionality (1 = full)
    "AT+CPIN?",     // SIM state
    "AT+CSQ",       // live signal (99,99 = nothing measured)
    "AT+CREG?",     // GSM registration state
    "AT+CGREG?",    // GPRS registration state
    "AT+CEREG?",    // EPS/LTE-M registration state (2nd digit: 1 home, 2 searching, 3 DENIED, 5 roaming)
    "AT+CPSI?",     // full radio status line
  };
  Serial.printf("=== MODEM RADIO CONFIG (read-only dump) ===\n");
  for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
    std::string r = sendRawAt(queries[i], 3000);
    std::string flat; // collapse CR/LF runs so each query prints as one compact greppable line
    for (char c : r) {
      if (c == '\r' || c == '\n') { if (!flat.empty() && flat.back() != ' ') flat += ' '; }
      else flat += c;
    }
    Serial.printf("%-14s -> %s\n", queries[i], flat.c_str());
  }
  Serial.printf("=== END RADIO CONFIG ===\n");
}

std::string CellWrapper::netDiag() {
  // Cheap signal + modem-voltage probe, logged alongside each upload so a failure can be attributed:
  //   AT+CSQ -> "+CSQ: <rssi>,<ber>"  rssi 0-31 (99=unknown); dBm = rssi*2-113 (so 20 -> -73dBm, 31 -> -51)
  //   AT+CBC -> "+CBC: <v>"           modem VBAT (raw payload logged as-is: volts like 4.05, or bcs,bcl,mV)
  // Both are standalone AT queries and do NOT touch the SH data session, so they are safe with a POST session
  // open. Fast: each xbReceive returns the moment "OK" arrives (~tens of ms), not the full timeout.
  const char* okerr[] = { "OK", "ERROR" };
  char b[96] = { 0 };
  int csq = 99;
  sys.xbPurge();
  sys.xbWrite("AT+CSQ\r");
  sys.xbReceive(b, sizeof(b), 800, okerr, 2);
  char* p = strstr(b, "+CSQ:");
  if (p) csq = atoi(p + 5);

  char vbat[24] = "?";
  memset(b, 0, sizeof(b));
  sys.xbPurge();
  sys.xbWrite("AT+CBC\r");
  sys.xbReceive(b, sizeof(b), 800, okerr, 2);
  p = strstr(b, "+CBC:");
  if (p) {
    p += 5;
    while (*p == ' ') ++p;
    int n = 0;
    while (*p && *p != '\r' && *p != '\n' && n < (int)sizeof(vbat) - 1) vbat[n++] = *p++;
    vbat[n] = 0;
  }

  // EPS registration state (2nd digit of +CEREG: 0,<stat>): 1 home, 2 searching, 3 denied,
  // 5 roaming, 0 idle. Makes offline heartbeats answer "is it still hunting or has it camped?".
  char cereg = '?';
  memset(b, 0, sizeof(b));
  sys.xbPurge();
  sys.xbWrite("AT+CEREG?\r");
  sys.xbReceive(b, sizeof(b), 800, okerr, 2);
  p = strstr(b, "+CEREG: 0,");
  if (p && p[10] >= '0' && p[10] <= '5') {
    cereg = p[10];
    m_registration = cereg;
  }

  int dbm = (csq != 99) ? (csq * 2 - 113) : 0;
  char out[72];
  snprintf(out, sizeof(out), "csq=%d(%ddBm) vbat=%s cereg=%c", csq, dbm, vbat, cereg);
  return std::string(out);
}

std::pair<std::string, std::string> CellWrapper::initFailureDiag() {
  std::string result = std::string("stage=") + m_init_stage;
  if (strcmp(m_init_stage, "registration") != 0) return { result, {} };
  result += " " + netDiag();
  std::string radio;
  std::string cpsi = sendRawAt("AT+CPSI?", 1000);
  size_t start = cpsi.find("+CPSI:");
  if (start != std::string::npos) {
    start += 6;
    while (start < cpsi.size() && cpsi[start] == ' ') ++start;
    size_t end = cpsi.find_first_of("\r\n", start);
    if (end == std::string::npos) end = cpsi.size();
    size_t comma = cpsi.find(',', start);
    if (comma < end) {
      size_t second = cpsi.find(',', comma + 1);
      if (second < end) end = second;
    }
    radio = "cpsi=" + cpsi.substr(start, std::min(end - start, size_t(24)));
  }
  std::string cops = sendRawAt("AT+COPS?", 1000);
  int mode;
  const char* p = strstr(cops.c_str(), "+COPS:");
  if (!radio.empty()) radio += " ";
  radio += "cops=" + (p && sscanf(p, "+COPS: %d", &mode) == 1 ? std::to_string(mode) : "?");
  return { result, radio };
}

HttpResponse CellWrapper::sendBulk(std::string payload) {
  // The headers logNetwork set authenticate the request (a signature, no token), so the path goes up
  // bare. The body may be gzip here, which is why it is passed with an explicit size and never treated
  // as a C string: it contains NULs.
#if PRINT
  Serial.printf("bulk body: %u bytes\n", (unsigned)payload.size());
#endif
  auto response = net.send(api_page.c_str(), payload.data(), payload.size());
  m_last_use_ms = millis();
  return HttpResponse{ response.returnValue, std::string(response.buffer) };
}

std::string CellWrapper::lastOpenTrace() {
  if (m_reused) return "reused";
  if (m_idle_closed_s < 0) return net.openTrace();
  return "idle=" + std::to_string(m_idle_closed_s) + "s " + net.openTrace();
}

long CellWrapper::upForMs() {
  return m_ready ? (long)(millis() - m_ready_ms) : -1;
}

bool CellWrapper::connected() {
  // "Link up" in the persistent sense the loop/upload-gate expects: modem set up + APN attached. NOT the
  // transient HTTP socket state (net.state()==HTTP_CONNECTED), which is only open between connect()/close().
  return m_ready;
}