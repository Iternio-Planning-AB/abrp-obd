// State and stubs for one side of the drive differential (run_sm_checks.py). Included once inside
// namespace sm_old and once inside namespace sm_cur, after that side's `enum ActivityState` and before its
// extracted blocks, so both sides run the same harness around different firmware text.

// The firmware globals the extracted blocks touch, with their initialisers. run_sm_checks.py checks every
// entry against its definition line in logger.ino (pin and tree), so a reset here is the firmware's own.
// SM_RAM is reset on every wake (deep sleep is a reboot); SM_RTC survives a wake and is reset on power-on.
#define SM_RAM(X) \
  X(bool, is_currently_parked, true) \
  X(bool, drive_unconfirmed, false) \
  X(bool, g_drive_gnss_refused, false) \
  X(unsigned long, g_last_obd_answer_ms, 0) \
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
  X(unsigned long, transmission_delay, 25000) \
  X(const char*, g_wake_reason, "POWERON") \
  X(bool, g_sample_poke, false) \
  X(float, g_rail_before_poke, 0.0f) \
  X(float, g_last_io36, 0.0f) \
  X(unsigned long, g_last_discharge_ms, 0) \
  X(int, g_next_wake_minutes, WAKE_PARKED_MINUTES) \
  X(unsigned long, g_fast_anchor_ms, 0) \
  X(bool, g_dc_confirmed, false) \
  X(uint8_t, g_dc_inferred_polls, 0) \
  X(unsigned long, g_dc_silent_since_ms, 0) \
  X(bool, g_pdw_ladder_done, false) \
  X(const char*, g_cadence_reason, nullptr) \
  X(bool, g_dc_logged, false) \
  X(bool, g_dc_refused_logged, false) \
  X(unsigned long, g_last_motion_ms, 0) \
  X(float, g_last_voltage, 0.0f) \
  X(bool, g_weak_rail_cold_boot, false)
#define SM_RTC(X) \
  X(bool, park_marker_owed, false) \
  X(bool, recently_charging, false) \
  X(uint32_t, taper_confirm_wakes, 0) \
  X(bool, charge_end_polled, false) \
  X(uint32_t, charging_fast_minutes, 0) \
  X(bool, dc_session, false) \
  X(bool, charging_cap_latched, false) \
  X(uint32_t, charge_uncorroborated_s, 0) \
  X(uint64_t, charge_corr_last_us, 0) \
  X(double, charge_corr_soc_hw, -1.0) \
  X(double, charge_corr_kwh_hw, -1.0) \
  X(double, charge_corr_soe_hw, -1.0) \
  X(double, prev_charge_soc, 0.0) \
  X(double, prev_parked_soc, -1.0) \
  X(double, prev_parked_kwh, -1.0) \
  X(double, prev_parked_soe, -1.0) \
  X(uint8_t, charge_release_polls, 0) \
  X(uint8_t, charge_silent_polls, 0) \
  X(uint8_t, charge_followup, 0) \
  X(uint8_t, pdw_open, 0) \
  X(uint64_t, pdw_open_us, 0) \
  X(uint8_t, pdw_polls, 0) \
  X(uint64_t, charge_last_evidence_us, 0) \
  X(uint32_t, skips_count, 0) \
  X(uint32_t, polls_count, 0) \
  X(time_t, last_poll_epoch, 0)
// Harness-only state: the trace's inputs and what the stubs recorded. Not firmware globals, so not checked.
// sm_store_marker is the RTC_NOINIT debt copy (wdt_streak.h; its own semantics are the energy wdt-streak
// group's). The buffer stands for RAM and the upload backlog together, except during the gate, which runs before
// the backlog loads: there it is the file (sm_backlog_file) and RAM is empty.
enum SmReset { ESP_RST_POWERON = 1, ESP_RST_PANIC = 4, ESP_RST_TASK_WDT = 6, ESP_RST_DEEPSLEEP = 8 };
#define SM_STUB(X) \
  X(SmReset, sm_reset, ESP_RST_POWERON) \
  X(bool, sm_store_marker, false) \
  X(bool, sm_backlog_file, false) \
  X(bool, sm_flush_ok, false) \
  X(unsigned long, sm_now, 0) \
  X(bool, g_ota_active, false) \
  X(bool, g_logdump_active, false) \
  X(bool, sm_gnss_fresh, false) \
  X(unsigned long, sm_gnss_age, 0) \
  X(bool, sm_clock_ok, true) \
  X(unsigned, sm_alarm_calls, 0) \
  X(double, sm_alarm_last, 0) \
  X(unsigned, sm_freezes, 0) \
  X(unsigned long, sm_rtc_ms, 0) \
  X(int, sm_armed_minutes, WAKE_PARKED_MINUTES) \
  X(double, sm_soc, 50) \
  X(double, sm_kwh, 10) \
  X(double, sm_soe, 40000) \
  X(unsigned, sm_polls, 0) \
  X(bool, sm_asleep, false) \
  X(bool, sm_gate_open, true) \
  X(bool, sm_gate_weak, false) \
  X(double, sm_published, -1) \
  X(unsigned, sm_sleeps, 0) \
  X(std::string, sm_sleep_reason, "") \
  X(bool, sm_sleep_modem, false) \
  X(float, sm_atrv, 13.5f) \
  X(int, g_alarm_flag, -1) \
  X(bool, sm_cold_boot, false) \
  X(bool, sm_ble_connected, false)

#define SM_DEFINE(T, name, value) T name = value;
SM_RAM(SM_DEFINE)
SM_RTC(SM_DEFINE)
SM_STUB(SM_DEFINE)
#undef SM_DEFINE

struct SmGps { float speed = 0; } sm_gps;  // knots, as GPS_DATA
SmGps* gd = &sm_gps;
std::vector<OBDVector> obd_result_vector;
std::vector<std::string> sm_log, sm_serial;
unsigned long upload_capped = 0, upload_heap_dropped = 0;
struct SmHeap {
  unsigned long getFreeHeap() const { return 500000; }
  unsigned long getMaxAllocHeap() const { return 250000; }
  unsigned long getMaxAllocPsram() const { return 4000000; }
  unsigned long getPsramSize() const { return 4194304; }
  unsigned long getFreePsram() const { return 4000000; }
} ESP;

// Function-local statics in the extracted blocks, hoisted by run_sm_checks.py so that each boot resets them, as
// a deep-sleep wake does.
#define SM_LOCAL(X) \
  X(int, logged_dcfc, -1)
#define SM_DEFINE(T, name, value) T name = value;
SM_LOCAL(SM_DEFINE)
#undef SM_DEFINE

unsigned long millis() { return sm_now; }
// Logging and the freeze take real time on the device (UART, flash), so each moves the clock: a reordering of
// a clock read against a log line shows up as a different timestamp.
const unsigned long SM_IO_MS = 1;
// The RTC epoch runs with the RTC clock (esp_rtc_get_time_us below) while it is set.
unsigned long sm_rtc_s();
struct { uint32_t getEpoch() const { return sm_clock_ok ? 1791500000u + (uint32_t)sm_rtc_s() : 0u; } } rtc;
bool clockValid() { return sm_clock_ok; }

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
// The OBD-alarm flag, per wake: the gate's ALARMGATE branch and the energy vetoes read it. The loop's per-cycle
// permit is not modelled: an enforced car's polls still reach the car here.
bool alarmEnforced() { return g_alarm_flag == 1; }
// No phone in this harness, except where a scenario connects one (sm_ble_connected, reset at power-on): the extracted
// blocks see a phone only through bleConnected().
bool bleConnected() { return sm_ble_connected; }
// The wake cause setup() reads, as the ESP-IDF values.
enum SmWakeCause { ESP_SLEEP_WAKEUP_UNDEFINED = 0, ESP_SLEEP_WAKEUP_EXT0 = 2, ESP_SLEEP_WAKEUP_TIMER = 4 };
// loop()'s poll condition (sm_poll_reaches_car) reads these beside the weak-rail flag: a profile is always loaded here,
// and the per-cycle alarm permit is not modelled (run_sm_checks.py's header), so both pass.
const bool g_no_profile = false;
bool alarmGateAllowsPoll() { return true; }
// ATRV, the co-processor's pin-16 read: the event's.
struct { float getVoltage() const { return sm_atrv; } } obd;
// The RTC microsecond clock runs on through deep sleep and every reset but a power loss: the time of earlier
// wakes and sleeps (sm_rtc_ms) plus this wake's millis().
uint64_t esp_rtc_get_time_us() { return (uint64_t)(sm_rtc_ms + sm_now) * 1000ULL; }
unsigned long sm_rtc_s() { return (sm_rtc_ms + sm_now) / 1000UL; }
float readIO36Rail() { return g_last_io36; }
// The motion window's peaks only fill log lines; the motion stamp itself is a poll input.
float motion_window_peak = 0, motion_window_axmax = 0;
SmReset esp_reset_reason() { return sm_reset; }
void wdtStreakMarker(bool owed) { sm_store_marker = owed; }
bool wdtStreakMarkerOwed() { return sm_store_marker; }
bool uploadBacklogPending() { return sm_backlog_file; }
void enterDeepSleep(bool modem_on, const char* reason = nullptr);
