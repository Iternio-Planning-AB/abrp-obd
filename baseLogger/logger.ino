/*
  Arduino libraries
*/
#include <Arduino.h>
#include <ESP32Time.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <json.hpp>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp32/rtc.h>     // esp_rtc_get_time_us(): monotonic across deep sleep (charge corroboration)
#include <esp_task_wdt.h> // wedge-guard task watchdog (WEDGE_WDT_TIMEOUT_S) - see boot 153 in the 71h dump
#include <driver/i2c.h>
#include <cstdarg> // va_list/vsnprintf for logEvent()

/*
  Custom library files
*/
#include <obdwrapper.h>
#include <blewrapper.h>
#include "config.h"
#include "Helpers/icm42627_cal.h"
#include <cellwrapper.h>
#include <wifiwrapper.h>
#if ENABLE_LOGGING
#include <datalogger.h>
#endif

/*
  c++ std libraries
*/
#include <sstream>
#include <deque>

using json = nlohmann::json;


Preferences preferences;
ESP32Time rtc(0);
// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long last_poll = 0;
unsigned long last_active = 0;
unsigned long last_broadcast = 0;
// millis() of the last BLE disconnect (stamped in changeState on leaving the BLE state). Gates cellular so a
// TLS POST doesn't collide with the phone's reconnect handshake on the shared modem UART (see BLE_UPLOAD_GRACE_MS).
// 0 = no BLE session has ended this wake -> no hold-off (cellular runs immediately when there's no phone).
unsigned long g_ble_disconnect_ms = 0;
// Fast BLE->cellular handoff: set when a BLE session ends; makes the FIRST cellular upload fire as soon as
// the grace window clears (bypassing the batching timer), so the cloud shows a live point within seconds of
// the phone leaving instead of feeling "stuck". Cleared on the first fired upload or a BLE reconnect.
bool g_ble_handoff_pending = false;
// Did the CURRENT (or just-ended) BLE session ever actually relay - i.e. did it pass the ABD8 proof? Sticky
// for the session: set the moment bleRelaying() is first observed, cleared on entering the BLE state.
//
// Exists because the grace window and the handoff above are both about a TRANSPORT HANDOFF, and a session
// that never authenticated was never the transport. A phone that connects, fails to authorize and retries -
// an old app build that doesn't speak ABD8 at all, or a user declining the pairing dialog - would otherwise
// stamp a fresh BLE_UPLOAD_GRACE_MS hold-off on every flap, and since the app's reconnect interval is about
// the same as the grace window that can starve cellular for as long as the phone keeps trying. Same
// reasoning as bleRelaying() itself: a peer that relays nothing must not be able to take cellular away.
bool g_ble_session_relayed = false;
// Consecutive connection-level upload failures (http=0). Drives the stale-bearer recovery: after
// HTTP0_RESTART_THRESHOLD in a row we re-init the modem. Reset on any successful upload or a server response (4xx/5xx).
int g_http0_streak = 0;
// Modem power-cycles since the last successful upload. Field-proven: a wedged SIM7070 can survive MULTIPLE
// full power-cycles (3 in one drive) - the only recovery that has ever worked is a fresh ESP32 boot. After
// 2 fruitless power-cycles we take that boot deliberately via the normal sleep path (backlog saves every
// frame; WOM re-wakes within ~a second while driving). net_reboot_count (RTC, survives that reboot) bounds
// the escalation to 2 reboots per outage so a genuine dead-coverage drive can't reboot-loop.
int g_recovers_since_ok = 0;
RTC_DATA_ATTR uint32_t net_reboot_count = 0;
// CONSECUTIVE wedge-guard (task WDT) resets. RTC RAM survives a WDT reset, so this counts back-to-back
// wedged wakes: cleared on any clean deep-sleep wake (the previous wake ended normally). At >=2 the wake
// after a WDT reset goes straight back to sleep on the parked cadence instead of re-running the wedge-prone
// init a third time in a row - bounding a persistently-wedging modem to ~2 WDT windows per parked cycle
// instead of a 5-minute reset loop burning the 12V all night.
RTC_DATA_ATTR uint32_t wdt_reset_count = 0;
bool g_wdt_wake = false; // this boot came from a wedge-guard (WDT) reset - set at wake, drives the escalation
unsigned long polling_delay = 5000; //5 seconds
// broadcast delay 25 seconds by default
unsigned long transmission_delay = 25000;

//Don't perform actions unless device has been initialized
bool config_initialized = false;
bool config_message_printed = false; // print message once
bool logging_message_printed = false; // print message once

// Latched state behind the is_parked telemetry parameter. Defaults to parked on every boot/wake;
// unlatched to driving by speed > 0 in loop() and re-latched by the activity state machine once
// motion has stopped (see changeState). This is the IMU "smart cheat" - there is no real gear PID.
// Written only by the drive region (Helpers/drive_state.h), as are g_last_speed_ms and park_marker_owed.
bool is_currently_parked = true;
// Set when a DRIVE frame enters the upload buffer, cleared when the pre-sleep PARK marker is queued: the
// server has been told DRIVE and not yet PARK. RTC so a trip end that straddles a reboot (the mid-drive
// modem-recovery sleep, a WDT reset) is still closed on the next wake, while a parked heartbeat wake owes
// nothing and opens no connection.
RTC_DATA_ATTR bool park_marker_owed = false;
// millis() of the last speed>0 frame. Drives the TIME-BASED parked re-latch below: the state-machine re-latch
// (changeState -> CHARGING/STANDBY) never fires while a connected phone holds the leash in the BLE state, so
// after a drive the latch stayed "driving" forever - which disabled the MAX_AWAKE backstop (parked-gated) AND
// is_charging (parked-gated). A phone forgotten in the car could then hold the device + the car's ECUs awake
// indefinitely - the exact 12V-drain scenario the backstop exists to prevent.
unsigned long g_last_speed_ms = 0;
// Last sample whose current was above CHARGE_DISCHARGE_AMPS. Plain RAM: 0 at every wake, so a streaming wake
// confirms a charge only after CHARGE_STREAM_CONFIRM_SECONDS of its own samples.
unsigned long g_last_discharge_ms = 0;

#if ENABLE_WIFI
// One-shot guard for the cold-boot WiFi RF-flush recovery (see setup()). RTC memory survives the recovery
// deep-sleep but is cleared by a true power loss, so the flush can fire at most once per power-up - this is
// what prevents a boot loop when WiFi is simply unavailable.
RTC_DATA_ATTR bool wifi_recovery_pending = false;
#endif

// Latched by the serial console (any command). While set, the device refuses to deep-sleep so a USB
// session - e.g. a log DUMP - can't be cut off mid-stream by the activity state machine. Cleared by RESUME,
// or auto-released after MAINTENANCE_IDLE_MS of console silence so a missed RESUME can't keep it awake forever.
bool maintenance_mode = false;
unsigned long maintenance_since = 0;                // millis() of the last console command
const unsigned long MAINTENANCE_IDLE_MS = 120000;   // auto-release maintenance_mode after this much idle

// TRUE only while a BLE firmware update is actually transferring/staging (see Helpers/ota_update.h). This is
// the single flag every subsystem checks to stand down: deep sleep, the max-awake backstop, OBD polling, BLE
// telemetry notifies, the cellular retry/upload paths, the config-push apply, and the serial console.
// Declared HERE, before the helper includes, so all of them can read it. Deliberately NOT maintenance_mode:
// that flag auto-releases after MAINTENANCE_IDLE_MS of *console* silence, which would expire mid-transfer.
// RAM-only, never RTC/NVS - any reset clears it, so the device can never boot into "updating".
volatile bool g_ota_active = false;

// TRUE only while a BLE log dump is transferring (see Helpers/log_dump.h). Pauses OBD polling for the same
// two reasons OTA does (readPIDs latency would crawl the transfer; row writes contend with the segment
// reads) and blocks the flash-mutating console commands. Declared here, before the helper includes, like
// g_ota_active. RAM-only - any reset clears it.
volatile bool g_logdump_active = false;

bool logNetwork(const std::vector<OBDVector>& obd_result_vector);

// Time sources, highest-to-lowest accuracy. Declared here (not down by the clock functions) so Arduino's
// auto-generated prototype for setClock(ClockSource, ...) sees the type.
enum ClockSource { CLK_GPS, CLK_HTTP, CLK_NITZ, CLK_NTP };

FreematicsESP32 sys;

GPS_DATA* gd = 0;
GPS_DATA last_gd{};

OBDWrapper obd;
using OBDVector = std::vector<std::pair<std::string, double>>;
std::vector<OBDVector> obd_result_vector{};
// Cap on the in-RAM upload buffer (frames awaiting upload). Prevents unbounded growth -> OOM during long
// offline periods; oldest frames are dropped (the full log is preserved in flash regardless).
const size_t MAX_UPLOAD_BUFFER = 120;
// How often the loop kicks a WiFi reconnect while offline. Kept comfortably longer than a normal
// association (a cold-boot connect can take ~10-15s) so a retry never aborts an attempt mid-flight.
const unsigned long WIFI_RETRY_MS = 20000;

int logging_errors{};
int logging_errors_limit{ 10 };
// Consecutive failed uploads in the current awake window. Once it hits MAX_UPLOAD_FAILS_PER_WAKE we stop
// attempting (radio goes quiet) until the next wake; reset on success. RAM = naturally resets each wake.
int upload_fail_count = 0;
// One-shot per wake: have we already logged whether the car's ECUs answered OBD this wake? A plain global
// (not RTC) so it resets to false on every deep-sleep reboot = one OBD-awake/asleep event per wake window.
bool logged_obd_state = false;

// Set by the ATRV gate when this wake is a stationary one-shot SAMPLE (rail-awake = charging/climate, or the
// optional parked heartbeat). Lets the loop poll once and sleep immediately instead of idling the full ACTIVE
// leash. A drive (motion) is NOT a sample poke - it streams via the normal state machine.
bool g_sample_poke = false;

/*
  This wake is a POWER-ON boot whose rail is in the weak-battery band, so the ATRV gate refused the poll -
  but we stayed awake anyway to serve BLE for a bounded window instead of deep-sleeping immediately.

  WHY a power-on is different. It is the one wake cause that means a HUMAN just did something physical: the
  dongle runs on permanent 12V in the OBD port, so it only loses power when somebody pulls it out and puts it
  back. That person may well need to talk to it - reset or receive a connection key (the key reset is only
  accepted inside this very window, see auth_gate.h), pull a log, start an update - and sleeping before
  bleInitForWake() denied all of it for the full parked cadence. Measured: rail 10.95 against an 11.00
  threshold, BLE never came up, 30 minutes of silence, on a bench supply sitting 50 mV under the line.

  What this does NOT relax: should_poll stays false, so no OBD request is made and the modem is never
  powered. That is where the battery actually goes; BLE advertising is small change beside it. The window is
  bounded so a weak battery is never held awake indefinitely - see WEAK_RAIL_COLD_BOOT_AWAKE_MS.
*/
bool g_weak_rail_cold_boot = false;
// Must outlive the key-reset window itself (AUTH_RESET_WINDOW_MS, timed from this same boot) plus the
// handshake that consumes it, so a reset asked for at the last second still has time to finish. Then the
// dongle sleeps on the parked cadence like any other skipped wake.
//
// The two are asserted equal-or-greater after the includes below, because the constant they must beat is
// declared in auth_gate.h - which is included later in this file, so it cannot simply be referenced here.
// Getting this backwards is silent and nasty: the radio would go away mid-window and the reset would look
// like a dongle that stopped answering.
static const uint32_t WEAK_RAIL_COLD_BOOT_AWAKE_MS = 330000;

// Cached OBD-port voltage (ATRV). Refreshed once per poll cycle + at the gate, and reused by buildLogLine()
// and logEvent() so we don't fire a fresh ATRV AT-exchange on every logged row/event while polling PIDs.
float g_last_voltage = 0.0f;

// Cached true-rail voltage from the IO36_BatDec ADC (see ENABLE_IO36_RAIL). Logged beside ATRV everywhere
// so the adapter value and the real 12V rail sit in the same row/event - the brownout disambiguator.
float g_last_io36 = 0.0f;

// True if this wake came through our own deep-sleep path (Test B). Stamped on every row so a true-rail (IO36)
// reading can be trusted only from healthy boots, and brownout boots can be filtered out.
bool g_clean_sleep = false;

// GNSS freeze (Feature 4). The accurate fix exists at the END of a drive (GNSS stabilized over minutes of
// being awake); a parked wake-up can't re-lock in the few seconds we're up, so its reading is garbage. So we
// FREEZE the last good driving fix on the is_parked 0->1 transition (RTC-persisted, survives deep sleep) and
// reuse it for every parked/charging frame instead of powering GNSS. g_gps_powered = did we power GNSS this
// wake (only on a drive/cold boot) - gates the loop's gpsGetData and the pre-sleep gpsEnd.
RTC_DATA_ATTR double frozen_lat = 0.0;
RTC_DATA_ATTR double frozen_lon = 0.0;
RTC_DATA_ATTR bool   frozen_valid = false;
bool g_gps_powered = false;

// Did we bring BLE up this wake? BLE comes up only on a person-present wake (the same !g_sample_poke guard as
// GPS), so this gates the pre-sleep teardown - we never deinit a stack we didn't init. Per-wake (a deep-sleep
// reboot clears it).
bool g_ble_up = false;
// BLE frame log state for this wake: the first frame is logged, then again only when more fields are
// dropped than any line so far or the MTU changes (a wake's first frame is often a small parked one).
int g_logged_ble_drop = -1;
uint16_t g_logged_ble_mtu = 0;
// Cross-task-safe BLE event logging: the wrapper's callbacks run in the Bluedroid task, so they can't write
// the LittleFS log directly (that would race the loop task). They enqueue here; loop() drains + writes.
QueueHandle_t bleLogQueue = nullptr;

// Test C instrumentation: rail captured at the gate (BEFORE a heartbeat poll), compared to a fresh reading a
// few seconds AFTER the poll - if the poll woke the car, rail climbs toward charging voltage; if it did
// nothing, rail stays flat.
// Also a control input: a silent sample poke at a rail >= RAIL_AWAKE_VOLTS holds the charge latch
// (CHARGE_SILENT_HOLD_POLLS) and may shorten the cadence (charge_followup, POST_DRIVE_*).
float g_rail_before_poke = 0.0f;
// Per-power-session tallies (evidence the modem is rarely powered): cheap skip wakes vs poll wakes.
RTC_DATA_ATTR uint32_t skips_count = 0;
RTC_DATA_ATTR uint32_t polls_count = 0;

// Adaptive wake cadence (Feature B): the interval (minutes) the NEXT deep sleep will arm, chosen per-sleep
// from what this wake observed (short while charging, long while parked). Default long.
int g_next_wake_minutes = WAKE_PARKED_MINUTES;
// Cumulative MINUTES already spent on a fast (charging) cadence, against the WAKE_CHARGING_MAX_MINUTES
// anti-runaway cap. Counted in minutes rather than wakes because there are now two fast tiers (1 min on
// DC, 3 on AC): a wake count would mean different amounts of wall clock depending on the tier, and a long
// DC session would exhaust a budget denominated in 3-minute wakes three times too early.
RTC_DATA_ATTR uint32_t charging_fast_minutes = 0;
// Anti-runaway latch: set once the cap is hit, forcing the LONG cadence even while the charge signals persist
// (a stuck rail+current can't keep fast-waking us). Cleared only when the car is genuinely not charging, so
// the cap actually holds slow instead of resetting and bouncing back to fast every interval.
RTC_DATA_ATTR bool charging_cap_latched = false;
// Charge corroboration (CHARGE_CORROBORATE_MINUTES): real seconds of fast cadence since soc/energy
// last rose above their high-water marks. High-water, not the progress ratchet, so a soc dithering by one
// step can't keep re-corroborating. Seconds from the RTC clock: polls come every few seconds while awake.
RTC_DATA_ATTR uint32_t charge_uncorroborated_s = 0;
RTC_DATA_ATTR uint64_t charge_corr_last_us = 0;
RTC_DATA_ATTR double charge_corr_soc_hw = -1.0;
RTC_DATA_ATTR double charge_corr_kwh_hw = -1.0;
RTC_DATA_ATTR double charge_corr_soe_hw = -1.0;
void resetChargeCorroboration() {
  charge_uncorroborated_s = 0;
  charge_corr_last_us = 0;
  charge_corr_soc_hw = -1.0;
  charge_corr_kwh_hw = -1.0;
  charge_corr_soe_hw = -1.0;
}

// Charge-taper bridge: recently_charging = the charge latch. Set when the last poll found EVIDENCE of a
// charge - a negative current sample or the car's own counters moving (CHARGE_PROGRESS_* in config.h) - and
// held for CHARGE_RELEASE_POLLS unconfirmed polls after the evidence stops. It was a straight copy of
// is_charging, self-clearing on the first poll that read current >= 0; the field case in config.h is one such
// sample arriving mid-charge, which dropped this latch AND the cadence with it. When set and the rail has
// dropped into the taper band (below RAIL_AWAKE but >= RAIL_TAPER_FLOOR), the gate spends a confirm-poll
// rather than skipping. taper_confirm_wakes bounds those confirm-polls since the last high-rail sample.
RTC_DATA_ATTR bool recently_charging = false;
// Counts CONSECUTIVE confirm-polls that made NO SoC progress (a real rising taper resets this to 0). So it
// bounds only UNPRODUCTIVE bridging - a stuck-negative sensor, or the 100% balancing tail - never a legit
// still-climbing taper. SoC, not current, is what separates "still charging" from "stuck / done".
RTC_DATA_ATTR uint32_t taper_confirm_wakes = 0;
// The charge-end read (CHARGE_END_POLL_FLOOR) has been spent. Re-armed only by a high-rail sample: a car
// whose rail sits under the taper floor while charging, or a current stuck negative, would otherwise re-latch
// on every final read and poll a low rail at charging cadence with no stall budget.
RTC_DATA_ATTR bool charge_end_polled = false;
RTC_DATA_ATTR double prev_charge_soc = 0.0;   // highest SoC seen while charging, for the stall test above
bool g_taper_confirm = false;                  // this wake is a taper confirm-poll (set in gate, read in loop)

// Charge-PROGRESS baselines (CHARGE_PROGRESS_* in config.h): the last SoC / energy we anchored on, so
// the next poll can ask whether the car's own counters moved. -1 = no baseline yet (cold boot / power loss),
// which anchors silently rather than reading the first sample as a rise.
//
// Deliberately NOT prev_charge_soc, which looks similar and is not interchangeable: that one is the taper
// bridge's stall budget - charging-gated, and zeroed the moment a poll reads current >= 0. Zeroing on exactly
// the sample we need to survive is what makes it useless here; these baselines outlive that sample.
RTC_DATA_ATTR double prev_parked_soc = -1.0;
RTC_DATA_ATTR double prev_parked_kwh = -1.0;
RTC_DATA_ATTR double prev_parked_soe = -1.0;
RTC_DATA_ATTR char last_good_220105_reply[513] = {};
RTC_DATA_ATTR uint8_t obd_anomaly_logs = 0;
RTC_DATA_ATTR uint64_t obd_anomaly_last_us = 0;
// skipped_prefix has its own budget so recoveries never use up the raw captures above.
RTC_DATA_ATTR uint8_t obd_skip_logs = 0;
RTC_DATA_ATTR uint64_t obd_skip_last_us = 0;
// Consecutive polls with no charge evidence while the charge latch was still set (CHARGE_RELEASE_POLLS).
RTC_DATA_ATTR uint8_t charge_release_polls = 0;
// Consecutive latched polls the car did not answer, held off the release up to CHARGE_SILENT_HOLD_POLLS.
RTC_DATA_ATTR uint8_t charge_silent_polls = 0;
// One WAKE_CHARGING_MINUTES follow-up after a charge: armed when the latch clears on a stationary wake,
// spent by the first silent high-rail sample poke, cleared by motion or a cold boot. 0/1 only.
RTC_DATA_ATTR uint8_t charge_followup = 0;
// Post-drive window (POST_DRIVE_WINDOW_MINUTES), opened at the awake-ladder end with the rail high and timed
// on the RTC microsecond clock from the last motion, never the wall clock: it may be untrusted.
RTC_DATA_ATTR uint8_t pdw_open = 0;
RTC_DATA_ATTR uint64_t pdw_open_us = 0;
RTC_DATA_ATTR uint8_t pdw_polls = 0;
// RTC microseconds of the last poll with charge evidence: a hold or a rail-high release keeps the charge's
// budget, cap and corroboration until this is CHARGE_STALE_RESET_MINUTES old.
RTC_DATA_ATTR uint64_t charge_last_evidence_us = 0;
bool g_pdw_ladder_done = false;          // pdwOpenWindow() decides once per wake
const char* g_cadence_reason = nullptr;  // names an ambiguous cadence for SLEEP enter; null = derive from minutes
static const uint64_t POST_DRIVE_WINDOW_US = (uint64_t)POST_DRIVE_WINDOW_MINUTES * 60000000ULL;
bool pdwActive(uint64_t now_us) {
  return pdw_open && now_us >= pdw_open_us && now_us - pdw_open_us < POST_DRIVE_WINDOW_US &&
    pdw_polls < (uint8_t)POST_DRIVE_MAX_POLLS;
}
// A pause inside one session keeps the charge's budget, cap and corroboration; only this old is it dropped.
bool chargeStateStale(uint64_t now_us) {
  return now_us < charge_last_evidence_us ||
    now_us - charge_last_evidence_us >= (uint64_t)CHARGE_STALE_RESET_MINUTES * 60000000ULL;
}
// Other images can lay RTC memory out differently, so only a deep-sleep wake trusts these.
void chargeWakeStateSanitize(bool deep_sleep) {
  if (deep_sleep) return;
  charge_followup = pdw_open = pdw_polls = 0;
  pdw_open_us = charge_last_evidence_us = 0;
}

// Test B (brownout vs clean sleep): set true immediately before esp_deep_sleep_start(); checked on the next
// boot. If we wake and it's NOT set, we did NOT go through our own sleep path -> an uncommanded reset
// (power loss / brownout / reflash), not a real timer/motion wake. RTC-persisted; cleared on power loss.
RTC_DATA_ATTR bool clean_sleep_marker = false;
// Running count of genuine BROWNOUT resets (esp_reset_reason()==ESP_RST_BROWNOUT) this power session - the
// unambiguous "the rail sagged and reset the chip" signal for Test B. A deliberate reflash shows as POWERON
// (clean_sleep=0) and does NOT bump this, so a non-zero count here is a real supply problem. Cross-check
// against rail_v at that boot.
RTC_DATA_ATTR uint32_t brownout_count = 0;

#if ENABLE_LOGGING
DataLogger datalogger;
#endif
// Diagnostics captured for the data log. g_wake_reason is set in setup() from the deep-sleep wake cause;
// g_last_http / g_last_status hold the most recent upload outcome (set in logNetwork).
const char* g_wake_reason = "POWERON";
int g_last_http = 0;
std::string g_last_status{ "none" };
unsigned long g_last_post_ms = 0; // wall time of the most recent POST (diagnostic: slow success = marginal link)

#if ENABLE_CELL
std::string cell_apn{ CELL_APN };
#endif

#if ENABLE_WIFI
std::string wifi_ssid{ WIFI_SSID };
std::string wifi_password{ WIFI_PASSWORD };
#endif
std::string tlm_push_token{  };
std::string api_key{ API_KEY };

/*
  Initialize networkWrapper, either Wifi or Cellular
*/

#if ENABLE_WIFI 
WiFiWrapper network{ sys, wifi_ssid, wifi_password };
#elif ENABLE_CELL
CellWrapper network{ sys, cell_apn };
#endif

/*
  State machine
*/

enum ActivityState {
  BLE,  //Whenever connected via BLE
  ACTIVE, //Whenever moving, and for 5 minutes after
  CHARGING, //For an hour after stopping movement, allows for frequent updates during fastcharging
  STANDBY, //Whenever no movement or connection for over 1 hour, infrequent updates with lowest powerdraw
  UNINITIALIZED //Not yet initialized
};

ActivityState current_state = UNINITIALIZED;

/*
  Helper modules. Each category of helper code lives in its own file under Helpers/ - open the
  category you're looking for (MEMS, clock, power/sleep, BLE, logging, network upload, GPS, state
  machine, misc). They are #included here, in dependency order, so everything still compiles as
  ONE sketch translation unit sharing the globals above - the code was moved verbatim out of this
  file; no logic changed.

  Forward declarations first: a few helpers call functions defined in a LATER helper file (e.g.
  enterDeepSleep() flushes uploads and writes log events). Inside a single .ino those calls were
  covered by Arduino's auto-generated prototypes; with the definitions in included files the
  declarations must be explicit.
*/
#if ENABLE_LOGGING
void logEvent(const char* fmt, ...);           // Helpers/logging.h
#else
#define logEvent(...) ((void)0)                // helpers log unguarded; with logging off there is nowhere to write
#endif
#if ENABLE_NETWORK
bool uploadBuffered(bool drain);               // Helpers/network_upload.h
#if ENABLE_LOGGING
void saveUploadBacklog();                      // Helpers/network_upload.h
#endif
#endif
void bleDeinitForSleep();                      // Helpers/ble.h (no-op stub when BLE is disabled)
void changeState(ActivityState new_state);     // Helpers/state_machine.h

#include "Helpers/motion_stats.h" // pure MOTION-line aggregation, used by mems.h
#include "Helpers/drive_state.h"  // park/drive region: the only writer of the latch globals above
#include "Helpers/mems.h"
#include "Helpers/server_roots.h" // data only: the trust bundle setup() hands the cellular transport
#include "Helpers/clock_sync.h"
#include "Helpers/telemetry_format.h"
#include "Helpers/geo_backlog.h"
#include "Helpers/sleep_power.h"
#include "Helpers/ble.h"
#include "Helpers/logging.h"
#include "Helpers/fw_build_log.h" // FW event: uses logEvent
// after logging.h (uses logEvent) and before network_upload.h (uses gzipBody)
#include "Helpers/gzip_body.h"
#include "Helpers/tlm_signature.h" // pure builder used by network_upload.h
#include "Helpers/network_upload.h"
#include "Helpers/gps_location.h"
#include "Helpers/state_machine.h"
#include "Helpers/misc.h"
#include "Helpers/alarm_gate.h"   // OBD-alarm gate (§8/§10): uses preferences, logEvent, gpsMovingFresh;
                                  // must precede ota_update.h (otaPublishInfo publishes g_alarm_flag)
#include "Helpers/ota_update.h"   // last: uses ble, logEvent, the rail sense and the globals above
#include "Helpers/log_dump.h"     // BLE log retrieval (ABD7): uses ble, datalogger, logEvent, g_ota_active
#include "Helpers/auth_gate.h"    // phase-2 auth gate (ABD8): uses ble, logEvent, json - the ONLY session grant
#include "Helpers/jws_verify.h"    // phase-B RS256 profile signature verification (self-contained, mbedTLS)
#include "Helpers/profile_sig.h"   // phase-B signed-profile transport/apply (ABD2 {"JWS":...}), uses ble + logEvent
#include "Helpers/dummy_test.h"

// The weak-rail cold boot keeps BLE up so a person who just plugged the dongle in can reach it - and the
// main thing they reach it FOR is the key reset, which the dongle only accepts inside its own window. If
// the radio went away first, the reset would present as a dongle that simply stopped answering. Asserted
// rather than commented, because the two constants live in different files and drift silently.
static_assert(WEAK_RAIL_COLD_BOOT_AWAKE_MS > AUTH_RESET_WINDOW_MS,
  "the BLE-only wake must outlive the key-reset window it exists to serve");

#if ENABLE_OTA
/*
  Take over the OTA trial-boot decision from the Arduino core. arduino-esp32's initArduino() marks a
  PENDING_VERIFY image valid automatically (esp32-hal-misc.c) - which would cancel rollback before any of our
  hardware is even initialised, making the trial boot meaningless. This weak-symbol override tells the core to
  leave it alone; otaHandlePendingVerify() then runs our own self-test in setup() and marks valid (or lets the
  bootloader revert). extern "C" because the weak symbol is declared in a C translation unit.
  Guarded on ENABLE_OTA: without our handler, returning true would leave a pending image unconfirmed forever.
*/
extern "C" bool verifyRollbackLater() { return true; }
#endif


/*
  Initialize ESP32
*/

void setup() {
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  resetFrozenGpsProvenance(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#if ENABLE_NETWORK
  resetGeoAccepted(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#endif
  chargeWakeStateSanitize(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#if PRINT
  // Only stall on a true power-on, so a serial monitor can attach in time to see boot output. Skip it on
  // every deep-sleep wake - periodic, motion, and especially the WiFi RF-flush recovery - so those reboots
  // (which happen with the monitor already attached) stay fast.
  // A post-OTA boot is an esp_restart(), which reports ESP_SLEEP_WAKEUP_UNDEFINED - i.e. it takes every
  // "cold boot" branch below. Skip the serial-attach stall on it: the monitor is already attached, and the
  // app is waiting to reconnect to the new firmware.
  if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED && !otaIsTrialBoot()) delay(2000);
#endif
  Serial.begin(115200);

  // Arm the wedge-guard task watchdog FIRST - before anything that can block (boot 153 wedged inside modem
  // init forever; no cooperative check can fire when the task itself is blocked). Fed from loop() and
  // between bounded NET-init attempts; panic=true -> a wedged wake ends in an automatic reset within
  // WEDGE_WDT_TIMEOUT_S instead of a dead dongle needing a human unplug. Deep sleep disarms it naturally.
  esp_task_wdt_init(WEDGE_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL); // subscribe this task (loopTask runs both setup() and loop())

  // resetChip(); // bench only: wipes all NVS (misc.h). Never ship it uncommented.
  // Reset the clock ONLY on a true cold boot. The ESP32 RTC keeps running through deep sleep and retains the
  // NITZ-set time; wiping it every wake made the ATRV gate (which runs BEFORE NITZ re-syncs) see epoch 0,
  // think the heartbeat was always due, and poll on every wake - keeping the car awake. Preserve the retained
  // time on deep-sleep wakes so the gate's heartbeat-interval math actually works.
  // ...but do NOT zero the clock on a post-OTA boot. esp_restart() keeps the RTC timer running, so the time
  // survives; wiping it would leave every frame without a utc (and therefore unuploadable) until the GNSS
  // re-locks - and the cellular NTP recovery is rate-limited and suppressed while a phone is connected,
  // which is exactly the state right after an update.
  if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED && !otaIsTrialBoot()) rtc.setTime(0);
  // Before anything reads the clock (the ATRV gate's heartbeat math, the first event stamps).
  clockDriftRestore();
  clockCompensateSleep(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#if FAKE_DATA
  rtc.setTime(1780663013); // ~2026-06-05 12:36 UTC; flash promptly so utc stays near real time (ABRP marks stale data "sleeping")
#endif
  // Build identity banner - the first thing printed every boot. After an OTA, seeing YOUR new FW_BUILD_TAG
  // here is positive proof that the new image is what's running (and, since this line is printed from the new
  // firmware itself, that it booted far enough to run code). Compare against the tag you set in config.h.
  Serial.printf("\n=== FW BUILD: %s  v%d  (compiled %s %s) ===\n",
    FW_BUILD_TAG, OTA_FW_VERSION, __DATE__, __TIME__);
  Serial.printf("Starting setup\n");
  printf("OTA Test String: %s\n", DUMMY_OTA_TEST_STRING);
#if ENABLE_WIFI
  // The post-flush wake is a TIMER wake with the recovery flag set. Consume the flag and treat this boot as
  // normal (NOT a periodic heartbeat) - the flush's only job was to power-cycle the radio.
  bool is_wifi_recovery_wake = (wake_cause == ESP_SLEEP_WAKEUP_TIMER && wifi_recovery_pending);
  if (is_wifi_recovery_wake) {
    wifi_recovery_pending = false;
    g_wake_reason = "WIFI_RECOVERY";
  }
#endif

#if ENABLE_SLEEP && ENABLE_MEMS
  bootCount++;
  switch (wake_cause) {
  case ESP_SLEEP_WAKEUP_EXT0:
    g_wake_reason = "MOTION";
    Serial.printf("Wake from deep sleep: WOM motion on GPIO%d (boot #%lu, sleeps %lu)\n",
      (int)WOM_INT_PIN, bootCount, sleepCount);
    break;
  case ESP_SLEEP_WAKEUP_TIMER:
#if ENABLE_WIFI
    if (is_wifi_recovery_wake) {
      Serial.printf("Wake from deep sleep: WiFi RF-flush recovery (boot #%lu)\n", bootCount);
      break; // normal operation, not a heartbeat
    }
#endif
    g_wake_reason = "TIMER";
    Serial.printf("Wake from deep sleep: periodic timer (boot #%lu, sleeps %lu)\n", bootCount, sleepCount);
    break;
  default:
    Serial.printf("Power-on / reset boot (#%lu)\n", bootCount);
    break;
  }
#endif

#if ENABLE_LOGGING
  // Mount the flash log filesystem and open/continue the active segment. Done early so even setup-phase
  // failures could be logged later; survives across the deep-sleep reboots above.
  datalogger.onNewSegment(logFwBuild); // every segment names the build, so rotation never loses it
  datalogger.begin(LOG_HEADER);
#endif

  // Capture the return value + devType. Both were discarded, which made a co-processor that failed to answer
  // INVISIBLE: begin() gives it only 3 ATI attempts, and on failure it nulls sys.link AND rebinds the modem
  // UART to different pins - so a bad link shows up downstream as "cellular dead" or an OBD panic with no
  // trace of the cause. Cheap, once per boot, and the number that makes any co-processor sleep experiment
  // (ATLP / NRST-hold) measurable instead of guesswork.
  bool coproc_ok = sys.begin();
  Serial.printf("sys.begin()=%d devType=%d link=%s\n",
    coproc_ok ? 1 : 0, (int)sys.devType, sys.link ? "up" : "NULL");
#if ENABLE_LOGGING
  if (!coproc_ok || !sys.link) {
    logEvent("COPROC begin=%d devType=%d link=NULL - OBD dead, modem UART may be misbound",
      coproc_ok ? 1 : 0, (int)sys.devType);
  }
#endif
  // RE-ARM the wedge guard. FreematicsESP32::begin() ends with its own esp_task_wdt_init(600, 0), and because
  // esp_task_wdt_init() UPDATES the timeout + panic config when the TWDT is already running, that silently
  // overrode the esp_task_wdt_init(WEDGE_WDT_TIMEOUT_S, true) armed above: the guard was actually running at
  // 600s with PANIC DISABLED, so a genuine wedge never self-reset - the exact brick this guard exists to
  // prevent (corroborated by 71h of field logs containing zero WDT/PANIC recoveries). Restoring it here, after
  // sys.begin(), is what makes WEDGE_WDT_TIMEOUT_S and panic=true real. Our subscription survives, so only the
  // config needs restating.
  esp_task_wdt_init(WEDGE_WDT_TIMEOUT_S, true);
  obd.begin(sys.link);
  g_last_voltage = obd.getVoltage(); // prime the cached ATRV so the WAKE event (and gate) log a real voltage
#if ENABLE_IO36_RAIL
  g_last_io36 = readIO36Rail(); // prime the cached true-rail voltage too (logged beside ATRV everywhere)
#endif

  // Test B: did this boot come through our own deep-sleep path? clean_sleep_marker was set true right before
  // we slept and survives deep sleep in RTC RAM; a power loss / brownout clears it. So !clean_sleep => an
  // UNCOMMANDED reset (brownout / power blip / reflash), regardless of what esp_reset_reason claims.
  g_clean_sleep = clean_sleep_marker;
  bool clean_sleep = g_clean_sleep;
  clean_sleep_marker = false;            // consume it; the next clean sleep will re-set it
  if (esp_reset_reason() == ESP_RST_BROWNOUT) brownout_count++; // count only genuine supply brownouts (not reflashes)

  // Wedge-guard accounting: a TASK_WDT/WDT reset means the previous wake BLOCKED (boot-153 failure mode) and
  // the watchdog recovered it - count CONSECUTIVE occurrences (RTC RAM survives the reset). PANIC counts
  // too: the task-WDT's panic handler can surface as ESP_RST_PANIC on this IDF, and a genuine crash-loop is
  // the same overnight 12V hazard as a wedge-loop either way. Any clean deep-sleep wake means the previous
  // wake ended normally, so the streak clears.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    bool wdt_reset = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT ||
                      rr == ESP_RST_PANIC);
    if (wdt_reset) wdt_reset_count++;
    else if (rr == ESP_RST_DEEPSLEEP) wdt_reset_count = 0;
    g_wdt_wake = wdt_reset;
  }

#if ENABLE_LOGGING
  // First line of every wake window: why we woke, how deep we are, and - for the brownout test - whether it
  // was a clean (commanded) sleep wake, the decoded reset cause, the uncommanded count, and BOTH voltages
  // (ATRV vs true rail). Events fire on every wake including enforce-mode skips, so this captures wakes that
  // never write a telemetry row.
  logEvent("WAKE reason=%s reset=%s clean_sleep=%d brownouts=%lu atrv=%.2f rail=%.2f boot=%lu sleeps=%lu",
    g_wake_reason, resetReasonStr(), clean_sleep ? 1 : 0, (unsigned long)brownout_count,
    g_last_voltage, g_last_io36, (unsigned long)bootCount, (unsigned long)sleepCount);
  // A deep-sleep wake cannot change the image: a new one is confirmed (otaHandlePendingVerify) before
  // anything can sleep. A boot that also opened a segment logs it twice; harmless, and it keeps the rule
  // "no FW line after a non-deep-sleep WAKE = older firmware" (DONGLE_CONTEXT.md §5).
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) logFwBuild();
  if (g_clock_slept_s)
    logEvent("CLOCK wake slept %lus, adjusted %+ldms at %ld ppm%s", (unsigned long)g_clock_slept_s,
      (long)(g_clock_comp_us / 1000), (long)clock_drift_ppm, clock_drift_learned ? "" : " (unlearned)");
  // Make every watchdog recovery visible in the dump (acceptance: a wedge shows a WDT event, never a silent
  // multi-minute gap). streak = consecutive wedged wakes; at >=2 the escalation below sleeps instead of retrying.
  if (g_wdt_wake) logEvent("WDT/PANIC reset recovered a wedged/crashed wake (streak=%lu)", (unsigned long)wdt_reset_count);
#endif
  // NOTE: a low-battery lockout was tried here using obd.getVoltage() (ATRV), but the logs show ATRV reads
  // ~5.3V when the car/CAN is asleep and ~13.5V only when awake - it does NOT report the true 12V battery
  // while parked, so any cutoff would false-trigger and brick the device into permanent lockout. A real
  // lockout needs the dedicated 12V sense (IO36_BatDec ADC, see POWER_MANAGEMENT.md), not ATRV.

#if ENABLE_WIFI
  loadPreferences();
#endif

#if FILE_TEST
  sys.gpsBegin();
  pollGpsData();
#endif

  // NOTE: BLE is NO LONGER brought up here. It used to init before the ATRV gate, so it came up on every wake
  // (including cheap skip-wakes) and was never torn down for sleep. It's now brought up AFTER the gate, ONLY on
  // a person-present wake (the same !g_sample_poke guard GPS uses) - see the bleInitForWake() call below GPS.

  // NOTE: GNSS is NO LONGER powered here. It's powered after the gate, ONLY on a wake that needs a live fix
  // (a drive, or a cold boot) - see the ENABLE_GPS block below. A stationary/charging wake reuses the frozen
  // last-driving fix (Feature 4), so we don't waste power on a GNSS that can't re-lock in time anyway.

#if ENABLE_MEMS
#if ENABLE_SLEEP
  // Clear any WOM config persisted across deep sleep before re-initialising the sensor.
  Serial.printf("Soft-resetting ICM-42627 (clears persisted WOM config)...\n");
  softResetMEMS();
  // GPIO39 has no internal pulls; ICM INT is push-pull active-HIGH (LOW at rest).
  pinMode((int)WOM_INT_PIN, INPUT);
#endif
// Initialize and calibrate motion sensors
  do {
    mems = new ICM_42627;
    byte ret = mems->begin();
    if (ret) {
      Serial.println("ICM-42627");
      break;
    }
    delete mems;
    mems = 0;
    Serial.println("MEMS failed");
  } while (0);
  calibrateMEMS();
#endif

  // Resolve a post-OTA trial boot HERE - after the log/FS, OBD and MEMS init that the self-test inspects, and
  // deliberately BEFORE the ATRV gate below. The gate can deep-sleep this very boot (a weak rail satisfies its
  // skip condition), and with rollback armed ANY reset before mark-valid makes the bootloader discard the new
  // firmware - so a good update would be silently reverted just because the battery was low. This also detects
  // a completed rollback so the app can be told the previous attempt was reverted.
  otaHandlePendingVerify();

  // OBD-alarm gate: load the per-vehicle flag from NVS BEFORE the wake gate below consults it, and
  // arm the per-PID hard stop (§8.5 - loss of the permit must interrupt a frame mid-read, not wait
  // for the next cycle; the switch-off-to-lock window is seconds). Both no-ops when the flag is off.
  alarmGateLoad();
#if OBD_ALARM_GATE
  obd.setPollGuard(alarmPollGuard);
#endif

#if ENABLE_ATRV_GATE
  // Sleep gate. Read the OBD pin-16 voltage (ATRV) - a LOCAL read, NO CAN traffic - to learn whether the car
  // is awake WITHOUT poking the bus (poking it is what wakes a sleeping car). Run HERE, BEFORE the modem and
  // OBD profile are brought up: if the car is asleep and no heartbeat is due, we go straight back to deep
  // sleep having powered NOTHING this wake (no modem, no poll) - the cheapest possible skip. A long-asleep
  // car still gets ONE deliberate reading per HEARTBEAT_POLL_MINUTES. The clock comes from the RTC, which
  // keeps running across deep sleep (last set from NITZ), so the skip events still carry a valid utc.
  // OBSERVE mode (ATRV_GATE_ENFORCE 0) logs the verdict but changes nothing.
  {
    g_last_voltage = obd.getVoltage();
    float v = g_last_voltage;
    float rail = g_last_io36;                           // TRUE rail (IO36) - the trustworthy battery signal
    bool awake = v >= ATRV_AWAKE_VOLTS;                 // ATRV verdict: a COLLAPSE detector only (see below)
    bool is_motion = (wake_cause == ESP_SLEEP_WAKEUP_EXT0);
    bool is_cold   = (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED);
    time_t now = rtc.getEpoch();
    bool clock_ok = now > 1000000000UL;
    // since_poll is only meaningful once we have a clock AND a baseline stamp; -1 = "not established yet"
    // (this is what printed the raw epoch before - last_poll_epoch was stuck at 0 all run).
    long since_poll = (clock_ok && last_poll_epoch != 0) ? (long)(now - last_poll_epoch) : -1;
    // Backed-off heartbeat interval: base doubled once per consecutive asleep poke, capped. A car parked for
    // days is poked rarely instead of every HEARTBEAT_POLL_MINUTES forever (see heartbeat_asleep_streak).
    // Optional parked time-heartbeat (default OFF - see ENABLE_PARKED_HEARTBEAT). Proven to return nothing
    // useful on this car, so rail-rise + motion are the real triggers; this is kept only for A/B.
#if ENABLE_PARKED_HEARTBEAT
    long hb_interval = (long)HEARTBEAT_POLL_MINUTES * 60;
    for (uint32_t i = 0; i < heartbeat_asleep_streak && hb_interval < (long)HEARTBEAT_POLL_MAX_MINUTES * 60; i++)
      hb_interval *= 2;
    if (hb_interval > (long)HEARTBEAT_POLL_MAX_MINUTES * 60) hb_interval = (long)HEARTBEAT_POLL_MAX_MINUTES * 60;
    bool heartbeat_due = since_poll >= 0 && since_poll >= hb_interval;
#else
    long hb_interval = 0;
    bool heartbeat_due = false;
#endif

    // Low-rail lockout: rail in [LOW, HIGH) = weak-but-alive battery -> suppress ALL polling so we don't push
    // a recoverable 12V over the edge. rail < LOW = bench/no-car (allowed to run, so USB DUMP works); rail >=
    // HIGH = healthy. Uses the TRUE rail because ATRV is unreliable (diverges up to 3.6V).
    bool weak_rail = (rail >= IO36_LOCKOUT_LOW && rail < IO36_LOCKOUT_HIGH);

    // RAIL-AWAKE: the true rail is high -> the car's DC-DC is on (charging / remote climate) -> it's genuinely
    // awake and WILL answer OBD with real data. This is the main parked-period data source. We sample ONCE
    // then sleep (g_sample_poke below); the next wake re-checks the rail, so a charge is followed at the wake
    // cadence WITHOUT holding the car awake - we sleep between samples, so the rail can truthfully fall when
    // the charge ends (a free exit, no rail-drop masking, no continuous-poll state machine needed).
    bool rail_awake = rail >= RAIL_AWAKE_VOLTS;
    if (rail_awake) {                    // a real high-rail sample resets the taper-bridge budget
      taper_confirm_wakes = 0;
      charge_end_polled = false;
    }

    // TAPER BRIDGE: the last poll saw charging but the rail has since dropped into the taper band (below
    // RAIL_AWAKE yet >= RAIL_TAPER_FLOOR). Rail alone can't tell a taper from a parked car here, so spend a
    // confirm-poll to read the current sign. Bounded by taper_confirm_wakes. If the rail is below the floor
    // (weak/collapsed) or the budget is spent, give up: it's not a recoverable taper -> clear the latch.
    bool taper_bridge = false;
    bool charge_end_poll = false;
    if (recently_charging && !rail_awake && !weak_rail) {
      if (rail >= RAIL_TAPER_FLOOR && taper_confirm_wakes < (uint32_t)TAPER_CONFIRM_MAX_WAKES) {
        taper_bridge = true;             // confirm-poll; the loop updates the budget from SoC progress
      } else {
        // Rail fell out of the taper band: read the car once more so the charge end and final SoC reach ABRP
        // (CHARGE_END_POLL_FLOOR). A spent stall budget gets no extra read - those polls already reported.
        charge_end_poll = !charge_end_polled && rail < RAIL_TAPER_FLOOR && rail >= CHARGE_END_POLL_FLOOR;
        if (charge_end_poll) charge_end_polled = true;
        recently_charging = false;       // give up: rail too low, or too many SoC-stalled confirm-polls
        taper_confirm_wakes = 0;
        if (chargeStateStale(esp_rtc_get_time_us())) resetChargeCorroboration();
        if (!is_motion) {
          charge_followup = 1;
#if ENABLE_LOGGING
          logEvent("FOLLOWUP armed (latch cleared at rail=%.2f)", rail);
#endif
        }
      }
    }
    g_taper_confirm = taper_bridge;      // tell the loop this poll is a taper confirm (for the SoC-stall test)
    // Somebody is at the car: the charge the follow-up was for is not the one that matters now.
    if (is_motion && charge_followup) {
      charge_followup = 0;
#if ENABLE_LOGGING
      logEvent("FOLLOWUP cleared reason=motion");
#endif
    }

    // Decide whether to poll. A plain TIMER wake on a PARKED car (rail ~12.6) does NOT poll - polling a parked
    // Scenic returns nothing useful and only costs a voltage sag. We poll only for a real reason, never on a
    // weak rail:  cold boot | motion + awake (a drive) | rail high (charging/climate) | taper bridge (confirm
    // a tapering charge whose rail dropped) | charge end | optional parked heartbeat. ATRV's 8V threshold is just a collapse detector.
    bool should_poll = !weak_rail &&
      (is_cold || (is_motion && awake) || rail_awake || taper_bridge || charge_end_poll || heartbeat_due);
#if OBD_ALARM_GATE
    // ALARM GATE (§8.2): on a flagged vehicle a STATIONARY poke - rail-awake charge sample, taper
    // confirm, heartbeat - is exactly the parked-and-armed scenario, and polling it trips the OBD
    // alarm every cadence tick. Suppress the poke and sleep on the normal skip path (modem never
    // powered). Motion and cold-boot wakes still proceed: they only keep the device AWAKE - the
    // actual bus requests are gated per-cycle and per-PID by the §8.3 permit in loop().
    if (alarmEnforced() && should_poll && !is_motion && !is_cold) {
      should_poll = false;
#if ENABLE_LOGGING
      logEvent("ALARMGATE stationary poke suppressed (flag=%d rail=%.2f) - parked+armed scenario",
        g_alarm_flag, rail);
#endif
    }
#endif
    // A "sample poke" = a stationary one-shot read (rail-awake / taper bridge / charge end / heartbeat): poll
    // once, log, sleep, re-check next wake. NOT a drive (motion streams) and NOT a cold boot.
    g_sample_poke = should_poll && (rail_awake || taper_bridge || charge_end_poll || heartbeat_due) &&
      !is_motion && !is_cold;
    if (g_sample_poke) g_rail_before_poke = rail; // rail BEFORE the poke, vs rail_after in the fast-path
    if (should_poll) polls_count++; else skips_count++;
    const char* verdict = awake ? "AWAKE" : "ASLEEP";   // ATRV-vs-8V (collapse/bench detector)
    const char* action = !should_poll ? (weak_rail ? "skip(weak-rail)" : "skip")
                       : !g_sample_poke ? "poll"
                       : rail_awake ? "poll(rail-awake)"
                       : taper_bridge ? "poll(taper-confirm)"
                       : charge_end_poll ? "poll(charge-end)" : "poll(heartbeat)";
#if ENABLE_LOGGING
    if (taper_bridge)
      logEvent("TAPER_CONFIRM attempt=%lu/%d rail=%.2f (recently_charging) -> confirm-poll",
        (unsigned long)taper_confirm_wakes, TAPER_CONFIRM_MAX_WAKES, rail);
    if (charge_end_poll)
      logEvent("CHARGE_END rail=%.2f < taper floor %.2f -> final poll, latch cleared", rail, RAIL_TAPER_FLOOR);
#endif
    Serial.printf("ATRVGATE: atrv=%.2f rail=%.2f -> %s, %s %s\n",
      v, rail, verdict, action, ATRV_GATE_ENFORCE ? "[ENFORCE]" : "[observe]");
#if ENABLE_LOGGING
    // rail_awake and weak_rail follow from rail; logEvent keeps 119 characters.
    logEvent("ATRVGATE atrv=%.2f rail=%.2f %s %s since_poll=%lds skips=%lu polls=%lu rc=%d %s",
      v, rail, verdict, action, since_poll, (unsigned long)skips_count, (unsigned long)polls_count,
      recently_charging ? 1 : 0, ATRV_GATE_ENFORCE ? "ENFORCE" : "observe");
#endif
    // NOTE: last_poll_epoch is stamped AFTER NITZ (below), not here - on a cold boot the clock isn't valid
    // yet at this point, which is exactly why the heartbeat never fired before.

#if ATRV_GATE_ENFORCE && ENABLE_SLEEP && ENABLE_MEMS
    // Don't poll -> sleep without powering the modem. A COLD BOOT never takes this path, weak rail or not.
    //
    // It used to: the reasoning was that a bench supply reads below LOW, so a cold boot in the weak band had
    // to be a real weak car. That assumption is false in practice - a bench supply or a sagging feed sits
    // squarely in the band (measured 10.95 against the 11.00 threshold), and a genuinely weak car still has
    // an owner standing at it who just plugged the dongle in. Either way the device went silent on BLE for
    // the whole parked cadence, which is where the log-dump, the update and the connection-key reset all
    // live - the reset especially, since it is only accepted within seconds of this very boot.
    //
    // The poll is still refused (should_poll is false and stays false), so no OBD request is made and the
    // modem is never powered on this path. Only the radio comes up, for a bounded window.
    if (!should_poll && weak_rail && wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
      g_weak_rail_cold_boot = true;
#if ENABLE_LOGGING
      logEvent("ATRVGATE weak-rail COLD BOOT (rail=%.2f) - no poll, no modem, BLE up for %lus",
        rail, (unsigned long)(WEAK_RAIL_COLD_BOOT_AWAKE_MS / 1000UL));
#endif
      Serial.printf("ATRVGATE: weak rail on a power-on -> staying awake for BLE (no poll, no modem)\n");
    }
    if (!should_poll && !g_weak_rail_cold_boot &&
        (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED || weak_rail)) {
      Serial.printf("ATRVGATE: not polling -> deep sleep (%s)\n", weak_rail ? "weak rail lockout" : "asleep");
#if ENABLE_LOGGING
      logEvent("ATRVGATE skip -> deep sleep (%s, modem never powered)", weak_rail ? "WEAK RAIL" : "asleep");
#endif
      g_next_wake_minutes = WAKE_PARKED_MINUTES; // skip = parked: long cadence
      g_cadence_reason = nullptr;
      // A charge that ended via a skip wake resets the fast-cadence budget and the cap latch - once stale: a
      // 12 V top-up every few hours must not hand a stuck current a fresh budget each time.
      if (chargeStateStale(esp_rtc_get_time_us())) {
        charging_fast_minutes = 0;
        charging_cap_latched = false;
        resetChargeCorroboration();
      }
      enterDeepSleep(false); // modem_on=false: never powered it this wake - never returns
    }
#endif
  }
#endif

  // Bring BLE up HERE - the instant the gate decides to poll, BEFORE the ~12s cellular modem init - so the
  // dongle is advertising within ~2s of wake and is connectable well inside the app's 5s background-reconnect
  // window. (Behind cellular it took ~14s, so every post-sleep reconnect blew that window and churned -> the
  // recurring 0x13.) Same !g_sample_poke guard as GPS: a charge-sample poke (nobody at the car) brings up
  // neither. No-op if !ENABLE_BLE.
#if ENABLE_SLEEP && ENABLE_MEMS
  // Wedge-guard escalation: two CONSECUTIVE wakes just ended in a WDT reset (both wedged in the init path
  // below). Don't run it a third time back-to-back - sleep on the parked cadence and try fresh next wake,
  // so a persistently-wedging modem costs ~2 WDT windows per cycle, not a 5-minute reset loop all night.
  // Placed AFTER the gate/MEMS init (enterDeepSleep needs the sensor) and BEFORE BLE/modem bring-up.
  // modem_on=true: after a mid-init WDT reset the modem is almost certainly still powered from the wedged
  // wake (the wedge IS modem init) - CPOWD it on the way down rather than leave it drawing through the sleep.
  if (g_wdt_wake && wdt_reset_count >= 2) {
#if ENABLE_LOGGING
    logEvent("WDT streak=%lu -> skip init, sleep %dmin (retry fresh next wake)",
      (unsigned long)wdt_reset_count, WAKE_PARKED_MINUTES);
#endif
    g_next_wake_minutes = WAKE_PARKED_MINUTES;
    enterDeepSleep(true, "wdt-streak"); // never returns
  }
#endif

  if (!g_sample_poke) bleInitForWake();

#if ENABLE_OTA && ENABLE_BLE
  // Wire the OTA characteristics to the engine. Must follow bleInitForWake(): init() creates ABD5/ABD6 (and
  // deinit() nulls them), so the hooks are registered per wake alongside the rest of the BLE session. Until
  // this runs the characteristics exist but forward nowhere, so no update can start.
  if (g_ble_up) {
    ble.set_ota_hooks(otaCtrlHook, otaDataHook);
    otaPublishInfo();   // publish installed version + rollback state for the app to read
  }
#endif

#if ENABLE_BLE && ENABLE_LOGGING
  // Wire ABD7 to the log-dump engine (independent of ENABLE_OTA - log retrieval is its own feature).
  // Same per-wake registration rationale as the OTA hooks above.
  if (g_ble_up) {
    ble.set_logdump_hook(logDumpCtrlHook);
    ble.set_auth_hook(authGateHook); // ABD8 -> the phase-2 auth gate (Helpers/auth_gate.h)
    ble.set_profile_sig_hook(profileSigAppend); // ABD2 {"JWS":...} -> phase-B signed profile
  }
#endif

#if ENABLE_NETWORK
  // ONE bounded network bring-up attempt (~5-10s healthy, ~35s worst case when registration times out).
  // History: init(retry=true) had NO deadline and once looped 2.4 HOURS (boot 147); then a 90s retry window
  // still burned ~93s of drive start before the first poll at every bad-coverage departure (the measured
  // capture-loss windows). Now: try once, then CONTINUE THE WAKE OFFLINE - polling starts ~60s earlier,
  // frames buffer (backlog persists them across sleep), and the loop's cheap CGREG reconnect probe brings
  // the link up mid-drive the moment the modem registers (it keeps attaching in the background on its own).
  bool net_ok = false;
  // The modem is the expensive half of a wake, and the whole point of the weak-rail path is not to spend it.
  // BLE is already up by here; this wake exists only so a person can reach the dongle over the radio, and
  // nothing they can do in that window needs the cellular link. Frames are not being produced either (no
  // poll), so there is nothing to upload.
  if (g_weak_rail_cold_boot) {
#if ENABLE_LOGGING
    logEvent("NET skipped (weak-rail cold boot - BLE-only wake)");
#endif
  } else {
    unsigned long t0 = millis();
    esp_task_wdt_reset(); // one attempt is bounded (~35s max, inside the 300s wedge guard)
    net_ok = network.init(false);
#if ENABLE_LOGGING
    if (net_ok) logEvent("NET init=up t=%lus", (millis() - t0) / 1000UL);
    else {
      logEvent("NET init FAILED t=%lus -> continuing offline (loop re-attaches when registered)",
        (millis() - t0) / 1000UL);
#if ENABLE_CELL
      auto diag = network.initFailureDiag();
      logEvent("NET init diag %s", diag.first.c_str());
      if (!diag.second.empty()) logEvent("NET init radio %s", diag.second.c_str());
#endif
    }
#endif
  }
#if ENABLE_WIFI
  network.set_url(WIFI_BULK_PATH); // batch upload: all buffered frames in one POST (see logNetwork)

  // One-shot RF-flush recovery. A dirty power-up (USB unplug/replug, 12V blip) can leave the WiFi analog
  // front-end in a glitched state that NO software reset clears - not disconnect(), not esp_wifi stop/start
  // (that re-runs PHY calibration and still failed in testing), not esp_restart(). Only a real RF power-down
  // fixes it, which is exactly why a deep-sleep wake always reconnects. So if we COLD-BOOTED (power-on, not
  // a sleep wake) and WiFi didn't come up, do a brief deep sleep to power-cycle the radio, then wake and
  // retry. Guarded to fire at most once per power-up (RTC flag + cold-boot-only) so it can never boot-loop:
  // if WiFi is simply absent, the post-flush TIMER wake won't satisfy the UNDEFINED-cause check.
  // NOT on a weak-rail cold boot: we never attempted WiFi there, so !net_ok is our own choice rather than
  // evidence of a glitched front-end - and this recovery deep-sleeps immediately, which would cancel the
  // very BLE window that path exists to provide. (Dormant today at ENABLE_WIFI 0; guarded so turning WiFi
  // back on does not quietly reintroduce the silence.)
  if (!net_ok && !g_weak_rail_cold_boot && wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED &&
      !wifi_recovery_pending) {
    Serial.printf("WiFi failed on cold boot - deep-sleep RF flush (one-shot recovery)\n");
    wifi_recovery_pending = true;
    esp_sleep_enable_timer_wakeup(200000ULL); // 0.2 s; the RF powers down the instant we sleep, so duration is moot
    Serial.flush();
    esp_deep_sleep_start(); // never returns - reboots into a fresh setup() with the radio re-initialised
  }

  // Time comes from the GNSS fix (UTC), not NTP. Seed the clock from the fix acquired during GPS setup
  // above; addGpsData() then keeps it synced every loop. ABRP rejects frames without a valid utc, so
  // until the GNSS locks (e.g. indoors) utc is simply omitted and those frames are skipped.
  if (setClockFromGps()) {
    Serial.printf("Clock set from GNSS: %s\n", rtc.getTime("%Y-%m-%d %H:%M:%S").c_str());
  } else {
    Serial.printf("No GNSS fix yet; utc will be set once the GNSS locks\n");
  }
#elif ENABLE_CELL
  network.set_url(CELL_HOST);
  network.set_server_roots(SERVER_ROOTS_P7B, SERVER_ROOTS_LEN); // see SERVER ROOTS in config.h
  network.set_session_idle_max_ms(SESSION_IDLE_MAX_MS);

  // Acquire UTC for the cellular path. Skipped entirely on a deep-sleep wake (the ESP32 RTC keeps running
  // and stays valid), so this only actually executes on a cold/POWERON boot.
  //   1) One free AT+CCLK read: local query, ~50ms, no data. parseCclk() rejects the unset 1980/2080
  //      default. Whether this works is per-SIM, not rare-vs-common: the roaming Hologram SIM's visited
  //      networks deliver no NITZ, but the Telia SIM does (CLKPROBE: AT+CLTS: 1, AT+CCLK within 2 s of a
  //      just-synced RTC). So it is worth trying FIRST every time, and on a NITZ SIM step 2 never runs.
  //   2) NTP over the data bearer, for the SIMs where step 1 comes back empty (field-confirmed on the
  //      Hologram SIM: the old 10x1s NITZ wait always ran to exhaustion = pure boot delay). syncNtp() is
  //      ~1s now that xbReceive gets expected-match strings (see CellWrapper::syncNtp). Not infallible
  //      either - it resolves pool.ntp.org over the bearer, and CLKPROBE saw it fail "+CNTP: 62" (DNS).
  // GPS still overrides with authoritative time the moment it locks (setClock priority).
  //
  // BUG FIX (kept): this block (incl. set_api_page below) was #if !ENABLE_BLE - with BLE enabled api_page
  // stayed empty and every upload POSTed to the wrong URL (HTTP 404). It must always run for cellular.
  // UNSET *or* STALE. This used to be `!clockValid()` alone, which is only "epoch > 2001" and stays
  // true across every deep sleep - so the clock was set once on the first boot with coverage and then
  // free-ran on the RC oscillator at ~30 s/hour, forever, unless a GPS fix happened to correct it.
  // See CLOCK_RESYNC_SECONDS and the note on last_clock_sync_epoch for the measurements.
  if (clockNeedsSync()) {
    const time_t before = clockValid() ? rtc.getEpoch() : 0;
    // Captured BEFORE the sync: setClock() overwrites last_clock_sync_epoch, so reading it afterwards
    // would measure the interval against the value that just replaced it.
    const time_t prev_sync = last_clock_sync_epoch;
    time_t t = parseCclk(network.readClock().c_str());
    if (t) {
      setClock(CLK_NITZ, t);
      Serial.printf("Clock set from NITZ: %s UTC\n", rtc.getTime("%Y-%m-%d %H:%M:%S").c_str());
    } else if (network.connected()) {
      Serial.printf("No NITZ (expected on roaming SIM) - syncing clock via NTP...\n");
      if (network.syncNtp()) {
        t = parseCclk(network.readClock().c_str());
        if (t) {
          setClock(CLK_NTP, t);
          Serial.printf("Clock set from NTP: %s UTC\n", rtc.getTime("%Y-%m-%d %H:%M:%S").c_str());
        }
      }
    }
#if ENABLE_LOGGING
    // Log the CORRECTION, not just "set": the skew is invisible on the device. It is the residual left
    // after the wake's drift compensation; the raw drift and the learned rate are in the CLOCK drift line.
    if (t && before && prev_sync) {
      logEvent("NET clock resynced %+lds after %lds (residual)", (long)(t - before), (long)(before - prev_sync));
    } else if (t) {
      logEvent("NET clock: set");
    } else {
      logEvent("NET clock resync FAILED (no NITZ/NTP) - %s, wake compensated %+ldms",
        clockValid() ? "keeping RTC" : "UNSET (retry in loop / await GPS)", (long)(g_clock_comp_us / 1000));
    }
#endif
  }
  network.set_api_page(TLM_V2_PATH); // uploads use the v2 tlm endpoint (logNetwork -> sendBulk)
#endif
#endif

#if ENABLE_ATRV_GATE
  // Heartbeat baseline (THE fix for the heartbeat never firing). We only reach here on a wake that's going to
  // poll (a skip slept back in the gate), and the clock is now valid (NITZ ran above). Stamp last_poll_epoch
  // so the heartbeat interval measures from this poll. Previously this was attempted IN the gate gated on
  // clock_ok, but the gate runs BEFORE NITZ, so on a cold boot clock_ok was false, the stamp never happened,
  // last_poll_epoch stayed 0, and heartbeat_due could never become true.
  if (clockValid()) last_poll_epoch = rtc.getEpoch();
#endif

  // obd.begin(sys.link) already done earlier (before the low-battery lockout check).
  /*
    This used to be a commented-out list of per-car init calls to uncomment for bench testing without
    BLE. Those profiles are gone (see the profile block further down): they were compiled-in PID lists
    for a dozen specific cars, and the one that was live shipped a developer's telemetry token with it.
    To test without the app, push a profile over ABD2 the way the wizard does, or set FAKE_DATA 1 to
    exercise the upload path with synthetic frames and no bus at all.
  */

#if ENABLE_CAN_SNIFF
  // PASSIVE diagnostic sniff: listen-only, BEFORE any active poll, so the verdict reflects whether the car
  // was already awake (bus alive) or asleep (bus silent) on its own. Logged + printed; does NOT yet gate
  // polling (observe mode) - the proof we need before adding the decision logic. See ENABLE_CAN_SNIFF.
  {
    char sample[64] = { 0 };
    int frames = sniffCanBus(CAN_SNIFF_WINDOW_MS, sample, sizeof(sample));
    bool alive = frames > 0;
#if CAN_SNIFF_JOIN_PROBE
    const char* mode = "[JOIN-PROBE/diag]";
#else
    const char* mode = "[passive]";
#endif
    Serial.printf("CANSNIFF %s: %d frame(s) in %dms -> %s%s%s\n",
      mode, frames, CAN_SNIFF_WINDOW_MS, alive ? "ALIVE (broadcast seen)" : "SILENT (no broadcast)",
      alive ? " sample=" : "", alive ? sample : "");
#if ENABLE_LOGGING
    logEvent("CANSNIFF %s frames=%d window=%dms %s%s%s", mode, frames, CAN_SNIFF_WINDOW_MS,
      alive ? "ALIVE" : "SILENT", alive ? " sample=" : "", alive ? sample : "");
#endif
  }
#endif

#if ENABLE_GPS
  // Power GNSS ONLY on a wake that needs a live position: a drive or a cold boot. A stationary sample
  // (charging/heartbeat = g_sample_poke) reuses the frozen last-driving fix (Feature 4), so we don't power
  // the GNSS at all - it couldn't re-lock in the few seconds we're awake, and the frozen fix is the truth.
  if (!g_sample_poke) {
    // The FLAG MUST FOLLOW THE RESULT. gpsBegin() gives the GNSS ~1.4s to emit a sentence and, on
    // failure, powers it back DOWN and clears FLAG_GNSS_USE_LINK - so setting g_gps_powered = true
    // regardless was a lie with two consequences. gpsGetData() then took the dead direct-UART branch
    // and returned false every cycle, and nothing retried: one quiet failure at wake meant no fix
    // for the entire wake, gps_moving_streak pinned at 0, and on an alarm-flagged car the driving
    // permit structurally unreachable - a whole blind drive with no telemetry by any transport
    // (Appendix J root cause 3). It also made the pre-sleep gpsEnd() conditional on a flag that did
    // not describe reality.
    g_gps_powered = sys.gpsBegin();
    Serial.printf(g_gps_powered ? "GPS success\n" : "GPS Setup failed - will retry from the loop\n");
    if (g_gps_powered) {
      pollGpsData();
    }
#if ENABLE_LOGGING
    else logEvent("GPS init FAILED at wake - retrying once per poll cycle");
#endif
  }
#endif

  // NOTE: BLE bring-up MOVED earlier - right after the ATRV gate, before the ~12s cellular init - so the dongle
  // advertises within ~2s of wake instead of ~14s. A 14s delay blew the app's 5s background-reconnect window and
  // churned every post-sleep reconnect (the recurring 0x13). See the bleInitForWake() call just before NET init.

  /*
    The vehicle profile comes from ONE place: the persisted BLE push (/vehicle_profile.json), which
    carries both the PID list and the vehicle's telemetry token and survives sleep and reboot, so it
    drives standalone cellular wakes too.

    There is no factory fallback any more, on either branch. It used to call obd.initScenic() - a PID
    list for one specific car compiled into every image - and set a TLM_PUSH_TOKEN from config.h. On a
    Scenic that was merely redundant; on anything else it polled the wrong PIDs and uploaded whatever
    came back under a developer's vehicle token. And an unclaimed dongle poking an unknown bus is the
    scenario the alarm gate exists for (§10.3), which is why that path was already disabled under
    OBD_ALARM_ENFORCE - this just makes it unconditional and deletes the profiles it depended on.

    So before the wizard pushes a profile the dongle has no vehicle identity: g_no_profile stops the
    poll (see the guard in loop()), and with no token there is nothing to upload and no one to upload
    it as. It still advertises and answers BLE, which is exactly what the wizard needs to reach it.
  */
  bool custom_profile = false;
#if ENABLE_BLE && ENABLE_LOGGING
  custom_profile = loadPersistedProfile(); // applies profile + token from flash; false if none/unreadable
#endif
  if (!custom_profile) {
    g_no_profile = true;
    Serial.printf("No persisted vehicle profile - polling disabled until a profile is pushed\n");
#if ENABLE_LOGGING
    logEvent("PROFILE none persisted - polling and upload wait for the BLE push");
#endif
  }
  config_initialized = true;

#if (ENABLE_NETWORK) && ENABLE_LOGGING // parenthesised: ENABLE_NETWORK is a bare `||`
  // Restore telemetry that couldn't be uploaded before the last sleep (wedged bearer / dead spot). Only
  // reachable on a polling wake - gate skips slept long before this - and the modem is freshly booted here,
  // so the normal upload path drains these with their original (correct) utc stamps.
  loadUploadBacklog();
#endif
}

/*
  Main loop of the program
*/

void loop() {
  esp_task_wdt_reset(); // feed the wedge guard - the ONLY steady-state feed, so any block anywhere in the
                        // loop body (modem, OBD, BLE, FS) trips a reset within WEDGE_WDT_TIMEOUT_S
  handleActivityState();
#if ENABLE_LOGGING
  handleLogCommands(); // serve DUMP / STAT / CLEAR over serial at any time
  // Drain BLE handshake events enqueued by the Bluedroid task (see bleLogCb) - written to flash HERE, on the
  // loop task, so LittleFS is never touched from two tasks at once. This is what makes the BLE handshake
  // (CONNECT/AUTH/HASH/CLEAN/SETUP/DISCONNECT+reason) visible in the DUMP for in-car debugging.
  if (bleLogQueue) {
    char tmp[100];
    while (xQueueReceive(bleLogQueue, tmp, 0) == pdTRUE) logEvent("BLE %s", tmp);
  }
#endif

  // Service an in-flight firmware update: stage completed sectors to flash, send ACKs, enforce the
  // inactivity/session/power guards. Placed HERE deliberately - ahead of the UNINITIALIZED and FILE_TEST
  // early returns below and ahead of the poll block - so a transfer is serviced on every iteration. A cheap
  // flag test when idle. All flash work happens on this (loop) task, never in the BLE callbacks.
  otaService();

  // Service an in-flight BLE log dump the same way: one command per iteration, guards enforced. A cheap
  // flag test when idle (see Helpers/log_dump.h).
  logDumpService();

  // Service the ABD8 auth gate (provision/challenge/proof) - NVS + HMAC on the loop task, never the
  // Bluedroid task. A cheap flag test when idle (see Helpers/auth_gate.h).
  authGateService();

  // Verify + apply a fully received signed profile - RSA and the config apply belong on this task,
  // never the Bluedroid one. A cheap flag test when idle (see Helpers/profile_sig.h).
  profileSigService();

  // Persist a pending OBD-alarm flag write (ABD2 "obd_alarm") - NVS on the loop task, never the
  // Bluedroid one - and republish the ABD5 info JSON. A cheap flag test when idle (alarm_gate.h).
  alarmGateService();

#if ENABLE_BLE
  // A phone utc push set the clock without clockNoteRealSync(): the sleep counted so far no longer explains
  // the clock's error, so restart the drift window rather than learn ~0 ppm from it.
  static uint32_t seen_time_pushes = 0;
  if (ble.timePushes() != seen_time_pushes) {
    seen_time_pushes = ble.timePushes();
    clock_slept_since_sync = 0;
  }

  // Apply a BLE-pushed profile HERE, on the loop task, between poll cycles - never on the Bluedroid task
  // (see configCallback: applying there raced the in-flight readPIDs -> garbage/PANIC). Skip the OBD re-init
  // when the pushed profile matches what's already running (utc stripped first - it changes every push;
  // nlohmann dump() is key-sorted = canonical), so a routine re-push never ATZ-resets the link mid-session.
  // Deferred while a firmware update is transferring: applying a profile runs obd.initialize() (a full ATZ +
  // ~10 init AT commands, seconds of blocking) plus a LittleFS profile write and possibly an NVS commit - all
  // of which would stall the sector ACKs and contend with esp_ota_write for the same flash chip. The flag is
  // sticky, so nothing is lost: the push applies on the first iteration after the update finishes (or, if
  // the update ends in its reboot, otaFinalize drops it so the app pushes it again).
  // Deferred during a log dump for the same two reasons (window replies stall; the profile write mutates the
  // filesystem the dump is mid-way through reading).
  if (g_config_pending && !g_ota_active && !g_logdump_active) {
    g_config_pending = false;
    static std::string last_applied_profile;
    try {
      json newConfig = json::parse(g_pending_config_str);
      json config = newConfig["configJson"];
      config.erase("utc"); // clock-set side channel, not a profile field; also breaks the identity compare
      tlm_push_token = newConfig.at("tlm_push_token").get<std::string>();
      std::string profile_dump = config.dump();
      // A completed full push (either branch below) = a human actively drove the wizard at the car:
      // grant the bounded setup-session permit so the wizard's live PID test works while stationary
      // (§8.3 - the driving term can never pass in a driveway). The HASH fast path never gets here,
      // so a background pocket-reconnect grants nothing. Also clears the no-profile wait state.
      alarmGrantSetupPermit();
      g_no_profile = false;
      // NOTE: g_obd_needs_init is NOT cleared here. The identical-profile branch below skips
      // initialize(), so a flagged car whose boot init was deferred still needs it - the poll
      // site's lazy init runs it under the setup permit just granted. Only an actual initialize()
      // clears the flag.
      if (profile_dump != last_applied_profile) {
        obd.setVehicleCommands(config);
        obd.initialize();
        g_obd_needs_init = false;
        last_applied_profile = profile_dump;
#if ENABLE_LOGGING
        logEvent("CONFIG applied from BLE push (%d bytes, on loop task)", (int)profile_dump.size());
#endif
      } else {
#if ENABLE_LOGGING
        logEvent("CONFIG push identical to running profile - re-init skipped");
#endif
      }
#if ENABLE_LOGGING
      // Persist the accepted push (profile + token) so it survives sleep/reboot. This is what keeps the
      // NVS hash truthful: boot restores THIS file (loadPersistedProfile) instead of reverting to factory,
      // and standalone cellular wakes then run the pushed profile too. Every full push is written, an
      // identical profile included: the app's hash covers the token.
      File pf = LittleFS.open(PROFILE_PATH, "w");
      const bool saved = pf && pf.print(g_pending_config_str.c_str()) == g_pending_config_str.size();
      if (pf) pf.close();
      if (saved) {
        logEvent("PROFILE persisted to flash (%d bytes)", (int)g_pending_config_str.size());
      } else {
        dropUnsavedPush("write");
      }
#endif
      config_initialized = true;
    }
    catch (const json::exception& err) {
      Serial.printf("CONFIG apply failed: %s\n", err.what());
      // The wrapper committed the new hash at SETUP OK, but the profile never applied (the RAM token may
      // already have moved): drop the hash and the file so no wake runs either half - the app re-pushes.
#if ENABLE_LOGGING
      logEvent("CONFIG apply FAILED: %.60s", err.what());
#endif
      dropUnsavedPush("apply");
    }
  }
#endif

  if (current_state == UNINITIALIZED) {
    return; // no logging performed before initialization over BLE
  }

#if ENABLE_NETWORK
  // Keep the link alive INDEPENDENTLY of whether we have telemetry to upload. Reconnection used to live
  // inside the upload block, which is gated on a non-empty buffer - so with no OBD/GPS data (bench, or a
  // dead spot) the buffer stayed empty and WiFi was never retried after the boot attempt failed. Retry on
  // a fixed cadence whenever offline so the device always works its way back online on its own.
  static unsigned long last_reconnect_try = 0;
  // Do NOT churn the modem while a phone is RELAYING over BLE (bleRelaying = connected AND authenticated).
  // BLE is the transport then (the phone relays to the cloud) and cellular deliberately stands down, so
  // reconnecting it is pointless - and reconnect() hits the modem over the shared UART / can block the
  // loop, which starves the BLE stack and drops the phone (the "BLE pauses and reconnects" symptom).
  // Deliberately NOT bleConnected(): with the GATT table gated, an unpaired/declined phone stays connected
  // but relays nothing - suppressing on mere presence let such a phone silently starve cellular of a modem
  // (field-found). See bleRelaying() in Helpers/ble.h. When relaying ends, cellular resumes on the cadence.
  // !g_ota_active is on all three cellular gates below. The relaying suppression evaporates
  // the instant the phone drops - which is exactly the OTA resume window - and a modem reconnect/POST there
  // would block the loop for seconds (starving sector ACKs) or, via the http=0 recovery ladder, deep-sleep
  // the device mid-transfer.
  if (!bleRelaying() && !g_ota_active && !network.connected() && (millis() - last_reconnect_try) > WIFI_RETRY_MS) {
    Serial.printf("Network offline - retrying\n");
    network.reconnect();
    last_reconnect_try = millis();
#if ENABLE_CELL && ENABLE_LOGGING
    // Offline search heartbeat (once/min, serial + flash log): csq and CEREG state while hunting
    // for the network, so "did it ever camp, and after how long?" is answerable from a dump. The
    // silent search is why cold-attach behavior looked random - a cold LTE-M roaming scan can take
    // many minutes, and without this line there is no way to tell "still scanning" from "stuck".
    static unsigned long last_search_log = 0;
    static char last_reg = '?';
    char reg = network.registrationState();
    if (reg != '?' && reg != last_reg) {
      logEvent("NET reg %c->%c", last_reg, reg);
      last_reg = reg;
      last_search_log = millis();
    }
    if (!network.connected() && millis() - last_search_log > 60000) {
      last_search_log = millis();
      logEvent("NET searching %s t=%lus", network.netDiag().c_str(), millis() / 1000UL);
    }
#endif
  }
#endif

  int num{};
  bool logged_data{ false };
#if FILE_TEST
  file_test();
  delay(1000);
  return;
#endif
  // Polling is suspended for the whole update. Two hard reasons, not just tidiness: (1) readPIDs() blocks up
  // to ~5s on a parked car (5 PIDs x the 1s OBD timeout, no answer), and since sector ACKs are emitted from
  // this task that latency would throttle the transfer to a crawl - it is the single biggest throughput trap;
  // (2) it writes a telemetry row to LittleFS per cycle, contending with esp_ota_write on the same flash.
  // Telemetry resumes automatically on the first cycle after the update ends.
  // (A BLE log dump pauses polling for the same two reasons - g_logdump_active below.)
  if (!g_ota_active && !g_logdump_active && ( millis() - last_poll ) > polling_delay) { //Only measure PIDS at specified interval, depending on state_machine
#if PRINT
    Serial.printf("Starting measure loop\n");
#endif
#if ENABLE_NETWORK
    // Acquire a clock BEFORE polling if we don't have one yet (e.g. garage/no-GPS, and the RTC is reset on
    // each wake). Otherwise the clock only gets set by the end-of-session upload's Date header - too late,
    // so the readings we already buffered go up without utc and the API can't timestamp them.
    // clockNeedsSync, not just !clockValid: on a wake that stays up for hours - a drive, or a parked
    // BLE session - setup()'s sync never runs again, so without staleness here the RC oscillator's
    // ~30 s/hour would accumulate for the whole session anywhere GPS cannot correct it.
    if (clockNeedsSync() && network.connected()) {
      // WiFi: one small GET for the server's Date header (fetchDate is a no-op "" on cellular).
      time_t t = parseHttpDate(network.fetchDate().c_str());
      if (t) setClock(CLK_HTTP, t);
#if ENABLE_CELL
      // Cellular clock RECOVERY: if the boot sync missed (marginal attach), retry here - without this,
      // one failed boot sync left the device at 1970 forever indoors (no GPS) and EVERY frame was
      // skipped for lack of utc (seen on the bench). Also the ONLY correction during a long awake
      // stretch (driving, or a BLE relay session), since setup() does not run again until the next sleep.
      //
      // Same two sources in the same order as setup(): the modem's own NITZ-set clock first, because
      // AT+CCLK is a local query (~50 ms, no data, cannot fail on DNS), then NTP. The order matters more
      // than it looks - CLKPROBE on the Telia SIM got a good AT+CCLK while AT+CNTP failed outright with
      // "+CNTP: 62" (DNS). This path used to be NTP-ONLY, so on that SIM a whole driving session had no
      // working correction at all even though the right answer was one free AT command away.
      // Both sources sit behind ONE rate limit. AT+CCLK is cheap but not free, and on a SIM with
      // neither NITZ nor working DNS this block never succeeds - unlimited, it would pay ~50 ms of AT
      // per loop iteration (plus syncNtp's ~1 s) for the whole session. 30 s between attempts costs
      // nothing against the CLOCK_RESYNC_SECONDS budget. Never while a phone is RELAYING so it can't
      // stall the live stream - bleRelaying, not bleConnected: frames without utc are skipped for
      // upload, so an unpaired camping phone that also blocked clock recovery would starve cellular of
      // usable frames on top of blocking the POSTs.
      static unsigned long last_clock_retry = 0;
      if (!t && !bleRelaying() && !g_ota_active && (millis() - last_clock_retry) > 30000UL) {
        last_clock_retry = millis();
        t = parseCclk(network.readClock().c_str());
        if (t) {
          setClock(CLK_NITZ, t);
#if ENABLE_LOGGING
          logEvent("NET clock: NITZ (loop recovery)");
#endif
        } else if (network.syncNtp()) {   // no NITZ on this SIM - fall back to NTP over the bearer
          t = parseCclk(network.readClock().c_str());
          if (t) {
            setClock(CLK_NTP, t);
#if ENABLE_LOGGING
            logEvent("NET clock: NTP (loop recovery)");
#endif
          }
        }
      }
#endif
    }
#endif
    // ---- Refresh the OBD-alarm gate's INPUTS before anything consults them ----
    // Both permit terms (true rail + GNSS movement) used to be refreshed further down, AFTER the
    // gate had already decided, so every verdict was made on the previous cycle's data. Two
    // consequences, both fixed by doing it here: a car switched off since the last cycle could be
    // granted one more full frame of bus traffic, and at the start of every wake the movement
    // streak was still stale-zero, denying the first few cycles of a drive on top of the GNSS
    // acquisition wait. Neither read touches the OBD bus - they are local ADC/GNSS reads, which is
    // precisely why they are safe to take before the permit exists.
#if ENABLE_IO36_RAIL
    g_last_io36 = readIO36Rail();
#endif
#if ENABLE_GPS
    if (g_gps_powered) {
      pollGpsData();
      parkAccumSample();   // advance the movement streak now; idempotent, so addGpsData() below is a no-op for this fix
    }
#endif

#if FAKE_DATA
    // Synthetic telemetry to validate the WiFi -> Iternio upload path (see FAKE_DATA in config.h).
    OBDVector current_frame{
      { "soc", 55.5 },
      { "soh", 98.0 },
      { "voltage", 380.4 },
      { "current", -32.1 },
      { "power", -12.2 },
      { "is_charging", 1 },
      { "speed", 0 },
      { "lat", 59.3293 },  // fixed coords so the API's "Missing telemetry: lat, lon" note clears
      { "lon", 18.0686 },
    };
    bool obd_read_attempted = true;
#else
#if OBD_ALARM_GATE
    // §8.3/§8.4: the ONE poll decision. No profile -> nothing to ask and nowhere to send it (wait
    // for the wizard's push). Gate denied -> the frame stays empty; GPS/utc are still appended
    // below, ATRV/rail refreshes still run (both local reads, no bus traffic - they are the gate's
    // own inputs). Deferred boot init (flagged car) runs here on the first permitted cycle.
#if ENABLE_GPS
    // State-driven recovery of a failed GNSS init, not a blind retry loop: one attempt per poll
    // cycle until it takes. Without this a single quiet gpsBegin() failure at wake cost the whole
    // wake - and on an alarm-flagged car the driving permit needs a fresh fix to exist at all, so
    // it cost the whole drive (Appendix J root cause 3). Skipped on a sample poke, which never
    // powers GNSS by design, and stops the moment it succeeds.
    if (!g_gps_powered && !g_sample_poke) {
      if (sys.gpsBegin()) {
        g_gps_powered = true;
        pollGpsData();
        Serial.printf("GPS recovered on a later attempt\n");
#if ENABLE_LOGGING
        logEvent("GPS init recovered after a failed wake attempt");
#endif
      }
    }
#endif

    OBDVector current_frame{};
    bool obd_read_attempted = false;
    if (!g_no_profile && alarmGateAllowsPoll()) {
      if (g_obd_needs_init) {
        obd.initialize();
        g_obd_needs_init = false;
#if ENABLE_LOGGING
        logEvent("ALARMGATE deferred OBD init ran (first permitted cycle)");
#endif
      }
      current_frame = obd.readPIDs(PRINT);
      obd_read_attempted = true;
    }
#else
    auto current_frame = obd.readPIDs(PRINT); // pass PRINT so raw SENDING/RECEIVING CAN buffers are shown while debugging
    bool obd_read_attempted = true;
#endif
#endif
    int obd_count = (int)current_frame.size(); // PIDs returned this reading (0 = car asleep / no answer)
#if ENABLE_MEMS && ENABLE_LOGGING
    if (obd_count > 0) motion_stats.obd = true;
#endif
    g_last_voltage = obd.getVoltage(); // refresh the cached ATRV once per cycle (reused by row + events below)
    // NOTE: g_last_io36 is NOT refreshed here any more - it is taken at the top of the cycle, before
    // the alarm gate reads it. Re-reading here would only make the telemetry row's rail marginally
    // fresher while re-opening the stale-input bug the hoist fixed if the two ever got reordered.

    // Heartbeat back-off bookkeeping: any successful read means the car is responsive -> reset. A heartbeat
    // poke that still finds nothing -> lengthen the interval (see heartbeat_asleep_streak / the gate).
    if (obd_count > 0) heartbeat_asleep_streak = 0;
    else if (g_sample_poke && heartbeat_asleep_streak < 16) heartbeat_asleep_streak++;
#if ENABLE_LOGGING
    if (obd_read_attempted) {
      const uint64_t now_us = esp_rtc_get_time_us();
      if (!obd.anomalous_220105_reason.empty() && obd_anomaly_logs < 3 &&
          (!obd_anomaly_last_us || now_us < obd_anomaly_last_us ||
           now_us - obd_anomaly_last_us >= 15ULL * 60ULL * 1000000ULL)) {
        const size_t prev_len = strnlen(last_good_220105_reply, sizeof(last_good_220105_reply));
        logEvent("OBD220105 reason=%s raw=%u prev=%u", obd.anomalous_220105_reason.c_str(),
          (unsigned)obd.anomalous_220105_reply.size(), (unsigned)prev_len);
        auto logRaw = [&](const char* kind, const char* data, size_t size) {
          for (size_t off = 0; off < size; off += 40) {
            size_t n = size - off;
            if (n > 40) n = 40;
            char hex[81];
            for (size_t i = 0; i < n; ++i)
              snprintf(hex + 2 * i, 3, "%02X", (unsigned char)data[off + i]);
            logEvent("OBDRAW %s %u/%u %s", kind, (unsigned)(off / 40 + 1),
              (unsigned)((size + 39) / 40), hex);
          }
        };
        logRaw("prev", last_good_220105_reply, prev_len);
        logRaw("bad", obd.anomalous_220105_reply.data(), obd.anomalous_220105_reply.size());
        obd_anomaly_last_us = now_us;
        obd_anomaly_logs++;
      }
      // A reply recovered by dropping lines ahead of it: same cap and spacing, own budget, no bytes logged.
      if (!obd.skipped_prefix_pid.empty() && obd_skip_logs < 3 &&
          (!obd_skip_last_us || now_us < obd_skip_last_us ||
           now_us - obd_skip_last_us >= 15ULL * 60ULL * 1000000ULL)) {
        logEvent("OBD%s reason=skipped_prefix lines=%u", obd.skipped_prefix_pid.c_str(),
          obd.skipped_prefix_lines);
        obd_skip_last_us = now_us;
        obd_skip_logs++;
      }
      if (!obd.good_220105_reply.empty()) {
        size_t n = obd.good_220105_reply.size();
        if (n >= sizeof(last_good_220105_reply)) n = sizeof(last_good_220105_reply) - 1;
        memcpy(last_good_220105_reply, obd.good_220105_reply.data(), n);
        last_good_220105_reply[n] = '\0';
      }
    }
    // The key question for the 12V drain: is our polling keeping the car's ECUs awake? obd_count > 0 means
    // the CAN bus answered = the car is awake (and our Mode 22 requests are part of why). Log it ONCE per
    // wake (first poll) so it's a clear marker without flooding the log every 5 s; the per-row obd_pids
    // column still carries the full picture.
    // Guarded on obd_read_attempted: a gate-denied or no-profile cycle never asked the car, so
    // logging "no response (car asleep)" here would be false - and it is exactly the misleading
    // line that sends a debugging session into the cellular stack instead of at the gate. The
    // adjacent ALARMGATE DENY / PROFILE-wait events carry the real reason.
    if (!logged_obd_state && obd_read_attempted) {
      if (obd_count > 0) {
        logEvent("OBD car RESPONDING (%d pids) - polling is keeping ECUs awake", obd_count);
        // Chunked: logEvent truncates at 120 chars. Profile keys are sanitised so none can forge a log line.
        std::string keys;
        for (const auto& r : current_frame) {
          std::string k = r.first;
          for (auto& c : k) if (!isalnum((unsigned char)c) && c != '_') c = '?';
          if (!keys.empty() && keys.size() + k.size() > 100) { logEvent("OBD keys=%s", keys.c_str()); keys.clear(); }
          if (!keys.empty()) keys += ",";
          keys += k;
        }
        logEvent("OBD keys=%s", keys.c_str());
      }
      else               logEvent("OBD no response (car asleep / CAN quiet)");
      logged_obd_state = true;
    }
    // Field drops are Serial-only inside readPIDs; log the first poll per wake that had any.
    static bool logged_obd_drops = false;
    if (!logged_obd_drops && (obd.drop_eval || obd.drop_range || obd.drop_trunc)) {
      logEvent("OBD drops: eval=%lu range=%lu trunc=%lu", (unsigned long)obd.drop_eval,
        (unsigned long)obd.drop_range, (unsigned long)obd.drop_trunc);
      logged_obd_drops = true;
    }
#endif
#if PRINT
    Serial.printf("current frame: \n");
    for (auto res : current_frame) {
      Serial.printf("pid: %s, value: %.2f\n", res.first.c_str(), res.second);
    }
#endif

    scrubGeoMetadata(current_frame);
#if ENABLE_GPS
    // gd was refreshed at the top of this cycle (the alarm gate needs the movement verdict before it
    // decides), so only the frame-building call remains here.
    addGpsData(current_frame);              // parked -> frozen fix; driving -> live gd (handles null gd)
#if PRINT
    if (gd) { Serial.printf("Lat: %.6f, Lon: %.6f, Time: ", gd->lat, gd->lng); Serial.println(rtc.getTimeDate()); }
#endif
#endif
    if (rtc.getEpoch() > 1e9) { // only send utc if up to date, September 9 2001
      current_frame.emplace_back("utc", rtc.getEpoch());
    }
#if PRINT
    Serial.printf("UTC: %lu, lat: %f, lon: %f\n", (unsigned long)rtc.getEpoch(),
      gd ? gd->lat : 0.0f, gd ? gd->lng : 0.0f);
#endif

    // is_parked: any reported speed > 0 means we are driving, so unlatch. Brief stops (red lights)
    // keep speed-only frames at 0 but stay latched to driving; the state machine re-latches to
    // parked after motion stops for ACTIVE_TIMER. Check both standard and proprietary speed keys.
    for (const auto& res : current_frame) {
      if (( res.first == "speed" || res.first == "vehicle_reported_speed" ) && res.second > 0.0) {
        // Driving. Unlatch parked AND refresh the sleep leash from SPEED, not just the accelerometer. THE
        // smooth-road fix: on glassy asphalt detectMovement() sees no accel spikes, so last_active would go
        // stale and the state machine would sleep mid-drive (and changeState() would then mislabel the frame
        // is_parked:1). Feeding speed into the leash holds us awake as long as the car reports motion.
        driveSpeed(res.second);
        // Feed the OBD-alarm gate's speed-hold: a genuine road speed is independent proof the car is
        // in transit, and it carries an already-granted driving permit through a GNSS outage (tunnel,
        // multi-storey). Continuation only - reaching this line at all means a poll was permitted.
        alarmNoteSpeed(res.second);
        break;
      }
    }
#if ENABLE_GPS
    // SECOND, independent movement source: the GNSS. The loop above needs the car's speed PID, which goes
    // quiet the moment the ECUs sleep and is missing from some vehicle profiles entirely - and until something
    // unlatches is_parked, the position logic keeps publishing the frozen PARKING SPOT while the car is
    // actually driving (the "coordinates update late / no GPS data" symptom). The GNSS knows we're moving
    // without asking the car anything, so let it unlatch too and refresh the same leash.
    // Debounced (2 consecutive samples) so GNSS speed jitter at standstill can't spuriously unlatch. The
    // COORDINATE fix in addGpsData is deliberately NOT debounced - it reacts on the first moving sample.
    //
    // AGE-BOUNDED (gpsMovingFresh, not gpsMovingConfirmed). The confirmed verdict is latched and its
    // streak only resets on a fresh STATIONARY fix, so driving into a garage froze it at "moving"
    // for the rest of the wake. Because this branch also refreshes g_last_speed_ms, the time-based
    // re-latch below could never expire either - the two together pinned is_parked at 0, and
    // is_charging is gated on that latch, so a car charging at -52 A reported NOT_CHARGING until it
    // next slept. Requiring a recent fix makes the unlatch expire on its own when fixes stop, and
    // then the 5-minute re-latch does what it was written to do.
    if (gpsMovingFresh(GPS_MOVING_MAX_AGE_MS)) {
      driveGnss(gd->speed * 1.852f);
    }
#endif
    // TIME-BASED parked re-latch (state-machine independent). The changeState re-latch can't fire while a
    // connected phone holds the leash (we never leave the BLE state), which left the latch on "driving"
    // forever after a drive - disabling the MAX_AWAKE backstop and is_charging. Same semantics as the state
    // machine (no reported speed for ACTIVE_TIMER -> parked), just not blocked by the BLE hold. With the
    // latch restored, a forgotten connected phone is genuinely bounded by MAX_AWAKE_MINUTES, and plugging in
    // at a charger with the phone connected still detects is_charging.
    // The 0->1 transition freezes the stabilized last-driving fix, as changeState does.
    drivePollEnd();
    // Before is_parked is appended: a frame whose only v2 field would be drivingState is not worth a POST.
    // utc is always present, so this asks the serializer. A location or heading already accepted, or already
    // buffered behind an unsent one, is not news; any other, frozen or live, is.
#if ENABLE_NETWORK
    const double poll_utc = uploadSendUtc();
    seedGeoQueued(poll_utc);
    GeoMarks poll_geo = obd_result_vector.empty() ? g_geo_accepted : g_geo_queued;
    bool frame_has_data = v2HasTelemetry(current_frame, poll_utc, &poll_geo);
#else
    bool frame_has_data = !current_frame.empty();
#endif
    current_frame.emplace_back("is_parked", is_currently_parked ? 1.0 : 0.0);

    // Feature A - is_charging from the current PID's sign. Charging pushes current INTO the battery (negative,
    // ~-51A); discharge/idle is >= 0. Gate on parked so a drive's regen braking isn't mislabeled. Per-frame.
    // A streaming wake can still be a drive the latch has not caught yet, so there the sample is trusted only
    // after CHARGE_STREAM_CONFIRM_SECONDS without a discharge sample (config.h); until then the frame carries no
    // verdict, which the v2 mapping sends as no chargingState rather than NOT_CHARGING.
    bool is_charging = false;
    bool charging_current_valid = false;
    for (const auto& res : current_frame) {
      if (res.first == "current") {
        // Bounds-guard the cadence-critical read: a garbage current (e.g. the -2.9e8 seen in bad buffer
        // reads) is < CHARGING_CURRENT_THRESHOLD and would falsely flag charging -> trip fast cadence.
        // readPIDs already clamps to the profile's [-2000,2000], but don't depend on it here.
        float c = res.second;
        if (c > -2000.0f && c < 2000.0f) {
          charging_current_valid = true;
          if (c > CHARGE_DISCHARGE_AMPS) g_last_discharge_ms = millis();
          is_charging = is_currently_parked && (c < CHARGING_CURRENT_THRESHOLD);
        }
        break;
      }
    }
    const bool charge_signal_trusted = g_sample_poke ||
      millis() - g_last_discharge_ms >= (unsigned long)CHARGE_STREAM_CONFIRM_SECONDS * 1000UL;
    const bool unconfirmed_charging = is_charging && !charge_signal_trusted;
    if (!charge_signal_trusted) is_charging = false;
    if (!is_currently_parked || (charging_current_valid && charge_signal_trusted))
      current_frame.emplace_back("is_charging", is_charging ? 1.0 : 0.0);

    /*
      CHARGE PROGRESS - the second, independent charge signal (CHARGE_PROGRESS_* in config.h).

      is_charging above is ONE instantaneous current sample, and it is the only one this wake: readPIDs
      does a single read per PID with no retry or averaging, and a sample poke sleeps the moment the frame
      is built. The field case that forced this: a car charging at a measured 3.7kW returned +0.479A on
      that one sample, which relaxed the cadence 3min -> 30min and dropped the taper latch mid-charge.

      SoC and energy progress offer evidence independent of the instantaneous current sample. Estimated
      remaining energy (soe, Wh) needs a wider noise floor than a cumulative kwh_charged counter because
      the BMS can re-estimate it. This decides CADENCE only - published is_charging stays the current-sign verdict,
      because that is what the frame has always meant and changing it changes what the API is told.

      RATCHETED, not last-poll-vs-this-poll. The baseline moves up only when a value actually crosses the
      epsilon, so a coarsely-quantised soc (0.5% steps on some profiles) accumulates across polls instead of
      reading as zero change at every interval. A FALL re-anchors - that is the discharge / per-session-reset
      case, and it is also what stops a high baseline left over from an earlier charge from ever reading as
      progress later.
    */
    auto chargeCounterRose = [&](const char* key, double& baseline, double eps) {
      for (const auto& res : current_frame) {
        if (res.first != key) continue;
        const double v = res.second;
        if (!is_currently_parked || baseline < 0.0) baseline = v; // driving / first sample: anchor only
        else if (v > baseline + eps) { baseline = v; return true; }
        else if (v < baseline)       baseline = v;   // fell: re-anchor (discharge / counter reset)
        break;
      }
      return false;
    };
    // Anchor on EVERY poll, parked or not. A drive's regen raises kwh_charged, so leaving the baseline at a
    // pre-drive value would make the first parked poll after arriving read the regen as a charge.
    const bool soc_rose = chargeCounterRose("soc", prev_parked_soc, CHARGE_PROGRESS_SOC_PCT);
    const bool kwh_rose = chargeCounterRose("kwh_charged", prev_parked_kwh, CHARGE_PROGRESS_KWH);
    const bool soe_rose = chargeCounterRose("soe", prev_parked_soe, CHARGE_PROGRESS_SOE_WH);
    // Same parked gate as is_charging: SoC / energy changes during a drive can include regen, and neither
    // says anything about a charge session.
    const bool charge_progress = is_currently_parked && charge_signal_trusted && (soc_rose || kwh_rose || soe_rose);

    /*
      DC or AC? The driver of a fast-charging car is usually watching progress from somewhere else in
      the building, so a DC session earns a much denser cadence (WAKE_DC_CHARGING_MINUTES) than an AC
      one. This decides that, and also fills in is_dcfc for the uploaded frame - which is how the v2
      chargingState becomes CHARGING_DC instead of the blanket CHARGING_AC it reported before, since
      nothing in the firmware had ever produced is_dcfc and every consumer just read the default.

      THE CAR'S OWN WORD WINS. If the profile provides an is_dcfc PID it is already in current_frame
      by now, and it is better evidence than anything we can infer - so we read it and stop.

      Otherwise infer from POWER. Note the dongle deliberately does not report derived power (it
      reports only what the car provides), and that rule is untouched: this computes V*I to make a
      local DECISION, and publishes no number. 40 kW sits far above any AC session - three-phase 22 kW
      is the practical ceiling, and the -52 A at 381 V measured on this car is ~20 kW - so a frame
      over it is DC with room to spare. Both PIDs are needed; a profile missing either infers nothing
      and the session is treated as AC, which is the safe way to be wrong (a 3-minute cadence on a DC
      charge is a slower update, where 1 minute on an AC charge would be wasted wakes for hours).
    */
    bool is_dcfc = false;
    bool dcfc_from_car = false;
    if (is_charging) {
      double volts = 0, amps = 0;
      bool have_v = false, have_a = false;
      for (const auto& res : current_frame) {
        if (res.first == "is_dcfc") { is_dcfc = res.second != 0; dcfc_from_car = true; break; }
        if (res.first == "voltage") { volts = res.second; have_v = true; }
        else if (res.first == "current") { amps = res.second; have_a = true; }
      }
      if (!dcfc_from_car && have_v && have_a) {
        const double power_kw = fabs(volts * amps) / 1000.0;
        is_dcfc = power_kw >= (double)DC_CHARGING_MIN_KW;
#if ENABLE_LOGGING
        logEvent("CHARGE %.1fkW (%.1fV x %.1fA) -> %s", power_kw, volts, amps, is_dcfc ? "DC" : "AC");
#endif
      }
      // Only published while charging: is_dcfc says nothing on its own, and asserting a charge KIND
      // on a frame that is not charging would be a claim we have no basis for.
      if (!dcfc_from_car) current_frame.emplace_back("is_dcfc", is_dcfc ? 1.0 : 0.0);
    }
    // NOTE: power is deliberately NOT derived here (V*I/1000). Design decision: the dongle reports only what
    // the car/profile actually provides; derived metrics are the app/backend's job. The API's "missing:
    // power" note is informational - uploads are accepted regardless. If a profile later includes a real
    // power PID, it flows through like any other value.

    /*
      The charge verdict everything downstream of the frame runs on: the cadence, the taper latch, and the
      bridge budget. Either signal is enough - a negative current sample OR the car's counters moving.

      RELEASE HYSTERESIS (CHARGE_RELEASE_POLLS). recently_charging used to be a straight copy of is_charging,
      so it self-cleared on the FIRST poll that read current >= 0 - which is precisely the sample that turned
      out to be untrustworthy. One reading now only starts the release; it takes CHARGE_RELEASE_POLLS
      consecutive polls with no evidence at all to actually let go. Holding the latch keeps two things alive
      for that extra poll: the fast cadence (so the confirming poll happens in 3 minutes, not 30) and the
      gate's taper bridge (so a low-rail wake still spends a confirm-poll instead of skipping outright).
    */
    const bool was_recently_charging = recently_charging; // pre-poll latch, read before we overwrite it
    const bool charge_evidence = is_charging || charge_progress;
    // A silent poll (CHARGE_SILENT_HOLD_POLLS) is not evidence the charge ended, so it holds the release. An
    // unconfirmed charging sample restarts it, so only consecutive non-charging samples release. Neither can
    // set the latch.
    const bool silent_hold = was_recently_charging && obd_count == 0 &&
      charge_silent_polls < (uint8_t)CHARGE_SILENT_HOLD_POLLS;
    // Silence with the DC-DC on is the car, not the charge ending: it never releases, so the rail drop still
    // gets its read. Past the hold it waits at the parked cadence (high_rail_held, below).
    const bool high_rail_unanswered = g_sample_poke && g_rail_before_poke >= RAIL_AWAKE_VOLTS && obd_count == 0;
    const bool high_rail_silent = was_recently_charging && high_rail_unanswered;
    const bool high_rail_held = high_rail_silent && !silent_hold;
    if (obd_count > 0) charge_silent_polls = 0;
    else if (silent_hold) charge_silent_polls++;
    if (charge_evidence || unconfirmed_charging) {
      charge_release_polls = 0;
    } else if (was_recently_charging && !silent_hold && !high_rail_silent && charge_release_polls < 255) {
      charge_release_polls++;
    }
    // Still "charging" for cadence purposes while the release is unconfirmed. Also the taper latch: the gate
    // reads this next wake to decide whether a low-rail wake is worth a confirm-poll.
    recently_charging = charge_evidence ||
      (was_recently_charging && charge_release_polls < (uint8_t)CHARGE_RELEASE_POLLS);
    if (charge_evidence) charge_last_evidence_us = esp_rtc_get_time_us();
    if (was_recently_charging && !recently_charging && g_sample_poke) {
      charge_followup = 1;
#if ENABLE_LOGGING
      logEvent("FOLLOWUP armed (latch released)");
#endif
    }

    if (charge_evidence) {
      // Taper-bridge budget is bound on SoC STALL, not confirm-count: a rising SoC resets it (a real taper
      // bridges forever); a flat/falling SoC on a confirm-poll burns it (stuck sensor, or the 100% balancing
      // tail -> give up). Only confirm-poll wakes (g_taper_confirm) accrue stalls.
      double soc = 0.0;
      for (const auto& res : current_frame) { if (res.first == "soc") { soc = res.second; break; } }
      if (soc > prev_charge_soc)      taper_confirm_wakes = 0;        // SoC progressed -> productive, reset
      else if (g_taper_confirm)       taper_confirm_wakes++;          // confirm-poll with no SoC gain -> stall
      if (soc > 0.0)                  prev_charge_soc = soc;          // remember THIS poll's SoC for next compare
    } else if (!recently_charging) {
      // Released, not merely held by a silent or unconfirmed poll: a held poll resetting these would let an
      // intermittently answering car defeat TAPER_CONFIRM_MAX_WAKES.
      taper_confirm_wakes = 0; // charge over -> reset the bridge budget
      prev_charge_soc = 0.0;   // ...and the SoC baseline, so the NEXT charge compares against its own start
    }

    // Feature B - pick the next-wake cadence. Dense while the charge latch is up, which is now either signal
    // plus the unconfirmed-release window above; it was the bare current sign, and a single bad sample of it
    // cost a 30-minute hole mid-charge. Still NOT keyed on rail: that would MISS the taper, whose rail has
    // dropped to ~13.2. Everything else relaxes to the long parked interval, and the cumulative cap still
    // stops a stuck signal - of either kind - from fast-waking forever.
    // On a progress-only or release-hold poll is_dcfc is false (it is only inferred from a current reading),
    // so those take the AC tier. That is the safe way to be wrong, for the same reason config.h gives: a
    // 3-minute cadence on a DC charge is a slower update, where 1 minute on an AC charge is hours of waste.
    bool cadence_charging = recently_charging;
    // Corroboration: does a counter that moves during any real charge (soc / kwh_charged) back the charge?
    bool has_charge_counter = false;
    bool corroborated = false;
    if (cadence_charging) {
      auto roseAboveHigh = [](double v, double& hw, double eps) {
        if (hw < 0.0)     { hw = v; return false; }   // first sample of this charge: anchor only
        if (v > hw + eps) { hw = v; return true; }
        return false;
      };
      for (const auto& res : current_frame) {
        if (res.first == "soc") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_soc_hw, CHARGE_PROGRESS_SOC_PCT);
        } else if (res.first == "kwh_charged") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_kwh_hw, CHARGE_PROGRESS_KWH);
        } else if (res.first == "soe") {
          has_charge_counter = true;
          corroborated |= roseAboveHigh(res.second, charge_corr_soe_hw, CHARGE_PROGRESS_SOE_WH);
        }
      }
      if (corroborated) {
#if ENABLE_LOGGING
        if (charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60)
          logEvent("CHARGE corroborated (soc/energy rose) -> charging cadence");
#endif
        charge_uncorroborated_s = 0;
      }
    }
    const bool uncorroborated = charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60;
    const uint64_t now_us = esp_rtc_get_time_us();
    // An answer ends the post-drive window: from here the charge evidence decides.
    if (pdw_open && (obd_count > 0 || !pdwActive(now_us))) {
#if ENABLE_LOGGING
      logEvent("POSTDRIVE window closed reason=%s polls=%u", obd_count > 0 ? "answered"
        : pdw_polls >= (uint8_t)POST_DRIVE_MAX_POLLS ? "spent" : "expired", (unsigned)pdw_polls);
#endif
      pdw_open = 0;
    }
    // A hold, or a release at a sample poke (a pause in the session), keeps the charge's budget, cap and
    // corroboration until its last evidence is stale; a stuck current must not get a fresh budget each time.
    // A streaming wake (a drive) always resets.
    const bool charge_state_stale = chargeStateStale(now_us);
    g_cadence_reason = nullptr;
    if (!cadence_charging) {
      // Not charging -> long cadence, and clear BOTH the budget and the cap latch (the charge genuinely
      // ended / rail dropped). Only here and in the high-rail hold below does the cap latch clear.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      if (!g_sample_poke || charge_state_stale) {
        charging_fast_minutes = 0;
        charging_cap_latched = false;
        resetChargeCorroboration();
      }
      // A silent car at a high rail may be charging without saying so. Look again soon, a bounded number of
      // times: inside the post-drive window (POST_DRIVE_*), else once after a charge (charge_followup).
      if (high_rail_unanswered && pdwActive(now_us)) {
        pdw_polls++;
        g_next_wake_minutes = WAKE_CHARGING_MINUTES;
        g_cadence_reason = "pdw";
      } else if (high_rail_unanswered && charge_followup) {
        charge_followup = 0;
        g_next_wake_minutes = WAKE_CHARGING_MINUTES;
        g_cadence_reason = "followup";
#if ENABLE_LOGGING
        logEvent("FOLLOWUP spent rail=%.2f -> %dmin", g_rail_before_poke, WAKE_CHARGING_MINUTES);
#endif
      }
    } else if (high_rail_held) {
      // Latched, silent past the hold, rail still high: wait at the parked cadence so the car can sleep
      // between polls. No budget accrues; once stale, a car whose rail never drops leaves no budget, cap or
      // high-water mark for the next charge.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-held";
      if (charge_state_stale) {
        charging_fast_minutes = 0;
        charging_cap_latched = false;
        resetChargeCorroboration();
        prev_charge_soc = 0.0;
      }
    } else if (charging_cap_latched ||
               charging_fast_minutes >= (uint32_t)WAKE_CHARGING_MAX_MINUTES) {
      // Charging, but we've spent the cumulative fast-cadence budget -> latch slow and STAY slow (don't
      // reset the budget here - that's what previously let it bounce straight back to fast).
      charging_cap_latched = true;
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-cap";
    } else if (uncorroborated) {
      // Current says charging, counters say not. Slow, not latched: a later counter rise resets the
      // count above and restores fast cadence.
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      g_cadence_reason = "parked-uncorroborated";
      charge_corr_last_us = now_us;
    } else {
      // Charging within budget -> dense sampling, denser still on DC.
      g_next_wake_minutes = is_dcfc ? WAKE_DC_CHARGING_MINUTES : WAKE_CHARGING_MINUTES;
      // Accumulate MINUTES, not wakes. The budget means "12 hours of fast cadence", and counting
      // wakes x WAKE_CHARGING_MINUTES only expressed that while there was a single fast tier - with a
      // 1-minute DC tier the same count would represent a third of the wall clock, so a long DC
      // session would latch back to the parked cadence three times sooner than intended.
      charging_fast_minutes += (uint32_t)g_next_wake_minutes;
      // A silent poll counts too: no answer is not corroboration, so a car that rarely answers still reaches
      // CHARGE_CORROBORATE_MINUTES on wall-clock time. Only once a counter has answered this charge: a
      // profile without one could never corroborate, and silence alone must not slow a real charge.
      const bool counter_seen = charge_corr_soc_hw >= 0.0 || charge_corr_kwh_hw >= 0.0 || charge_corr_soe_hw >= 0.0;
      if ((has_charge_counter || (obd_count == 0 && counter_seen)) && !corroborated && charge_corr_last_us &&
          now_us > charge_corr_last_us) {
        // Capped at one fast interval, so time before this charge (or a long sleep) is never counted.
        uint64_t d = (now_us - charge_corr_last_us) / 1000000ULL;
        const uint64_t cap = (uint64_t)(WAKE_CHARGING_MINUTES + 1) * 60;
        if (d > cap) d = cap;
        const uint32_t before = charge_uncorroborated_s;
        charge_uncorroborated_s += (uint32_t)d;
#if ENABLE_LOGGING
        if (before < (uint32_t)CHARGE_CORROBORATE_MINUTES * 60 &&
            charge_uncorroborated_s >= (uint32_t)CHARGE_CORROBORATE_MINUTES * 60)
          logEvent("CHARGE uncorroborated %lus (soc/energy not rising) -> parked cadence",
            (unsigned long)charge_uncorroborated_s);
#endif
      }
      charge_corr_last_us = now_us;
    }

    if (frame_has_data) { // skip frames that would carry only drivingState so we never POST a meaningless tlm
      obd_result_vector.push_back(current_frame); // store multiple frames to send at the same time, lowers cellular usage
#if ENABLE_NETWORK
      g_geo_queued = poll_geo;
#endif
      driveFrameBuffered();
      // Bound the RAM upload buffer so a long offline stretch can't grow it without limit (-> OOM). The
      // full history is already in flash; for live telemetry the freshest frames matter most, so drop the
      // oldest when over the cap.
      if (obd_result_vector.size() > MAX_UPLOAD_BUFFER) {
        obd_result_vector.erase(obd_result_vector.begin(),
          obd_result_vector.begin() + (obd_result_vector.size() - MAX_UPLOAD_BUFFER));
      }
    }
#if PRINT
    else {
      Serial.printf("Empty frame (no OBD/GPS data) - skipping upload\n");
    }
#endif

#if ENABLE_LOGGING
    // Log every reading (even when the frame is too thin to upload) with full hardware diagnostics.
    datalogger.writeLine(buildLogLine(current_frame, obd_count));
#endif
#if ENABLE_BLE
    const uint16_t ble_mtu = ble.mtu();
    const BleTlmFrame ble_frame = bleTlmFrame(current_frame, bleNotifyBudget(ble_mtu));
    if (ble.logData(ble_frame.notify, ble_frame.full)) {
      if (ble_mtu != g_logged_ble_mtu || ble_frame.dropped > g_logged_ble_drop) {
        g_logged_ble_mtu = ble_mtu;
        g_logged_ble_drop = ble_frame.dropped;
        Serial.printf("BLE frame (%d B, mtu %u): %s\n", (int)ble_frame.full.size(), (unsigned)ble_mtu,
          ble_frame.notify.c_str());
#if ENABLE_LOGGING
        logEvent("BLE tlm mtu=%u bytes=%u sent=%u %s drop=%d", (unsigned)ble_mtu, (unsigned)ble_frame.full.size(),
          (unsigned)ble_frame.notify.size(), ble_frame.dropped ? "trimmed" : "fit", ble_frame.dropped);
#endif
      }
      if (!logging_message_printed) {
        Serial.printf("Logging data over BLE: ");
        logging_message_printed = true;
      } else {
        Serial.printf("."); // Add a dot (show that something is happening)
      }
      // NOTE: do NOT refresh last_active here. BLE streaming must not pin us awake - the motion/speed leash
      // governs sleep (person-present model). (Was last_active = rtc.getEpoch(), also a unit bug: last_active
      // is compared against millis(), not epoch seconds.)
#if PRINT
      Serial.printf("Successfully logged BLE\n");
#endif
      obd_result_vector.clear(); // BLE connected: stream to the phone, stand cellular down (clear the buffer)
    } else { // if logging data over BLE failed / no phone connected -> fall through to the cellular block
      if (logging_message_printed) {
        logging_message_printed = false;
        Serial.println(); // Newline to end line of single dots
      }
#endif

#if PRINT
      Serial.printf("Resultvector: %d\n", ++num);
      for (auto res : current_frame) {
        Serial.printf("Value: %-12s : %2f\n", res.first.c_str(), res.second);
      }
#endif
#if ENABLE_NETWORK
      // Skip cellular entirely while a phone is RELAYING over BLE: BLE is the transport (the phone relays to
      // the ABRP cloud), and a TLS POST here competes with the BLE link for CPU + contiguous heap and can
      // hiccup it -> the app sees the drop and reconnects (the 0x13 churn). Buffered frames flush over
      // cellular once relaying ends or the device sleeps.
      // bleRelaying (connected AND authenticated), NOT bleConnected: with the gated GATT table a phone that
      // declined pairing stays connected but takes nothing, and suppressing on mere presence made the dongle
      // buffer silently while such a phone camped - data flowed nowhere live (field-found). Cost of the swap:
      // in the few seconds between CONNECT and AUTH OK a POST may now overlap the connect/pairing handshake;
      // accepted - it is rare (needs the batching timer to expire inside that window), a hiccup there only
      // delays pairing/setup by one retry, and the alternative recreates the buffering hole.
      // Ride through the phone's connect/disconnect flap: after a BLE drop, hold cellular off for the grace
      // window so a POST can't collide with the reconnect handshake on the shared modem UART (the http=0
      // regression). g_ble_disconnect_ms==0 => no RELAYING session has ended this wake => no hold-off, which
      // covers both the no-phone case and a peer that connected but never authorized (see
      // g_ble_session_relayed - changeState only stamps this for a session that actually held the transport).
      bool ble_upload_grace = (g_ble_disconnect_ms != 0) && (( millis() - g_ble_disconnect_ms ) < BLE_UPLOAD_GRACE_MS);
      // Fast handoff: the first upload after a BLE session ends skips the batching timer and fires the moment
      // the grace clears - the user watching the app/cloud sees the transport switch within seconds. Later
      // uploads batch on transmission_delay as normal.
      bool ble_handoff_fire = g_ble_handoff_pending && !ble_upload_grace;
      // The BLE state sets transmission_delay = 0 (see getTransmissionDelay), which is only safe while that
      // state implies RELAYING - the upload is suppressed anyway, so the zero never fires. It is not safe
      // for a phone that is connected but has not passed the ABD8 proof: nothing suppresses those uploads
      // (correctly - see bleRelaying below) and with a zero cadence nothing rate-limits them either, so the
      // dongle POSTs on EVERY loop iteration, each one opening and tearing down an SH/TLS session with the
      // whole BLE stack resident. That is the http=0 thrash, and it delivers less than doing nothing would.
      // A connection must not change the cellular cadence before it has earned the transport, so fall back
      // to the ACTIVE cadence until AUTH lands. Computed here rather than in getTransmissionDelay because
      // transmission_delay is a snapshot taken at changeState() and AUTH arrives seconds AFTER the
      // ACTIVE -> BLE transition, so a value fixed at transition time could never track it.
      const unsigned long upload_delay = (current_state == BLE && !bleRelaying())
        ? getTransmissionDelay(ACTIVE) : transmission_delay;
      // No token, no upload. Belt-and-braces since an unpaired dongle also never polls, so the buffer
      // should be empty anyway - but the alternative is powering the modem for a POST the API can only
      // answer 401, and this is the one condition where that is knowable in advance.
      if (!bleRelaying() && !g_ota_active && !ble_upload_grace && !obd_result_vector.empty() &&
          !tlm_push_token.empty() &&
          (ble_handoff_fire || ( millis() - last_broadcast ) > upload_delay)) {
        // Awake-aware backoff. The fail cap exists to stop the modem thrashing a SLEEPING car's 12V when
        // uploads keep failing (the overnight drain). But when the car is AWAKE/driving (OBD answering or
        // we're moving) there's no battery concern and the live telemetry matters most, so keep retrying
        // regardless of the count. Only an asleep, repeatedly-failing car goes quiet. (A mid-drive cell
        // hiccup used to trip the cap and silence the whole rest of the drive - this is the fix.)
        bool car_awake = (obd_count > 0) || !is_currently_parked;
        bool backed_off = !car_awake && (upload_fail_count >= MAX_UPLOAD_FAILS_PER_WAKE);
        if (!backed_off && network.connected()) {
          // Split the socket-open (connect) check out of the outer gate. BUG: when connect() returned false
          // (modem registered but the SH/TLS session failed to open - common under car electrical load), the
          // OLD single condition `&& network.connect()` skipped the ENTIRE block - no failure counted,
          // g_http0_streak never advanced, no cleanup, and the recovery ladder never fired - so the loop
          // silently stalled (field-confirmed: ~2 min of zero upload attempts after a single http=0). Now an
          // open failure is handled in the else below so recovery still engages.
          if (network.connect()) {
          g_ble_handoff_pending = false; // one-shot: consume the fast handoff only once a session is actually open
#if ENABLE_LOGGING
          // Signal + modem-voltage snapshot right before the POST, so each upload outcome below can be
          // attributed to RF (low csq), power (sagging vbat), or session (both healthy but still http=0).
          logEvent("NET diag %s ble=%d obd=%d open=%s", network.netDiag().c_str(), g_ble_up ? 1 : 0, obd_count,
            network.lastOpenTrace().c_str());
#endif
          int before = (int)obd_result_vector.size();
          bool ok = uploadBuffered(false); // send one capped batch (<= MAX_FRAMES_PER_POST); drains backlog over cycles
          if (ok) {
            int sent = before - (int)obd_result_vector.size();
            Serial.printf("Succesfully logged %d messages (%d left)\n", sent, (int)obd_result_vector.size());
#if ENABLE_LOGGING
            logEvent("UPLOAD ok http=%d t=%lums sent=%d left=%d %s", g_last_http, g_last_post_ms, sent,
              (int)obd_result_vector.size(), g_last_status.c_str());
#endif
            upload_fail_count = 0;
            g_http0_streak = 0;
            g_recovers_since_ok = 0;
            net_reboot_count = 0; // outage over - re-arm the bounded reboot backstop
          } else {
            // http=0 = connection-level failure (bearer/PDP context dead); a 4xx/5xx means the server DID
            // respond, so the bearer is fine - only http=0 counts toward the stale-bearer recovery below.
            // A batch that never left the dongle (no token, signing or a refused header) says nothing about the
            // bearer, and restarting or rebooting the modem would not change it.
            if (!g_upload_local_fail) { if (g_last_http == 0) g_http0_streak++; else g_http0_streak = 0; }
            if (!car_awake && ++upload_fail_count >= MAX_UPLOAD_FAILS_PER_WAKE) {
              Serial.printf("Upload failed %d times this wake (asleep) - backing off until next wake\n",
                upload_fail_count);
#if ENABLE_LOGGING
              logEvent("UPLOAD backoff after %d fails (http=%d, asleep) - quiet until next wake",
                upload_fail_count, g_last_http);
#endif
            }
#if ENABLE_LOGGING
            else {
              logEvent("UPLOAD FAIL http=%d (%d this wake, awake=%d) - %d frame(s) kept",
                g_last_http, upload_fail_count, car_awake ? 1 : 0, (int)obd_result_vector.size());
            }
#endif
            // Only drop the SH session on FAILURE - a possibly-dead session is torn down (SHDISC) so the next
            // connect() opens a fresh one. On SUCCESS we deliberately keep it alive for reuse (reference
            // keep-alive pattern); tearing it down every cycle forced a full TLS handshake per upload.
            network.disconnect();
          }
          } else {
            // Registered modem but the SH/TLS session would not open. Treat it as a connection-level failure
            // (like http=0) so the recovery ladder below engages, instead of the loop silently stalling on a
            // dead link. The handoff flag is deliberately NOT consumed here, so the next cycle retries fast.
            g_last_http = 0;
            if (!car_awake) ++upload_fail_count;
            // Except a refused certificate: the bearer is fine, and restarting or rebooting the modem cannot
            // change the server's chain. The frames stay buffered for firmware that trusts it.
            if (!network.trustRefused()) g_http0_streak++;
#if ENABLE_LOGGING
            logEvent("UPLOAD open FAILED %s awake=%d streak=%d kept=%d",
              network.netDiag().c_str(), car_awake ? 1 : 0, g_http0_streak, (int)obd_result_vector.size());
            logEvent("UPLOAD open trace %s up=%lds", network.lastOpenTrace().c_str(), network.upForMs() / 1000);
#endif
            network.disconnect();
          }
          // Stale-bearer recovery: repeated http=0 means the modem's data context died (e.g. after a long BLE
          // session left cellular idle). Reopening the HTTP session can't revive a dead PDP context; a full
          // re-init (restart -> begin/checkSIM/APN/CNCFG) does. Bounded, and only reachable with NO phone
          // connected (cellular is BLE-gated), so it can't stall a live BLE stream.
          if (g_http0_streak >= HTTP0_RESTART_THRESHOLD) {
#if ENABLE_LOGGING
            logEvent("NET recover: %dx http=0 -> modem restart (stale bearer)", g_http0_streak);
#endif
            network.restart();
            g_http0_streak = 0;
            g_recovers_since_ok++;
#if ENABLE_LOGGING
            logEvent("NET recover done: link=%s (power-cycle %d since last ok)",
              network.connected() ? "up" : "STILL DOWN", g_recovers_since_ok);
#if ENABLE_CELL
            if (!network.connected()) {
              auto diag = network.initFailureDiag();
              logEvent("NET recover diag %s", diag.first.c_str());
              if (!diag.second.empty()) logEvent("NET recover radio %s", diag.second.c_str());
            }
#endif
#endif
            // Escalation: a wedged modem has survived 3 consecutive power-cycles in the field - the only
            // recovery that has EVER worked is a fresh ESP32 boot. Take it deliberately through the normal
            // sleep path: the flush gets one last try, the BACKLOG persists every frame, and WOM re-wakes
            // within ~a second while driving (timer backstop 1 min when parked). Bounded by net_reboot_count
            // (RTC) so a genuine dead-coverage drive reboots at most twice, then just buffers.
            if (g_recovers_since_ok >= 2 && net_reboot_count < 2) {
              net_reboot_count++;
#if ENABLE_LOGGING
              logEvent("NET recover exhausted -> reboot via deep sleep (#%lu, %d frames to backlog)",
                (unsigned long)net_reboot_count, (int)obd_result_vector.size());
#endif
              pdwOpenWindow(); // the reboot ends this awake period
              g_next_wake_minutes = 1;
              enterDeepSleep(true, "net-recover"); // never returns - fresh boot restores the backlog and re-inits everything
            }
          }
        }
        last_broadcast = millis();
      }
#endif
#if ENABLE_BLE
    }
#endif
    last_poll = millis();

#if ATRV_GATE_ENFORCE && ENABLE_SLEEP && ENABLE_MEMS
    // Parked-poke fast-path: this wake was a deliberate HEARTBEAT poke of an asleep car (not a drive/charge).
    // We've now taken the one reading; before sleeping, run the Test C before/after experiment: did the poll
    // wake the car? Wait a few seconds for any ECU power-up, re-read the true rail, and log it beside the
    // pre-poll rail + the running tallies. Then sleep IMMEDIATELY instead of idling ACTIVE->CHARGING->STANDBY.
    // Weak-rail cold boot: the window this wake exists for has expired. Sleep on the parked cadence, the
    // same as the skip path would have done immediately - the only difference is that a person had a couple
    // of minutes to reach the dongle first. A connected phone holds it open (the BLE state refreshes
    // last_active, and maintenance_mode covers a serial session), so this only fires when nobody came.
    if (g_weak_rail_cold_boot && !maintenance_mode && !ble.connected() &&
        millis() > WEAK_RAIL_COLD_BOOT_AWAKE_MS) {
#if ENABLE_LOGGING
      logEvent("WEAK-RAIL cold-boot BLE window expired (nobody connected) -> deep sleep %dmin",
        WAKE_PARKED_MINUTES);
#endif
      g_next_wake_minutes = WAKE_PARKED_MINUTES;
      enterDeepSleep(false); // modem was never powered this wake - never returns
    }

    if (g_sample_poke && !maintenance_mode) {
#if ENABLE_IO36_RAIL
      delay(5000);                       // give ECUs time to power up IF the poll woke the car
      float rail_after = readIO36Rail(); // local only - do NOT overwrite g_last_io36, or the SLEEP-enter log
                                         // and cadence decision (top-of-cycle rail) would disagree with it
#else
      float rail_after = 0.0f;
#endif
#if ENABLE_LOGGING
      // SAMPLE_POLL: did the one-shot read of an awake/charging car return real data, and did the rail move?
      // is_charging here lets us confirm the current-sign verdict tracks the rail/SoC; the adjacent telemetry
      // row carries the full soc/power/current values for this same poll.
      // prog/rel are what make the two charge signals separable in a field log: prog=1 is_charging=0 is the
      // exact case this exists for (counters moving, current sample says otherwise), and rel counts down the
      // unconfirmed-release window, so a cadence that relaxed can be traced to which signal gave up when.
      // No running tallies: logEvent keeps 119 characters, and the dump counts these lines and pids=0 itself.
      // Fields: DONGLE_CONTEXT §9.
      const uint64_t pdw_now_us = esp_rtc_get_time_us();
      const unsigned pdw_left = pdwActive(pdw_now_us)
        ? (unsigned)((POST_DRIVE_WINDOW_US - (pdw_now_us - pdw_open_us) + 59999999ULL) / 60000000ULL) : 0;
      logEvent("SAMPLE_POLL pids=%d is_charging=%d prog=%d rel=%u taper=%d stall=%lu rail=%.2f->%.2f hold=%d sil=%u fu=%u pdw=%u",
        obd_count, is_charging ? 1 : 0, charge_progress ? 1 : 0, (unsigned)charge_release_polls,
        g_taper_confirm ? 1 : 0, (unsigned long)taper_confirm_wakes, g_rail_before_poke, rail_after,
        high_rail_silent ? 1 : 0, (unsigned)charge_silent_polls, (unsigned)charge_followup, pdw_left);
#endif
      enterDeepSleep(true); // modem was powered this wake; flush + sleep
    }
#endif
  }
}