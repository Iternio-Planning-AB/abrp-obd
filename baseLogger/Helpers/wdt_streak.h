#pragma once
/*
  The consecutive panic/WDT/brownout reset count behind setup()'s crash-loop escalation, whether a wake may
  have left the modem powered, so a sleep that did not power it this wake still powers it down (and one that
  never powered it does not PWRKEY it on), and whether a trip's closing PARK may not have been sent
  (park_marker_owed, logger.ino).

  #included by logger.ino ahead of sleep_power.h into the single sketch translation unit (and by
  tests/energy_checks.cpp) - NOT a standalone compilation unit.

  Not RTC_DATA_ATTR: the bootloader reloads .rtc.data on every reset that is not a deep-sleep wake, so a
  streak kept there never passed 1 (#30: 20 panics in the field, up to six in a row, each logged streak=1). RTC_NOINIT_ATTR
  is never loaded or cleared, so it also survives panic, WDT and software resets, and holds garbage after a
  power-on. Its address moves with the size of the RTC data before it, so after a rollback this image may
  read bytes another image left there: the magic and the check word reject anything this code did not
  write. Bump the magic if the struct changes.
*/

#define WDT_STREAK_MAGIC 0x57445332u // "WDS2"

struct WdtStreakStore {
  uint32_t magic;
  uint32_t count;
  uint32_t modem;  // 1 = the modem may be powered: set by CellWrapper::init, cleared by an acknowledged CPOWD
  uint32_t marker; // 1 = a PARK may not have reached the server since the last DRIVE (#49)
  uint32_t check;  // ~(count ^ modem << 31 ^ marker << 30)
};
RTC_NOINIT_ATTR WdtStreakStore wdt_streak_store;

void wdtStreakWrite(uint32_t count, uint32_t modem, uint32_t marker = wdt_streak_store.marker) {
  wdt_streak_store = {WDT_STREAK_MAGIC, count, modem, marker, ~(count ^ modem << 31 ^ marker << 30)};
}

// Once per boot, before anything that can crash or hang. A panic, WDT or brownout reset extends the streak
// (a brownout loop drains the battery like a crash loop, #50); any other reset ends it: a deep-sleep wake means
// the last wake ended normally, a software reset was chosen (the OTA commit, a failed trial boot), and a
// power-on is a re-plug. A brownout deep enough to take RTC memory with it may read as POWERON or leave
// garbage, so it restarts at 1. *retained says whether the store held a valid count, which on a crash boot
// is the proof it survived. The modem flag and the marker survive every reset with the store. Lost, the modem
// is "on", except on a power-on: a power loss cut the modem's power too (an EN-pin reset reads POWERON as
// well, but leaves the store intact). A lost marker is not owed.
uint32_t wdtStreakAdvance(esp_reset_reason_t rr, bool* retained) {
  const WdtStreakStore& s = wdt_streak_store;
  *retained = s.magic == WDT_STREAK_MAGIC && s.count < 1u << 30 && s.modem <= 1 && s.marker <= 1 &&
              s.check == ~(s.count ^ s.modem << 31 ^ s.marker << 30);
  bool crash = rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC ||
               rr == ESP_RST_BROWNOUT;
  uint32_t count = crash ? (*retained ? s.count : 0) + 1 : 0;
  wdtStreakWrite(count, *retained ? s.modem : rr == ESP_RST_POWERON ? 0 : 1, *retained ? s.marker : 0);
  return count;
}

void wdtStreakModem(bool on) { wdtStreakWrite(wdt_streak_store.count, on ? 1 : 0); }
bool wdtStreakModemMayBeOn() { return wdt_streak_store.modem != 0; }
void wdtStreakMarker(bool owed) { wdtStreakWrite(wdt_streak_store.count, wdt_streak_store.modem, owed ? 1 : 0); }
bool wdtStreakMarkerOwed() { return wdt_streak_store.marker != 0; }

// CellWrapper's hooks (weak no-ops in cellwrapper.cpp). Every init() may power the modem on; only an
// acknowledged AT+CPOWD proves it off - an unanswered one ends in a PWRKEY pulse, which leaves it unknown.
void cellModemPowerOnHook() { wdtStreakModem(true); }
void cellModemPowerOffHook() { wdtStreakModem(false); }

// Before the OTA commit reboot: a rollback boots this image on the trial image's crash reset, and that
// crash must not be added to one from before the update.
void wdtStreakEnd() { wdtStreakWrite(0, wdt_streak_store.modem); }
