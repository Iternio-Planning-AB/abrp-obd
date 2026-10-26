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
                    choices=["all", "ordinary", "late", "fast", "parked", "safety"])
args = parser.parse_args()
source = (root / "libraries/FreematicsPlus/FreematicsNetwork.cpp").read_text()
header = (root / "libraries/FreematicsPlus/FreematicsNetwork.h").read_text()
logger = (root / "baseLogger/logger.ino").read_text()
sleep = (root / "baseLogger/Helpers/sleep_power.h").read_text()
config = (root / "baseLogger/config.h").read_text()
drive_state = (root / "baseLogger/Helpers/drive_state.h").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))

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
        r"^#define (?:CELL_REGISTRATION_\w+|UPLOAD_MIN_FREE_HEAP|UPLOAD_MIN_MAX_BLOCK)\b.*",
        config, re.MULTILINE)))
    upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
    upload_start = find(upload, "bool queueUploadFrame(", what="network_upload.h")
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
