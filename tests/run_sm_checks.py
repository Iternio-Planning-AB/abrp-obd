"""Differential checks for the park/drive decision and the poll-side energy block (state machine #23, steps 0a
#28 and 0b-1 #81).

Two regions are compared. The drive region is drive_state.h and the blocks that call it: the speed loop, the
GNSS branch and the time re-latch, the is_parked publish, the OTA/dump hold, changeState, the frame_has_data
buffer block with the queue helper, the pre-sleep marker and the setup() marker restore. The energy region is
the poll's rail refresh and the contiguous block in loop() from the is_charging verdict through dcAwakeTrack():
the release latch and silent hold, the taper stall count, the DC session, the next-wake cadence, the fast budget
and cap, corroboration and the stale guard, with the helpers it calls (resetChargeCorroboration, pdwActive,
chargeStateStale, the dcAwake* functions). Both are extracted with the same anchors from two places: OLD from the
pinned commit (git show), CUR from the working tree. The context around them (the setup() sanitize and wake gate
with its skip path, the post-drive window at the ladder end, a sample poke's sleep after its poll, the backstop,
the delays) comes from the working tree for both sides, so a deliberate change there cannot fail the differential; a property checks that the shared
backstop never sleeps a driving car.
tests/sm_checks.cpp compiles each side into its own namespace around the same harness
(tests/sm/adapter_*.inc) and runs identical event sequences through both, asserting equal outputs after
every event: the latch, the leash and hold times, the marker debt and its crash copy, published is_parked,
buffered frames, alarm speed notes, freezes, every energy RTC field and RAM output, the next-wake minutes, and
the exact logEvent and Serial sequences. It also checks the weak-rail gate and the max-awake backstop properties,
on the working tree's gate and backstop. Groups: enumerate (exhaustive over a drive and an energy alphabet,
--depth, default 5), random (fixed seeds), scenarios (named field and charge cases), crash (CUR alone: a panic,
also inside the pre-sleep flush, never loses the marker debt; an absolute property, not a comparison), rules
(CUR alone: absolute checks of the #6 driving rules), or all. A Charge or Silent poll keeps the wake's rail and
motion; any answered poll stamps the last OBD answer.
Before compiling, it checks that only drive_state.h assigns the park/drive storage, that alarm_gate.h names no
drive state, and that every drive call is in an extracted block.

The pin. SM_PIN is the main commit a step is measured against: a refactor step must leave both regions equal to
it. Re-pin only when a behaviour change to a compared region lands on main between steps, to the merge-base of
the step being measured; it is a commit on main, so it stays reachable. A clone without it fails with a message,
never a skip.

What it cannot prove: 32-bit millis() wrap (the host unsigned long is 64-bit); any feature-flag set other
than the config.h defaults with PRINT 0 (lines under #if PRINT and other branches are not compiled); code
outside the extracted blocks (the rest of handleActivityState, dcPromotePoke and motion inside a wake, the
SAMPLE_POLL line, the modem, detectMovement timing, RTC retention, hardware). The setup() wake gate as a compared region is #82; the
energy properties and the multi-region layer are #83.
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
from extract import block, find, wake_gate  # noqa: E402
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
         "motion_stats": "baseLogger/Helpers/motion_stats.h", "config": "baseLogger/config.h"}


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
GATE_THROUGH = "    if (g_sample_poke) g_rail_before_poke = rail;"
GATE_SKIP_START = "      g_next_wake_minutes = WAKE_PARKED_MINUTES; // skip = parked"
GATE_SKIP_END = "      enterDeepSleep(false);"
POKE_SLEEP = "    if (g_sample_poke && !maintenance_mode && !dcAwakeHeld(current_state)) {"


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
    skip = find(logger, GATE_SKIP_START, what=where["logger"])
    for header in ("motion_stats", "drive_state"):
        if src[header].count("#pragma once\n") != 1 or '#include "' in src[header]:
            sys.exit(f"FAIL: {where[header]} must be one #pragma once and no project include, to inline it")
    return {
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
        "gate": wake_gate(logger, where["logger"], through=GATE_THROUGH),
        "queue_helper": src["upload"][upload_start:upload_end],
        "energy_rail": logger[rail:find(logger, "\n#endif", rail, where["logger"]) + 1],
        "energy": energy.replace(LOGGED_DCFC, ""),
        "energy_helpers": block(logger, "void resetChargeCorroboration() {", where["logger"]) +
                          line(logger, "static const uint64_t POST_DRIVE_WINDOW_US = ", where["logger"]) +
                          block(logger, "bool pdwActive(uint64_t now_us) {", where["logger"]) +
                          block(logger, "bool chargeStateStale(uint64_t now_us) {", where["logger"]) +
                          machine[dc_start:find(machine, "\n// Open the post-drive window", dc_start, where["machine"]) + 1],
        "sanitize": block(logger, "void chargeWakeStateSanitize(bool deep_sleep) {", where["logger"]),
        "skip": logger[skip:find(logger, GATE_SKIP_END, skip, where["logger"])],
        "ladder_end": machine[find(machine, "bool pdwOpenWindow() {", what=where["machine"]):
                              find(machine, "\n/*\n  Use movement and BLE", what=where["machine"]) + 1],
    }


SIDE = """{enum}
{buffer_cap}
#include "adapter_pre.inc"
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
void sm_skip() {{
{skip}}}
bool sm_poke_sleeps() {{ return {poke_sleep}; }}
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
bool sm_gate(float rail, bool is_cold, bool is_motion, bool awake, bool heartbeat_due) {{
{gate}  return should_poll;
}}
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

# The differential covers the drive region only. OLD takes the blocks that write drive state from SM_PIN - the
# speed loop, the GNSS branch and the time re-latch, the is_parked publish, the OTA/dump hold, changeState, the
# frame_has_data buffer block and the pre-sleep marker - and the rest (the wake gate, the backstop, the
# delays) from the working tree, as CUR does. A deliberate change to that shared context is then identical
# on both sides and cannot fail the differential.
DRIVE_BLOCKS = ("motion_stats", "drive_state", "drive", "emplace", "session", "marker", "change_state", "buffer", "queue_helper", "restore")
# Step 0b-1 (#81) adds the poll-side energy region the same way: the rail refresh, the energy block and the
# helpers it calls come from SM_PIN for OLD. What setup() and the ladder do to energy state (the sanitize, the
# gate and its skip path, the post-drive window) is shared context until #82.
ENERGY_BLOCKS = ("energy_rail", "energy", "energy_helpers")
PIN_BLOCKS = DRIVE_BLOCKS + ENERGY_BLOCKS
old = dict(cur)
for name in PIN_BLOCKS:
    old[name] = pin[name]

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
        pattern = rf"^{prefix}(?:volatile )?{re.escape(type_)} {name} = {re.escape(value)};"
        for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
            if label == "SM_PIN" and name == "maintenance_mode":
                continue
            if not re.search(pattern, src["logger"], re.M):
                sys.exit(f"FAIL: {region} {name} = {value} does not match its definition in logger.ino ({label})")



def code_only(text):
    """Text with string/char literals and comments blanked (newlines kept), so a check sees code only."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


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
