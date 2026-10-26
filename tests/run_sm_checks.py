"""Differential checks for the park/drive decision (state machine #23, step 0a #28).

The differential covers the drive region; shared context comes from the working tree. The blocks that write
drive state (the speed loop, the GNSS branch and the time re-latch, the is_parked publish, the OTA/dump
hold, changeState, the frame_has_data buffer block and the pre-sleep marker) are extracted with the same
anchors from two places: OLD from the pinned commit (git show), CUR from the working tree. The context
around them (the setup() wake gate, the backstop, the delays) comes from the working tree for both sides,
so a deliberate change there cannot fail the differential; a property checks that the shared backstop
never sleeps a driving car.
tests/sm_checks.cpp compiles each side into its own namespace around the same harness
(tests/sm/adapter_*.inc) and runs identical event sequences through both, asserting equal outputs after
every event: the latch, the leash and hold times, the marker debt and its crash copy, published is_parked,
buffered frames, alarm speed notes, freezes, and the exact logEvent and Serial sequences. It also checks the
weak-rail gate and the max-awake backstop properties, on the working tree's gate and backstop. Groups: enumerate (exhaustive, --depth, default 5), random (fixed seeds),
scenarios (named field cases), crash (CUR alone: a panic, also inside the pre-sleep flush, never loses the
marker debt; SM_PIN has no restore to compare against), or all. Before compiling, it checks that only drive_state.h assigns the
park/drive storage, that alarm_gate.h names no drive state, and that every drive call is in an extracted block.

The pin. SM_PIN is the main commit a refactor step is measured against. The step 1 (#24) and step 2 (#6)
branches use the same pin: the code they move is unchanged there. Re-pin only when a behaviour change to
the drive writers lands on main between steps, to the merge-base of the step being measured; merges into
main are merge commits, so the pin stays reachable. A clone without it fails with a message, never a skip.

What it cannot prove: 32-bit millis() wrap (the host unsigned long is 64-bit); any feature-flag set other
than the config.h defaults with PRINT 0 (lines under #if PRINT and other branches are not compiled); code
outside the extracted blocks (the rest of handleActivityState, the modem, detectMovement timing, RTC
retention, hardware). Energy and wake surfaces, the fast-minutes and alarm properties and a trace format
are step 0b.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

SM_PIN = "6457b2a1a355a43b57ae877d40bae2481afad49c"
# Headers the CUR side includes ahead of its extracted blocks (inside namespace sm_cur).
CUR_HEADERS = ["drive_state.h"]

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
         "logging": "baseLogger/Helpers/logging.h",
         "config": "baseLogger/config.h"}


# Code anchors only (never comment text), each failing with its own text when the code it names has moved.
DRIVE_START = ('    for (const auto& res : current_frame) {\n'
               '      if (( res.first == "speed" || res.first == "vehicle_reported_speed" )')
DRIVE_END = "#if ENABLE_NETWORK\n    const double poll_utc = uploadSendUtc();"


def recipe(src, side):
    logger, machine = src["logger"], src["machine"]
    where = {k: f"{PATHS[k]} ({side})" for k in PATHS}
    start = find(logger, DRIVE_START, what=where["logger"])
    queue_helper = ""
    if side == "tree":
        upload_start = find(src["upload"], "bool isSyntheticParkMarker(", what=where["upload"])
        upload_end = find(src["upload"], "\n#if ENABLE_NETWORK", upload_start, where["upload"])
        queue_helper = src["upload"][upload_start:upload_end]
    return {
        "enum": block(logger, "enum ActivityState {", where["logger"]).rstrip() + ";\n",
        "buffer_cap": line(logger, "const size_t MAX_UPLOAD_BUFFER = ", where["logger"]),
        "queue_helper": queue_helper,
        "state_name": block(src["logging"], "static const char* stateName(ActivityState s) {", where["logging"]),
        "delays": block(machine, "unsigned long getTransmissionDelay(", where["machine"]) +
                  block(machine, "unsigned long getPollingDelay(", where["machine"]),
        "change_state": block(machine, "void changeState(ActivityState new_state) {", where["machine"]),
        "drive": logger[start:find(logger, DRIVE_END, start, where["logger"])],
        "emplace": line(logger, '    current_frame.emplace_back("is_parked"', where["logger"]),
        "buffer": block(logger, "    if (frame_has_data) {", where["logger"]),
        "marker": block(src["sleep"], "  if (rtc.getEpoch() > 1e9 && is_currently_parked", where["sleep"]),
        "session": block(machine, "if (g_ota_active || g_logdump_active) {", where["machine"]),
        "backstop": block(machine, "if (!maintenance_mode && !g_ota_active && !g_logdump_active", where["machine"]),
        "gate": wake_gate(logger, where["logger"]),
    }


SIDE = """{enum}
{buffer_cap}
#include "adapter_pre.inc"
{headers}
{queue_helper}
{state_name}
{delays}
{change_state}
void sm_drive_poll(OBDVector& current_frame) {{
{drive}}}
void sm_emplace(OBDVector& current_frame) {{
{emplace}}}
void sm_buffer(OBDVector& current_frame, bool frame_has_data) {{
  int g_geo_queued = 0, poll_geo = 0;
  (void)g_geo_queued;
  const bool ble_relayed = false; // no phone in this harness; OLD ignores both
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

old_src = {k: pinned(p) for k, p in PATHS.items()}
cur_src = {k: tree(p) for k, p in PATHS.items()}
pin, cur = recipe(old_src, "SM_PIN"), recipe(cur_src, "tree")

# The crash copy of the marker debt (#49, Helpers/wdt_streak.h): the setup() restore, the set after the buffer
# block and the clear at sleep. Context from the tree on both sides; each must sit where a crash cannot slip
# between it and what it guards.
logger, sleep = cur_src["logger"], cur_src["sleep"]
advance = line(logger, "  wdt_reset_count = wdtStreakAdvance(", PATHS["logger"])
cur["restore"] = line(logger, "  if (esp_reset_reason() != ESP_RST_DEEPSLEEP && wdtStreakMarkerOwed())", PATHS["logger"])
# The restore is a drive writer SM_PIN does not have: OLD runs none, CUR the tree's line. The differential's
# alphabet has no crash, where the restore is a no-op on both sides; the crash group runs it on CUR alone.
if "wdtStreakMarkerOwed" in old_src["logger"]:
    sys.exit("FAIL: SM_PIN already restores the marker debt; extract it from the pin as a drive block")
if advance + cur["restore"] not in logger:
    sys.exit("FAIL: the marker restore must directly follow the streak advance in setup(), ahead of every sleep")
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
DRIVE_BLOCKS = ("drive", "emplace", "session", "marker", "change_state", "buffer")
old = dict(cur)
old["queue_helper"] = ""
for name in DRIVE_BLOCKS:
    old[name] = pin[name]
old["restore"] = ""

old_drive = [pin[name] for name in DRIVE_BLOCKS]

# OLD must never pull a project header into its namespace: #pragma once would hand it CUR's copy.
for name, text in zip(DRIVE_BLOCKS, old_drive):
    if '#include "' in text:
        sys.exit(f"FAIL: OLD extraction {name} includes a project header; isolate it first (step 0b)")


def defines(config):
    found = {}
    for m in re.finditer(r"^#define (\w+)\b.*(?:\\\n.*)*", config, re.M):
        found.setdefault(m.group(1), m.group(0))
    return found


# Both sides compile against the tree's config.h, so every macro OLD uses must be byte-identical at the pin.
old_defines, cur_defines = defines(old_src["config"]), defines(cur_src["config"])
NAME = r"\b[A-Z][A-Z0-9_]{2,}\b"
used, pending = set(), set(re.findall(NAME, "".join(old_drive)))
while pending:  # through the macros' own bodies: ENABLE_NETWORK is (ENABLE_CELL || ENABLE_WIFI)
    macro = pending.pop()
    if macro in used or macro not in cur_defines:
        continue
    used.add(macro)
    pending |= set(re.findall(NAME, re.sub(r"^#define \w+", "", cur_defines[macro])))
# ENABLE_NETWORK gained its parentheses (#62). Every use OLD reaches is a bare `#if ENABLE_NETWORK`, where they
# change nothing; strip them for this one macro so the compare stays byte-exact for all the others. Drop this
# once SM_PIN moves past the commit that added them.
def unwrapped(macro, text):
    return text.replace("(ENABLE_CELL || ENABLE_WIFI)", "ENABLE_CELL || ENABLE_WIFI") if macro == "ENABLE_NETWORK" else text
for macro in sorted(used):
    if unwrapped(macro, old_defines.get(macro, "")) != unwrapped(macro, cur_defines[macro]):
        sys.exit(f"FAIL: config.h {macro} differs between SM_PIN and the tree; the OLD side would run with the new value")

# The harness resets each firmware global to a literal; that literal must be the firmware's initialiser: in
# the tree for all of them, and at SM_PIN too except for those only the shared context reads.
CONTEXT_GLOBALS = {"recently_charging", "taper_confirm_wakes", "charge_end_polled", "g_taper_confirm", "maintenance_mode"}
adapter = tree("tests/sm/adapter_pre.inc")
for region, prefix in (("SM_RAM", ""), ("SM_RTC", "RTC_DATA_ATTR ")):
    body = re.search(rf"^#define {region}\(X\) \\\n((?:.*\\\n)*.*)", adapter, re.M).group(1)
    for type_, name, value in re.findall(r"X\(([\w ]+), (\w+), ([^)]+)\)", body):
        pattern = rf"^{prefix}(?:volatile )?{re.escape(type_)} {name} = {re.escape(value)};"
        for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
            if label == "SM_PIN" and name in CONTEXT_GLOBALS:
                continue
            if not re.search(pattern, src["logger"], re.M):
                sys.exit(f"FAIL: {region} {name} = {value} does not match its definition in logger.ino ({label})")



def code_only(text):
    """Text with string/char literals and comments blanked (newlines kept), so a check sees code only."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


# Step 1 (#24): drive_state.h is the only code that assigns the park/drive storage. An assignment here is
# =, a compound assignment, ++/--, taking the address or binding a reference (an alias could assign), or
# swap(); matched on the whole file, so a statement split over lines is still seen.
DRIVE_STORAGE = ("is_currently_parked", "g_last_speed_ms", "park_marker_owed")
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
extracted = len(re.findall(DRIVE_CALL, code_only("".join(cur.values()))))
if calls != extracted:
    sys.exit(f"FAIL: {calls} drive_state.h calls in the firmware but {extracted} in the extracted blocks; "
             "a call outside them is not compared against SM_PIN - extract its block")

energy = tree("tests/energy_checks.cpp")
field = "".join(block(energy, f"const std::vector<Sample> {n} = ", "tests/energy_checks.cpp").rstrip() + ";\n" for n in ("DRIVE_0930", "DRIVE_1012"))

compiler = shlex.split(os.environ.get("CXX", "clang++"))
with tempfile.TemporaryDirectory(prefix="obd-sm-checks-") as temporary:
    build = Path(temporary)
    (build / "old_side.inc").write_text(SIDE.format(headers="", **old))
    cur_headers = "".join(f'#include "{h}"\n' for h in CUR_HEADERS)
    (build / "cur_side.inc").write_text(SIDE.format(headers=cur_headers, **cur))
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
