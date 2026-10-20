import argparse
import csv
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "clang++"))
parser = argparse.ArgumentParser()
parser.add_argument("group", nargs="?", default="all", choices=["all", "schema", "logging", "schema-failures", "ble-write", "motion"])
parser.add_argument("--capture-helper", type=Path)
arguments = parser.parse_args()
# The ble-write group tests the summary; this keeps blewrapper.cpp using it. A log or print call that names a
# raw write buffer (input/value) other than through bleWriteSummary(), or a client key other than through
# bleKeyForLog(), would put the AUTH secret or tlm_push_token into the flash log or onto USB serial.
if arguments.group in ("all", "ble-write"):
    import re
    ble_source = (root / "libraries/BLEWrapper/blewrapper.cpp").read_text()
    calls = re.findall(r"(?:logEvt|logError|Serial\.printf)\((.*?)\);", ble_source, re.S)
    leaks = [call for call in calls
             if re.search(r"\b(input|value|message_key)\b",
                          re.sub(r'bleWriteSummary\((input|value)\)|bleKeyForLog\(message_key\)|"(?:\\.|[^"\\])*"', "", call))]
    if leaks or ble_source.count("bleWriteSummary(") < 2:
        raise AssertionError(f"blewrapper.cpp logs a raw BLE write: {leaks}")
with tempfile.TemporaryDirectory(prefix="obd-log-diagnostics-") as temporary:
    build = Path(temporary)
    source = (root / "libraries/DataLogger/datalogger.cpp").read_text()
    (build / "datalogger.inc").write_text(source)
    baseline_logger = subprocess.check_output(
        ["git", "show", "eacc642:libraries/DataLogger/datalogger.cpp"], cwd=root, text=True)
    baseline_logging = subprocess.check_output(
        ["git", "show", "eacc642:baseLogger/Helpers/logging.h"], cwd=root, text=True)
    begin = baseline_logger.index("bool DataLogger::begin(")
    end = baseline_logger.index("\nbool DataLogger::writeLine", begin)
    (build / "legacy_begin.inc").write_text(
        baseline_logger[begin:end].replace("DataLogger::begin(", "DataLogger::beginBaseline(", 1))
    header = (root / "libraries/DataLogger/datalogger.h").read_text()
    (build / "datalogger.h").write_text(header.replace(
        "  bool begin(const std::string& header);",
        "  bool begin(const std::string& header);\n  bool beginBaseline(const std::string& header);", 1))
    begin = baseline_logging.index("const std::string LOG_HEADER =")
    end = baseline_logging.index("\n\n", begin)
    (build / "legacy_header.inc").write_text(
        baseline_logging[begin:end].replace("LOG_HEADER", "LEGACY_LOG_HEADER", 1))
    begin = baseline_logging.index("std::string buildLogLine(")
    end = baseline_logging.index("// Append a discrete EVENT", begin)
    (build / "legacy_logging.inc").write_text(
        baseline_logging[begin:end].replace("buildLogLine(", "buildLegacyLogLine(", 1))
    logging = (root / "baseLogger/Helpers/logging.h").read_text()
    end = logging.index("// Defined in ota_update.h")
    (build / "logging.inc").write_text(logging[:end] + "\n#endif\n")
    executable = build / "log_diagnostics_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(root / "tests/support"),
        "-I", str(root / "libraries/DataLogger"),
        "-I", str(root / "libraries/FreematicsPlus"),
        "-I", str(root / "libraries/nlohmann"),
        "-I", str(root / "libraries/BLEWrapper"),
        "-I", str(root / "baseLogger/Helpers"), "-I", str(build),
        str(root / "tests/log_diagnostics_checks.cpp"),
        str(root / "libraries/FreematicsPlus/FreematicsGPS.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    subprocess.run([str(executable), arguments.group], env=environment, check=True)
    if arguments.capture_helper:
        spec = importlib.util.spec_from_file_location("dongle_capture", arguments.capture_helper)
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        raw = subprocess.check_output([str(executable), "dump"], env=environment)
        framed = io.BytesIO()
        stream = helper.DumpStream(framed)
        for begin in range(0, len(raw), 7):
            stream.feed(raw[begin:begin + 7])
        if not stream.complete or framed.getvalue() != raw:
            raise AssertionError("Real DataLogger dump failed framing validation")
        capture = build / "capture"
        helper.private_directory(capture)
        digest = hashlib.sha256(raw).hexdigest()
        for filename in ["serial_capture.log", "flash_dump.log"]:
            with helper.private_file(capture / filename, True) as handle:
                handle.write(raw)
        with helper.private_file(capture / "capture.json") as handle:
            json.dump({"end_marker_received": True, "serial_sha256": digest, "dump_sha256": digest,
                       "segments": stream.segments}, handle)
        output = build / "processed"
        summary = helper.process_capture(capture, output)
        with (output / "telemetry.csv").open(newline="") as handle:
            rows = list(csv.DictReader(handle))
        if not summary["records_valid"] or summary["telemetry_rows"] != 4 or len(rows) != 4:
            raise AssertionError("Actual baseline rollback dump did not process exactly four valid rows")
        if [row["soc"] for row in rows] != ["50", "51.000", "52.000", "53.000"] or (
                [row["gps_read"] for row in rows] != ["", "none", "", "none"]):
            raise AssertionError("Rollback capture shifted legacy or diagnostic columns")
        if summary["legacy_shaped_rows"] != 1:
            raise AssertionError("Actual baseline rollback row did not remain explicitly unknown")
        print("PASS actual eacc642 logger/CSV rollback through capture helper")
