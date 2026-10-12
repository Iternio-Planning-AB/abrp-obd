import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

arguments = argparse.ArgumentParser()
arguments.add_argument("group", nargs="?", default="all",
                       choices=["all", "progress", "corroboration", "wake-gate", "logging", "schema", "transport", "profiles"])
arguments.add_argument("--profiles", type=Path)
args = arguments.parse_args()
if args.group == "profiles" and args.profiles is None:
    arguments.error("profiles checks require --profiles /path/to/abrp/obd")
root = Path(__file__).resolve().parents[1]
logger = (root / "baseLogger/logger.ino").read_text()
config = (root / "baseLogger/config.h").read_text()
logging = (root / "baseLogger/Helpers/logging.h").read_text()
datalogger = (root / "libraries/DataLogger/datalogger.cpp").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))
with tempfile.TemporaryDirectory(prefix="obd-energy-checks-") as temporary:
    build = Path(temporary)
    (build / "charge_config.inc").write_text("\n".join(re.findall(
        r"^#define (?:CHARGE_PROGRESS_\w+|CLOCK_MAX_EPOCH|GEO_MAX_SEND_AGE_SECONDS|RAIL_AWAKE_VOLTS|RAIL_TAPER_FLOOR|"
        r"CHARGE_END_POLL_FLOOR|TAPER_CONFIRM_MAX_WAKES|IO36_LOCKOUT_\w+)\b.*(?:\n[ \t]+.*)*",
        config, re.MULTILINE)))
    start = logger.index("    auto chargeCounterRose =")
    end = logger.index("\n    /*\n      DC or AC", start)
    (build / "charge_progress.inc").write_text(logger[start:end])
    start = logger.index("    bool has_charge_counter = false;")
    end = logger.index("\n    const bool uncorroborated =", start)
    (build / "charge_corroboration.inc").write_text(logger[start:end])
    start = logger.index("    bool weak_rail = (rail >= IO36_LOCKOUT_LOW")
    end = logger.index("heartbeat_due);\n", start) + len("heartbeat_due);\n")
    (build / "wake_gate.inc").write_text(logger[start:end])
    start = logger.index("void resetChargeCorroboration() {") + len("void resetChargeCorroboration() {")
    end = logger.index("\n}", start)
    (build / "reset_corroboration.inc").write_text(logger[start:end])
    start = logging.index("const std::string LOG_HEADER =")
    end = logging.index("\n// Append a discrete EVENT", start)
    (build / "logging.inc").write_text(logging[start:end])
    start = datalogger.index("bool DataLogger::begin(")
    end = datalogger.index("\nbool DataLogger::writeLine(", start)
    (build / "logger_begin.inc").write_text(datalogger[start:end])
    executable = build / "energy_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
        "-I", str(root / "tests/support"), "-I", str(root / "libraries/FreematicsPlus"),
        "-I", str(root / "libraries/nlohmann"), "-I", str(root / "libraries/mathparser"),
        "-I", str(root / "baseLogger/Helpers"), "-I", str(build),
        str(root / "tests/energy_checks.cpp"), str(root / "libraries/mathparser/parser.cpp"),
        str(root / "libraries/mathparser/expression_tree.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    command = [str(executable), args.group]
    if args.profiles:
        command.append(str(args.profiles.resolve()))
    subprocess.run(command, env=environment, check=True)
