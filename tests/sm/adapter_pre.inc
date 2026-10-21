// State and stubs for one side of the drive differential (run_sm_checks.py). Included once inside
// namespace sm_old and once inside namespace sm_cur, after that side's `enum ActivityState` and before its
// extracted blocks, so both sides run the same harness around different firmware text.

// The firmware globals the extracted blocks touch, with their initialisers. run_sm_checks.py checks every
// entry against its definition line in logger.ino (pin and tree), so a reset here is the firmware's own.
// SM_RAM is reset on every wake (deep sleep is a reboot); SM_RTC survives a wake and is reset on power-on.
#define SM_RAM(X) \
  X(bool, is_currently_parked, true) \
  X(unsigned long, g_last_speed_ms, 0) \
  X(unsigned long, last_active, 0) \
  X(ActivityState, current_state, UNINITIALIZED) \
  X(unsigned long, g_ble_disconnect_ms, 0) \
  X(bool, g_ble_handoff_pending, false) \
  X(bool, g_ble_session_relayed, false) \
  X(bool, logging_message_printed, false) \
  X(bool, maintenance_mode, false) \
  X(bool, g_taper_confirm, false) \
  X(unsigned long, polling_delay, 5000) \
  X(unsigned long, transmission_delay, 25000)
#define SM_RTC(X) \
  X(bool, park_marker_owed, false) \
  X(bool, recently_charging, false) \
  X(uint32_t, taper_confirm_wakes, 0) \
  X(bool, charge_end_polled, false)
// Harness-only state: the trace's inputs and what the stubs recorded. Not firmware globals, so not checked.
#define SM_STUB(X) \
  X(unsigned long, sm_now, 0) \
  X(bool, g_ota_active, false) \
  X(bool, g_logdump_active, false) \
  X(bool, sm_gnss_fresh, false) \
  X(unsigned long, sm_gnss_age, 0) \
  X(bool, sm_clock_ok, true) \
  X(unsigned, sm_alarm_calls, 0) \
  X(double, sm_alarm_last, 0) \
  X(unsigned, sm_freezes, 0) \
  X(unsigned, sm_corr_resets, 0) \
  X(unsigned, sm_polls, 0) \
  X(bool, sm_asleep, false) \
  X(bool, sm_gate_open, true) \
  X(bool, sm_gate_weak, false) \
  X(double, sm_published, -1) \
  X(unsigned, sm_sleeps, 0) \
  X(std::string, sm_sleep_reason, "")

#define SM_DEFINE(T, name, value) T name = value;
SM_RAM(SM_DEFINE)
SM_RTC(SM_DEFINE)
SM_STUB(SM_DEFINE)
#undef SM_DEFINE

struct SmGps { float speed = 0; } sm_gps;  // knots, as GPS_DATA
SmGps* gd = &sm_gps;
std::vector<OBDVector> obd_result_vector;
std::vector<std::string> sm_log, sm_serial;

unsigned long millis() { return sm_now; }
// Logging and the freeze take real time on the device (UART, flash), so each moves the clock: a reordering of
// a clock read against a log line shows up as a different timestamp.
const unsigned long SM_IO_MS = 1;
struct { uint32_t getEpoch() const { return sm_clock_ok ? 1791500000u : 0u; } } rtc;

void logEvent(const char* fmt, ...) {
  char text[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  sm_log.emplace_back(text);
  sm_now += SM_IO_MS;
}
struct {
  void printf(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    sm_serial.emplace_back(text);
    sm_now += SM_IO_MS;
  }
} Serial;

bool gpsMovingFresh(unsigned long max_age_ms) {
  sm_gnss_age = max_age_ms;  // compared across sides: the age bound is part of the decision
  return sm_gnss_fresh;
}
// Into the logEvent stream, so the order of the freeze against the LEASH lines is compared too.
void freezeParkedLocation() {
  sm_freezes++;
  sm_now += SM_IO_MS;
  logEvent("PARKED_LOC (harness)");
}
void alarmNoteSpeed(double kmh) { sm_alarm_calls++; sm_alarm_last = kmh; }
void resetChargeCorroboration() { sm_corr_resets++; }
void enterDeepSleep(bool modem_on, const char* reason = nullptr);
