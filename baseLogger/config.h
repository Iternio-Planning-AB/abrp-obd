/*  
  Contains macros and default values
  Everything should work with limited functionality if different variable disabled, but has not been tested
*/

// ====================================================================================
// APP & SECURITY CONFIGURATION MAP - PRODUCTION DEFAULTS
// ====================================================================================

// ------------------------------------------------------------------------------------
// 1. SECURITY & ACCESS CONTROL (All locked, do not change for production)
// ------------------------------------------------------------------------------------
// BLE_ENABLE_BONDING          1  // Link encrypted (LE Secure Connections). 0 = plaintext link
// BLE_MAX_BONDS              10  // Soft cap under Bluedroid's hard 15; prunes at bring-up
//                                // (the ABD8 HMAC proof is the ONLY session grant - not switchable)
// PROFILE_REQUIRE_SIGNATURE   1  // Profile values must arrive in a verified JWS
// OTA_REQUIRE_SIGNATURE       1  // Firmware images must be ECDSA-signed

// ------------------------------------------------------------------------------------
// 2. DIAGNOSTICS (Must ALL be 0 in normal use)
// ------------------------------------------------------------------------------------
// PRINT                       0
// PRINT_MEMS                  0
// FAKE_DATA                   0
// FILE_TEST                   0
// NETWORK_TEST                0
// ENABLE_CAN_SNIFF            0
// CAN_SNIFF_JOIN_PROBE        0
// BLE_FORCE_REPUSH            0  // WARNING: If set to 1, it forces a full clean + JWS push 
                                  // + ATZ on every single reconnect instead of the fast path.

// ------------------------------------------------------------------------------------
// 3. TRANSPORT & FEATURES (Standard Cellular Build)
// ------------------------------------------------------------------------------------
// ENABLE_BLE                  1
// ENABLE_CELL                 1
// ENABLE_WIFI                 0  // Disabled for cellular build
// ENABLE_GPS                  1
// ENABLE_MEMS                 1
// ENABLE_LOGGING              1
// ENABLE_SLEEP                1
// ENABLE_OTA                  1

// ------------------------------------------------------------------------------------
// 4. POWER & BATTERY SAFETY (Values validated during power review)
// ------------------------------------------------------------------------------------
// ENABLE_ATRV_GATE            1
// ATRV_GATE_ENFORCE           1  // Never poll a sleeping car
// WAKE_PARKED_MINUTES        30  // Production field value (test value was 8)
// WAKE_CHARGING_MINUTES       3
// MAX_AWAKE_MINUTES          60
// ACTIVE_TIMER                5  // Minutes
// CHARGING_TIMER              5  // Minutes

// ------------------------------------------------------------------------------------
// 5. DEVELOPER MAINTAINED (Check these before compiling!)
// ------------------------------------------------------------------------------------
// HEARTBEAT_POLL_MAX_MINUTES 30  // Test value ("CONFIRMATION run"). Inert right now because 
                                  // ENABLE_PARKED_HEARTBEAT is 0. If ever enabling heartbeat 
                                  // for production, change this to 1440.


#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#include <cstdint>
#include <ctime>
#include <limits>

#ifndef PRINT
#define PRINT 0
#endif

// Separate, very chatty motion-sensor debug. Off by default so PRINT can stay on for OBD/network debug
// without the MEMS loop flooding the serial monitor every iteration.
#ifndef PRINT_MEMS
#define PRINT_MEMS 0
#endif

// Bench aid: when set, the poll loop emits a synthetic telemetry frame (standard ABRP keys) instead of
// reading the OBD bus, so the upload path can be validated without a car or emulator. Never ship it on.
#ifndef FAKE_DATA
#define FAKE_DATA 0
#endif

/*
  There is deliberately NO built-in telemetry token here.

  A TLM_PUSH_TOKEN used to sit in this file as a factory default, so an unpaired dongle uploaded to one
  developer's test vehicle. Two problems with that, and the second is the serious one: every image ever
  built carried a live credential for a real vehicle in plaintext flash, readable by anyone with the
  binary or a serial console; and a dongle that had not been paired still POSTed - real data about
  whichever car it was plugged into, attributed to the wrong vehicle.

  The token now comes only from the app's profile push (ABD2), which is also where the vehicle-correct
  PID list comes from, and both persist together in /vehicle_profile.json. Before that push the dongle
  has no vehicle identity and does not poll or upload at all - see the g_no_profile path in setup().

  API_KEY further down is a different thing and stays: it identifies the INTEGRATION rather than any
  vehicle or user, the same key ships in every image by design, and the API requires it alongside the
  per-vehicle token it can do nothing without.
*/

#ifndef ENABLE_WIFI
#define ENABLE_WIFI 0    // bench/office builds only; replaces cellular, it does not run alongside it
#endif
// Outside the #ifndef above so -D ENABLE_WIFI=1 gets the seeds too.
#if ENABLE_WIFI
#warning "ENABLE_WIFI: TLS is not verified - bench only"
/*
  Empty on purpose - no network credentials in the source tree.

  These are only a SEED: loadPreferences() (Helpers/misc.h) reads the credentials from the NVS
  "network" namespace ("ssid", "password") and, on a dongle where that key does not exist yet, writes
  these values into it when they are non-empty. Provision NVS on a dongle that needs WiFi, or pass
  -D WIFI_SSID=... at build time for a one-off. With neither, the radio will not associate, which is
  the correct failure for a missing credential.

  Dormant at ENABLE_WIFI 0: this whole block is inside the guard.
*/
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef WIFI_PATH
#define WIFI_PATH "https://api.iternio.com/1/tlm/send"
#endif
#ifndef WIFI_BULK_PATH
#define WIFI_BULK_PATH "https://api.iternio.com/2/app/tlm" // v2 tlm endpoint (see TLM_V2_PATH)
#endif
#endif

// #ifndef ENABLE_OBD
// #define ENABLE_OBD 1
// #endif

#ifndef ENABLE_CELL
#define ENABLE_CELL 1    // the product transport
#endif
#if ENABLE_WIFI && ENABLE_CELL
#error "ENABLE_WIFI replaces the modem: build it with -D ENABLE_CELL=0"
#endif
// Outside the #ifndef above so -D ENABLE_CELL=1 gets the seeds too.
#if ENABLE_CELL
#ifndef CELL_APN
#define CELL_APN "hologram"
#endif
#ifndef CELL_HOST
#define CELL_HOST "api.iternio.com"
#endif
/*
  SERVER ROOTS: uploads go only over a chain that verifies against Helpers/server_roots.h, else nothing
  is sent. A server moving to a new CA needs firmware trusting the new root shipped BEFORE the switch.
  Which roots, the modem's limits and the procedure: DONGLE_CONTEXT.md §6a.
*/
#define API_PATH "/1/tlm/send"
#define CELL_BULK_PATH "/1/tlm/bulk" // legacy v1 bulk endpoint - superseded by TLM_V2_PATH, kept for reference
// The endpoint telemetry actually goes to. v2 takes a typed InputPointList (see bulkStringV2) and, unlike
// v1, its edge accepts a gzipped request body - which is the whole reason for the move: the SIM7070's
// 1024-byte body cap is what limits points-per-POST, and gzip turns 5 points into ~21.
// Auth is by header: an HMAC signature keyed by the vehicle token (Helpers/tlm_signature.h) plus X-API-KEY.
#define TLM_V2_PATH "/2/app/tlm"
#endif

#ifndef ENABLE_GPS
#define ENABLE_GPS 1
#endif

#ifndef ENABLE_BLE
#define ENABLE_BLE 1    // BLE phase: Bluedroid wrapper wired into the awake window (see handleActivityState).
#endif                  // Set 0 for a cellular-only build.

// Pair + bond with the phone (LE Secure Connections, Just Works) so the link is ENCRYPTED - the OTA image,
// the log dump and the profile (which carries the tlm push token) stop being readable by a sniffer.
//
// NOT support-only, despite what this comment used to claim. Setting 1 gates EVERY characteristic value
// AND every CCCD with *_ENCRYPTED (see the `if (bonding_enabled)` blocks throughout blewrapper.cpp), so an
// unpaired phone can connect and read/write NOTHING - it cannot even subscribe. The old wording said
// "if pairing fails everything still works exactly as it did before", which was true of an earlier phase
// and is now the opposite of what the code does; it would send someone into a car expecting a soft
// failure. Corrected 2026-09-05 after checking against blewrapper.cpp.
//
// What that means in practice:
//   - the OS pairing dialog appears at the FIRST gated access, which since the auth-before-push reorder
//     is the app's ABD8 subscribe. Both platforms run pairing and retry the operation transparently, so
//     an ATT security error only surfaces on an actual decline;
//   - pairing is CENTRAL-driven by attribute gating, never a peripheral security request - that is what
//     keeps Android >= 12 to one dialog instead of two (see pruneBonds' comment block);
//   - flipping this needs a reflash, and every already-paired phone must FORGET the device first, or its
//     stale LTK makes the next pairing fail with SEC failed reason=0x0f.
// Recovery from a wedged bond is the BONDCLEAR serial command, not this flag.
#ifndef BLE_ENABLE_BONDING
#define BLE_ENABLE_BONDING 1
#endif

// Soft cap on stored bonds, pruned at BLE bring-up. Bluedroid's HARD limit is CONFIG_BT_SMP_MAX_BONDS=15 and
// hitting it makes NEW phones fail to pair, so we stay well under it and evict eagerly instead. Eviction is
// arbitrary (whatever order the stack lists bonds in) - an evicted phone just re-pairs silently on its next
// connect, so "oldest first" would be cosmetic. 10 is far more than a dev fleet needs while leaving 5 slots
// of slack for several phones pairing inside one awake window (the next wake prunes them back).
#ifndef BLE_MAX_BONDS
#define BLE_MAX_BONDS 10
#endif

// The phase-2 auth gate (ABD8 nonce + HMAC, see Helpers/auth_gate.h) is UNCONDITIONAL - there is no
// AUTH_ENFORCE switch any more. The legacy {"AUTH":"<tlm_push_token>"} write only unlocks the ABD2
// profile push (so setup still works); ONLY a valid ABD8 HMAC proof grants the session, and
// telemetry notify, OTA, the log dump and the cellular stand-down all stay closed until it lands.
//
// The switch existed for an audit mode that let the token grant a session, so an app without the
// handshake lost nothing. That mode provided NO access control - anyone signed into the account
// could read the push token and get everything - and no app without the handshake exists, so it was
// pure attack surface. Removed 2026-09-05.
//
// If you lock yourself out: AUTHCLEAR over serial (physical access) wipes the secret and returns the
// dongle to TOFU. It is also the REVOCATION path - the next phone to connect provisions a brand-new
// secret, locking out every other holder - so connect the phone that should keep access first.

// Phase B lockdown: require every profile VALUE to arrive inside a verified RS256 JWS (see
// Helpers/profile_sig.h + jws_verify.h). 0 = ACCEPT-BOTH: raw per-key profile packages still work, so
// an app that knows nothing about signing can still configure a dongle. 1 = REQUIRED: raw profile
// packages are refused with "UNSIGNED" and only a signature-verified push can change what this dongle
// runs on the CAN bus.
//
// The protocol packages `keys`, `tlm_push_token`, `utc` and `HASH` are NOT profile content and stay
// allowed either way - without them the handshake could not complete at all.
//
// ONE lockout scenario to know before flipping: the app falls back to a package push when it holds no
// JWS for the vehicle's typecode (an unsigned fetch, or a profile cached before obdPidsJws existed).
// With this at 1 the dongle refuses that push and setup fails until the app refetches - which it does
// on the next app start, since the store transform clears obdPidsRefreshed. A stale cache therefore
// costs one app restart, not a bricked dongle. Revert is this one line.
#ifndef PROFILE_REQUIRE_SIGNATURE
#define PROFILE_REQUIRE_SIGNATURE 0
#endif

// DIAGNOSTIC (leave 0): force HASH NOT OK on every connect -> app re-runs the FULL profile push. Served its
// purpose: proved the complete push path works end-to-end (keys=11 config=11, SETUP OK). Now OFF, because a
// full push on every reconnect re-runs the config-apply path each time (needless churn, and it was what kept
// re-triggering the OBD-UART race before the apply was deferred to the loop task). With the hash persisted in
// NVS (getULong fix), reconnects take the cheap HASH OK fast path; the app pushes only when the profile
// actually changes.
#ifndef BLE_FORCE_REPUSH
#define BLE_FORCE_REPUSH 0
#endif

// A CONNECTED (not merely advertising) phone holds the awake leash. The motion leash can't see a person
// sitting still in the seat: on the bench it slept the device UNDER a live streaming session at ~3min (the
// phone saw HCI reason 0x16 = we terminated), killing pairing/Save mid-use. Bounded: the AWAKE_BACKSTOP
// (MAX_AWAKE_MINUTES, parked-gated) still forces sleep, so a phone forgotten in the car with ABRP open can
// hold the device awake for at most that long. Set 0 to restore the pure motion+speed leash.
#ifndef BLE_CONNECTED_HOLDS_AWAKE
#define BLE_CONNECTED_HOLDS_AWAKE 1
#endif

// Hold cellular uploads off for this long after a BLE disconnect, so a TLS POST can't collide with the
// phone's reconnect handshake on the shared modem UART (http=0, seen in the field). Field data: flap
// reconnects land 0-3s after the drop, so 10s covers them with margin while keeping the BLE->cellular
// handoff snappy - the FIRST upload after a real BLE loss fires the moment this window clears (see the
// fast-handoff flag in the upload gate), so the cloud sees a data point within seconds of the phone leaving.
#ifndef BLE_UPLOAD_GRACE_MS
#define BLE_UPLOAD_GRACE_MS 10000
#endif

#ifndef ENABLE_MEMS
#define ENABLE_MEMS 1
#endif

// Circular data logging to onboard flash (LittleFS). Records one CSV row per reading - vehicle metrics
// plus hardware diagnostics - and wraps (overwrites oldest) when the filesystem fills. Survives deep
// sleep. Retrieve/clear over Serial with the DUMP / STAT / CLEAR commands.
// "ENABLE_LOGGING=0 also disables vehicle-profile persistence (see ble.h loadPersistedProfile)"

#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

// Deep-sleep low-power mode. When the device reaches STANDBY (no motion/connection),
// it powers off WiFi/GPS, arms Wake-on-Motion (ICM-42627 INT1 -> GPIO39) and enters
// ESP32 deep sleep (~10mA). A door/motion event wakes the device, which reboots.
// Requires ENABLE_MEMS (the WOM sensor) and the ICM INT1 -> GPIO39 wiring.
#ifndef ENABLE_SLEEP
#define ENABLE_SLEEP 1
#endif

// Periodic wake: in addition to Wake-on-Motion, also wake from deep sleep on a timer to grab a quick
// parked-status reading (a "heartbeat"), then sleep again. Interval is in MINUTES - easy to tune.
// HEARTBEAT_TIMEOUT_MS bounds how long the device stays awake per timer wake trying to read the car, so a
// non-responsive (sleeping) car costs almost no ESP32 power.
//   DUTY CYCLE / 12V SAFETY: active OBD polling wakes a "vampire" EV's ECUs, which then stay awake on the
//   CAR's own ~15 min timer drawing from the 12V battery (the ESP32 sleeping again does NOT stop this).
//   So drain is governed by interval, not ESP32 awake time: wake every 30 min -> car awake ~50% of the day
//   (kills 12V in days); wake every 6 h -> ~4% (12V copes, HV tops it off in normal background routines).
//   Keep the interval long. Set ENABLE_PERIODIC_WAKE 0 to disable entirely. See enterDeepSleep().
#ifndef ENABLE_PERIODIC_WAKE
#define ENABLE_PERIODIC_WAKE 1
#endif
#define PERIODIC_WAKE_MINUTES 5    // VESTIGIAL - no longer referenced anywhere. The adaptive per-sleep timer
                                     // (g_next_wake_minutes = WAKE_CHARGING_MINUTES / WAKE_PARKED_MINUTES,
                                     // below) replaced it. Kept only so old notes referencing it still parse.
// NOTE on the two wake-related intervals (they are NOT duplicates - different axes):
//   PERIODIC_WAKE_MINUTES = how often the ESP32 WAKES from deep sleep to LOOK (cheap: just reads ATRV).
//   HEARTBEAT_POLL_MINUTES (ATRV gate, below) = how often we actually POKE an asleep car for a reading.
// Wake often to cheaply check voltage / catch a charging session; poke a sleeping car rarely. The old
// HEARTBEAT_TIMEOUT_MS (awake-duration leash for a timer wake) is gone - the ATRV gate now decides whether
// to poll at all, and a timer wake that does poll just uses the normal ACTIVE leash (ACTIVE_TIMER).

// NOTE: a low-battery lockout was attempted using obd.getVoltage() (ATRV), but field logs show ATRV reads
// ~5.3V while the car is asleep (not the true 12V), so it can't gate the radio without false-locking out.
// A real lockout needs the IO36_BatDec ADC 12V sense (see POWER_MANAGEMENT.md). Removed for now.

// Bounded upload retries (POWER_MANAGEMENT.md §4). After this many consecutive FAILED uploads in one awake
// window, stop attempting until the next wake - so a failing network/server can't keep the modem TX-ing all
// night (the overnight drain). Resets on a successful upload and on every wake.
#define MAX_UPLOAD_FAILS_PER_WAKE 3

// Consecutive http=0 (no-response) uploads after which we RE-INITIALIZE the modem. http=0 (vs a 4xx/5xx) means
// the request never reached the server -> the data bearer/PDP context is dead, typically after a long idle
// (e.g. a long BLE session where cellular correctly stood down). Reopening the HTTP session does NOT revive a
// dead bearer; a full modem restart (begin/checkSIM/APN/CNCFG) does. 2 = recover fast without thrashing on a blip.
#define HTTP0_RESTART_THRESHOLD 2

// A kept-alive SH/TLS session idle longer than this is closed and reopened instead of reused. Field log
// (24-30 Sep): reuse at the normal 60-75 s cadence failed ~4%, but every reuse after >200 s idle failed
// http=0 after ~8 s (13/13 after a phone session paused uploads, idle 3-34 min). 120 s keeps the normal
// cadence on the reused session and costs one TLS handshake after any longer pause.
#ifndef SESSION_IDLE_MAX_MS
#define SESSION_IDLE_MAX_MS 120000UL
#endif

// How long the clock may go without a real time source before re-acquiring one is worth the cost.
//
// Deep sleep keeps time on the ESP32's internal RC oscillator, measured at 30 s/hour FAST on a parked
// car (~8250 ppm) - so an uncorrected clock stamps frames further and further into the future and the
// backend logs a growing NEGATIVE registration delay (-201 s after one night in a garage). GPS is the
// only correction that was actually live, and a garage is exactly where there is none.
//
// 15 minutes caps the visible skew at ~7.5 s. It used to be an hour (~30 s) on the assumption that each
// re-sync costs an NTP round trip, but CLKPROBE on the Telia SIM showed the modem clock is NITZ-set and
// accurate (AT+CLTS: 1, AT+CCLK within 2 s of a just-synced RTC) - and AT+CCLK is a LOCAL query, ~50 ms,
// no data, no server. So the common case is nearly free and there is little reason to let skew build.
// NTP remains the fallback for SIMs whose visited networks push no NITZ; on those this cadence does cost
// a ~1 s exchange per 15 min of awake time, which is still small against the 60 s upload cadence.
#define CLOCK_RESYNC_SECONDS 900

// Deep-sleep drift compensation (clock_sync.h).
//   MIN_SLEEP: only learn from windows with at least this much sleep - NITZ and the RTC both have 1 s
//              resolution, so an hour bounds one window's error at ~550 ppm.
//   GAIN:      after the first window sets the rate outright, each later one moves it 1/GAIN of the way,
//              so one noisy window cannot undo a night's worth of learning.
//   MAX_PPM:   a window implying a rate past this is a bad time source, not an oscillator, and is ignored.
//              About 2.5x the fastest rate measured (6500 ppm fast).
#define CLOCK_DRIFT_MIN_SLEEP_S 3600
#define CLOCK_DRIFT_GAIN 2
#define CLOCK_DRIFT_MAX_PPM 15000
#define CLOCK_MAX_EPOCH ((uint64_t)std::numeric_limits<time_t>::max() < UINT32_MAX ? \
                         (uint64_t)std::numeric_limits<time_t>::max() : (uint64_t)UINT32_MAX)

// Passive CAN-bus sniff (DIAGNOSTIC / observe-only for now). Each wake, BEFORE any active polling, the
// device puts the OBD adapter in ELM monitor-all mode (ATM1) and just LISTENS for CAN_SNIFF_WINDOW_MS,
// counting frames. It transmits NOTHING on the CAN bus (no Mode22/010D/ClearDTC), so it cannot wake or
// hold the car awake - unlike active polling. Logs a CANSNIFF event (frames + ALIVE/SILENT verdict + a
// sample frame) so we can PROVE whether the Scenic's bus actually goes silent when the car sleeps and that
// ATM1 yields frames on this unit. NOTE: this only OBSERVES + LOGS; it does NOT yet gate polling. Once the
// soak confirms the verdict tracks reality, we add the decision logic (skip the deliberate poll when the
// bus is silent + the heartbeat interval hasn't elapsed). Protocol/baud must match the active profile
// (initScenic = ATSP7, ISO15765 29-bit 500K).
// DISABLED: proved unusable on the Scenic - its gatewayed OBD-II port carries only request/response UDS,
// no free-running broadcast, so monitor mode reads SILENT even with the car fully awake (confirmed with the
// join-probe). Code kept (preserved, not deleted) in case a future vehicle's OBD port does broadcast.
#ifndef ENABLE_CAN_SNIFF
#define ENABLE_CAN_SNIFF 0
#endif
#define CAN_SNIFF_WINDOW_MS 2500   // listen window per wake (ms). A free-running bus emits frames every 10-100ms, so this is ample.

// EXPERIMENT (set back to 0 after ONE awake-car run): the passive sniff read SILENT even while the car was
// awake and answering polls. That means EITHER (a) this OBD-II port carries NO free-running CAN broadcast
// (gatewayed UDS request/response only - common on modern EVs), OR (b) the adapter's CAN controller never
// went "bus-active" (pure monitor without a prior real exchange doesn't join the bus - note the Freematics
// example does init(PROTO_) first). This flag tells them apart: it joins the bus like initScenic() + sends
// ONE UDS request (controller now definitely on the bus) + opens the receive filter wide, THEN monitors.
//   CANSNIFF still 0 frames on an awake car -> (a) confirmed: passive sniffing can't detect sleep here, pivot.
//   CANSNIFF shows frames                  -> (b): the bus has broadcast we can use (find a passive join).
// NOTE: this version TRANSMITS one frame (NOT passive) - it's a diagnostic. Run once on a known-awake car,
// then set back to 0 to restore the passive listen-only sniff.
#ifndef CAN_SNIFF_JOIN_PROBE
#define CAN_SNIFF_JOIN_PROBE 0
#endif

// ---- ATRV (OBD pin-16 voltage) sleep gate ----
// CAN sniffing can't see sleep on the Scenic (gatewayed port, no broadcast). Field logs show the pin-16
// voltage - read via ATRV LOCALLY, with NO CAN traffic - tracks car state cleanly instead:
//     ~5.3V asleep (ECUs silent, OBD returns nothing)  |  ~11V waking  |  ~13.5V awake (ECUs answer).
// So we gate active polling on voltage: read it BEFORE polling (passive) and only poll when the car is
// already awake - polling a SLEEPING car is exactly what wakes it (captured in the logs: 5.3 -> 11 -> 13.5V
// climbing while we hammered it). The threshold sits in the wide ~6V gap.
//   GENERALITY: more portable than CAN sniffing (any OBD adapter reads pin voltage, no broadcast needed),
//   but the THRESHOLD is vehicle-specific - some cars keep pin 16 permanently hot, where only charging
//   voltage (~14V vs ~12.5V resting) separates states. Treat ATRV_AWAKE_VOLTS as a per-profile value.
#ifndef ENABLE_ATRV_GATE
#define ENABLE_ATRV_GATE 1
#endif
#define ATRV_AWAKE_VOLTS 8.0f   // >= this -> awake (poll OK); below -> asleep (a poll would wake it). Scenic: 5.3 asleep vs 13.5 awake.
#define ATRV_GATE_ENFORCE 1     // 0 = OBSERVE (poll every wake); 1 = ENFORCE (skip the poll when asleep = battery-safe).
                                // ENFORCE for the leave-it-in-the-car A-G run: skip wakes are cheap and don't wake the
                                // car (capture B/brownout, E/drift, F/rail, G/trend + validate the skip path live),
                                // while the periodic heartbeat poke still gives C (poll wakes car?) and D (verdict vs OBD).

// RAIL-AWAKE sampling (the main parked-period data source). PROVEN: a parked Scenic returns no useful
// telemetry, and polling it is pure cost (~0.3V sag, no data). The ONE time a parked car will answer with
// real data is when its DC-DC converter is ON - i.e. charging or a remote climate/precondition event - which
// raises the true rail to ~14V. So instead of poking a sleeping car on a timer, we WATCH the rail and only
// sample when it's high (car genuinely awake). One poll per wake, then back to cheap skip-wakes; if the rail
// is still high next wake we sample again (this follows a charge at the wake cadence WITHOUT holding the car
// awake - we sleep between samples, so the rail can truthfully fall when the charge ends = a free exit).
//   CHARGE RUN CONFIRMED the states (calibrated rail): asleep-resting ~12.6-13.2V (a freshly-charged battery
//   rests near the TOP of that), light/energized/post-drive ~13.5V, ACTIVE charge ~14.9-15.0V. 13.3 sits just
//   above the resting band so a full battery doesn't false-trigger; it still catches the 13.5V energized and
//   14.9V charge states. NOTE: rail-rise only decides whether to SAMPLE once; the DENSE charge cadence is
//   gated on current sign (is_charging), not rail - so an idle energized car is sampled rarely, not hammered.
#define RAIL_AWAKE_VOLTS 13.3f

// Parked time-heartbeat: the OLD "poke an asleep car every N minutes" behaviour. DISABLED by default now -
// it's proven to return nothing useful and only costs a voltage sag; rail-rise + motion are the only poll
// triggers. Kept behind this flag so we can A/B it back on if needed. When 0, HEARTBEAT_POLL_* are unused.
#ifndef ENABLE_PARKED_HEARTBEAT
#define ENABLE_PARKED_HEARTBEAT 0
#endif
// (Only used when ENABLE_PARKED_HEARTBEAT 1.) How often to poke an asleep car, and the back-off ceiling.
#define HEARTBEAT_POLL_MINUTES 30

// Heartbeat back-off: a car parked for days doesn't need poking every 40 min. Each heartbeat poll that finds
// the car STILL asleep (no PIDs) doubles the effective interval (40 -> 80 -> 160 ...), capped here. The
// instant a poll returns data (car used again) the interval resets to HEARTBEAT_POLL_MINUTES. Bounds how
// often a long-asleep car gets disturbed.
#define HEARTBEAT_POLL_MAX_MINUTES 30     // CONFIRMATION run: == HEARTBEAT_POLL_MINUTES so the back-off can't
                                          // double the interval - we want the heartbeat to fire FLAT every 20
                                          // min for maximum repetitions. (Production: 1440 to let it back off.)

// ---- is_charging (Feature A) ----
// The charge run proved the current PID's sign is a clean charge signal: charging draws current INTO the
// battery -> NEGATIVE current (~-51A, tapering to ~0 as it fills); discharge/idle is >= 0. We mark
// is_charging when PARKED and current is below a small-negative threshold (the parked gate stops a drive's
// regen-braking negative current from being mislabeled). Per-frame, read from the one sample - never latched.
#define CHARGING_CURRENT_THRESHOLD -2.0f  // A; current below this (more negative) while parked = charging
// The parked gate is only as good as the latch. FIELD (Ioniq 5-profile car, 2026-09-30..10-12): a motion wake
// starts latched PARK, the profile has no speed PID, and the GNSS took 3.5-11.5 min to unlatch - so every regen
// sample in that window published CHARGING_AC, on 9 of 13 drives. A streaming wake (motion / BLE / cold boot,
// polling every few seconds) therefore believes a charging sample only once no sample above
// CHARGE_DISCHARGE_AMPS has been seen for CHARGE_STREAM_CONFIRM_SECONDS: those drives drew discharge current
// at least every 94 s, a plugged-in car draws none. A stationary sample poke keeps the single-sample verdict.
#define CHARGE_DISCHARGE_AMPS          2.0f  // A; a sample above this is the car driving or running loads
#define CHARGE_STREAM_CONFIRM_SECONDS  180

// ---- Charge PROGRESS: the second, independent charge signal ----
// FIELD (boot 14, 13:58 local, AC charge): the current sign is ONE instantaneous sample per wake - readPIDs
// does a single read per PID with no retry, and a sample-poke wake sleeps immediately after - and it lied.
// A car charging at a measured 3.7kW returned current=+0.479A on that one sample. Its own counters, taken
// from the same frame, proved the charge never stopped: soc 28.19 -> 28.44 -> 29.31 and kwh_charged
// 24.407 -> 24.626 -> 25.397 (0.219kWh over the 213s bracketing the bad sample = 3.7kW). Cost of believing
// the sample: cadence relaxed 3min -> 30min, the taper latch dropped, and the charge went unreported for
// half an hour. So cadence also asks whether energy or SoC MOVED. This Scenic field was misnamed:
// kwh_charged there was estimated remaining energy, now soe in Wh, not a cumulative charging counter.
//
// Thresholds are the "is this bigger than noise" floor, not a rate. Genuine cumulative kwh_charged retains
// its 0.005 kWh floor. Estimated soe needs 200 Wh: the Scenic test car rose 61 Wh at unchanged SoC without
// charging, while the measured 3.7 kW charge rose 219 Wh across the bad current sample. SoC retains its
// wider band because a resting BMS can re-estimate it without energy transfer. All use a RATCHETED
// baseline (see logger.ino), so coarsely-quantised values accumulate across polls instead of showing zero
// change at every single interval; while driving the baselines track each sample to exclude regen.
#define CHARGE_PROGRESS_KWH     0.005  // kWh; kwh_charged rise since the baseline that counts as charging
#define CHARGE_PROGRESS_SOC_PCT 0.20   // percentage points; independent SoC progress evidence
#define CHARGE_PROGRESS_SOE_WH  200.0

// Consecutive polls with NO charge evidence (neither current sign nor counter progress) before the charge
// latch releases and the cadence relaxes to WAKE_PARKED_MINUTES. 1 = the old behaviour that this whole
// block exists to fix: fall off the charge on the first sample that says nothing. 2 = one sample must be
// CONFIRMED by the next. The cost of being wrong is one extra WAKE_CHARGING_MINUTES wake at the end of
// every charge; the cost of being wrong the other way is the 30-minute hole above.
#define CHARGE_RELEASE_POLLS    2
// A poll the car did not answer is not evidence the charge stopped. FIELD (Ioniq 5-profile car, 2026-10-05):
// OBD went silent on ~60% of polls mid-charge at a 14.8 V rail; two silent polls released the latch while the
// rail was still high, so neither the taper confirm nor the charge-end read fired and ABRP kept CHARGING at
// 77%. Up to this many consecutive silent polls hold the release at the charging cadence; past it they count,
// which bounds a car that never answers to a few extra fast-cadence wakes.
// Except on a sample poke at a rail >= RAIL_AWAKE_VOLTS. FIELD (issue #7, Ioniq 5-profile car, AC charge): the
// car answered near charge transitions but was silent for about 80 minutes mid-charge at 14.9 V, the bound
// released the latch with the rail still high, and the pause that followed was a plain skip with no read.
// Silence with the DC-DC on never counts toward the release; past this many polls it waits at
// WAKE_PARKED_MINUTES with the latch up, so the car can sleep between polls and the rail drop still gets its
// charge-end read. That costs no extra wake: a rail-high wake polls at the parked cadence anyway. Silence at
// a lower rail (the taper band) keeps the bound above.
#define CHARGE_SILENT_HOLD_POLLS 4
// A charge held through rail-high silence, released at a sample poke or ended at a skip keeps its budget,
// cap and corroboration: a stuck current that answers between silences, or at every 12 V top-up, would
// otherwise get a fresh WAKE_CHARGING_MAX_MINUTES and CHARGE_CORROBORATE_MINUTES each time and never relax.
// A drive still resets them. Only this long after the last charge evidence is
// the state stale enough to drop, so a car whose rail never falls (a 12 V maintainer) starts its next real
// charge fresh. 6 h is past any pause inside one session and short of the gap between two.
#define CHARGE_STALE_RESET_MINUTES 360

// ---- Adaptive wake cadence (Feature B) ----
// The next deep-sleep timer is chosen PER-SLEEP from what this wake observed - there is no persistent "mode":
//   actively charging (is_charging + rail high) -> SHORT interval = dense SoC curve (car's energized, safe);
//   everything else (parked/asleep/idle)        -> LONG interval  = minimal battery sip.
// Driving is governed by motion/the state machine, not this timer. Charging self-exits: when the charge ends,
// current goes >= 0 AND the rail falls, so the next sleep automatically picks the long timer - no masking,
// because we sleep between samples (we never hold the car awake). MAX is a cheap anti-runaway backstop: cap
// the TOTAL time spent in fast cadence so a stuck-high rail can't wake us every few minutes forever.
#define WAKE_CHARGING_MINUTES      3      // dense sampling while actively AC charging
// DC fast charging gets a denser cadence still. The reason is the person, not the battery: a fast
// charge takes 20-40 minutes and the driver is usually watching progress from a restaurant or a shop
// rather than sitting in the car, so a 3-minute-old SoC is visibly stale in a way it never is on an
// overnight AC charge. The car's DC-DC is up throughout, so the extra wakes cost no 12 V.
#define WAKE_DC_CHARGING_MINUTES   1
// Charge power (kW) at or above which a session is treated as DC when the car does not say so itself.
// Only consulted if the vehicle profile has no is_dcfc PID - the car's own word always wins.
// Far above any AC session: three-phase 22 kW is the practical ceiling and a measured Scenic AC charge
// sat at ~20 kW, so nothing AC reaches this. Inferring needs BOTH voltage and current in the profile;
// missing either means no inference and the session is treated as AC, which is the safe way to be
// wrong (a slower update, rather than hours of wasted 1-minute wakes on an overnight charge).
#define DC_CHARGING_MIN_KW         40
#define WAKE_PARKED_MINUTES        30    // FIELD: parked voltage-watch. Each wake is a cheap ~5s rail check
                                         // (no modem, no poll, no BLE on a skip) ~ a few mAh/day. 30 min
                                         // bounds how late we catch a SCHEDULED charge start (no motion, only
                                         // the rail rise shows it); plugging in by hand is caught instantly
                                         // via WOM. Test value was 8.
#define WAKE_CHARGING_MAX_MINUTES  720    // backstop: after this much cumulative fast-cadence, force long
// Fast cadence held by the current sign alone must be confirmed by the car's counters: if the frame has
// soc or kwh_charged and neither rose (CHARGE_PROGRESS_*) within this many fast-cadence minutes, drop to
// parked cadence until one does (not latched). Bounds a wrong current equation to this, not
// WAKE_CHARGING_MAX. Sized for the slowest real case: ~1.2 kW net into 100 kWh with whole-percent soc
// and no kwh_charged steps once in ~50-60 min.
#define CHARGE_CORROBORATE_MINUTES 90
// Post-drive window (DONGLE_CONTEXT §6): a fast charge started right after parking must not wait out the
// parked cadence because its first rail-high polls went unanswered. For this long after the last motion,
// at most this many times, a silent rail-high poke comes back in WAKE_CHARGING_MINUTES. Opened only at the
// awake-ladder end with a fresh high rail, never from OBD; the ladder spends about five minutes of it.
#define POST_DRIVE_WINDOW_MINUTES  15
#define POST_DRIVE_MAX_POLLS       5

// ---- Charge-taper bridging (recently-charging confirm-poll latch) ----
// PROBLEM proven in the field: during a charge taper (CV phase) the DC-DC backs off and the rail drops to
// ~13.2V - indistinguishable from parked-rest ~13.19V by voltage alone - so the rail-awake gate SKIPS the
// whole 89->100% window, even though current was still ~-18A (charging). The reliable signal (current sign)
// is locked behind the unreliable trigger (rail). FIX: when the last poll saw charging (recently_charging),
// spend a cheap confirm-poll on the next low-rail wakes to read the current sign - if still negative, the
// taper's still going (sample it); if not, the charge ended (drop the latch). Bounded + rail-floor guarded.
#define RAIL_TAPER_FLOOR        13.0f  // confirm-poll only if rail >= this. NOT a charge signal (taper ~13.18
                                       // and parked-rest ~13.19 both sit above it) - purely a safety floor so
                                       // we never confirm-poll a weak/collapsing battery. Current sign decides.
// Charge-end read: when the rail falls below RAIL_TAPER_FLOOR with the latch still up, the charge has most
// likely just ended, and without one more poll ABRP keeps the last CHARGING point until the next drive. FIELD
// (Scenic, 2026-10-09): rail 13.17 -> 12.70 three minutes after a 20kW AC charge, the latch dropped silently,
// and ABRP showed CHARGING at 78% for 5.3h while the car had finished at 80.5%. Not vehicle-specific: any car
// whose rail falls when its charge ends takes this path. One poll per charge, re-armed only by a high-rail
// sample, so a car that charges with its rail under the taper floor is not polled at charging cadence. The
// floor is about the 12V battery, not the car: well above the IO36_LOCKOUT band, so a sagging one is never polled.
#define CHARGE_END_POLL_FLOOR   12.2f
#define TAPER_CONFIRM_MAX_WAKES 8      // give up after this many consecutive SoC-STALLED confirm-polls (no SoC
                                       // gain). A still-rising taper resets the count, so it's NEVER cut off no
                                       // matter how long; this only bounds a stuck sensor / the 100% tail
                                       // (8 x 3-min cadence = ~24 min of flat SoC before giving up).

// Cap frames per bulk POST. FIELD-CALIBRATED (71h dump, 1,091 POSTs): batches of 1-4 frames succeeded
// 1,011/1,017 (99.4%); EVERY batch >=7 frames failed (0/15) with http=0 after ~21.5s - alternating
// ok(3)/FAIL(7)/ok(3)/FAIL(10) in the same modem session, so it's payload size, not signal. The SIM7070's
// HTTP body path can't deliver POST bodies past ~1.2-1.4KB (~200-220B/frame -> cliff between 4 and 7).
// ROOT CAUSE FOUND (code audit): the modem session is configured with AT+SHCONF="BODYLEN",1024 - the
// SIM7070's documented hard maximum - and a body over that limit makes AT+SHBOD reply ERROR; the old send()
// then streamed the JSON into the AT parser anyway, desyncing the modem (the observed http=0 cliff). The
// REAL guard is now the byte cap below; this frame-count cap remains as the batch-size heuristic.
//
// 12, not the 4 it was, because the body is now GZIPPED (see gzip_body.h). Measured on realistic varying
// samples, 1000 compressed bytes holds ~21 points against 5 raw - so at 4 the frame count, not the byte
// cap, would have become the binding limit and the compression would have bought smaller POSTs instead of
// FEWER POSTs, which is the half that costs airtime. 12 is one minute of 5s driving samples in a single
// POST, deliberately short of the measured 21 so a richer profile or noisier values (which deflate less
// well) are absorbed by the byte-cap trim rather than running at the edge.
#define MAX_FRAMES_PER_POST 12

// Hard byte ceiling for one cellular bulk POST body (SHCONF BODYLEN is 1024, the modem's maximum;
// 1000 leaves margin). uploadBuffered() shrinks each batch until its serialized body fits, so richer
// frames (more OBD PIDs) automatically mean fewer frames per POST instead of undeliverable POSTs.
#define CELL_MAX_POST_BYTES 1000
#define GEO_MAX_SEND_AGE_SECONDS (29UL * 24UL * 60UL * 60UL)

// Catch-up burst: how many capped POSTs the STEADY-STATE (in-loop) upload may fire back-to-back in one
// transmission cycle. More SMALL posts, never bigger ones (see the hard per-POST size ceiling above).
// Self-limiting: uploadBuffered stops on the first failed POST, so a dead bearer is never hammered.
//
// A CEILING, not a schedule: uploadBuffered stops the moment the buffer is empty, so a normal driving
// minute fires exactly ONE post (12 samples, one 12-frame batch) whatever this is set to. What it buys is
// recovery, and 1 would buy none - the drain rate would exactly equal the fill rate, so any backlog would
// be permanent. Two ordinary events produce one: a failed POST (uploadBuffered stops on the first
// failure), and a BLE session, during which cellular is suppressed entirely and frames just bank.
//
// 3 clears 36 frames a cycle against 12 arriving, so a five-minute phone session drains in about three
// minutes and a single failed POST is absorbed by the next cycle. Unused budget costs nothing.
#define UPLOAD_MAX_BATCHES_PER_CYCLE 3

// Minimum GNSS speed (km/h) to emit heading (course-over-ground) on the API payload. A LOW "is it moving"
// bar, NOT an accuracy threshold: above it COG is directionally usable (better than no heading, per ABRP);
// below it (standstill/crawl) COG is undefined noise, so we omit heading and ABRP holds the last good value.
// Raise if slow parking-lot maneuvers produce jittery headings; lower to include even slower crawl.
#define HEADING_MIN_SPEED_KMH 3

// GNSS speed (km/h) above which the receiver is believed to be genuinely MOVING. Deliberately higher than
// HEADING_MIN_SPEED_KMH: this one drives real decisions (publish the live fix instead of the frozen parking
// spot, and reset the parking average), so it must sit clearly above the 0-2 km/h speed jitter a stationary
// receiver reports, or a parked car would flip between frozen and jittery live positions. 5 km/h ~ walking
// pace; a car pulling away crosses it within a second or two. Lower it if slow crawls should count as moving.
#define GPS_MOVING_MIN_KMH 5

// How long a "the GNSS says we are moving" verdict keeps unlatching is_parked after the last
// genuinely-new fix. The verdict itself is latched (the streak only resets on a fresh STATIONARY
// fix), so without an age bound it survives forever once fixes stop arriving - drive into a garage
// and the car stays "driving" for the whole wake. That pinned is_parked at 0 and, since is_charging
// is gated on the latch, reported NOT_CHARGING on a car charging at -52 A.
//
// 45s: comfortably longer than the 5s poll cadence and the GNSS 1Hz update, so an ordinary momentary
// dropout under a bridge does not flap the latch, and far shorter than the ACTIVE_TIMER (5 min) that
// then has to elapse before the re-latch actually fires - so the total is ~6 min of no fresh fix
// before a car reads as parked again. A drive whose profile HAS a speed PID is unaffected either
// way: OBD speed refreshes the same leash and never goes stale while the ECUs are awake.
#define GPS_MOVING_MAX_AGE_MS 45000UL

// How many stationary GNSS samples must be averaged before the parking freeze prefers the average over a
// single instantaneous fix. At the 5s ACTIVE poll cadence 12 samples ~= 1 minute; the car normally sits still
// ~5 min before latching parked, so ~60 samples are typically available. FIELD EVIDENCE for why this exists:
// single-sample freezes at one repeatedly-used parking spot scattered up to 22.4 m apart (individual samples
// up to 14.9 m from their own mean) even though fix quality was good (HDOP 0.47-0.69, 12 sats) - the error
// came from using one sample, not from poor reception. Averaging N samples cuts random error by ~sqrt(N).
#define PARK_ACC_MIN 12

// Distance (m) between a parked wake's stationary average (PARK_ACC_MIN fresh fixes or more) and the frozen spot
// beyond which the frozen spot moves there. Only a drive the GNSS never saw at GPS_MOVING_MIN_KMH needs it: no
// park transition happens, so nothing re-freezes. FIELD (Ioniq 5, week of 2026-10-15): GNSS took 2.5-31 min to
// lock, so 6 of 12 drives ended before any moving fix and the car kept reporting the previous spot; on 2026-10-17 it
// locked on arrival with 12 satellites, which this uses. Far outside one spot's 22 m scatter and a
// garage's multipath, so a parked car never wanders; any real trip is far beyond it.
#define PARK_RELOCATE_MIN_M 500
// Relocation evidence must be a good fix: HDOP at most this, in the co-processor's x100 units (12-satellite
// parking fixes log 47-69). Moving fixes on the 2026-10-19 drives logged 66-209 with 4-12 satellites, so 250
// keeps every real one and rejects multipath. Cold-start first fixes logged LOW values (1, 25 at 5-6 sats), so
// HDOP alone cannot reject those; the cluster bound below and PARK_ACC_MIN do.
#define PARK_RELOCATE_MAX_HDOP 250
// ...and one tight cluster: a fix this far from the window's first sample restarts it. A stopped receiver
// scatters under ~15 m from its mean; a 4 km/h crawl leaves 50 m in 9 polls, before PARK_ACC_MIN fills.
#define PARK_RELOCATE_MAX_SPREAD_M 50

// The GNSS jump filter's tunables (GPS_JUMP_*: margin, max speed, re-anchor run) live with gpsGetData in
// libraries/FreematicsPlus/FreematicsPlus.cpp, which cannot include this file.

// ---- IO36_BatDec true-rail voltage (12V sense ADC) ----
// ATRV (batt12v) is the OBD ADAPTER's reported voltage - it reads ~5.3V on a sleeping Scenic, which may be
// an adapter artifact rather than the true 12V rail. GPIO36 (IO36_BatDec) sits on a resistive divider off
// the real rail (downstream of D5), so reading it gives the ACTUAL voltage. Logged side-by-side with ATRV
// in every row AND every event (events matter because an enforce-mode SKIP sleeps before any row is written).
// This is what disambiguates the brownout question: at a low-voltage wake, does the TRUE rail read ~12.5V
// (port stays powered, no brownout) or also ~5V (real brownout)? A ~7V gap swamps any ADC error, so even
// UNCALIBRATED the relative answer is trustworthy on the first run. Ground-truth against a multimeter once
// for absolute accuracy. Divider 100k/10k -> rail = pin x 11. analogReadMilliVolts() applies eFuse cal.
#ifndef ENABLE_IO36_RAIL
#define ENABLE_IO36_RAIL 1
#endif
#define IO36_RAIL_PIN 36         // GPIO36 / ADC1_CH0 (SENSOR_VP) = IO36_BatDec
#define IO36_RAIL_DIVIDER 11.0f  // 100k/10k divider: 12V -> ~1.09V at the pin; rail = pin_voltage x 11
#define IO36_RAIL_SAMPLES 12     // average this many ADC samples per reading to tame ESP32 ADC noise
// Multimeter calibration: true = A*raw + B (12 pts 12.4-15V, residual avg 0.008V / max 0.021V). NOTE: cal
// points span 12.4-15V only; readings BELOW 12.4V are EXTRAPOLATED (trust ~+-0.1V there vs ~+-0.02V in-range)
// - add 11-12V points later to anchor the lockout region. The lockout band below is in CALIBRATED volts.
#define IO36_RAIL_CAL_A  0.96327f
#define IO36_RAIL_CAL_B  1.19591f

// Low-rail lockout BAND (using the true rail, not ATRV). If the rail sits in [LOW, HIGH) we treat the
// battery as weak-but-alive and SUPPRESS all polling (incl. cold boot + heartbeat) so we don't push a
// recoverable 12V over the edge - the original dead-overnight scenario. The car recovers the moment it's
// driven (rail jumps to ~14V charging). Field data: healthy rest ~12.1V, charging ~14V, bench/USB ~4.5V.
//   HIGH 11.0: clearly below the 12.1V healthy rest (with ADC margin) -> no false lockout on a good battery.
//   LOW  8.0 : ABOVE bench/no-car (~4.5V), so plugging into USB to DUMP is NOT locked out (rail < LOW ->
//              "no car / bench", allowed to run normally; a dead car's OBD won't answer anyway, no harm).
#define IO36_LOCKOUT_LOW  8.0f
#define IO36_LOCKOUT_HIGH 11.0f

// OBD-ALARM GATE (IGNITION_SENSE_ARCHITECTURE.md §8/§10). Some vehicles (BMW i3 field-confirmed)
// sound their anti-theft alarm when a device transmits on the OBD bus while the car is locked and
// armed. When the vehicle is flagged (per-vehicle, written by the app's save-screen toggle over
// ABD2, persisted in NVS), polling requires a PERMIT: rail energised AND GNSS confirmed-moving
// (driving), or an active setup-session window (a completed full profile push - a human at the
// car). Stationary charge sampling is suppressed on flagged cars - a parked charge session is
// locked and armed, and the 3-5 min cadence would trip the alarm every cycle. Unflagged vehicles
// are byte-identical to stock (the gate is a single early return). See Helpers/alarm_gate.h.
#ifndef OBD_ALARM_GATE
#define OBD_ALARM_GATE 1
#endif
// There is no OBD_ALARM_ENFORCE bypass any more. The per-vehicle NVS flag IS the switch: unset or 0
// means poll freely (the default, and what most cars want), 1 means ration bus traffic to avoid
// tripping that car's alarm. The app's connection-details toggle is what writes it.
// The compile-time bypass existed while three enforcement-breaking bugs were open. The two that made
// an opted-in car unusable are fixed - the ABD2 re-AUTH misparse and the GNSS one-shot (Appendix J
// root causes 1 and 3). What remains is documented there and affects only an opted-in vehicle:
// radio silence on suppressed timer wakes (RC2), the per-vehicle rail margin (RC4), and a GNSS
// freshness bound that is inert on the link path.
// How long a completed full profile push keeps the setup-session permit alive. Long enough for the
// wizard's PID test + save + a re-run; short enough that a phone left in the car does not hold a
// parked car pollable for long. Only matters on flagged vehicles.
#define ALARM_SETUP_PERMIT_MINUTES 10

// The setup permit ALSO requires the phone to still be around. Without this, finishing the wizard,
// switching off, locking and walking away left the car pollable for the rest of the window - which
// re-creates the exact locked-and-armed scenario the gate exists to prevent. The phone leaving IS
// "the human left". Grace, not an instant revoke: field logs show BLE flaps reconnecting 0-3s after
// a drop, and killing the permit on a flap would break the wizard's live PID test mid-session.
#define ALARM_PERMIT_BLE_GRACE_MS 30000UL

// SPEED-HOLD (tunnel continuation). GNSS is the only way to ENTER a driving permit, but it dies in
// tunnels and multi-storey car parks. A fresh OBD road speed at or above this threshold is
// independent proof the car is in transit, so it sustains an already-granted permit across a GNSS
// outage. Well above parking-manoeuvre speed on purpose: at 25 km/h, decelerating, parking,
// switching off, getting out and locking cannot happen inside the hold below, so the hold can never
// still be alive once the car is armed. Lower speeds deliberately fall back to "wait for GNSS".
#define ALARM_SPEED_HOLD_KMH 25.0f

// How long one qualifying speed reading sustains the permit. Must comfortably exceed the poll
// cadence (5s driving) so a couple of unanswered polls don't drop it mid-tunnel, and stay far below
// the time it takes to stop and arm a car. The rail term still applies on top: switching off
// collapses the rail and denies immediately, without waiting for this to expire.
#define ALARM_SPEED_HOLD_MS 30000UL

// Maximum age of the GNSS fix backing a "moving" permit. The movement streak only resets when a
// FRESH stationary fix arrives, so when fixes stop coming (covered/underground parking) it stays
// latched true indefinitely. Harmless for the is_parked latch and the awake leash - both fail open
// and the latch has its own time-based re-latch - but as a permit input it fails the wrong way:
// drive into a garage, park, switch off (rail collapses, correctly denied), then plug in and the
// returning DC-DC rail would meet a still-latched "moving" and permit polling on a locked, armed
// car. Bounding the age turns latched movement into vouched-for movement. Generous enough that a
// bridge or a brief dropout does not drop the permit; far shorter than the ~5 min awake window that
// creates the exposure. Longer outages (tunnels) are covered by the speed-hold above instead.
#define ALARM_GNSS_MAX_AGE_MS 10000UL

// How long the rail must stay sub-threshold before the stored speed-hold is VOIDED. Distinct from
// the instant deny, which still fires on any single sub-threshold read: denying is safe and
// self-correcting, whereas voiding is destructive because the hold can only be re-armed by a
// completed poll, which needs a permit. Without this, one ADC dip mid-tunnel (measured ~0.16%/read,
// ~1.1%/cycle, ~25% over two minutes) would void the hold with no GNSS to fall back on and lock
// polling out for the rest of the tunnel. NOT the threshold hysteresis rejected earlier -
// RAIL_AWAKE_VOLTS is untouched; this is a confirmation time on a different decision, and it
// cleanly separates a one-sample dip from the park/switch-off/plug-in sequence (tens of seconds).
#define ALARM_RAIL_COLLAPSE_MS 3000UL

// How often alarmGateAllowsPoll() re-logs its per-term values while DENYING. It used to log once per
// wake plus transitions, so an all-deny drive left a single line and no way to see which term was
// failing over time - and no way to spot a car whose driving rail sits just under RAIL_AWAKE_VOLTS
// (the per-vehicle margin, Appendix J root cause 4). Two minutes is frequent enough to profile a
// drive and rare enough not to bloat the flash log; only fires while denied.
#define ALARM_DENY_RELOG_MS 120000UL


#ifndef ENABLE_NETWORK
#define ENABLE_NETWORK ENABLE_CELL || ENABLE_WIFI
#endif

#ifndef FILE_TEST
#define FILE_TEST 0
#endif

#ifndef NETWORK_TEST
#define NETWORK_TEST 0
#endif

// FIELD/SHIP values. Idle-to-sleep is ~5 min of no motion AND no reported speed - NOT the sum of the two
// timers. Both transitions are measured against the SAME `last_active` stamp (refreshed only by motion or
// reported speed), so by the time ACTIVE -> CHARGING fires at last_active+5min, CHARGING's own exit test
// (now - last_active > CHARGING_TIMER) is ALREADY true and STANDBY -> deep sleep follows within one or two
// loop iterations. Bench-confirmed: the device sleeps ~5 min after motion stops.
// Deliberately NOT "corrected" by raising CHARGING_TIMER to 10 min: that would double the post-drive awake
// tail - the one part of a drive the 12V battery actually pays for - for no functional gain, since parked
// charge monitoring runs on the rail-awake sample-poke path, not on the CHARGING state (which a parked car
// traverses instantly anyway).
// Long enough that a drawbridge / drive-through / long red light doesn't fragment the trip (the
// speed>0 leash covers normal stops; WOM self-heals if we do sleep early), short enough that a parked car
// stops being polled quickly. Test values were 2+1 min.
#define ACTIVE_TIMER 5*60*1000   // 5 min: driving/idle stage (speed>0 or accel refreshes this leash)
#define CHARGING_TIMER 5*60*1000 // 5 min: second idle stage before STANDBY -> deep sleep

// VESTIGIAL - no longer referenced. Setup now makes exactly ONE init attempt (~35s worst case, inherently
// bounded) and continues offline on failure; the loop's cheap CGREG reconnect probe re-attaches mid-drive.
// History kept for context: init(retry=true) once looped 2.4 HOURS at a no-service spot (boot 147); a 90s
// retry window then still burned ~93s of drive start before the first poll (the 41%-capture windows).
#define NET_INIT_TIMEOUT_MS 90000UL

// Task-watchdog wedge guard (the brick preventer). Boot 153: the device wedged INSIDE modem init - main
// loop blocked so hard that already-advertising BLE couldn't accept a connection - and sat there 52+ min
// until a human unplugged it. No cooperative check can fire when the loop itself is blocked, so this is
// enforced by hardware: armed first thing in setup(), fed only from loop() (and between bounded NET init
// attempts), panic=true -> ANY future infinite block ends in an automatic reset within this window instead
// of a dead dongle. Sized well above every legitimate block (worst POST timeout ~22s, healthy setup ~30s,
// bounded NET init 90s). The serial DUMP unsubscribes while streaming (a full log can exceed this).
#define WEDGE_WDT_TIMEOUT_S 300

// Max-awake backstop (glitch guard for the awake latch, mirrors WAKE_CHARGING_MAX_MINUTES). The awake window
// is held by the motion+speed leash; if something keeps it awake past this while the car is actually PARKED
// (accel noise refreshing the leash, a wedged state, a connected-but-idle phone), force deep sleep so the 12V
// can't drain. Gated on is_parked, so a genuinely long DRIVE (speed keeps us unlatched) is NEVER cut off; a
// moving car re-wakes via WOM within a second. Raise if legit parked BLE sessions get cut; lower for a tighter
// guard. This is the safety net behind the whole awake-latch/BLE design.
#define MAX_AWAKE_MINUTES 60

#define API_KEY "e23e7564-b703-4ea5-b91b-de85e27717ee"

/*
  ======================================================================================================
  BLE firmware OTA (see ota-update-architecture-v4.md)
  ======================================================================================================
  Staged dual-slot update over BLE: the app streams a signed image to the INACTIVE app partition
  (ota_0/ota_1, 6.5MB each - the running image is never touched), the dongle verifies it, sets the boot
  pointer, reboots, and the native bootloader auto-rolls-back if the trial boot doesn't mark itself valid.

  SAFETY MODEL - the device can ONLY enter the update state when the app commands it over BLE:
    - entered solely by a START command on ABD5 from a CONNECTED + AUTHENTICATED phone,
    - all state is RAM-only (never RTC/NVS), so ANY reset clears it - it cannot get stuck across a reboot,
    - bounded by an inactivity timeout AND an absolute session cap, so a vanished app can't wedge it,
    - a BLE disconnect keeps the session alive only for a short resume window, then aborts cleanly.
*/
#ifndef ENABLE_OTA
#define ENABLE_OTA 1
#endif

// Monotonic firmware version, injected by tools/pio_version.py: X.Y.Z packs to major*1000000 + minor*1000 + patch
// (tools/fw_version.py). A release build gets the version the release workflow chose; any other build gets the
// newest v* tag reachable from HEAD. Two jobs: (1) anti-downgrade - a START whose version is BELOW the running
// version is refused (equal IS allowed, so a bench re-flash of the same build works); (2) rollback detection -
// it's recorded in NVS before the commit reboot, so if the bootloader reverts, the old firmware sees "attempted
// version != my version" and can report ROLLED_BACK to the app. Pre-semver releases were plain integers up to 7.
// The fallback only covers compiles outside PlatformIO (host tests); 0 is below every real release.
#ifndef OTA_FW_VERSION
#define OTA_FW_VERSION 0
#endif

// Build identity banner, injected alongside OTA_FW_VERSION: "v1.2.0" for a release, "v1.2.0+3.gabc1234.dirty"
// for a build 3 commits past it with local changes. Watch for it on the serial monitor after the dongle reboots:
// seeing the expected tag is positive proof the new image is the one now running (and that it survived its trial
// boot). Also written to the flash log as an EVT and exposed in the ABD5 info JSON, so a DUMP tells you which
// firmware produced which rows. __DATE__/__TIME__ are appended to the banner automatically.
// NOTE: the timestamp makes builds non-byte-reproducible. That only matters if delta/patch OTA is adopted
// later (a patch's base image must match exactly) - irrelevant for full-image updates.
#ifndef FW_BUILD_TAG
#define FW_BUILD_TAG "unversioned"
#endif

// Require a valid ECDSA-P256 signature before an update is allowed to commit. THIS IS THE TRUST BOUNDARY:
// with it enabled, neither a tampered GitHub release, a malicious app, nor anyone speaking BLE to the dongle
// can install code that wasn't signed with your private key - the transport does not have to be trusted.
// Set to 0 ONLY for a first bench test of the raw transfer, before the signing pipeline is set up. With it 1
// and no key installed, START is refused with a loud reason rather than silently accepting unsigned images.
#ifndef OTA_REQUIRE_SIGNATURE
#define OTA_REQUIRE_SIGNATURE 1
#endif

// Flash sector = the unit of transfer and of one esp_ota_write(). 4096 matches the flash page size, so a
// sector write is a small bounded operation (~4 page programs) that can't stall the BLE core for long.
#define OTA_SECTOR_SIZE 4096

// Reject implausible images before erasing anything. The floor is the real guard against a truncated or
// 0-byte download reporting false success (the app-side case where an empty .bin "succeeded"); the ceiling
// is the OTA slot size. Current image is ~1.72MB.
#define OTA_MIN_IMAGE_BYTES (256UL * 1024UL)
#define OTA_MAX_IMAGE_BYTES (6UL * 1024UL * 1024UL)

// No data packet for this long -> abort and release every hold. THE anti-stuck guard for an app that
// crashes or walks out of range mid-transfer.
#define OTA_INACTIVITY_MS 30000UL

// Absolute ceiling on one session regardless of activity, so a pathological/malicious client can't hold the
// device awake indefinitely. A healthy 1.72MB transfer is ~1.5-5 min.
#define OTA_SESSION_MAX_MS (15UL * 60UL * 1000UL)

// Resume window: on a BLE drop mid-transfer the session (and the esp_ota handle, so no re-erase) is kept
// this long. A reconnect + identical START resumes from the last acknowledged sector instead of restarting
// a multi-minute upload. NOTE: resume is WITHIN one boot only - the esp_ota handle lives in RAM, so a device
// reboot always requires a fresh transfer.
#define OTA_RESUME_GRACE_MS 90000UL

// Refuse to start without this much largest-contiguous-block free. Field-observed worst maxblock is ~45KB
// with BLE up, so 20KB is a floor that catches a genuinely fragmented heap, not normal operation.
#define OTA_MIN_MAXBLOCK 20480UL

// Power gate. Mirrors the ATRV gate's THREE-BAND rail logic rather than a single floor, which is what makes
// bench testing possible: rail < IO36_LOCKOUT_LOW (8.0) = bench/USB (~5.6V measured) -> ALLOWED;
// rail >= OTA_RAIL_HEALTHY = a real, healthy car battery -> ALLOWED; in between = weak-but-alive battery ->
// REFUSED, because flash-write current spikes on a sagging 12V can trip the brownout detector mid-write.
#define OTA_RAIL_HEALTHY 12.2f

/*
  Testing
*/
// #define ACTIVE_TIMER 10*1000 //10 seconds
// #define CHARGING_TIMER 30*1000 //30 seconds

#endif