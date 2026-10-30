import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from support.host_build import compile_cpp, runtime_environment


def line(text, prefix):
    found = [l for l in text.splitlines() if l.startswith(prefix)]
    if len(found) != 1:
        raise SystemExit(f"FAIL: expected one line starting {prefix!r}, found {len(found)}")
    return found[0] + "\n"

arguments = argparse.ArgumentParser()
arguments.add_argument("group", nargs="?", default="all",
                       choices=["all", "progress", "signal", "release", "corroboration", "wake-gate", "logging", "transport", "sleep-reason", "wdt-streak", "ble-frame", "dc-awake", "profiles"])
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
sleep = (root / "baseLogger/Helpers/sleep_power.h").read_text()
state_machine = (root / "baseLogger/Helpers/state_machine.h").read_text()
streak = (root / "baseLogger/Helpers/wdt_streak.h").read_text()
ota = (root / "baseLogger/Helpers/ota_update.h").read_text()




# logEvent() formats into char msg[120]: a longer line loses its tail, which is where fields get added.
WORST_EVENT_ARGS = {
    "SAMPLE_POLL ": (99, 1, 1, 255, 1, 99, 15.99, 15.99, 1, 255, 1, 15),
    "ATRVGATE atrv=": (15.99, 15.99, "ASLEEP", "poll(taper-confirm)", 9999999, 999999, 999999, 1, "ENFORCE"),
    "DC_AWAKE on ": (15.99, 4294967295),
    "DC_AWAKE off reason=motion": (15.99, 99.999, 99.999),
    "DC_AWAKE off reason=%s": ("parked-uncorroborated", 15.99),
    "DC_AWAKE refused ": ("motion", 15.99),
    "DC_AWAKE poke promoted": (1, 1),
}
event_sources = logger + state_machine + sleep
# The host stand-in for the streak store keeps its bytes across boots, as RTC_NOINIT_ATTR does and
# RTC_DATA_ATTR (reloaded on every reset but a deep-sleep wake) does not.
if not re.search(r"^RTC_NOINIT_ATTR WdtStreakStore wdt_streak_store;", streak, re.MULTILINE):
    raise SystemExit("FAIL: the WDT streak store must be RTC_NOINIT_ATTR: RTC_DATA_ATTR is reloaded by every crash it counts")
# Counted before anything that can crash or hang: a reset ahead of the advance would repeat with the count frozen.
setup = logger.index("void setup() {")
advance = logger.index("  wdt_reset_count = wdtStreakAdvance(", setup)
if not setup < advance < logger.index("  Serial.begin(115200);", setup):
    raise SystemExit("FAIL: setup() must advance the WDT streak before Serial.begin()")
prefix = logger[setup + len("void setup() {"):advance]
prefix = re.sub(r"//[^\n]*|/\*.*?\*/", "", prefix, flags=re.S)
approved_prefix = """
esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
resetFrozenGpsProvenance(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#if ENABLE_NETWORK
resetGeoAccepted(esp_reset_reason() == ESP_RST_DEEPSLEEP);
#endif
chargeWakeStateSanitize(esp_reset_reason() == ESP_RST_DEEPSLEEP);
"""
if " ".join(prefix.split()) != " ".join(approved_prefix.split()):
    raise SystemExit("FAIL: only approved reset-state initialization may precede accounting")
# The modem flag is kept by CellWrapper's hooks: every init() may power the modem on, only an acknowledged
# AT+CPOWD proves it off. The fake network in the test stands on these.
cell = (root / "libraries/NetworkWrapper/cellwrapper.cpp").read_text()
init = cell[cell.index("bool CellWrapper::init("):]
if not init.index("cellModemPowerOnHook();") < init.index("net.begin(&sys)"):
    raise SystemExit("FAIL: CellWrapper::init must call cellModemPowerOnHook() before net.begin()")
power_off = cell[cell.index("void CellWrapper::powerOff() {"):]
if not re.search(r"\n  if \(r == 1\) cellModemPowerOffHook\(\);", power_off[:power_off.index("\n}\n")]):
    raise SystemExit("FAIL: CellWrapper::powerOff must call cellModemPowerOffHook() on the CPOWD ack only")
if not re.search(r"wdtStreakEnd\(\);\n\s*esp_restart\(\);\s*// never returns", ota):
    raise SystemExit("FAIL: the OTA commit must end the WDT streak right before its esp_restart()")
for prefix, values in WORST_EVENT_ARGS.items():
    start = event_sources.index('logEvent("' + prefix) + len('logEvent("')
    message = event_sources[start:event_sources.index('"', start)] % values
    if len(message) > 119:
        raise SystemExit(f"FAIL: {len(message)} > 119 characters, logEvent cuts: {message}")
# A held DC charge sets the poll and upload cadence (dcAwakeDelay, checked below) and sends its first frame at once.
for gate in ("( millis() - last_poll ) > dcAwakeDelay(current_state, polling_delay)) {",
             "? getTransmissionDelay(ACTIVE) : dcAwakeDelay(current_state, transmission_delay);",
             "const bool dc_first_upload = dcAwakeHeld(current_state) && last_broadcast == 0;",
             "(ble_handoff_fire || dc_first_upload || ",
             "  handleActivityState();\n  dcPromotePoke();\n  dcAwakeTrack();\n",
             "    dcPromotePoke();\n    if (g_sample_poke && !maintenance_mode"):
    if logger.count(gate) != 1:
        raise SystemExit(f"FAIL: the loop's upload and poll gates must take the DC hold exactly once: {gate}")
# Only detectMovement() stamps the motion that vetoes the hold (not the console or an unlatch, via last_active).
mems = (root / "baseLogger/Helpers/mems.h").read_text()
if mems.count("last_active = g_last_motion_ms = millis();") != 1 or (logger + state_machine).count("g_last_motion_ms = ") != 1:
    raise SystemExit("FAIL: g_last_motion_ms must be stamped by detectMovement() alone")
with tempfile.TemporaryDirectory(prefix="obd-energy-checks-") as temporary:
    build = Path(temporary)
    (build / "charge_config.inc").write_text("\n".join(re.findall(
        r"^#define (?:CHARGE_PROGRESS_\w+|CHARGING_CURRENT_THRESHOLD|CHARGE_DISCHARGE_AMPS|CHARGE_STREAM_CONFIRM_SECONDS|"
        r"CHARGE_RELEASE_POLLS|CHARGE_SILENT_HOLD_POLLS|CHARGE_STALE_RESET_MINUTES|CHARGE_CORROBORATE_MINUTES|WAKE_PARKED_MINUTES|"
        r"WAKE_CHARGING_MINUTES|WAKE_DC_CHARGING_MINUTES|WAKE_CHARGING_MAX_MINUTES|CLOCK_MAX_EPOCH|GEO_MAX_SEND_AGE_SECONDS|RAIL_AWAKE_VOLTS|RAIL_TAPER_FLOOR|"
        r"CHARGE_END_POLL_FLOOR|MAX_AWAKE_MINUTES|TAPER_CONFIRM_MAX_WAKES|IO36_LOCKOUT_\w+|POST_DRIVE_\w+|OBD_ALARM_GATE|ENABLE_IO36_RAIL|DC_CHARGING_MIN_KW|DC_CHARGING_AWAKE_SECONDS|"
        r"SESSION_IDLE_MAX_MS|CHARGING_TIMER|ACTIVE_TIMER|DC_CHARGING_POLL_MARGIN_MS)\b.*(?:\n[ \t]+.*)*",
        config, re.MULTILINE)))
    start = logger.index("    // Feature A - is_charging")
    end = logger.index("\n    /*\n      CHARGE PROGRESS", start)
    (build / "charge_signal.inc").write_text(logger[start:end])
    start = logger.index("    const bool was_recently_charging")
    end = logger.index("\n\n#if ENABLE_BLE\n    const uint16_t ble_mtu", start)
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
    start = sleep.index('  logEvent("SLEEP enter -> %s+GPS off, timer=')
    end = sleep.index("WOM_THRESHOLD_G);\n", start) + len("WOM_THRESHOLD_G);\n")
    (build / "sleep_log.inc").write_text(sleep[start:end])
    (build / "reset_reason.inc").write_text(block(sleep, "static const char* resetReasonStr() {"))
    (build / "net_recover_sleep.inc").write_text(block(logger, "if (g_recovers_since_ok >= 2 && net_reboot_count < 2) {"))
    start = logger.index("  wdt_reset_count = wdtStreakAdvance(")
    (build / "wdt_streak_wake.inc").write_text(logger[start:logger.index("\n", start) + 1])
    (build / "wdt_streak_sleep.inc").write_text(block(logger, "if (wdt_reset_count >= 2) {"))
    end = logger.index("#if PRINT", start)
    startup = logger[start:end]
    startup_end = start + len(startup)
    for anchor in ("clockDriftRestore();", "datalogger.begin(", "sys.begin()", "logEvent(",
                   "softResetMEMS();", "mems->begin()", "calibrateMEMS();", "alarmGateLoad();"):
        if startup_end >= logger.index(anchor, setup):
            raise SystemExit(f"FAIL: the WDT startup guard must precede {anchor}")
    (build / "wdt_streak_startup.inc").write_text(startup)
    start = sleep.index("  bool modem_down = modem_on;")
    (build / "sleep_probe.inc").write_text(sleep[start:sleep.index("#endif\n", start) + 7])
    start = sleep.index("  if (modem_down) network.powerOff();")
    (build / "sleep_modem.inc").write_text(sleep[start:sleep.index("\n", start) + 1])
    (build / "reconnect.inc").write_text(block(logger, "if (!g_weak_rail_cold_boot && !bleRelaying() && !g_ota_active"))
    (build / "backstop_sleep.inc").write_text(block(state_machine, "if (!maintenance_mode && !g_ota_active && !g_logdump_active"))
    (build / "ble_frame_log.inc").write_text(block(logger, "if (ble_mtu != g_logged_ble_mtu"))
    (build / "activity_state.inc").write_text(block(logger, "enum ActivityState {").rstrip() + ";\n")
    (build / "dc_delay.inc").write_text("".join(block(state_machine, f) for f in (
        "bool dcMotionRecent()", "const char* dcAwakeVeto(", "bool dcAwakeHeld(", "unsigned long dcAwakeDelay(",
        "void dcAwakeTrack()")))
    (build / "dc_promote.inc").write_text(block(logger, "void dcPromotePoke() {"))
    (build / "dc_rail_refresh.inc").write_text(line(logger, "    if (g_sample_poke && g_dc_confirmed) g_rail_before_poke"))
    (build / "dc_sleep_log.inc").write_text(line(sleep, "  if (g_dc_logged) logEvent(\"DC_AWAKE off"))
    start = state_machine.index("  case CHARGING:\n", state_machine.index("void handleActivityState() {"))
    end = state_machine.index("    break;\n", start) + len("    break;\n")
    (build / "charging_case.inc").write_text(state_machine[start:end])
    start = logger.index("    if (g_sample_poke && !maintenance_mode") + len("    if (")
    end = logger.index(") {\n", start)
    (build / "poke_sleeps.inc").write_text(f"bool sleeps() {{ return {logger[start:end]}; }}\n")
    executable = build / "energy_checks"
    compile_cpp(executable,
                [root / "tests/energy_checks.cpp", root / "libraries/mathparser/parser.cpp",
                 root / "libraries/mathparser/expression_tree.cpp"],
                includes=[root / "tests/support", root / "libraries/FreematicsPlus",
                          root / "libraries/nlohmann", root / "libraries/mathparser",
                          root / "baseLogger/Helpers", build],
                flags=["-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function"])
    environment = runtime_environment()
    command = [str(executable), args.group]
    if args.profiles:
        command.append(str(args.profiles.resolve()))
    subprocess.run(command, env=environment, check=True)
