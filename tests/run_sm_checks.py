"""Differential checks for the park/drive decision, the poll-side energy block, the setup() wake gate, the awake
ladder and loop()'s own sleeps (state machine #23, steps 0a #28, 0b-1 #81, 0b-2 #82 and 4b #29).

Five regions are compared. The drive region is drive_state.h and the blocks that call it: the speed loop, the
GNSS branch and the time re-latch, the is_parked publish, the frame_has_data buffer block with the queue helper, the
pre-sleep marker and the setup() marker restore. The awake ladder (#29) is all of state_machine.h: the delays,
changeState, the DC hold's helpers and handleActivityState() whole, with its OTA/dump hold, maintenance release,
backstop and both ladder sleeps; a TICK event runs it from loop()'s place. The energy region is
the poll's rail refresh and the contiguous block in loop() from the is_charging verdict through dcAwakeTrack():
the release latch and silent hold, the taper stall count, the DC session, the next-wake cadence, the fast budget
and cap, corroboration and the stale guard, with the helpers it calls (resetChargeCorroboration, pdwActive,
chargeStateStale). The wake region is setup()'s RTC sanitize and the whole ATRV gate block:
the ATRV read, the taper bridge and its give-up, the charge-end read, the follow-up, the poll decision, the alarm
suppression, the counters and log lines, the weak-rail cold boot and the skip path. The sleep region (4b-ii) is
loop()'s block after a poll cycle (the weak-rail window's recovery and expiry, and a sample poke's sleep with its
SAMPLE_POLL line), the net-recover reboot, loop()'s poll condition that decides whether a poll asks the car, the SLEEP
enter line's reason= and the cadence reason's type. All are extracted with the same anchors from two places: OLD from
the pinned commit (git show), CUR from the working tree. The context around them (every sleep's OTA/dump refusal and
modem override for that wake, the key-reset window) comes from the working tree for both sides, so a deliberate change
there cannot fail the differential. Since #25 the energy region is energy_state.h: energyOnPoll (the block, verbatim) with the
helpers beside it, the RTC sanitize and the ladder end's post-drive window. Each side inlines its copy and runs loop()'s
call site, and the three values the poll returns for the SAMPLE_POLL line are compared. Likewise (#26) the gate block
and setup()'s last-poll stamp are wake_gate.h's wakeGateRun and gateStampPoll, inlined and run from both call sites.
tests/sm_checks.cpp compiles each side into its own namespace around the same harness
(tests/sm/adapter_*.inc) and runs identical event sequences through both, asserting equal outputs after
every event: the ladder state and its motion windows, the latch, the leash and hold times, the marker debt and its
crash copy, published is_parked,
buffered frames, alarm speed notes, freezes, every energy and gate RTC field and RAM output, the next-wake
minutes and the cadence reason's text, the reason= each sleep names, and the exact logEvent and Serial sequences; the
gate's poll or skip shows in its counters. It also checks
the weak-rail gate, the weak-rail cold boot's modem-off sleep, the max-awake backstop (never a driving car or a held DC
charge), no ladder or loop() sleep under a transfer, at most two net-recover reboots an outage and the fast-cadence
budget (#83) as properties.
The OBD-alarm permit (#83) is alarm_gate.h from the setup grant through alarmGateAllowsPoll(), pinned for OLD; it is
the last term of loop()'s poll condition (compared since 4b-ii, after the weak-rail veto), and this runner fails unless loop()
asks the car (readPIDs and the deferred obd.initialize) only inside that condition, compiled in the default build,
after the cycle's rail and GNSS refresh. On an enforced car a poll asks
the car (obd.tx_count moves) exactly when an oracle kept from the trace's inputs permits it: the setup window with an
authenticated phone or within ALARM_PERMIT_BLE_GRACE_MS of one, or the rail awake and a moving fix no older than ALARM_GNSS_MAX_AGE_MS or a speed note within
ALARM_SPEED_HOLD_MS, a sustained rail collapse voiding the note. Every DENY line carries tx=, which never moves
between two of them; only the gated branch moves the counter here, so that checks the transition logging. Groups:
enumerate (exhaustive over a drive, an energy and a wake alphabet, --depth, default 5), random (fixed seeds), scenarios (named field and charge cases), crash (CUR alone: a panic,
also inside the pre-sleep flush, never loses the marker debt; an absolute property, not a comparison), rules
(the #6 driving rules' named cases), composition (#83, below), dc (#128's early DC release, as config.h builds it with
DC_EARLY_HOLD on: named cases and generated traces, also against an oracle kept from the inputs), or all; rules,
ladder (#29: the rungs in time, a phone at each level, the holds, the backstop and UNINITIALIZED, enumerated and random),
sleeps (#29, 4b-ii: each of loop()'s sleeps, refused under a dump, and the net-recover bound, enumerated and random),
composition, dc, ladder and sleeps compare the two sides throughout and assert their cases on CUR. A Charge or Silent poll keeps the
wake's rail and motion; any answered poll stamps the last OBD answer. A wake carries the rail, the ATRV read, the alarm flag and an
optional charge latch; the RTC clock and epoch run through sleeps, and a cold boot zeroes the clock until a poll.
Before compiling, it checks that only drive_state.h assigns the park/drive storage, that alarm_gate.h names no
drive state, and that every drive call is in an extracted block; likewise that only energy_state.h assigns the
charging state and every energy call is in a compared block or at a named site (ENERGY_UNEXTRACTED), and that only
wake_gate.h assigns the gate's persisted state, its calls checked the same way (GATE_UNEXTRACTED), and that only
state_machine.h assigns the ladder's state, its calls checked the same way (LADDER_UNEXTRACTED), and that only
sleep_paths.h assigns the next sleep's cadence, its calls checked the same way (SLEEP_UNEXTRACTED). A poll reaches the car only when loop()'s poll
condition holds (around it a profile is always loaded and the per-cycle alarm permit passes), so the property "a
poll ran on a weak-rail wake" holds a weak-rail cold boot to the firmware's own veto (#104); it also checks that the
condition has the veto as a term and guards the lazy init and the bus read, that every obd.readPIDs( and
sys.gpsBegin() in logger.ino sits behind it (dcPromotePoke's and FILE_TEST's excepted), that dcPromotePoke still
needs a sample poke, that setup()'s one boot clock resync skips that wake, and that the weak-rail recovery (a cycle's
rail at RAIL_AWAKE_VOLTS ends the wake, held by a phone only inside the key-reset window, whose production
predicate is shared context) sits under #if ENABLE_IO36_RAIL directly ahead of the window's expiry. A phone
exists only where a scenario or a PHONE event connects one (sm_ble_connected, read through bleConnected(), dropped at
every sleep). A tick's detectMovement() is the event's motion with mems.h's stamp (a transfer or a phone may start
inside it), and a sleep inside a tick, a poll cycle's sleeps or the net-recover block ends it.

The pin. SM_PIN is the main commit a step is measured against: a refactor step must leave both regions equal to
it. Re-pin only when a behaviour change to a compared region lands on main between steps, to the merge-base of
the step being measured; it is a commit on main, so it stays reachable. A clone without it fails with a message,
never a skip.

What it cannot prove: 32-bit millis() wrap (the host unsigned long is 64-bit); any feature-flag set other
than the config.h defaults with PRINT 0 (lines under #if PRINT and other branches are not compiled); code
outside the extracted blocks (dcPromotePoke, a no-op stub here, and motion inside a wake, the console's own effects
beyond its flag and stamp, the upload that counts the net-recover power-cycles, the silent retry (#131, see the stub in sm/adapter_pre.inc), the modem, detectMovement timing,
RTC retention, hardware); the wdt-streak sleeps before and after
the gate; resets other than a deep-sleep wake, a power loss, a panic and a WDT (a brownout or software reset reloads
RTC as those do, so the sanitize cannot show); the heartbeat (ENABLE_PARKED_HEARTBEAT 0, so the branch is not
compiled and loop()'s gateOnPollResult, which keeps heartbeat_asleep_streak, never runs); the per-PID alarm guard inside readPIDs (alarmPollGuard), the deferred
OBD init and a missing profile (g_no_profile stays false); the loop's own phone-presence stamp (alarmGateService), so
the setup permit's grace runs from the last permit read that saw the phone; since_poll at the target's 32-bit width;
GNSS power (a poll's GNSS input is the event's, also on a weak-rail cold boot, whose GNSS the firmware never powers:
that is checked by position only), the boot profile restore and a profile push (tests/run_profile_checks.py). The multi-region layer
is covered by the composition group (#83): playComposed resolves the temporary drive charge input from the real
previous energy latch, then runs the unchanged adapter on both sides. Named transition checks and fixed-seed mixed
traces assert the prior-energy/current-drive ordering on CUR.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

SM_PIN = "818131a56b8b3c60d0606e16bcd961a1af4919b6"

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests/support"))
from extract import block, find  # noqa: E402
if subprocess.run(["git", "merge-base", "--is-ancestor", SM_PIN, "HEAD"], cwd=root).returncode != 0:
    sys.exit(f"FAIL: SM_PIN {SM_PIN} is not an ancestor of HEAD: fetch main, or re-pin (see this file's header)")


def pinned(path):
    if path in OPTIONAL and subprocess.run(["git", "cat-file", "-e", f"{SM_PIN}:{path}"], cwd=root,
                                           stderr=subprocess.DEVNULL).returncode != 0:
        return ""
    return subprocess.check_output(["git", "show", f"{SM_PIN}:{path}"], cwd=root, text=True)


def tree(path):
    return (root / path).read_text()


def line(text, prefix, what):
    found = [l for l in text.splitlines() if l.startswith(prefix)]
    if len(found) != 1:
        sys.exit(f"FAIL: expected one line starting {prefix!r} in {what}, found {len(found)}")
    return found[0] + "\n"


def code_only(text):
    """Text with string/char literals and comments blanked (newlines kept), so a check sees code only."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


PATHS = {"logger": "baseLogger/logger.ino", "machine": "baseLogger/Helpers/state_machine.h",
         "sleep": "baseLogger/Helpers/sleep_power.h", "upload": "baseLogger/Helpers/network_upload.h",
         "logging": "baseLogger/Helpers/logging.h", "drive_state": "baseLogger/Helpers/drive_state.h",
         "motion_stats": "baseLogger/Helpers/motion_stats.h", "config": "baseLogger/config.h",
         "alarm": "baseLogger/Helpers/alarm_gate.h", "energy_state": "baseLogger/Helpers/energy_state.h",
         "wake_gate_h": "baseLogger/Helpers/wake_gate.h", "sleep_paths_h": "baseLogger/Helpers/sleep_paths.h"}
# Newer than SM_PIN: empty on a side that has none (4b-ii, #29: SM_PIN keeps loop()'s sleeps in logger.ino).
OPTIONAL = ("baseLogger/Helpers/sleep_paths.h",)


# Code anchors only (never comment text), each failing with its own text when the code it names has moved.
DRIVE_START = ('    for (const auto& res : current_frame) {\n'
               '      if (( res.first == "speed" || res.first == "vehicle_reported_speed" )')
DRIVE_END = "#if ENABLE_NETWORK\n    const double poll_utc = uploadSendUtc();"
# The poll-side energy block (#81) is energy_state.h's energyOnPoll (#25): the charge verdict, the release latch and
# silent hold, the taper stall count, the DC session, the next-wake cadence, the fast budget and cap, corroboration and
# the stale guard. loop() calls it once, directly ahead of #if ENABLE_BLE.
ENERGY_END = "\n#if ENABLE_BLE\n    const uint16_t ble_mtu"
ENERGY_RAIL = "    g_last_io36 = readIO36Rail();\n    // A poke held by a DC charge"
ENERGY_CALL_SITE = "    const EnergyPoll ep = energyOnPoll(current_frame, obd_count);\n    dcAwakeTrack();\n"
LOGGED_DCFC = "        static int logged_dcfc = -1;"
# The setup() wake gate (#82): the ATRV read, the taper bridge and its give-up, the charge-end read, the poll decision,
# the counters and log lines, the weak-rail cold boot and the skip path. Since #26 it is wake_gate.h's wakeGateRun,
# called here, and the last-poll stamp its gateStampPoll.
WAKE_GATE_CALL = "  wakeGateRun(wake_cause);\n"
STAMP_CALL = "  gateStampPoll();"
# The weak-rail window's two sleeps and the poke's, in sleep_paths.h's glue (#29, 4b-ii).
WEAK_GLUE = "void loopWeakRailSleep() {"
WEAK_EXPIRY = "  if (g_weak_rail_cold_boot && !maintenance_mode && !bleConnected() && millis() > WEAK_RAIL_COLD_BOOT_AWAKE_MS) {"
WEAK_RECOVERED = ("  if (g_weak_rail_cold_boot && !maintenance_mode && g_last_io36 >= RAIL_AWAKE_VOLTS &&\n"
                  "      !(bleConnected() && authResetWindowOpen())) {")
LOOP_SLEEP_CALLS = "    loopWeakRailSleep();\n    dcPromotePoke();\n    loopSamplePokeSleep(ep, obd_count);\n"
# loop()'s own sleeps after a poll cycle (#29, 4b-ii): the weak-rail window's recovery and expiry, dcPromotePoke() and the
# sample poke's sleep, from the #if after the poll stamp through its #endif.
LOOP_SLEEPS = "    last_poll = millis();\n\n#if ATRV_GATE_ENFORCE && ENABLE_SLEEP && ENABLE_MEMS\n"
# The net-recover reboot (#29, 4b-ii): everything between the modem-restart diagnostics and the end of the http=0 block.
NET_RECOVER_START = ('logEvent("NET recover radio %s", diag.second.c_str());\n            }\n'
                     "#endif\n#endif\n")
NET_RECOVER_END = "          }\n        }\n        last_broadcast = millis();"
# The SLEEP enter line's reason= argument (sleep_power.h), compared as the text each sleep names.
SLEEP_TEXT_START = '    modem_down ? "modem" : "(modem already off)", g_next_wake_minutes,\n'
SLEEP_TEXT_END = ",\n    g_last_io36, WOM_THRESHOLD_G);"
# sleep_paths.h's functions the headers before it call, declared in logger.ino beside the cadence's storage.
CADENCE_DECLARATIONS = ("void cadenceArm(int minutes, CadenceReason why);",
                        "const char* cadenceReasonName(CadenceReason why);",
                        "const char* sleepReasonText(const char* reason, CadenceReason why, int minutes);")
BOOT_RESYNC = '  if (!g_weak_rail_cold_boot && clockNeedsSync() && !clockResync("boot")) clockNtpNoClock();'
# loop()'s poll condition (the OBD_ALARM_GATE branch, the one config.h builds): whether a poll cycle asks the car.
POLL_IF = "    if (!g_no_profile && "
POLL_VETO = "!g_weak_rail_cold_boot"
# The OBD-alarm permit (#83): alarm_gate.h from the setup grant through alarmGateAllowsPoll(), its function-local
# statics hoisted (adapter_pre.inc SM_LOCAL) and the speed note renamed, so the adapter's stub can count it first.
ALARM_START = "void alarmGrantSetupPermit() {"
ALARM_END = "\n/* Loop-task service:"
ALARM_STATICS = ("  static bool logged_this_wake = false;\n  static bool last_permit = false;\n"
                 "  static unsigned long last_deny_log_ms = 0;\n")
ALARM_NOTE = "void alarmNoteSpeed(double kmh) {"
MAINT_IDLE = "const unsigned long MAINTENANCE_IDLE_MS = "
# The OTA/dump block on every sleep (#29: it stays in enterDeepSleep; the ladder must tolerate the refusal).
SLEEP_REFUSAL = "  if (g_ota_active || g_logdump_active) return;\n"


def recipe(src, side):
    logger, machine = src["logger"], src["machine"]
    where = {k: f"{PATHS[k]} ({side})" for k in PATHS}
    start = find(logger, DRIVE_START, what=where["logger"])
    upload_start = find(src["upload"], "bool isSyntheticParkMarker(", what=where["upload"])
    upload_end = find(src["upload"], "\n#if ENABLE_NETWORK", upload_start, where["upload"])
    rail = find(logger, ENERGY_RAIL, what=where["logger"])
    # The awake ladder (#29) is all of state_machine.h: the delays, changeState, the DC hold and handleActivityState.
    if machine.count("#pragma once\n") != 1 or '#include "' in machine:
        sys.exit(f"FAIL: {where['machine']} must be one #pragma once and no project include, to inline it")
    # The energy region (#25) is all in energy_state.h, with loop()'s call site.
    energy_state = src["energy_state"]
    if energy_state.count("#pragma once\n") != 1 or '#include "' in energy_state:
        sys.exit(f"FAIL: {where['energy_state']} must be one #pragma once and no project include, to inline it")
    if logger.count(ENERGY_CALL_SITE) != 1 or logger.count("energyOnPoll(") != 1:
        sys.exit(f"FAIL: loop() must call energyOnPoll once, as {ENERGY_CALL_SITE!r} ({side})")
    energy_start = find(logger, ENERGY_CALL_SITE, what=where["logger"])
    energy_end = energy_start + len(ENERGY_CALL_SITE)
    if not logger[energy_end:].startswith(ENERGY_END):
        sys.exit(f"FAIL: nothing may sit between loop()'s energyOnPoll call site and #if ENABLE_BLE ({side})")
    # Function-local, so a boot resets it (adapter_pre.inc SM_LOCAL), as a deep-sleep wake does.
    if energy_state.count(LOGGED_DCFC) != 1:
        sys.exit(f"FAIL: expected one {LOGGED_DCFC.strip()!r} in the energy block ({side})")
    # The wake gate (#26) is all in wake_gate.h, with setup()'s two call sites.
    wake_gate_h = src["wake_gate_h"]
    gate = find(logger, WAKE_GATE_CALL, what=where["logger"])
    under = logger.rfind("#if ENABLE_ATRV_GATE\n", 0, gate)
    if logger.count(WAKE_GATE_CALL) != 1 or under < 0 or re.search(r"^\s*#", logger[under + 1:gate], re.M):
        sys.exit(f"FAIL: expected one wake gate block, under #if ENABLE_ATRV_GATE with no directive between ({side})")
    if wake_gate_h.count("#pragma once\n") != 1 or '#include "' in wake_gate_h:
        sys.exit(f"FAIL: {where['wake_gate_h']} must be one #pragma once and no project include, to inline it")
    if not logger[gate + len(WAKE_GATE_CALL):].startswith("#endif\n") or "wakeGateRun(" in logger.replace(WAKE_GATE_CALL, ""):
        sys.exit(f"FAIL: setup() must call wakeGateRun once, as {WAKE_GATE_CALL!r} directly before its #endif ({side})")
    for header in ("motion_stats", "drive_state"):
        if src[header].count("#pragma once\n") != 1 or '#include "' in src[header]:
            sys.exit(f"FAIL: {where[header]} must be one #pragma once and no project include, to inline it")
    alarm_start = find(src["alarm"], ALARM_START, what=where["alarm"])
    alarm = src["alarm"][alarm_start:find(src["alarm"], ALARM_END, alarm_start, where["alarm"]) + 1]
    if alarm.count(ALARM_STATICS) != 1 or alarm.count(ALARM_NOTE) != 1:
        sys.exit(f"FAIL: expected alarmGateAllowsPoll's three statics together and one alarmNoteSpeed in {where['alarm']}")
    # loop()'s own sleeps, the net-recover reboot and the SLEEP enter reason (#29, 4b-ii), each by the same anchors on
    # both sides.
    if logger.count(LOOP_SLEEPS) != 1 or logger.count(NET_RECOVER_START) != 1 or src["sleep"].count(SLEEP_TEXT_START) != 1:
        sys.exit(f"FAIL: expected one loop() sleep block, one net-recover block and one SLEEP enter reason ({side})")
    sleeps_at = find(logger, LOOP_SLEEPS, what=where["logger"]) + LOOP_SLEEPS.index("#if")
    sleeps_end = find(logger, "#endif\n  }\n", sleeps_at, where["logger"]) + len("#endif\n")
    recover_at = find(logger, NET_RECOVER_START, what=where["logger"]) + len(NET_RECOVER_START)
    recover_end = find(logger, NET_RECOVER_END, recover_at, where["logger"])
    text_at = find(src["sleep"], SLEEP_TEXT_START, what=where["sleep"]) + len(SLEEP_TEXT_START)
    text_end = find(src["sleep"], SLEEP_TEXT_END, text_at, where["sleep"])
    # loop()'s poll condition (the OBD_ALARM_GATE branch config.h builds): a plain && chain on one line.
    poll_match = re.fullmatch(r"    if \((.*)\) \{\n", line(logger, POLL_IF, where["logger"]))
    if not poll_match or "||" in poll_match.group(1):
        sys.exit(f"FAIL: loop()'s poll condition must be one line, `if (a && b && ...) {{`, a plain && chain ({side})")
    # g_cadence_reason's type and initialiser, from this side's definition: the adapter declares it as SmCadence.
    cadence = re.findall(r"^(\w[\w :*]*?) ?\bg_cadence_reason = ([^;]+);", logger, re.M)
    if len(cadence) != 1:
        sys.exit(f"FAIL: expected one g_cadence_reason definition in {where['logger']}")
    # Where it is an enum (CadenceReason, 4b-ii), the enum and the owner's declarations before it, from logger.ino,
    # and its compared view is its text.
    cadence_enum = ""
    if cadence[0][0] != "const char*":
        cadence_enum = (block(logger, f"enum {cadence[0][0]} : uint8_t {{", where["logger"]).rstrip() + ";\n" +
                        "".join(line(logger, d, where["logger"]) for d in CADENCE_DECLARATIONS) +
                        f"const char* smShow({cadence[0][0]} r) {{ return cadenceReasonName(r); }}\n")
    # sleep_paths.h (4b-ii), inlined as drive_state.h, where the side has it.
    sleep_paths_h = src["sleep_paths_h"]
    if sleep_paths_h and (sleep_paths_h.count("#pragma once\n") != 1 or '#include "' in sleep_paths_h):
        sys.exit(f"FAIL: {where['sleep_paths_h']} must be one #pragma once and no project include, to inline it")
    if not (start < find(logger, DRIVE_END, start, where["logger"]) <
            find(logger, '    if (!drive_unconfirmed) current_frame.emplace_back("is_parked"', what=where["logger"]) <
            energy_start < energy_end <=
            find(logger, "    if (frame_has_data) {", what=where["logger"])):
        sys.exit(f"FAIL: composition requires drive, is_parked publication, energy, then buffering in firmware order ({side})")
    return {
        "alarm": alarm.replace(ALARM_STATICS, "").replace(ALARM_NOTE, "void alarmNoteSpeedBody(double kmh) {"),
        # Inlined, never #included: #pragma once would hand OLD the tree's copy.
        "drive_state": src["drive_state"].replace("#pragma once\n", ""),
        "motion_stats": src["motion_stats"].replace("#pragma once\n", ""),
        "enum": block(logger, "enum ActivityState {", where["logger"]).rstrip() + ";\n",
        "buffer_cap": line(logger, "const size_t MAX_UPLOAD_BUFFER = ", where["logger"]),
        "state_name": block(src["logging"], "static const char* stateName(ActivityState s) {", where["logging"]),
        "drive": logger[start:find(logger, DRIVE_END, start, where["logger"])],
        "emplace": line(logger, '    if (!drive_unconfirmed) current_frame.emplace_back("is_parked"', where["logger"]),
        "buffer": block(logger, "    if (frame_has_data) {", where["logger"]),
        "marker": block(src["sleep"], "  if (rtc.getEpoch() > 1e9 && is_currently_parked", where["sleep"]),
        "wake_gate": WAKE_GATE_CALL,
        "stamp": line(logger, STAMP_CALL, where["logger"]),
        # Inlined, never #included, as drive_state.h.
        "wake_gate_h": wake_gate_h.replace("#pragma once\n", ""),
        "weak_awake_ms": line(logger, "static const uint32_t WEAK_RAIL_COLD_BOOT_AWAKE_MS = ", where["logger"]),
        "maint_idle": line(logger, MAINT_IDLE, where["logger"]),
        "queue_helper": src["upload"][upload_start:upload_end],
        "energy_rail": logger[rail:find(logger, "\n#endif", rail, where["logger"]) + 1],
        "energy": ENERGY_CALL_SITE,
        # Inlined, never #included, as drive_state.h.
        "energy_state": energy_state.replace("#pragma once\n", "").replace(LOGGED_DCFC, ""),
        # Inlined, never #included, as drive_state.h.
        "machine": machine.replace("#pragma once\n", ""),
        "loop_sleeps": logger[sleeps_at:sleeps_end],
        "net_recover": logger[recover_at:recover_end],
        "sleep_text": src["sleep"][text_at:text_end],
        "poll_condition": poll_match.group(1),
        "cadence": (cadence_enum + f"using SmCadence = {cadence[0][0]};\n"
                    f"constexpr SmCadence SM_CADENCE_INIT = {cadence[0][1]};\n"),
        "sleep_paths_h": sleep_paths_h.replace("#pragma once\n", ""),
    }


SIDE = """{enum}
{cadence}
{buffer_cap}
{weak_awake_ms}
{maint_idle}
#include "adapter_pre.inc"
{alarm}
{motion_stats}
{drive_state}
{motion_inputs}
{queue_helper}
{state_name}
void sm_drive_poll(OBDVector& current_frame) {{
{drive}}}
void sm_emplace(OBDVector& current_frame) {{
{emplace}}}
{energy_state}
bool sm_sleep_refused() {{
{sleep_refusal}  return false;
}}
{machine}
void sm_energy_rail() {{
{energy_rail}}}
void sm_energy(OBDVector& current_frame, int obd_count) {{
{energy}  sm_ep_charging = ep.is_charging, sm_ep_progress = ep.charge_progress, sm_ep_silent = ep.high_rail_silent;
}}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"  // the gate's `now > 1000000000UL` on the host's signed time_t
{wake_gate_h}
void sm_wake_gate(SmWakeCause wake_cause) {{
{wake_gate}}}
#pragma GCC diagnostic pop
void sm_stamp() {{
{stamp}}}
{auth_window}{sleep_paths_h}
void sm_loop_sleeps(int obd_count) {{
  const EnergyPoll ep{{sm_ep_charging, sm_ep_progress, sm_ep_silent}};
  (void)ep;
  (void)obd_count;
{loop_sleeps}}}
void sm_net_recover() {{
{net_recover}}}
const char* sm_sleep_text(const char* reason) {{
  return
{sleep_text};
}}
void sm_modem_override(bool& modem_on) {{
{modem_override}}}
bool sm_poll_reaches_car() {{ return {poll_condition}; }}
void sm_buffer(OBDVector& current_frame, bool frame_has_data) {{
  int g_geo_queued = 0, poll_geo = 0;
  (void)g_geo_queued;
  const bool ble_relayed = false; // no phone in this harness
  const uint32_t relay_seq = 0; // nor relay acks (#90, tests/run_relay_checks.py)
  const char* const RELAY_SEQ_KEY = "__relay_seq";
  (void)relay_seq; (void)RELAY_SEQ_KEY;
{buffer}}}
void sm_marker() {{
{marker}}}
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
cur["marker_set"] = line(logger, "    if (park_marker_owed) wdtStreakMarker(true);", PATHS["logger"])
# loop()'s own sleeps (#29, 4b-ii) are pinned and compared (recipe, loop_sleeps): sleep_paths.h's weak-rail window
# (expiry and recovery), then dcPromotePoke(), then the poke's sleep, called once each, inside the poll block after the
# poll's buffer and marker set, ...
glue = cur_src["sleep_paths_h"]
if (code_only(cur["loop_sleeps"]).count(LOOP_SLEEP_CALLS) != 1 or logger.count(LOOP_SLEEP_CALLS) != 1
        or not logger.index(cur["marker_set"]) < logger.index(LOOP_SLEEP_CALLS)
        or any(code_only(logger).count(f) != 1 for f in ("loopWeakRailSleep(", "loopSamplePokeSleep("))):
    sys.exit(f"FAIL: loop() must call {LOOP_SLEEP_CALLS!r} once, after the poll's buffer and marker set")
# ... and in the window's glue the recovery (#104), a weak-rail cold boot whose cycle reads a rail at RAIL_AWAKE_VOLTS
# (the car switched on), which sleeps so the next wake takes the normal path; a connected phone holds it only inside the
# key-reset window. It must sit alone under #if ENABLE_IO36_RAIL and directly ahead of the expiry: only #endif and
# comments between, so it runs on every cycle the expiry does, after the poll, and before the expiry can take the
# parked sleep.
weak_glue = block(glue, WEAK_GLUE, PATHS["sleep_paths_h"])
recovered = find(weak_glue, WEAK_RECOVERED, what=PATHS["sleep_paths_h"])
weak_recovered = block(weak_glue, WEAK_RECOVERED, PATHS["sleep_paths_h"])
# The key-reset window that recovery waits out under a connected phone: the production predicate and its constant, from
# the tree (auth_gate.h), over the harness's reset reason and millis().
auth = tree("baseLogger/Helpers/auth_gate.h")
cur["auth_window"] = (line(auth, "static const uint32_t AUTH_RESET_WINDOW_MS = ", "baseLogger/Helpers/auth_gate.h")
                      + block(auth, "static bool authResetWindowOpen() {", "baseLogger/Helpers/auth_gate.h"))
lead_in = weak_glue[:recovered].splitlines()
while lead_in and lead_in[-1].lstrip().startswith("//"):
    lead_in.pop()
gap = weak_glue[recovered + len(weak_recovered):find(weak_glue, WEAK_EXPIRY, what=PATHS["sleep_paths_h"])].splitlines()
if (glue.count(WEAK_RECOVERED) != 1 or glue.count(WEAK_EXPIRY) != 1 or not lead_in or lead_in[-1] != "#if ENABLE_IO36_RAIL" or not gap
        or gap[0] != "#endif" or any(not l.lstrip().startswith("//") for l in gap[1:])):
    sys.exit("FAIL: the weak-rail recovery block must be the only one, under #if ENABLE_IO36_RAIL, directly ahead of "
             "the weak-rail window expiry (only #endif and comments between)")
# loop()'s poll condition, pinned and compared (recipe): the adapter counts a poll as reaching the car only when it
# holds (sm_poll_reaches_car), so "a poll ran on a weak-rail wake" tests the firmware's own veto (#104), not a copy.
# In the tree, a plain && chain whose terms include the veto, guarding the lazy init and the bus read; see also below.
poll_if = line(logger, POLL_IF, PATHS["logger"])
poll_match = re.fullmatch(r"    if \((.*)\) \{\n", poll_if)
if POLL_VETO not in [term.strip() for term in poll_match.group(1).split("&&")]:
    sys.exit(f"FAIL: loop()'s poll condition has no `{POLL_VETO}` term: a weak-rail cold boot would poll the car "
             f"during its BLE window (#104): {poll_if.strip()!r}")
poll_block = block(logger, poll_if, PATHS["logger"])
for call in ("obd.initialize();", "current_frame = obd.readPIDs(PRINT);"):
    if call not in poll_block:
        sys.exit(f"FAIL: loop()'s poll condition must guard {call!r} (#104): the veto would not cover it")
cur["modem_override"] = line(sleep, "  if (g_weak_rail_cold_boot) modem_on = false;", PATHS["sleep"])
if SLEEP_REFUSAL + cur["modem_override"] not in sleep or sleep.count(SLEEP_REFUSAL) != 1:
    sys.exit("FAIL: enterDeepSleep's weak-rail modem override must directly follow the OTA/dump guard, ahead of every "
             "use of modem_on")
# The guard itself, from the tree for both sides: the adapter's enterDeepSleep refuses a sleep as the firmware does, and
# must open enterDeepSleep ahead of every side effect.
if "void enterDeepSleep(bool modem_on, const char* reason = nullptr) {\n" + SLEEP_REFUSAL not in re.sub(
        r"\n *//[^\n]*", "", sleep):
    sys.exit("FAIL: the OTA/dump guard must be enterDeepSleep's first statement")
cur["sleep_refusal"] = SLEEP_REFUSAL.replace("return;", "return true;")
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
# speed loop, the GNSS branch and the time re-latch, the is_parked publish, the frame_has_data buffer block and the
# pre-sleep marker - and the rest from the working tree, as CUR does. A deliberate change to that shared context is
# then identical on both sides and cannot fail the differential.
DRIVE_BLOCKS = ("motion_stats", "drive_state", "drive", "emplace", "marker", "buffer", "queue_helper", "restore")
# Step 0b-1 (#81) adds the poll-side energy region the same way: the rail refresh, the energy block and the
# helpers it calls come from SM_PIN for OLD. Since step 3 (#25) that is all of energy_state.h, so the RTC sanitize and
# the ladder end's post-drive window (pdwArmAtLadderEnd, with #22's re-check) are pinned with it.
ENERGY_BLOCKS = ("energy_rail", "energy", "energy_state")
# Step 0b-2 (#82) adds the setup() wake: the whole gate block, with its skip path; step 4a (#26) moved it and the
# last-poll stamp into wake_gate.h, called from setup(). The poke's sleep and the weak-rail window stay shared context.
WAKE_BLOCKS = ("wake_gate_h", "wake_gate", "stamp")
# Step 0b-3 (#83) adds the OBD-alarm permit the loop asks before every poll. The poll site itself is the adapter's,
# pinned below by position.
ALARM_BLOCKS = ("alarm",)
# Step 4b (#29) adds the awake ladder: all of state_machine.h, pinned for OLD. That takes the OTA/dump hold, changeState
# and the DC hold's helpers (pinned before, as drive and energy blocks) with the backstop and the delays (shared before)
# into one compared block. The maintenance idle constant must match, as WEAK_RAIL_COLD_BOOT_AWAKE_MS does.
LADDER_BLOCKS = ("machine",)
# Step 4b-ii (#29) adds loop()'s own sleeps: the weak-rail window's recovery and expiry and the sample poke's sleep with
# its SAMPLE_POLL line (shared before), the net-recover reboot, the SLEEP enter line's reason and loop()'s poll condition
# (shared before), and the cadence reason's type; on CUR, all of sleep_paths.h, which owns them.
SLEEP_BLOCKS = ("loop_sleeps", "net_recover", "sleep_text", "poll_condition", "cadence", "sleep_paths_h")
PIN_BLOCKS = DRIVE_BLOCKS + ENERGY_BLOCKS + WAKE_BLOCKS + ALARM_BLOCKS + LADDER_BLOCKS + SLEEP_BLOCKS
old = dict(cur)
for name in PIN_BLOCKS:
    old[name] = pin[name]

# A C++ constant, out of the macro check's reach: the pinned gate runs with the tree's value, so they must be equal.
if pin["weak_awake_ms"] != cur["weak_awake_ms"]:
    sys.exit("FAIL: WEAK_RAIL_COLD_BOOT_AWAKE_MS differs between SM_PIN and the tree; the OLD gate would run with the new value")
if pin["maint_idle"] != cur["maint_idle"]:
    sys.exit("FAIL: MAINTENANCE_IDLE_MS differs between SM_PIN and the tree; the OLD ladder would run with the new value")

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
# the tree for all of them, and at SM_PIN too except those only the shared context reads (none since #29).
# g_cadence_reason's type and initialiser come from each side's own definition (recipe's cadence), so it is exempt.
SHARED_ONLY = ()
OWN_DEFINITION = ("g_cadence_reason",)
adapter = tree("tests/sm/adapter_pre.inc")
for region, prefix in (("SM_RAM", ""), ("SM_RTC", "RTC_DATA_ATTR ")):
    body = re.search(rf"^#define {region}\(X\) \\\n((?:.*\\\n)*.*)", adapter, re.M).group(1)
    for type_, name, value in re.findall(r"X\(([\w ]+), (\w+), ([^)]+)\)", body):
        pattern = rf"^{prefix}(?:static )?(?:volatile )?{re.escape(type_)} {name} = {re.escape(value)};"
        for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
            if (label == "SM_PIN" and name in SHARED_ONLY) or name in OWN_DEFINITION:
                continue
            if not re.search(pattern, src["logger"] + src["sleep"] + src["alarm"], re.M):
                sys.exit(f"FAIL: {region} {name} = {value} does not match its definition in logger.ino, "
                         f"sleep_power.h or alarm_gate.h ({label})")



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

# Step 1 (#24), extended by #6 to drive_unconfirmed and the refusal-streak flag: drive_state.h is the only code that
# assigns the park/drive storage, since step 3 (#25) energy_state.h the charging state, and since step 4a (#26)
# wake_gate.h the gate's persisted state. An assignment here is =, a
# compound assignment (also chained), ++/--, taking the address or binding a reference (an alias could assign), or
# swap(); matched on the whole file, so a statement split over lines is still seen. The storage is defined in
# logger.ino (two gate fields in sleep_power.h) until #27, so a definition there is exempt, but only at file scope: a
# local of the same name is not.
# Blind spot: passing a field to a non-const reference parameter writes it unseen, as chargeCounterRose does inside
# the energy owner; review must catch one outside it.
DRIVE_STORAGE = ("is_currently_parked", "g_last_speed_ms", "park_marker_owed", "drive_unconfirmed", "g_drive_gnss_refused",
                 "g_dc_release_polls", "g_dc_reset_logged")
ENERGY_STORAGE = ("charging_fast_minutes", "dc_session", "charging_cap_latched", "charge_uncorroborated_s",
                  "charge_corr_last_us", "charge_corr_soc_hw", "charge_corr_kwh_hw", "charge_corr_soe_hw",
                  "recently_charging", "taper_confirm_wakes", "charge_end_polled", "prev_charge_soc", "prev_parked_soc",
                  "prev_parked_kwh", "prev_parked_soe", "charge_release_polls", "charge_silent_polls", "charge_followup",
                  "pdw_open", "pdw_open_us", "pdw_polls", "charge_last_evidence_us", "g_dc_confirmed",
                  "g_dc_inferred_polls", "g_dc_silent_since_ms", "g_fast_anchor_ms", "g_last_discharge_ms",
                  "g_pdw_ladder_done", "g_pdw_recheck")
# Step 4a (#26): the wake gate's persisted state. Not owned: the poke-lifetime flags g_sample_poke, g_taper_confirm and
# g_rail_before_poke (also written by dcPromotePoke, energyOnPoll and the DC re-poke, by design; #27), g_last_voltage,
# wdt_reset_count, g_next_wake_minutes and g_cadence_reason (4b) and g_alarm_flag.
GATE_STORAGE = ("skips_count", "polls_count", "heartbeat_asleep_streak", "last_poll_epoch", "g_weak_rail_cold_boot")
# Step 4b (#29): the awake ladder's state, owned by state_machine.h. Not owned: the leash last_active (detectMovement,
# the drive region and the console stamp it too), the console's maintenance_mode, g_ble_handoff_pending (the relay and
# the upload clear it), logging_message_printed and the two delays (loop() and the silent retry write them).
LADDER_STORAGE = ("current_state", "g_ble_session_relayed", "g_ble_disconnect_ms", "g_dc_logged", "g_dc_refused_logged",
                  "config_message_printed")
# Step 4b-ii (#29): the next sleep's cadence, owned by sleep_paths.h. The energy region, the ladder end and the gate's
# skip decide it and arm it through cadenceArm; loop()'s and the crash streak's sleeps through sleepFor.
CADENCE_STORAGE = ("g_next_wake_minutes", "g_cadence_reason")
DEFINITION = re.compile(r"(?:^|\n)\s*(?:RTC_DATA_ATTR\s+)?(?:static\s+)?"
                        r"(?:bool|int|uint8_t|uint32_t|uint64_t|double|float|time_t|unsigned\s+long|ActivityState|"
                        r"CadenceReason)\s*$")
# Where each field is defined, if not logger.ino: only there is a file-scope definition exempt.
DEFINED_IN = {"last_poll_epoch": "sleep_power.h", "heartbeat_asleep_streak": "sleep_power.h"}


def file_scope_blocks(code):
    """(open, close) of every brace block opened at file scope."""
    spans, depth, opened = [], 0, 0
    for m in re.finditer(r"[{}]", code):
        if m.group(0) == "{":
            if depth == 0:
                opened = m.start()
            depth += 1
            continue
        depth -= 1
        if depth == 0:
            spans.append((opened, m.start()))
    return spans


def writes(file_name, text, names):
    """(line, name) of every assignment to names in text."""
    code = code_only(text)
    spans = file_scope_blocks(code)
    found = []
    for name in names:
        for m in re.finditer(rf"\b{name}\b", code):
            before, after = code[:m.start()].rstrip(), code[m.end():].lstrip()
            inside = any(a < m.start() < b for a, b in spans)
            if (file_name == DEFINED_IN.get(name, "logger.ino") and not inside and DEFINITION.search(before)
                    and after.startswith("=")):
                continue  # its definition
            assigned = re.match(r"(?:[-+*/%&|^]|<<|>>)?=(?!=)|\+\+|--", after)
            stepped = before.endswith(("++", "--"))
            addressed = before.endswith("&") and not before.endswith("&&") and not re.search(r"[\w)\]]\s*&$", before)
            bound = re.search(r"&\s*\w+\s*=$", before)
            swapped = re.search(r"\bswap\s*\([^;]*$", before)
            if assigned or stepped or addressed or bound or swapped:
                found.append((code.count("\n", 0, m.start()) + 1, name))
    return sorted(found)


helpers = sorted((root / "baseLogger/Helpers").glob("*.h"))
firmware = [root / "baseLogger/logger.ino", *helpers]
# The check must see these: a chained assignment (every name in it), a write from another helper, a shadowing local.
# A file-scope definition must pass.
ENERGY_FIXTURES = (("logger.ino", "void energyFixture() {\n  charge_followup = pdw_open = pdw_polls = 0;\n}\n",
                    ["charge_followup", "pdw_open", "pdw_polls"]),
                   ("state_machine.h", "void energyFixture() {\n  recently_charging = false;\n}\n", ["recently_charging"]),
                   ("logger.ino", "void energyFixture() {\n  bool recently_charging = false;\n}\n", ["recently_charging"]),
                   ("logger.ino", "RTC_DATA_ATTR uint8_t charge_followup = 0;\n", []))
# A gate field written in logger.ino or in the file that defines it is seen; its definitions pass.
GATE_FIXTURES = (("logger.ino", "void gateFixture() {\n  skips_count++;\n  polls_count += 1;\n}\n",
                  ["skips_count", "polls_count"]),
                 ("sleep_power.h", "void gateFixture() {\n  last_poll_epoch = 0;\n  ++heartbeat_asleep_streak;\n}\n",
                  ["last_poll_epoch", "heartbeat_asleep_streak"]),
                 ("logger.ino", "RTC_DATA_ATTR static time_t last_poll_epoch = 0;\n", ["last_poll_epoch"]),
                 ("sleep_power.h", "RTC_DATA_ATTR static time_t last_poll_epoch = 0;\n"
                                   "RTC_DATA_ATTR static uint32_t heartbeat_asleep_streak = 0;\n", []),
                 ("logger.ino", "bool g_weak_rail_cold_boot = false;\nRTC_DATA_ATTR uint32_t skips_count = 0;\n", []))
# A chained assignment in logger.ino, a write from another helper and a shadowing local are seen; definitions pass.
LADDER_FIXTURES = (("logger.ino", "void ladderFixture() {\n  g_dc_logged = g_dc_refused_logged = false;\n}\n",
                    ["g_dc_logged", "g_dc_refused_logged"]),
                   ("sleep_power.h", "void ladderFixture() {\n  current_state = STANDBY;\n  g_ble_disconnect_ms++;\n}\n",
                    ["current_state", "g_ble_disconnect_ms"]),
                   ("logging.h", "void ladderFixture() {\n  ActivityState current_state = ACTIVE;\n}\n", ["current_state"]),
                   ("logger.ino", "ActivityState current_state = UNINITIALIZED;\nbool config_message_printed = false;\n", []))
# A chained assignment in another helper, a ++ in the gate and a shadowing local are seen; the definitions pass.
CADENCE_FIXTURES = (("energy_state.h", "void cadenceFixture() {\n  charge_followup = g_next_wake_minutes = 1;\n"
                                       "  g_cadence_reason = CADENCE_PDW;\n}\n", ["g_next_wake_minutes", "g_cadence_reason"]),
                    ("wake_gate.h", "void cadenceFixture() {\n  g_next_wake_minutes++;\n}\n", ["g_next_wake_minutes"]),
                    ("logger.ino", "void cadenceFixture() {\n  int g_next_wake_minutes = 1;\n}\n", ["g_next_wake_minutes"]),
                    ("logger.ino", "int g_next_wake_minutes = WAKE_PARKED_MINUTES;\n"
                                   "CadenceReason g_cadence_reason = CADENCE_TIER;\n", []))
for fixtures, names in ((ENERGY_FIXTURES, ENERGY_STORAGE), (GATE_FIXTURES, GATE_STORAGE), (LADDER_FIXTURES, LADDER_STORAGE),
                        (CADENCE_FIXTURES, CADENCE_STORAGE)):
    for file_name, fixture, expected in fixtures:
        text = next(p for p in firmware if p.name == file_name).read_text().rstrip("\n") + "\n"
        first = text.count("\n") + 1
        seen = [(number, name) for number, name in writes(file_name, text + fixture, names) if number >= first]
        if sorted(name for _, name in seen) != sorted(expected):
            sys.exit(f"FAIL: the ownership check saw {seen} in the fixture {fixture!r} on {file_name}, "
                     f"expected {expected}")

for path in firmware:
    for owner, names, issue in (("drive_state.h", DRIVE_STORAGE, "#24"), ("energy_state.h", ENERGY_STORAGE, "#25"),
                                ("wake_gate.h", GATE_STORAGE, "#26"), ("state_machine.h", LADDER_STORAGE, "#29"),
                                ("sleep_paths.h", CADENCE_STORAGE, "#29")):
        if path.name == owner:
            continue
        for number, name in writes(path.name, path.read_text(), names):
            sys.exit(f"FAIL: {path.relative_to(root)}:{number} assigns {name}; only {owner} may ({issue})")

# The OBD-alarm gate takes no drive input. A regex sees identifiers, not wrappers: a helper defined elsewhere
# that returns drive state is out of its reach, so review still matters.
alarm = code_only(tree("baseLogger/Helpers/alarm_gate.h"))
for pattern in (r"\bdrive[A-Z_]\w*", r"\bDrive\w*", r"\bDRIVE_\w*", r"\bis_currently_parked\b",
                r"\bg_last_speed_ms\b", r"\bpark_marker_owed\b", r"\blast_active\b", r"\bg_drive\w*"):
    if re.search(pattern, alarm):
        sys.exit(f"FAIL: alarm_gate.h reads drive state ({pattern}); the OBD-alarm gate takes no drive input")

# Every call into the drive region sits in a block this harness compares, so none can be added unseen.
DRIVE_CALL = r"\bdrive[A-Z]\w*\s*\("
calls = sum(len(re.findall(DRIVE_CALL, code_only(p.read_text()))) for p in firmware if p.name != "drive_state.h")
extracted = len(re.findall(DRIVE_CALL, code_only("".join(cur[k] for k in PIN_BLOCKS if k != "drive_state"))))
if calls != extracted:
    sys.exit(f"FAIL: {calls} drive_state.h calls in the firmware but {extracted} in the extracted blocks; "
             "a call outside them is not compared against SM_PIN - extract its block")

# Every call into energy_state.h sits in a block this harness compares, or at one of these sites, which the
# differential does not: (file, code line, how many such lines), each with what checks it instead.
ENERGY_CALL = (r"\b(?:energy[A-Z]\w*|resetChargeCorroboration|pdwActive|chargeStateStale|chargeWakeStateSanitize|"
               r"pdwOpenWindow|pdwArmAtLadderEnd)\s*\(")
ENERGY_UNEXTRACTED = (
    # setup()'s RTC sanitize: run_energy_checks.py pins it ahead of the streak advance; the adapter boots through it.
    ("logger", "chargeWakeStateSanitize(esp_reset_reason() == ESP_RST_DEEPSLEEP);", 1),
    # dcPromotePoke: run_energy_checks.py's Dongle runs it whole.
    ("logger", "energyOnPromote();", 1),
    ("logger", "if (!g_last_discharge_ms) energyOnDcRelease();", 1),
)


def energy_calls(text, pattern=ENERGY_CALL):
    """Calls into the energy region (or pattern's) in text, its definitions (a declaration at column 0) excluded."""
    code = code_only(text)
    return sum(1 for m in re.finditer(pattern, code)
               if not code[code.rfind("\n", 0, m.start()) + 1:m.start()][:1].strip())


# The inlined wake_gate.h counts as compared: both sides run it whole from setup()'s call site.
calls = sum(energy_calls(p.read_text()) for p in firmware if p.name != "energy_state.h")
extracted = energy_calls("".join(cur[k] for k in PIN_BLOCKS if k != "energy_state"))
for key, site, count in ENERGY_UNEXTRACTED:
    found = [l for l in cur_src[key].splitlines() if l.strip() == site]
    if len(found) != count:
        sys.exit(f"FAIL: expected {count} {site!r} in {PATHS[key]}, found {len(found)}; update ENERGY_UNEXTRACTED")
    extracted += count * len(re.findall(ENERGY_CALL, code_only(site)))
if calls != extracted:
    sys.exit(f"FAIL: {calls} energy_state.h calls in the firmware but {extracted} in the compared blocks and the "
             "named sites; a call outside them is not compared against SM_PIN - extract its block or name it")

# Likewise every call into wake_gate.h (#26), or at one of these sites: (file, code line, how many such lines).
GATE_CALL = r"\b(?:gate[A-Z]\w*|wakeGateRun)\s*\("
GATE_UNEXTRACTED = (
    # loop()'s heartbeat back-off: ENABLE_PARKED_HEARTBEAT 0 here, so nothing reads the streak; a one-off build with
    # the heartbeat on compiles it.
    ("logger", "gateOnPollResult(obd_count); // the heartbeat back-off", 1),
)
calls = sum(energy_calls(p.read_text(), GATE_CALL) for p in firmware if p.name != "wake_gate.h")
extracted = energy_calls("".join(cur[k] for k in PIN_BLOCKS if k != "wake_gate_h"), GATE_CALL)
for key, site, count in GATE_UNEXTRACTED:
    found = [l for l in cur_src[key].splitlines() if l.strip() == site]
    if len(found) != count:
        sys.exit(f"FAIL: expected {count} {site!r} in {PATHS[key]}, found {len(found)}; update GATE_UNEXTRACTED")
    extracted += count * len(re.findall(GATE_CALL, code_only(site)))
if calls != extracted:
    sys.exit(f"FAIL: {calls} wake_gate.h calls in the firmware but {extracted} in the compared blocks and the named "
             "sites; a call outside them is not compared against SM_PIN - extract its block or name it")

# Likewise every call into state_machine.h (#29), or at one of these sites: (file, code snippet, how many times it
# occurs), each with what checks it instead. The inlined header is the owner; loop()'s energy call site is compared.
LADDER_CALL = (r"\b(?:handleActivityState|changeState|getTransmissionDelay|getPollingDelay|dcMotionRecent|dcAwake[A-Z]\w*|"
               r"ladder[A-Z]\w*|backstopDue|awake(?:Holds|Phone))\s*\(")
LADDER_UNEXTRACTED = (
    # loop()'s top: the adapter's TICK runs handleActivityState() whole; run_energy_checks.py pins this order.
    ("logger", "  handleActivityState();\n  dcPromotePoke();\n  dcAwakeTrack();\n", 1),
    # silentRetryArm and dcPromotePoke: run_energy_checks.py's silent-retry group and Dongle run them whole.
    ("logger", "    polling_delay = getPollingDelay(current_state);\n", 1),
    ("logger", "  if (!g_sample_poke || !g_dc_confirmed || !dcMotionRecent()) return;\n", 1),
    # The poll and upload gates' DC cadence and first upload: run_energy_checks.py pins each once.
    ("logger", "( millis() - last_poll ) > dcAwakeDelay(current_state, polling_delay)) {", 1),
    ("logger", "? getTransmissionDelay(ACTIVE) : dcAwakeDelay(current_state, transmission_delay);\n", 1),
    ("logger", "const bool dc_first_upload = dcAwakeHeld(current_state) && last_broadcast == 0;\n", 1),
    # The console's wake-up from an idle rung: review only.
    ("logging", "  if (current_state == CHARGING || current_state == STANDBY) {\n    changeState(ACTIVE);\n", 1),
)
calls = sum(energy_calls(p.read_text(), LADDER_CALL) for p in firmware if p.name != "state_machine.h")
extracted = energy_calls("".join(cur[k] for k in PIN_BLOCKS if k != "machine"), LADDER_CALL)
for key, site, count in LADDER_UNEXTRACTED:
    if cur_src[key].count(site) != count:
        sys.exit(f"FAIL: expected {count} {site!r} in {PATHS[key]}, found {cur_src[key].count(site)}; update LADDER_UNEXTRACTED")
    extracted += count * len(re.findall(LADDER_CALL, code_only(site)))
if calls != extracted:
    sys.exit(f"FAIL: {calls} state_machine.h calls in the firmware but {extracted} in the compared blocks and the named "
             "sites; a call outside them is not compared against SM_PIN - extract its block or name it")

# Likewise every call into sleep_paths.h (#29, 4b-ii), or at one of these sites: (file, code snippet, how many times).
SLEEP_CALL = (r"\b(?:cadenceArm|cadenceReasonName|sleepReasonText|sleepFor|samplePokeSleeps|netRecoverSleep|"
              r"loop(?:WeakRail|SamplePoke)Sleep)\s*\(")
SLEEP_UNEXTRACTED = (
    # setup()'s crash-streak sleep: run_energy_checks.py's sleep-reason and wdt-streak groups run that block whole.
    ("logger", '    sleepFor(minutes, wdtStreakModemMayBeOn(), "wdt-streak"); // never returns\n', 1),
)
calls = sum(energy_calls(p.read_text(), SLEEP_CALL) for p in firmware if p.name != "sleep_paths.h")
extracted = energy_calls("".join(cur[k] for k in PIN_BLOCKS if k != "sleep_paths_h"), SLEEP_CALL)
for key, site, count in SLEEP_UNEXTRACTED:
    if cur_src[key].count(site) != count:
        sys.exit(f"FAIL: expected {count} {site!r} in {PATHS[key]}, found {cur_src[key].count(site)}; update SLEEP_UNEXTRACTED")
    extracted += count * len(re.findall(SLEEP_CALL, code_only(site)))
if calls != extracted:
    sys.exit(f"FAIL: {calls} sleep_paths.h calls in the firmware but {extracted} in the compared blocks and the named "
             "sites; a call outside them is not compared against SM_PIN - extract its block or name it")

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

# The adapter's detectMovement() stub stamps the leash and the motion as mems.h does, on motion only.
if code_only(tree("baseLogger/Helpers/mems.h")).count("last_active = g_last_motion_ms = millis();") != 1:
    sys.exit("FAIL: mems.h's detectMovement() must stamp last_active = g_last_motion_ms = millis() once, as the adapter does")
# Two wires the adapter bypasses (it sets the inputs directly): the sampled motion windows must reach the
# accumulator, and an OBD answer must stamp the car-on evidence.
if not re.search(r"motion_sampled = !h\.ota && !h\.dump && current_state != UNINITIALIZED;\s*"
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
