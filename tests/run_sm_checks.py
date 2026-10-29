"""Differential checks for the park/drive decision, the poll-side energy block and the setup() wake gate (state
machine #23, steps 0a #28, 0b-1 #81 and 0b-2 #82).

Three regions are compared. The drive region is drive_state.h and the blocks that call it: the speed loop, the
GNSS branch and the time re-latch, the is_parked publish, the OTA/dump hold, changeState, the frame_has_data
buffer block with the queue helper, the pre-sleep marker and the setup() marker restore. The energy region is
the poll's rail refresh and the contiguous block in loop() from the is_charging verdict through dcAwakeTrack():
the release latch and silent hold, the taper stall count, the DC session, the next-wake cadence, the fast budget
and cap, corroboration and the stale guard, with the helpers it calls (resetChargeCorroboration, pdwActive,
chargeStateStale, the dcAwake* functions). The wake region is setup()'s RTC sanitize and the whole ATRV gate block:
the ATRV read, the taper bridge and its give-up, the charge-end read, the follow-up, the poll decision, the alarm
suppression, the counters and log lines, the weak-rail cold boot and the skip path. All are extracted with the same
anchors from two places: OLD from the pinned commit (git show), CUR from the working tree. The context around them
(the post-drive window at the ladder end, a sample poke's sleep after its poll, setup()'s last-poll stamp, the
weak-rail window's expiry and the recovery ahead of it, every sleep's modem override for that wake, loop()'s poll
condition that decides whether a poll asks the car, the backstop, the delays) comes from the working tree for both
sides, so a deliberate change there cannot fail the differential; a property checks that the shared backstop never
sleeps a driving car.
tests/sm_checks.cpp compiles each side into its own namespace around the same harness
(tests/sm/adapter_*.inc) and runs identical event sequences through both, asserting equal outputs after
every event: the latch, the leash and hold times, the marker debt and its crash copy, published is_parked,
buffered frames, alarm speed notes, freezes, every energy and gate RTC field and RAM output, the next-wake
minutes, and the exact logEvent and Serial sequences; the gate's poll or skip shows in its counters. It also checks
the weak-rail gate, the weak-rail cold boot's modem-off sleep, the max-awake backstop and the fast-cadence budget
(#83) as properties.
The OBD-alarm permit (#83) is alarm_gate.h from the setup grant through alarmGateAllowsPoll(), pinned for OLD; it is
the last term of loop()'s poll condition (from the tree, after the weak-rail veto), and this runner fails unless loop()
asks the car (readPIDs and the deferred obd.initialize) only inside that condition, compiled in the default build,
after the cycle's rail and GNSS refresh. On an enforced car a poll asks
the car (obd.tx_count moves) exactly when an oracle kept from the trace's inputs permits it: the setup window with an
authenticated phone or within ALARM_PERMIT_BLE_GRACE_MS of one, or the rail awake and a moving fix no older than ALARM_GNSS_MAX_AGE_MS or a speed note within
ALARM_SPEED_HOLD_MS, a sustained rail collapse voiding the note. Every DENY line carries tx=, which never moves
between two of them; only the gated branch moves the counter here, so that checks the transition logging. Groups:
enumerate (exhaustive over a drive, an energy and a wake alphabet, --depth, default 5), random (fixed seeds), scenarios (named field and charge cases), crash (CUR alone: a panic,
also inside the pre-sleep flush, never loses the marker debt; an absolute property, not a comparison), rules
(CUR alone: absolute checks of the #6 driving rules), or all. A Charge or Silent poll keeps the wake's rail and
motion; any answered poll stamps the last OBD answer. A wake carries the rail, the ATRV read, the alarm flag and an
optional charge latch; the RTC clock and epoch run through sleeps, and a cold boot zeroes the clock until a poll.
Before compiling, it checks that only drive_state.h assigns the park/drive storage, that alarm_gate.h names no
drive state, and that every drive call is in an extracted block. A poll reaches the car only when loop()'s poll
condition holds (around it a profile is always loaded and the per-cycle alarm permit passes), so the property "a
poll ran on a weak-rail wake" holds a weak-rail cold boot to the firmware's own veto (#104); it also checks that the
condition has the veto as a term and guards the lazy init and the bus read, that every obd.readPIDs( and
sys.gpsBegin() in logger.ino sits behind it (dcPromotePoke's and FILE_TEST's excepted), that dcPromotePoke still
needs a sample poke, that setup()'s one boot clock resync skips that wake, and that the weak-rail recovery (a cycle's
rail at RAIL_AWAKE_VOLTS ends the wake, held by a phone only inside the key-reset window, whose production
predicate is shared context) sits under #if ENABLE_IO36_RAIL directly ahead of the window's expiry. A phone
exists only where a scenario connects one (sm_ble_connected, read through bleConnected(), dropped at every sleep).

The pin. SM_PIN is the main commit a step is measured against: a refactor step must leave both regions equal to
it. Re-pin only when a behaviour change to a compared region lands on main between steps, to the merge-base of
the step being measured; it is a commit on main, so it stays reachable. A clone without it fails with a message,
never a skip.

What it cannot prove: 32-bit millis() wrap (the host unsigned long is 64-bit); any feature-flag set other
than the config.h defaults with PRINT 0 (lines under #if PRINT and other branches are not compiled); code
outside the extracted blocks (the rest of handleActivityState, dcPromotePoke and motion inside a wake, the
SAMPLE_POLL line, the modem, detectMovement timing, RTC retention, hardware); the wdt-streak sleeps before and after
the gate; resets other than a deep-sleep wake, a power loss, a panic and a WDT (a brownout or software reset reloads
RTC as those do, so the sanitize cannot show); the heartbeat (ENABLE_PARKED_HEARTBEAT 0, so the branch and
heartbeat_asleep_streak are not compiled); the per-PID alarm guard inside readPIDs (alarmPollGuard), the deferred
OBD init and a missing profile (g_no_profile stays false); the loop's own phone-presence stamp (alarmGateService), so
the setup permit's grace runs from the last permit read that saw the phone; bleRelaying(), which the pin asked and a
phone with no relay session cannot tell from bleAuthenticated(); since_poll at the target's 32-bit width; GNSS power (a
poll's GNSS input is the event's, also on a weak-rail cold boot, whose GNSS the firmware never powers: that is checked
by position only), the boot profile restore and a profile push (tests/run_profile_checks.py). The multi-region layer
is #83.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

SM_PIN = "d146449b7b6a7790dfa964a0966673349766aa98"

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests/support"))
from extract import block, find  # noqa: E402
if subprocess.run(["git", "merge-base", "--is-ancestor", SM_PIN, "HEAD"], cwd=root).returncode != 0:
    sys.exit(f"FAIL: SM_PIN {SM_PIN} is not an ancestor of HEAD: fetch main, or re-pin (see this file's header)")


def pinned(path):
    return subprocess.check_output(["git", "show", f"{SM_PIN}:{path}"], cwd=root, text=True)


def tree(path):
    return (root / path).read_text()


def line(text, prefix, what):
    found = [l for l in text.splitlines() if l.startswith(prefix)]
    if len(found) != 1:
        sys.exit(f"FAIL: expected one line starting {prefix!r} in {what}, found {len(found)}")
    return found[0] + "\n"


PATHS = {"logger": "baseLogger/logger.ino", "machine": "baseLogger/Helpers/state_machine.h",
         "sleep": "baseLogger/Helpers/sleep_power.h", "upload": "baseLogger/Helpers/network_upload.h",
         "logging": "baseLogger/Helpers/logging.h", "drive_state": "baseLogger/Helpers/drive_state.h",
         "motion_stats": "baseLogger/Helpers/motion_stats.h", "config": "baseLogger/config.h",
         "alarm": "baseLogger/Helpers/alarm_gate.h"}


# Code anchors only (never comment text), each failing with its own text when the code it names has moved.
DRIVE_START = ('    for (const auto& res : current_frame) {\n'
               '      if (( res.first == "speed" || res.first == "vehicle_reported_speed" )')
DRIVE_END = "#if ENABLE_NETWORK\n    const double poll_utc = uploadSendUtc();"
# The poll-side energy block (#81): the charge verdict, the release latch and silent hold, the taper stall count,
# the DC session, the next-wake cadence, the fast budget and cap, corroboration and the stale guard.
ENERGY_START = "    // Feature A - is_charging"
ENERGY_END = "\n#if ENABLE_BLE\n    const uint16_t ble_mtu"
ENERGY_RAIL = "    g_last_io36 = readIO36Rail();\n    // A poke held by a DC charge"
LOGGED_DCFC = "        static int logged_dcfc = -1;"
# The setup() wake gate (#82): the ATRV read, the taper bridge and its give-up, the charge-end read, the poll decision,
# the counters and log lines, the weak-rail cold boot and the skip path, as one brace-matched block.
WAKE_GATE = "  {\n    g_last_voltage = obd.getVoltage();\n"
WEAK_EXPIRY = "    if (g_weak_rail_cold_boot && !maintenance_mode && !bleConnected() &&"
WEAK_RECOVERED = ("    if (g_weak_rail_cold_boot && !maintenance_mode && g_last_io36 >= RAIL_AWAKE_VOLTS && "
                  "!(bleConnected() && authResetWindowOpen())) {")
BOOT_RESYNC = '  if (!g_weak_rail_cold_boot && clockNeedsSync() && !clockResync("boot")) clockNtpNoClock();'
# loop()'s poll condition (the OBD_ALARM_GATE branch, the one config.h builds): whether a poll cycle asks the car.
POLL_IF = "    if (!g_no_profile && "
POLL_VETO = "!g_weak_rail_cold_boot"
POKE_SLEEP = "    if (g_sample_poke && !maintenance_mode && !dcAwakeHeld(current_state)) {"
# The OBD-alarm permit (#83): alarm_gate.h from the setup grant through alarmGateAllowsPoll(), its function-local
# statics hoisted (adapter_pre.inc SM_LOCAL) and the speed note renamed, so the adapter's stub can count it first.
ALARM_START = "void alarmGrantSetupPermit() {"
ALARM_END = "\n/* Loop-task service:"
ALARM_STATICS = ("  static bool logged_this_wake = false;\n  static bool last_permit = false;\n"
                 "  static unsigned long last_deny_log_ms = 0;\n")
ALARM_NOTE = "void alarmNoteSpeed(double kmh) {"


def recipe(src, side):
    logger, machine = src["logger"], src["machine"]
    where = {k: f"{PATHS[k]} ({side})" for k in PATHS}
    start = find(logger, DRIVE_START, what=where["logger"])
    upload_start = find(src["upload"], "bool isSyntheticParkMarker(", what=where["upload"])
    upload_end = find(src["upload"], "\n#if ENABLE_NETWORK", upload_start, where["upload"])
    energy_start = find(logger, ENERGY_START, what=where["logger"])
    energy = logger[energy_start:find(logger, ENERGY_END, energy_start, where["logger"])]
    # Function-local, so a boot resets it (adapter_pre.inc SM_LOCAL), as a deep-sleep wake does.
    if energy.count(LOGGED_DCFC) != 1:
        sys.exit(f"FAIL: expected one {LOGGED_DCFC.strip()!r} in the energy block ({side})")
    rail = find(logger, ENERGY_RAIL, what=where["logger"])
    dc_start = find(machine, "bool dcMotionRecent() {", what=where["machine"])
    gate = find(logger, WAKE_GATE, what=where["logger"])
    under = logger.rfind("#if ENABLE_ATRV_GATE\n", 0, gate)
    if logger.count(WAKE_GATE) != 1 or under < 0 or re.search(r"^\s*#", logger[under + 1:gate], re.M):
        sys.exit(f"FAIL: expected one wake gate block, under #if ENABLE_ATRV_GATE with no directive between ({side})")
    for header in ("motion_stats", "drive_state"):
        if src[header].count("#pragma once\n") != 1 or '#include "' in src[header]:
            sys.exit(f"FAIL: {where[header]} must be one #pragma once and no project include, to inline it")
    alarm_start = find(src["alarm"], ALARM_START, what=where["alarm"])
    alarm = src["alarm"][alarm_start:find(src["alarm"], ALARM_END, alarm_start, where["alarm"]) + 1]
    if alarm.count(ALARM_STATICS) != 1 or alarm.count(ALARM_NOTE) != 1:
        sys.exit(f"FAIL: expected alarmGateAllowsPoll's three statics together and one alarmNoteSpeed in {where['alarm']}")
    return {
        "alarm": alarm.replace(ALARM_STATICS, "").replace(ALARM_NOTE, "void alarmNoteSpeedBody(double kmh) {"),
        # Inlined, never #included: #pragma once would hand OLD the tree's copy.
        "drive_state": src["drive_state"].replace("#pragma once\n", ""),
        "motion_stats": src["motion_stats"].replace("#pragma once\n", ""),
        "enum": block(logger, "enum ActivityState {", where["logger"]).rstrip() + ";\n",
        "buffer_cap": line(logger, "const size_t MAX_UPLOAD_BUFFER = ", where["logger"]),
        "state_name": block(src["logging"], "static const char* stateName(ActivityState s) {", where["logging"]),
        "delays": block(machine, "unsigned long getTransmissionDelay(", where["machine"]) +
                  block(machine, "unsigned long getPollingDelay(", where["machine"]),
        "change_state": block(machine, "void changeState(ActivityState new_state) {", where["machine"]),
        "drive": logger[start:find(logger, DRIVE_END, start, where["logger"])],
        "emplace": line(logger, '    if (!drive_unconfirmed) current_frame.emplace_back("is_parked"', where["logger"]),
        "buffer": block(logger, "    if (frame_has_data) {", where["logger"]),
        "marker": block(src["sleep"], "  if (rtc.getEpoch() > 1e9 && is_currently_parked", where["sleep"]),
        "session": block(machine, "if (g_ota_active || g_logdump_active) {", where["machine"]),
        "backstop": block(machine, "if (!maintenance_mode && !g_ota_active && !g_logdump_active", where["machine"]),
        "wake_gate": block(logger, WAKE_GATE, where["logger"]),
        "weak_awake_ms": line(logger, "static const uint32_t WEAK_RAIL_COLD_BOOT_AWAKE_MS = ", where["logger"]),
        "queue_helper": src["upload"][upload_start:upload_end],
        "energy_rail": logger[rail:find(logger, "\n#endif", rail, where["logger"]) + 1],
        "energy": energy.replace(LOGGED_DCFC, ""),
        "energy_helpers": block(logger, "void resetChargeCorroboration() {", where["logger"]) +
                          line(logger, "static const uint64_t POST_DRIVE_WINDOW_US = ", where["logger"]) +
                          block(logger, "bool pdwActive(uint64_t now_us) {", where["logger"]) +
                          block(logger, "bool chargeStateStale(uint64_t now_us) {", where["logger"]) +
                          machine[dc_start:find(machine, "\n// Open the post-drive window", dc_start, where["machine"]) + 1],
        "sanitize": block(logger, "void chargeWakeStateSanitize(bool deep_sleep) {", where["logger"]),
        "ladder_end": machine[find(machine, "bool pdwOpenWindow() {", what=where["machine"]):
                              find(machine, "\n/*\n  Use movement and BLE", what=where["machine"]) + 1],
    }


SIDE = """{enum}
{buffer_cap}
{weak_awake_ms}
#include "adapter_pre.inc"
{alarm}
{motion_stats}
{drive_state}
{motion_inputs}
{queue_helper}
{state_name}
{delays}
{change_state}
void sm_drive_poll(OBDVector& current_frame) {{
{drive}}}
void sm_emplace(OBDVector& current_frame) {{
{emplace}}}
{energy_helpers}
{sanitize}
{ladder_end}
void sm_energy_rail() {{
{energy_rail}}}
void sm_energy(OBDVector& current_frame, int obd_count) {{
{energy}}}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"  // the gate's `now > 1000000000UL` on the host's signed time_t
void sm_wake_gate(SmWakeCause wake_cause) {{
{wake_gate}}}
#pragma GCC diagnostic pop
void sm_stamp() {{
{stamp}}}
{auth_window}void sm_weak_recovered() {{
#if ENABLE_IO36_RAIL
{weak_recovered}#endif
}}
void sm_weak_expiry() {{
{weak_expiry}}}
void sm_modem_override(bool& modem_on) {{
{modem_override}}}
bool sm_poke_sleeps() {{ return {poke_sleep}; }}
bool sm_poll_reaches_car() {{ return {poll_condition}; }}
void sm_buffer(OBDVector& current_frame, bool frame_has_data) {{
  int g_geo_queued = 0, poll_geo = 0;
  (void)g_geo_queued;
  const bool ble_relayed = false; // no phone in this harness; OLD ignores both
  const uint32_t relay_seq = 0; // nor relay acks (#90, tests/run_relay_checks.py); OLD has neither
  const char* const RELAY_SEQ_KEY = "__relay_seq";
  (void)relay_seq; (void)RELAY_SEQ_KEY;
{buffer}}}
void sm_marker() {{
{marker}}}
void sm_session() {{
{session}}}
void sm_backstop() {{
{backstop}}}
void sm_restore() {{
{restore}}}
void sm_marker_set() {{
{marker_set}}}
void sm_marker_clear() {{
{marker_clear}}}
#include "adapter_post.inc"
"""

# The rolling accumulator the drive rules read. A poll that says `sustained` holds 20 s of motion windows ending now
# (every window active, exactly the minimum sampled time).
MOTION_INPUTS = """DriveMotion g_drive_motion;
void sm_motion_inputs(const SmEvent& e) {
  g_drive_motion = DriveMotion{};
  if (!e.sustained) return;
  if (sm_now < 20000) { fprintf(stderr, "harness: advance the clock 20 s before a sustained poll\\n"); abort(); }
  for (unsigned i = 0; i < 40; ++i) motionAccumulate(g_drive_motion, true, true, 500, sm_now - 19500 + 500UL * i);
}"""

old_src = {k: pinned(p) for k, p in PATHS.items()}
cur_src = {k: tree(p) for k, p in PATHS.items()}
pin, cur = recipe(old_src, "SM_PIN"), recipe(cur_src, "tree")

# The crash copy of the marker debt (#49, Helpers/wdt_streak.h): the setup() restore, the set after the buffer
# block and the clear at sleep. Context from the tree on both sides; each must sit where a crash cannot slip
# between it and what it guards.
logger, sleep = cur_src["logger"], cur_src["sleep"]
advance = line(logger, "  wdt_reset_count = wdtStreakAdvance(", PATHS["logger"])
RESTORE = "  if (esp_reset_reason() != ESP_RST_DEEPSLEEP && wdtStreakMarkerOwed())"
cur["restore"] = line(logger, RESTORE, PATHS["logger"])
pin["restore"] = line(old_src["logger"], RESTORE, f"{PATHS['logger']} (SM_PIN)")
if advance + cur["restore"] not in logger:
    sys.exit("FAIL: the marker restore must directly follow the streak advance in setup(), ahead of every sleep")
cur["poke_sleep"] = line(logger, POKE_SLEEP, PATHS["logger"]).strip()[len("if ("):-len(") {")]
cur["marker_set"] = line(logger, "    if (park_marker_owed) wdtStreakMarker(true);", PATHS["logger"])
# Around the gate, from the tree for both sides: setup()'s last-poll stamp once the network is up, the weak-rail cold
# boot's window expiry (inside the poll block, after the poll and before the poke's sleep) and every sleep's modem
# override for that wake.
cur["stamp"] = line(logger, "  if (clockValid()) last_poll_epoch = rtc.getEpoch();", PATHS["logger"])
cur["weak_expiry"] = block(logger, WEAK_EXPIRY, PATHS["logger"])
if not logger.index(cur["marker_set"]) < find(logger, WEAK_EXPIRY, what=PATHS["logger"]) < logger.index(POKE_SLEEP):
    sys.exit("FAIL: the weak-rail window expiry must follow the poll's buffer and marker set, before the poke's sleep")
# Its sibling (#104): a weak-rail cold boot whose cycle reads a rail at RAIL_AWAKE_VOLTS (the car switched on) sleeps,
# so the next wake takes the normal path; a connected phone holds it only inside the key-reset window. Shared context
# from the tree too. It must sit alone under #if ENABLE_IO36_RAIL (sm_weak_recovered keeps the guard) and directly
# ahead of the expiry: only #endif and comments between, so it runs on every cycle the expiry does, after the poll, and
# before the expiry can take the parked sleep.
recovered = find(logger, WEAK_RECOVERED, what=PATHS["logger"])
cur["weak_recovered"] = block(logger, WEAK_RECOVERED, PATHS["logger"])
# The key-reset window that recovery waits out under a connected phone: the production predicate and its constant, from
# the tree (auth_gate.h), over the harness's reset reason and millis().
auth = tree("baseLogger/Helpers/auth_gate.h")
cur["auth_window"] = (line(auth, "static const uint32_t AUTH_RESET_WINDOW_MS = ", "baseLogger/Helpers/auth_gate.h")
                      + block(auth, "static bool authResetWindowOpen() {", "baseLogger/Helpers/auth_gate.h"))
lead_in = logger[:recovered].splitlines()
while lead_in and lead_in[-1].lstrip().startswith("//"):
    lead_in.pop()
gap = logger[recovered + len(cur["weak_recovered"]):find(logger, WEAK_EXPIRY, what=PATHS["logger"])].splitlines()
if (logger.count(WEAK_RECOVERED) != 1 or not lead_in or lead_in[-1] != "#if ENABLE_IO36_RAIL" or not gap
        or gap[0] != "#endif" or any(not l.lstrip().startswith("//") for l in gap[1:])):
    sys.exit("FAIL: the weak-rail recovery block must be the only one, under #if ENABLE_IO36_RAIL, directly ahead of "
             "the weak-rail window expiry (only #endif and comments between)")
# loop()'s poll condition, from the tree for both sides too: the adapter counts a poll as reaching the car only when it
# holds (sm_poll_reaches_car), so "a poll ran on a weak-rail wake" tests the firmware's own veto (#104), not a copy.
# A plain && chain whose terms include the veto, guarding the lazy init and the bus read; see also the checks below.
poll_if = line(logger, POLL_IF, PATHS["logger"])
poll_match = re.fullmatch(r"    if \((.*)\) \{\n", poll_if)
if not poll_match or "||" in poll_match.group(1):
    sys.exit(f"FAIL: loop()'s poll condition must be one line, `if (a && b && ...) {{`, a plain && chain: {poll_if!r}")
if POLL_VETO not in [term.strip() for term in poll_match.group(1).split("&&")]:
    sys.exit(f"FAIL: loop()'s poll condition has no `{POLL_VETO}` term: a weak-rail cold boot would poll the car "
             f"during its BLE window (#104): {poll_if.strip()!r}")
poll_block = block(logger, poll_if, PATHS["logger"])
for call in ("obd.initialize();", "current_frame = obd.readPIDs(PRINT);"):
    if call not in poll_block:
        sys.exit(f"FAIL: loop()'s poll condition must guard {call!r} (#104): the veto would not cover it")
cur["poll_condition"] = poll_match.group(1)
cur["modem_override"] = line(sleep, "  if (g_weak_rail_cold_boot) modem_on = false;", PATHS["sleep"])
if "  if (g_ota_active || g_logdump_active) return;\n" + cur["modem_override"] not in sleep:
    sys.exit("FAIL: enterDeepSleep's weak-rail modem override must directly follow the OTA/dump guard, ahead of every "
             "use of modem_on")
buffered = find(logger, "    if (frame_has_data) {", what=PATHS["logger"])
buffered += len(block(logger, "    if (frame_has_data) {"))
if not re.fullmatch(r"(?:#if PRINT\n(?:.*\n)*?#endif\n)?", logger[buffered:logger.index(cur["marker_set"])]):
    sys.exit("FAIL: the marker set must directly follow the buffer block, before the upload can send the frame")
start = find(sleep, "  bool unsent = !obd_result_vector.empty();", what=PATHS["sleep"])
end = find(sleep, "wdtStreakMarker(false);\n", start, PATHS["sleep"]) + len("wdtStreakMarker(false);\n")
cur["marker_clear"] = sleep[start:end]
saved = find(sleep, "  saveUploadBacklog();\n#endif\n#endif\n", what=PATHS["sleep"])
between = sleep[saved:start].splitlines()[3:]
if not saved < start < sleep.index("  bleDeinitForSleep();") or any(not l.lstrip().startswith("//") for l in between):
    sys.exit("FAIL: the marker clear must follow saveUploadBacklog() outside its #if blocks, before bleDeinitForSleep()")
# crash() models a zeroed clock: it keeps the wdt-streak escalation sleep from queueing a restored marker and
# overwriting the backlog that boot never loaded.
line(logger, "  if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) rtc.setTime(0);", PATHS["logger"])

# Step 0a (#28) covers the drive region. OLD takes the blocks that write drive state from SM_PIN - the
# speed loop, the GNSS branch and the time re-latch, the is_parked publish, the OTA/dump hold, changeState, the
# frame_has_data buffer block and the pre-sleep marker - and the rest (the backstop, the delays) from the
# working tree, as CUR does. A deliberate change to that shared context is then identical
# on both sides and cannot fail the differential.
DRIVE_BLOCKS = ("motion_stats", "drive_state", "drive", "emplace", "session", "marker", "change_state", "buffer", "queue_helper", "restore")
# Step 0b-1 (#81) adds the poll-side energy region the same way: the rail refresh, the energy block and the
# helpers it calls come from SM_PIN for OLD.
ENERGY_BLOCKS = ("energy_rail", "energy", "energy_helpers")
# Step 0b-2 (#82) adds the setup() wake: the RTC sanitize and the whole gate block, with its skip path. The ladder's
# post-drive window, the poke's sleep, the last-poll stamp and the weak-rail window stay shared context.
WAKE_BLOCKS = ("sanitize", "wake_gate")
# Step 0b-3 (#83) adds the OBD-alarm permit the loop asks before every poll. The poll site itself is the adapter's,
# pinned below by position.
ALARM_BLOCKS = ("alarm",)
PIN_BLOCKS = DRIVE_BLOCKS + ENERGY_BLOCKS + WAKE_BLOCKS + ALARM_BLOCKS
old = dict(cur)
for name in PIN_BLOCKS:
    old[name] = pin[name]

# A C++ constant, out of the macro check's reach: the pinned gate runs with the tree's value, so they must be equal.
if pin["weak_awake_ms"] != cur["weak_awake_ms"]:
    sys.exit("FAIL: WEAK_RAIL_COLD_BOOT_AWAKE_MS differs between SM_PIN and the tree; the OLD gate would run with the new value")

# OLD must never pull a project header into its namespace: #pragma once would hand it CUR's copy.
for name in PIN_BLOCKS:
    if '#include "' in pin[name]:
        sys.exit(f"FAIL: OLD extraction {name} includes a project header; isolate it first")


def defines(config):
    found = {}
    for m in re.finditer(r"^#define (\w+)\b.*(?:\\\n.*)*", config, re.M):
        found.setdefault(m.group(1), m.group(0))
    return found


# Both sides compile against the tree's config.h, so every macro OLD uses must be byte-identical at the pin.
old_defines, cur_defines = defines(old_src["config"]), defines(cur_src["config"])
NAME = r"\b[A-Z][A-Z0-9_]{2,}\b"
used, pending = set(), set(re.findall(NAME, "".join(pin[name] for name in PIN_BLOCKS)))
while pending:  # through the macros' own bodies: ENABLE_NETWORK is (ENABLE_CELL || ENABLE_WIFI)
    macro = pending.pop()
    if macro in used or macro not in cur_defines:
        continue
    used.add(macro)
    pending |= set(re.findall(NAME, re.sub(r"^#define \w+", "", cur_defines[macro])))
for macro in sorted(used):
    if old_defines.get(macro, "") != cur_defines[macro]:
        sys.exit(f"FAIL: config.h {macro} differs between SM_PIN and the tree; the OLD side would run with the new value")

# The harness resets each firmware global to a literal; that literal must be the firmware's initialiser: in
# the tree for all of them, and at SM_PIN too except maintenance_mode, which only the shared context reads.
adapter = tree("tests/sm/adapter_pre.inc")
for region, prefix in (("SM_RAM", ""), ("SM_RTC", "RTC_DATA_ATTR ")):
    body = re.search(rf"^#define {region}\(X\) \\\n((?:.*\\\n)*.*)", adapter, re.M).group(1)
    for type_, name, value in re.findall(r"X\(([\w ]+), (\w+), ([^)]+)\)", body):
        pattern = rf"^{prefix}(?:static )?(?:volatile )?{re.escape(type_)} {name} = {re.escape(value)};"
        for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
            if label == "SM_PIN" and name == "maintenance_mode":
                continue
            if not re.search(pattern, src["logger"] + src["sleep"] + src["alarm"], re.M):
                sys.exit(f"FAIL: {region} {name} = {value} does not match its definition in logger.ino, "
                         f"sleep_power.h or alarm_gate.h ({label})")



def code_only(text):
    """Text with string/char literals and comments blanked (newlines kept), so a check sees code only."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


# The poll site (#83), modelled by the adapter: the car is asked (readPIDs, and the deferred OBD init) only inside the
# permit, compiled in the default build, after the cycle's rail and GNSS refresh (the permit's own inputs); the only
# other read is the build without the gate.
def open_directives(text, pos):
    """The #if blocks open at pos, each as its opening line and whether pos is past its #else."""
    stack = []
    for l in text[:pos].splitlines():
        t = l.strip()
        if re.match(r"#\s*if", t):
            stack.append((t, False))
        elif re.match(r"#\s*el", t):
            stack[-1] = (stack[-1][0], True)
        elif re.match(r"#\s*endif", t):
            stack.pop()
    return stack


code = code_only(logger)
gate_at = find(code, poll_if, what=PATHS["logger"])
gated = block(code, poll_if)
terms = [term.strip() for term in cur["poll_condition"].split("&&")]
reads = [m.start() for m in re.finditer(r"obd\.readPIDs\(", code)]
inits = [m.start() for m in re.finditer(r"obd\.initialize\(\);", code)]
outside = [r for r in reads if not gate_at <= r < gate_at + len(gated)]
if (terms[-1] != "alarmGateAllowsPoll()" or len(reads) != 2 or len(outside) != 1 or "current_frame = obd.readPIDs(PRINT);" not in gated
        or len(inits) != 2 or "obd.initialize();" not in gated
        or open_directives(code, gate_at) != [("#if FAKE_DATA", True), ("#if OBD_ALARM_GATE", False)]
        or open_directives(code, outside[0]) != [("#if FAKE_DATA", True), ("#if OBD_ALARM_GATE", True)]
        or "#define FAKE_DATA 0\n" not in cur_src["config"]
        or not code.index(ENERGY_RAIL.split("\n")[0]) < code.index("      pollGpsData();\n      parkAccumSample();") < gate_at):
    sys.exit("FAIL: loop() must ask the car (readPIDs and the deferred obd.initialize) only inside its poll condition, "
             "with alarmGateAllowsPoll() its last term, directly under #if OBD_ALARM_GATE in FAKE_DATA's #else, "
             "after the cycle's rail and GNSS refresh; the one other readPIDs is the gate's #else")

# Step 1 (#24), extended by #6 to drive_unconfirmed and the refusal-streak flag: drive_state.h is the only code that assigns the park/drive storage. An assignment here is
# =, a compound assignment, ++/--, taking the address or binding a reference (an alias could assign), or
# swap(); matched on the whole file, so a statement split over lines is still seen.
DRIVE_STORAGE = ("is_currently_parked", "g_last_speed_ms", "park_marker_owed", "drive_unconfirmed", "g_drive_gnss_refused")
DECLARATION = re.compile(r"(?:^|\n)\s*(?:RTC_DATA_ATTR\s+)?(?:bool|unsigned\s+long)\s*$")
helpers = sorted((root / "baseLogger/Helpers").glob("*.h"))
for path in [root / "baseLogger/logger.ino", *helpers]:
    if path.name == "drive_state.h":
        continue
    code = code_only(path.read_text())
    for name in DRIVE_STORAGE:
        for m in re.finditer(rf"\b{name}\b", code):
            before, after = code[:m.start()].rstrip(), code[m.end():].lstrip()
            if path.name == "logger.ino" and DECLARATION.search(before) and after.startswith("="):
                continue  # its definition
            assigned = re.match(r"(?:[-+*/%&|^]|<<|>>)?=(?!=)|\+\+|--", after)
            stepped = before.endswith(("++", "--"))
            addressed = before.endswith("&") and not before.endswith("&&") and not re.search(r"[\w)\]]\s*&$", before)
            bound = re.search(r"&\s*\w+\s*=$", before)
            swapped = re.search(r"\bswap\s*\([^;]*$", before)
            if assigned or stepped or addressed or bound or swapped:
                number = code.count("\n", 0, m.start()) + 1
                sys.exit(f"FAIL: {path.relative_to(root)}:{number} assigns {name}; only drive_state.h may (#24)")

# The OBD-alarm gate takes no drive input. A regex sees identifiers, not wrappers: a helper defined elsewhere
# that returns drive state is out of its reach, so review still matters.
alarm = code_only(tree("baseLogger/Helpers/alarm_gate.h"))
for pattern in (r"\bdrive[A-Z_]\w*", r"\bDrive\w*", r"\bDRIVE_\w*", r"\bis_currently_parked\b",
                r"\bg_last_speed_ms\b", r"\bpark_marker_owed\b", r"\blast_active\b", r"\bg_drive\w*"):
    if re.search(pattern, alarm):
        sys.exit(f"FAIL: alarm_gate.h reads drive state ({pattern}); the OBD-alarm gate takes no drive input")

# Every call into the drive region sits in a block this harness extracts, so none can be added unseen.
DRIVE_CALL = r"\bdrive[A-Z]\w*\s*\("
calls = sum(len(re.findall(DRIVE_CALL, code_only(p.read_text())))
            for p in [root / "baseLogger/logger.ino", *helpers] if p.name != "drive_state.h")
extracted = len(re.findall(DRIVE_CALL, code_only("".join(v for k, v in cur.items() if k != "drive_state"))))
if calls != extracted:
    sys.exit(f"FAIL: {calls} drive_state.h calls in the firmware but {extracted} in the extracted blocks; "
             "a call outside them is not compared against SM_PIN - extract its block")

# The weak-rail cold boot's veto (#104) covers every bus read in logger.ino, not only the one the harness runs: each
# obd.readPIDs( sits inside an `if (` block whose && terms include it (the poll condition above, and the
# OBD_ALARM_GATE=0 branch config.h does not build). GNSS is not powered on that wake either: every sys.gpsBegin() is
# behind the veto, or in dcPromotePoke(), which only a sample poke reaches (a cold boot is never one), or FILE_TEST.
def vetoed_blocks(text):
    """(start, end) of every brace block whose single-line `if (...) {` opener has the veto as an && term."""
    spans = []
    for m in re.finditer(r"^ *if \(([^\n]*)\) \{ *$", text, re.M):  # a trailing comment is blanked to spaces
        if "||" not in m.group(1) and POLL_VETO in [t.strip() for t in m.group(1).split("&&")]:
            spans.append((m.start(), m.start() + len(block(text[m.start():], m.group(0)))))
    return spans


logger_code = code_only(logger)
promote = block(logger_code, "void dcPromotePoke() {", PATHS["logger"])
promote_at = logger_code.index(promote)
file_test = [(m.start(), logger_code.index("#endif", m.start())) for m in re.finditer(r"^#if FILE_TEST\n", logger_code, re.M)]
for call, exempt in (("obd.readPIDs(", []), ("sys.gpsBegin()", [(promote_at, promote_at + len(promote))] + file_test)):
    for m in re.finditer(re.escape(call), logger_code):
        if not any(a <= m.start() < b for a, b in vetoed_blocks(logger_code) + exempt):
            number = logger_code.count("\n", 0, m.start()) + 1
            sys.exit(f"FAIL: {PATHS['logger']}:{number} calls {call} outside an `if (... && {POLL_VETO} ...)` block: "
                     "a weak-rail cold boot must not reach the bus or power GNSS (#104)")
# Nor does it ask the never-powered modem for the clock: setup()'s one boot resync is this exact line (#104), with no
# other clockResync call in setup() (comments stripped; code_only would blank the "boot" literal).
setup_code = re.sub(r"//[^\n]*", "", block(logger, "void setup() {", PATHS["logger"]))
if setup_code.count("clockResync(") != 1 or "\n" + BOOT_RESYNC + "\n" not in setup_code:
    sys.exit(f"FAIL: setup() must call clockResync exactly once, as {BOOT_RESYNC.strip()!r}: a weak-rail cold boot "
             "must not send AT+CCLK? to a modem it never powered (#104)")
if "if (!g_sample_poke || !g_dc_confirmed || !dcMotionRecent()) return;" not in promote:
    sys.exit("FAIL: dcPromotePoke() must return unless g_sample_poke: its sys.gpsBegin() is exempt from the weak-rail "
             "veto only because a cold boot is never a sample poke (#104)")

# Two wires the adapter bypasses (it sets the inputs directly): the sampled motion windows must reach the
# accumulator, and an OBD answer must stamp the car-on evidence.
if not re.search(r"motion_sampled = !g_ota_active && !g_logdump_active && current_state != UNINITIALIZED;\s*"
                 r"noteDriveMotionWindow\(motion_sampled, active\);", cur_src["machine"]):
    sys.exit("FAIL: state_machine.h must feed every sampled detectMovement window to noteDriveMotionWindow()")
if not re.search(r"if \(obd_count > 0\) \{[^}]*g_last_obd_answer_ms = millis\(\);", cur_src["logger"]):
    sys.exit("FAIL: logger.ino must stamp g_last_obd_answer_ms = millis() when obd_count > 0")

energy = tree("tests/energy_checks.cpp")
field = "".join(block(energy, f"const std::vector<Sample> {n} = ", "tests/energy_checks.cpp").rstrip() + ";\n" for n in ("DRIVE_0930", "DRIVE_1012"))

compiler = shlex.split(os.environ.get("CXX", "clang++"))
with tempfile.TemporaryDirectory(prefix="obd-sm-checks-") as temporary:
    build = Path(temporary)
    (build / "old_side.inc").write_text(SIDE.format(motion_inputs=MOTION_INPUTS, **old))
    (build / "cur_side.inc").write_text(SIDE.format(motion_inputs=MOTION_INPUTS, **cur))
    (build / "field.inc").write_text(field)
    executable = build / "sm_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
        "-Wno-unused-but-set-variable",
        "-I", str(root / "tests/sm"), "-I", str(root / "baseLogger/Helpers"), "-I", str(build),
        str(root / "tests/sm_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    started = time.monotonic()
    result = subprocess.run([str(executable), *sys.argv[1:]], env=environment)
    print(f"sm_checks: {time.monotonic() - started:.1f}s")
    sys.exit(result.returncode)
