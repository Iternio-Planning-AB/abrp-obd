"""Differential checks for the park/drive decision (state machine #23, step 0a #28).

The drive code is extracted from two places with the same anchors: OLD from the pinned commit (git show),
CUR from the working tree. tests/sm_checks.cpp compiles each into its own namespace around the same harness
(tests/sm/adapter_*.inc) and runs identical event sequences through both, asserting equal outputs after
every event: the latch, the leash and hold times, the marker debt, published is_parked, buffered frames,
alarm speed notes, freezes, and the exact logEvent and Serial sequences. It also checks the weak-rail gate
and the max-awake backstop properties. Groups: enumerate (exhaustive, --depth), random (fixed seeds),
scenarios (named field cases), or all.

The pin. SM_PIN is the main commit a refactor step is measured against. The step 1 (#24) and step 2 (#6)
branches use the same pin: the code they move is unchanged there. Re-pin only when a behaviour change to
the extracted code lands on main between steps, to the merge-base of the step being measured; merges into
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
CUR_HEADERS = []

root = Path(__file__).resolve().parents[1]
if subprocess.run(["git", "merge-base", "--is-ancestor", SM_PIN, "HEAD"], cwd=root).returncode != 0:
    sys.exit(f"FAIL: SM_PIN {SM_PIN} is not an ancestor of HEAD: fetch main, or re-pin (see this file's header)")


def pinned(path):
    return subprocess.check_output(["git", "show", f"{SM_PIN}:{path}"], cwd=root, text=True)


def tree(path):
    return (root / path).read_text()


def block(text, opener):
    start = text.index(opener)
    depth, i = 0, text.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
        if depth == 0:
            return text[start:i] + "\n"


def line(text, prefix):
    return next(l for l in text.splitlines() if l.startswith(prefix)) + "\n"


PATHS = {"logger": "baseLogger/logger.ino", "machine": "baseLogger/Helpers/state_machine.h",
         "sleep": "baseLogger/Helpers/sleep_power.h", "logging": "baseLogger/Helpers/logging.h",
         "config": "baseLogger/config.h"}


def recipe(src):
    logger, machine = src["logger"], src["machine"]
    start = next(m.start() for m in re.finditer(r"^.*// is_parked: any reported speed > 0 means we are driving",
                                                logger, re.M))
    drive = logger[start:logger.index("    // Before is_parked is appended", start)]
    gate_start = logger.index("    bool weak_rail = (rail >= IO36_LOCKOUT_LOW")
    gate = logger[gate_start:logger.index("heartbeat_due);\n", gate_start) + len("heartbeat_due);\n")]
    return {
        "enum": block(logger, "enum ActivityState {").rstrip() + ";\n",
        "state_name": block(src["logging"], "static const char* stateName(ActivityState s) {"),
        "delays": block(machine, "unsigned long getTransmissionDelay(") + block(machine, "unsigned long getPollingDelay("),
        "change_state": block(machine, "void changeState(ActivityState new_state) {"),
        "drive": drive,
        "emplace": line(logger, '    current_frame.emplace_back("is_parked"'),
        "buffer": block(logger, "    if (frame_has_data) {"),
        "marker": block(src["sleep"], "  if (rtc.getEpoch() > 1e9 && is_currently_parked"),
        "session": block(machine, "if (g_ota_active || g_logdump_active) {"),
        "backstop": block(machine, "if (!maintenance_mode && !g_ota_active && !g_logdump_active"),
        "gate": gate,
    }


SIDE = """{enum}
#include "adapter_pre.inc"
{headers}
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
{buffer}}}
void sm_marker() {{
{marker}}}
void sm_session() {{
{session}}}
void sm_backstop() {{
{backstop}}}
bool sm_gate(float rail, bool is_cold, bool is_motion, bool awake, bool heartbeat_due) {{
{gate}  return should_poll;
}}
#include "adapter_post.inc"
"""

old_src = {k: pinned(p) for k, p in PATHS.items()}
cur_src = {k: tree(p) for k, p in PATHS.items()}
old, cur = recipe(old_src), recipe(cur_src)

# OLD must never pull a project header into its namespace: #pragma once would hand it CUR's copy.
for name, text in old.items():
    if '#include "' in text:
        sys.exit(f"FAIL: OLD extraction {name} includes a project header; isolate it first (step 0b)")


def defines(config):
    found = {}
    for m in re.finditer(r"^#define (\w+)\b.*(?:\\\n.*)*", config, re.M):
        found.setdefault(m.group(1), m.group(0))
    return found


# Both sides compile against the tree's config.h, so every macro OLD uses must be byte-identical at the pin.
old_defines, cur_defines = defines(old_src["config"]), defines(cur_src["config"])
used = set(re.findall(r"\b[A-Z][A-Z0-9_]{2,}\b", "".join(old.values())))
for macro in sorted(used & set(cur_defines)):
    if old_defines.get(macro) != cur_defines[macro]:
        sys.exit(f"FAIL: config.h {macro} differs between SM_PIN and the tree; the OLD side would run with the new value")

# The harness resets each firmware global to a literal; that literal must be the firmware's initialiser.
adapter = tree("tests/sm/adapter_pre.inc")
for region, prefix in (("SM_RAM", ""), ("SM_RTC", "RTC_DATA_ATTR ")):
    body = re.search(rf"^#define {region}\(X\) \\\n((?:.*\\\n)*.*)", adapter, re.M).group(1)
    for type_, name, value in re.findall(r"X\(([\w ]+), (\w+), ([^)]+)\)", body):
        pattern = rf"^{prefix}(?:volatile )?{re.escape(type_)} {name} = {re.escape(value)};"
        for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
            if not re.search(pattern, src["logger"], re.M):
                sys.exit(f"FAIL: {region} {name} = {value} does not match its definition in logger.ino ({label})")
for label, src in (("SM_PIN", old_src), ("tree", cur_src)):
    if "const size_t MAX_UPLOAD_BUFFER = 120;" not in src["logger"]:
        sys.exit(f"FAIL: MAX_UPLOAD_BUFFER is no longer 120 in logger.ino ({label}); update adapter_pre.inc")

energy = tree("tests/energy_checks.cpp")
field = "".join(block(energy, f"const std::vector<Sample> {n} = ").rstrip() + ";\n" for n in ("DRIVE_0930", "DRIVE_1012"))

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
