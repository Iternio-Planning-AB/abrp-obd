"""Real cellular init/registration/reconnect against fake UART and a native 32-bit millis().

No device or network access. Unrelated HTTP/GPS operations are link stubs; the exercised modem
begin, SIM checks, AT exchange, setup, and the complete CellWrapper implementation are production.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests/support"))
from extract import block, find  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("group", nargs="?", default="all",
                    choices=["all", "ordinary", "late", "fast", "parked", "safety", "csq"])
args = parser.parse_args()
source = (root / "libraries/FreematicsPlus/FreematicsNetwork.cpp").read_text()
header = (root / "libraries/FreematicsPlus/FreematicsNetwork.h").read_text()
logger = (root / "baseLogger/logger.ino").read_text()
sleep = (root / "baseLogger/Helpers/sleep_power.h").read_text()
config = (root / "baseLogger/config.h").read_text()
drive_state = (root / "baseLogger/Helpers/drive_state.h").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))


def code_lines(text):
    """The code lines of text, stripped, with comments, blank lines and preprocessor directives left out."""
    lines = (re.sub(r"\s*//.*", "", l).strip() for l in text.splitlines())
    return [l for l in lines if l and not l.startswith("#")]


# #110's pre-sleep timings sit at fixed points of enterDeepSleep (compiled below with ENABLE_LOGGING 0, where they must
# vanish). prep= runs from the weak-rail override (after the OTA/dump guard and the override run_sm_checks.py pins
# together) to the BLE teardown, through the marker clear; open= is connect() alone; the FLUSH line stays as it was and
# FLUSHSTAT, its one companion, follows it on the connected branch only. No host runner compiles these with logging
# on, so both summary lines are pinned whole here; their widths are run_log_diagnostics_checks.py's stat-widths.
sleep_fn = block(sleep, "void enterDeepSleep(", "sleep_power.h")
OVERRIDE = "  if (g_weak_rail_cold_boot) modem_on = false;\n"
OPEN = "      if (!network.connect()) {\n"
FLUSH = ('        logEvent("FLUSH %s http=%d sent=%d left=%d up=%ds %s", ok ? "ok" : "partial", g_last_http,\n'
         '          n - (int)obd_result_vector.size(), (int)obd_result_vector.size(), up_s,\n'
         '          network.lastOpenTrace().c_str());\n')
TEARDOWN = "  bleDeinitForSleep();"
for anchor in (OVERRIDE, OPEN, FLUSH, TEARDOWN):
    if sleep_fn.count(anchor) != 1:
        sys.exit(f"FAIL: expected one {anchor.strip()!r} in enterDeepSleep")
after_override = sleep_fn.index(OVERRIDE) + len(OVERRIDE)
if code_lines(sleep_fn[after_override:])[0] != "const uint32_t prep_start_ms = (uint32_t)millis();":
    sys.exit("FAIL: WAKESTAT's prep start must be the first statement after the weak-rail modem override")
if code_lines(sleep_fn[:sleep_fn.index(TEARDOWN)])[-2:] != [
        "if (!park_marker_owed && !unsent) wdtStreakMarker(false);", "const uint32_t prep_end_ms = (uint32_t)millis();"]:
    sys.exit("FAIL: WAKESTAT's prep end must follow the marker clear, directly before bleDeinitForSleep()")
if code_lines(sleep_fn[:sleep_fn.index(OPEN)])[-1] != "const uint32_t flush_open_ms = (uint32_t)millis();":
    sys.exit("FAIL: FLUSHSTAT's open stamp must directly precede the flush's network.connect()")
# The connected branch: drain= starts at its first statement, with the counters' snapshot, and ends right after the
# drain, before the FLUSH line's own flash write.
CONNECTED = "      } else {\n"
connected = sleep_fn.index(CONNECTED, sleep_fn.index(OPEN))
branch = code_lines(sleep_fn[connected + len(CONNECTED):sleep_fn.index("        network.disconnect();\n", connected)])
if branch[:2] != ["const uint32_t flush_drain_ms = (uint32_t)millis();",
                  "const int posts = g_upload_posts, accepted = g_upload_accepted, frames_ok = g_upload_frames_ok;"]:
    sys.exit(f"FAIL: the connected flush branch must open with the drain stamp and the counters' snapshot: {branch[:2]}")
drained = branch.index("bool ok = uploadBuffered(true);") if "bool ok = uploadBuffered(true);" in branch else -1
if drained < 0 or branch[drained + 1:drained + 2] != ["const uint32_t flush_done_ms = (uint32_t)millis();"]:
    sys.exit("FAIL: the drain's end stamp must directly follow uploadBuffered(true)")
# The two lines whole, arguments and all: the values are the diagnostic. Comment lines may sit between.
FLUSHSTAT = ('        logEvent("FLUSHSTAT open=%lums drain=%lums posts=%d/%d frames=%d/%d",\n'
             '          (unsigned long)(flush_drain_ms - flush_open_ms), (unsigned long)(flush_done_ms - flush_drain_ms),\n'
             '          g_upload_accepted - accepted, g_upload_posts - posts, g_upload_frames_ok - frames_ok, n);\n')
WAKESTAT = ('  flushMotionStats();\n#endif\n'
            '  if (g_loop_gap.seeded) loopGapNote(g_loop_gap, prep_start_ms);\n'
            '  logEvent("WAKESTAT prep=%lums loopmax=%lums at=%lu loops=%lu", (unsigned long)(prep_end_ms - prep_start_ms),\n'
            '    (unsigned long)g_loop_gap.max_ms, (unsigned long)g_loop_gap.max_at, (unsigned long)g_loop_gap.n);\n')
uncommented = "".join(l for l in sleep_fn.splitlines(True) if not l.lstrip().startswith("//"))
if (sleep_fn.count('logEvent("FLUSHSTAT ') != 1 or (FLUSH + FLUSHSTAT) not in uncommented
        or not connected < sleep_fn.index(FLUSH) < sleep_fn.index("        network.disconnect();\n", connected)):
    sys.exit("FAIL: FLUSHSTAT, its only line, must directly follow the unchanged FLUSH line, exactly as pinned here")
if (sleep_fn.count('logEvent("WAKESTAT ') != 1 or uncommented.count(WAKESTAT) != 1
        or not uncommented.index(WAKESTAT) < uncommented.index('  logEvent("SLEEP enter')):
    sys.exit("FAIL: WAKESTAT, its only line, must follow flushMotionStats() and the sleep entry's closing loop-gap "
             "visit, ahead of SLEEP enter, exactly as pinned here")

with tempfile.TemporaryDirectory(prefix="obd-modem-checks-") as temporary:
    build = Path(temporary)
    start = find(header, "struct commandResponse", what="FreematicsNetwork.h")
    (build / "modem_types.inc").write_text(header[start:find(header, "class HTTPClientClass", start, "FreematicsNetwork.h")])
    (build / "FreematicsPlus.h").write_text('#pragma once\n#include "modem_transport_fixture.h"\n')
    functions = ["bool CellSIMCOM::begin(", "void CellSIMCOM::end(", "bool CellSIMCOM::check(",
                 "bool CellSIMCOM::checkSIM(", "commandResponse CellSIMCOM::sendCommand(",
                 "void HTTPClientSIM::end(", "bool HTTPClientSIM::setup("]
    (build / "modem_production.inc").write_text("\n".join(block(source, function, "FreematicsNetwork.cpp") for function in functions))
    start = find(logger, "  bool net_ok = false;", what="logger.ino")
    (build / "modem_boot.inc").write_text(logger[start:find(logger, "#if ENABLE_WIFI", start, "logger.ino")])
    (build / "modem_sleep.inc").write_text(block(sleep, "void enterDeepSleep(", "sleep_power.h"))
    (build / "modem_config.inc").write_text("\n".join(re.findall(
        r"^#define (?:CELL_REGISTRATION_\w+|UPLOAD_MIN_FREE_HEAP|UPLOAD_MIN_MAX_BLOCK|UPLOAD_MIN_FREE_PSRAM)\b.*",
        config, re.MULTILINE)))
    upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
    upload_start = find(upload, "bool isSyntheticParkMarker(", what="network_upload.h")
    upload_end = find(upload, "\n#if ENABLE_NETWORK", upload_start, "network_upload.h")
    (build / "upload_queue.inc").write_text(upload[upload_start:upload_end])
    (build / "drive_marker_restored.inc").write_text(
        block(drive_state, "inline void driveMarkerRestored() {", "drive_state.h"))
    (build / "drive_marker_queued.inc").write_text(
        block(drive_state, "inline void driveMarkerQueued() {", "drive_state.h"))
    reconnect = re.findall(r"^  if \(.*!network\.connected\(\).* \{", logger, re.MULTILINE)
    if len(reconnect) != 1:
        sys.exit("FAIL: expected one production offline reconnect gate")
    (build / "modem_reconnect.inc").write_text(block(logger, reconnect[0], "logger.ino"))
    start = find(logger, "const unsigned long WIFI_RETRY_MS", what="logger.ino")
    (build / "modem_cadence.inc").write_text(logger[start:].splitlines()[0])
    executable = build / "modem_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
        "-Wno-sign-compare", "-Wno-unused-private-field", "-Wno-deprecated-declarations", "-DPRINT=0",
        "-I", str(build), "-I", str(root / "tests"), "-I", str(root / "libraries/NetworkWrapper"),
        "-I", str(root / "libraries/nlohmann"), str(root / "tests/modem_checks.cpp"),
        str(root / "libraries/NetworkWrapper/cellwrapper.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    subprocess.run([str(executable), args.group], env=environment, check=True, timeout=30)
