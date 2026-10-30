import argparse
import csv
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "clang++"))
parser = argparse.ArgumentParser()
parser.add_argument("group", nargs="?", default="all", choices=["all", "schema", "logging", "schema-failures", "ble-write", "motion", "fw-build", "dump-since", "loop-gap", "stat-widths"])
parser.add_argument("--capture-helper", type=Path)
parser.add_argument("--since-helper", type=Path, help="capture2.py: compare DUMP SINCE with its SinceFilter")
arguments = parser.parse_args()
# The ble-write group tests the summary; this keeps blewrapper.cpp using it. A log or print call that names a
# raw write buffer (input/value) other than through bleWriteSummary(), or a client key other than through
# bleKeyForLog(), would put the AUTH secret or tlm_push_token into the flash log or onto USB serial.
if arguments.group in ("all", "ble-write"):
    ble_source = (root / "libraries/BLEWrapper/blewrapper.cpp").read_text()
    calls = re.findall(r"(?:logEvt|logError|Serial\.printf)\((.*?)\);", ble_source, re.S)
    leaks = [call for call in calls
             if re.search(r"\b(input|value|message_key)\b",
                          re.sub(r'bleWriteSummary\((input|value)\)|bleKeyForLog\(message_key\)|"(?:\\.|[^"\\])*"', "", call))]
    if leaks or ble_source.count("bleWriteSummary(") < 2:
        raise AssertionError(f"blewrapper.cpp logs a raw BLE write: {leaks}")
# fw-build drives logFwBuild() and the segment hook; these keep setup() wiring both up.
if arguments.group in ("all", "fw-build"):
    setup_source = (root / "baseLogger/logger.ino").read_text()
    hook = setup_source.find("datalogger.onNewSegment(logFwBuild);")
    if hook < 0 or setup_source.find("datalogger.begin(", hook) < 0 or "datalogger.begin(" in setup_source[:hook]:
        raise AssertionError("setup() must register logFwBuild as the segment hook before datalogger.begin()")
    boot = re.search(r"if \(esp_reset_reason\(\) != ESP_RST_DEEPSLEEP\) logFwBuild\(\);", setup_source)
    if not boot or boot.start() < setup_source.find('logEvent("WAKE '):
        raise AssertionError("setup() must log the build after WAKE on every boot that is not a deep-sleep wake")
# loop-gap drives the LoopGap helper (#110); these keep loop() visiting it once per iteration, at the top before the
# activity ladder (which can sleep), and re-seeding it after a console command, whose minutes are no loop gap.
if arguments.group in ("all", "loop-gap"):
    loop_source = (root / "baseLogger/logger.ino").read_text()
    loop_start = loop_source.find("\nvoid loop() {\n")
    head_end = loop_source.find("  handleActivityState();\n", loop_start)
    head = [l.strip() for l in loop_source[loop_start:head_end].splitlines()[2:]
            if l.strip() and not l.lstrip().startswith(("//", "#"))]
    if loop_start < 0 or head_end < 0 or [re.sub(r"\s*//.*", "", l) for l in head] != [
            "esp_task_wdt_reset();", "loopGapNote(g_loop_gap, (uint32_t)millis());"]:
        raise AssertionError(f"loop() must note the loop gap between esp_task_wdt_reset() and handleActivityState(): {head}")
    console = loop_source.find("  if (handleLogCommands()) loopGapReseed(g_loop_gap, (uint32_t)millis());", head_end)
    if console < 0 or loop_source.count("handleLogCommands()") != 1 or console > loop_source.find("\n}", loop_start):
        raise AssertionError("loop() must re-seed the loop gap after a console command, its only handleLogCommands() call")
    # A command read is a command served, on every path out (BUSY and usage included), so each re-seeds.
    commands = (root / "baseLogger/Helpers/logging.h").read_text()
    READ = "  if (!cmd.length()) return false;\n"
    start = commands.find("\nbool handleLogCommands() {\n")
    read = commands.find(READ, start)
    end = commands.find("\n}\n", start)
    returns = re.findall(r"\breturn\b[^;]*;", commands[read + len(READ):end])
    if start < 0 or not start < read < end or not returns or set(returns) != {"return true;"}:
        raise AssertionError(f"handleLogCommands() must return true on every path after reading a command: {returns}")


def synthetic_log(rng):
    """Random boots in the firmware's line formats, split into small segments that each start with a FW line."""
    epoch, lines, millis = 1_800_000_000, [], 0
    for _ in range(rng.randint(3, 12)):
        cold = rng.random() < 0.4
        clock_at = rng.choice([0, 1, 2, 3, None]) if cold else 0  # index of the first clocked line; None = never
        millis = rng.randint(1, 50)
        utc = lambda i: 0 if clock_at is None or i < clock_at else epoch
        boot = []
        if rng.random() < 0.9:
            boot.append(("EVT", "WAKE reason=timer reset=8"))
            if cold:
                boot.append(("EVT", "FW build=v0.0.0 v=0 elf=0 part=app0"))
        boot += [("ROW", None)] * rng.randint(1, 8)
        for i, (kind, text) in enumerate(boot):
            epoch += rng.randint(1, 600) if utc(i) else 0
            millis += rng.randint(1, 5000)
            lines.append((kind, text, utc(i), millis))
        epoch += rng.randint(60, 7200)
    segments, current, limit = [], [], rng.randint(150, 600)
    for kind, text, utc, ms in lines:
        if not current or sum(map(len, current)) > limit:
            current = [f"EVT,{utc},{ms},ACTIVE,12.00,12.10,FW build=v0.0.0 v=0 elf=0 part=app0\n"]
            segments.append(current)
        current.append(f"EVT,{utc},{ms},ACTIVE,12.00,12.10,{text}\n" if kind == "EVT" else f"{ms},{utc},50\n")
    return ["millis,utc,soc\n" + "".join(segment) for segment in segments]


def since_differential(helper_path, executable, environment, build):
    """DUMP SINCE against capture2.py's SinceFilter on random synthetic logs. Through the host filter, the device's
    output must equal the filter over DUMP of the segments it read, and lose no data line of a full DUMP; skipped
    segments can only cost context (a WAKE/FW line from before the start segment) and the clockless boots at the
    head of the log that the rule keeps for want of an earlier epoch."""
    import random
    spec = importlib.util.spec_from_file_location("dongle_capture2", helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)

    def host(text, since, skipped=False):
        out = io.StringIO()
        sf = helper.SinceFilter(since, out)
        if skipped:  # as the device seeds it: the segments before the start end before since
            sf.prev_epoch = 0
        for line in text.split("\n"):
            if sf.feed_line(line):
                break
        return out.getvalue()

    def dump(files):
        return "".join(f"===== /{name} ({len(body)} bytes) =====\n{body}" for name, body in files) + "===== end of dump =====\n"

    rng, cases = random.Random(76), 0
    for trial in range(300):
        segments = synthetic_log(rng)
        folder = build / f"since-{trial}"
        folder.mkdir()
        files = [(f"log{i:04d}.csv", body) for i, body in enumerate(segments)]
        for name, body in files:
            (folder / name).write_text(body)
        epochs = sorted({int(field) for body in segments for line in body.splitlines()
                         for field in re.findall(r"\b(1\d{9})\b", line)})
        for since in {0, epochs[-1] + 1, *rng.sample(epochs, min(6, len(epochs)))}:
            run = subprocess.run([str(executable), "since-dir", str(folder), str(since)], env=environment,
                                 capture_output=True, text=True, check=True)
            start = int(re.search(r"start=(-?\d+)", run.stderr).group(1))
            device, full = host(run.stdout, since), host(dump(files), since)
            if device != host(dump(files[start:]), since, start > 0):
                raise AssertionError(f"trial {trial} since {since}: device output differs from SinceFilter over the segments read")
            # Lines the rule keeps only because no epoch precedes them, at the head of the log, are skipped too.
            whole = dump(files)
            clock_starts = re.search(r"^(?:EVT,1\d{9},|\d+,1\d{9},)", whole, re.M).start()
            lost = [line for line in full.splitlines() if line not in device.splitlines()]
            if any(not line.startswith(("=====", "millis,utc,")) and "WAKE reason=" not in line and "FW build=" not in line
                   and whole.find(line + "\n") > clock_starts for line in lost):
                raise AssertionError(f"trial {trial} since {since}: a skipped segment held kept lines: {lost}")
            cases += 1
    print(f"PASS dump-since against SinceFilter ({cases} cases)")


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
    # stat-widths renders the pre-sleep summary lines (#110) from their literal in sleep_power.h, one each.
    sleep_source = (root / "baseLogger/Helpers/sleep_power.h").read_text()
    stat_formats = ""
    for prefix in ("FLUSHSTAT", "WAKESTAT"):
        found = re.findall(r'logEvent\("(' + prefix + r' [^"]*)"', sleep_source)
        if arguments.group in ("all", "stat-widths") and len(found) != 1:
            raise AssertionError(f"sleep_power.h must log exactly one {prefix} line, found {len(found)}")
        stat_formats += f'static const char* const {prefix}_FORMAT = "{found[0] if found else ""}";\n'
    (build / "stat_formats.inc").write_text(stat_formats)
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
    if arguments.since_helper:
        since_differential(arguments.since_helper, executable, environment, build)
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
