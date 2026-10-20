import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

arguments = argparse.ArgumentParser()
arguments.add_argument("group", nargs="?", default="all",
                       choices=["all", "progress", "signal", "release", "corroboration", "wake-gate", "logging", "schema", "transport", "sleep-reason", "ble-frame", "profiles"])
arguments.add_argument("--profiles", type=Path)
args = arguments.parse_args()
if args.group == "profiles" and args.profiles is None:
    arguments.error("profiles checks require --profiles /path/to/abrp/obd")
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests/support"))
from extract import block, wake_gate  # noqa: E402
logger = (root / "baseLogger/logger.ino").read_text()
config = (root / "baseLogger/config.h").read_text()
logging = (root / "baseLogger/Helpers/logging.h").read_text()
datalogger = (root / "libraries/DataLogger/datalogger.cpp").read_text()
sleep = (root / "baseLogger/Helpers/sleep_power.h").read_text()
state_machine = (root / "baseLogger/Helpers/state_machine.h").read_text()




compiler = shlex.split(os.environ.get("CXX", "clang++"))
# logEvent() formats into char msg[120]: a longer line loses its tail, which is where fields get added.
WORST_EVENT_ARGS = {
    "SAMPLE_POLL ": (99, 1, 1, 255, 1, 99, 15.99, 15.99, 1, 255, 1, 15),
    "ATRVGATE atrv=": (15.99, 15.99, "ASLEEP", "poll(taper-confirm)", 9999999, 999999, 999999, 1, "ENFORCE"),
}
for prefix, values in WORST_EVENT_ARGS.items():
    start = logger.index('logEvent("' + prefix) + len('logEvent("')
    message = logger[start:logger.index('"', start)] % values
    if len(message) > 119:
        raise SystemExit(f"FAIL: {len(message)} > 119 characters, logEvent cuts: {message}")
with tempfile.TemporaryDirectory(prefix="obd-energy-checks-") as temporary:
    build = Path(temporary)
    (build / "charge_config.inc").write_text("\n".join(re.findall(
        r"^#define (?:CHARGE_PROGRESS_\w+|CHARGING_CURRENT_THRESHOLD|CHARGE_DISCHARGE_AMPS|CHARGE_STREAM_CONFIRM_SECONDS|"
        r"CHARGE_RELEASE_POLLS|CHARGE_SILENT_HOLD_POLLS|CHARGE_STALE_RESET_MINUTES|CHARGE_CORROBORATE_MINUTES|WAKE_PARKED_MINUTES|"
        r"WAKE_CHARGING_MINUTES|WAKE_DC_CHARGING_MINUTES|WAKE_CHARGING_MAX_MINUTES|CLOCK_MAX_EPOCH|GEO_MAX_SEND_AGE_SECONDS|RAIL_AWAKE_VOLTS|RAIL_TAPER_FLOOR|"
        r"CHARGE_END_POLL_FLOOR|MAX_AWAKE_MINUTES|TAPER_CONFIRM_MAX_WAKES|IO36_LOCKOUT_\w+|POST_DRIVE_\w+|OBD_ALARM_GATE|ENABLE_IO36_RAIL|DC_CHARGING_MIN_KW)\b.*(?:\n[ \t]+.*)*",
        config, re.MULTILINE)))
    start = logger.index("    // Feature A - is_charging")
    end = logger.index("\n    /*\n      CHARGE PROGRESS", start)
    (build / "charge_signal.inc").write_text(logger[start:end])
    start = logger.index("    const bool was_recently_charging")
    end = logger.index("\n\n    if (frame_has_data) { // skip frames", start)
    (build / "charge_release.inc").write_text(logger[start:end])
    start = logger.index("    auto chargeCounterRose =")
    end = logger.index("\n    /*\n      DC or AC", start)
    (build / "charge_progress.inc").write_text(logger[start:end])
    start = logger.index("    bool is_dcfc = false;")
    end = logger.index("\n    // NOTE: power is deliberately NOT derived", start)
    (build / "charge_dcfc.inc").write_text(logger[start:end])
    start = logger.index("    bool has_charge_counter = false;")
    end = logger.index("\n    const bool uncorroborated =", start)
    (build / "charge_corroboration.inc").write_text(logger[start:end])
    (build / "wake_gate.inc").write_text(wake_gate(logger, through="    if (g_sample_poke) g_rail_before_poke = rail;"))
    start = logger.index("static const uint64_t POST_DRIVE_WINDOW_US")
    end = logger.index("\n}\n", logger.index("void chargeWakeStateSanitize(", start)) + 3
    (build / "charge_wake_state.inc").write_text(logger[start:end])
    start = logger.index("bool chargeStateStale(uint64_t now_us) {")
    end = logger.index("\n}\n", start) + 3
    (build / "charge_stale.inc").write_text(logger[start:end])
    start = logger.index("      g_next_wake_minutes = WAKE_PARKED_MINUTES; // skip = parked")
    end = logger.index("      enterDeepSleep(false);", start)
    (build / "gate_skip.inc").write_text(logger[start:end])
    machine = state_machine
    ladder = machine[machine.index("void handleActivityState() {"):]
    sleeps = re.findall(r"(\S+)\s*\n?\s*enterDeepSleep\(true(?:, \"[^\"]*\")?\);", ladder)
    if sleeps != ["pdwArmAtLadderEnd();"] * 2:
        raise SystemExit(f"FAIL: both ladder sleeps must follow pdwArmAtLadderEnd(): {sleeps}")
    start = machine.index("bool pdwOpenWindow() {")
    end = machine.index("\n}\n", machine.index("void pdwArmAtLadderEnd() {")) + 3
    (build / "pdw_ladder.inc").write_text(machine[start:end])
    start = logger.index("void resetChargeCorroboration() {") + len("void resetChargeCorroboration() {")
    end = logger.index("\n}", start)
    (build / "reset_corroboration.inc").write_text(logger[start:end])
    start = logging.index("const std::string LOG_HEADER =")
    end = logging.index("\n// Append a discrete EVENT", start)
    (build / "logging.inc").write_text(logging[start:end])
    start = datalogger.index("bool DataLogger::begin(")
    end = datalogger.index("\nbool DataLogger::writeLine(", start)
    (build / "logger_begin.inc").write_text(datalogger[start:end])
    start = sleep.index('  logEvent("SLEEP enter -> %s+GPS off, timer=')
    end = sleep.index("WOM_THRESHOLD_G);\n", start) + len("WOM_THRESHOLD_G);\n")
    (build / "sleep_log.inc").write_text(sleep[start:end])
    (build / "net_recover_sleep.inc").write_text(block(logger, "if (g_recovers_since_ok >= 2 && net_reboot_count < 2) {"))
    (build / "wdt_streak_sleep.inc").write_text(block(logger, "if (g_wdt_wake && wdt_reset_count >= 2) {"))
    (build / "backstop_sleep.inc").write_text(block(state_machine, "if (!maintenance_mode && !g_ota_active && !g_logdump_active"))
    (build / "ble_frame_log.inc").write_text(block(logger, "if (ble_mtu != g_logged_ble_mtu"))
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
