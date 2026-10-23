"""Host checks for the BLE profile push hand-off: what reaches /vehicle_profile.json, and what a reset does to a push
still waiting for the loop. Compiles the real config-apply block (logger.ino) and loadPersistedProfile (ble.h)
against an in-memory LittleFS and fake OBD/BLE objects. The BLE task, NVS and the reboot itself are not exercised;
the two pre-reset drops (OTA commit, sleep teardown) are checked for position only."""
import os
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tests/support"))
from extract import block, find  # noqa: E402

logger = (root / "baseLogger/logger.ino").read_text()
ble = (root / "baseLogger/Helpers/ble.h").read_text()
ota = (root / "baseLogger/Helpers/ota_update.h").read_text()

# A push the loop has not applied yet dies with RAM, while BLEWrapper has already committed its hash: both resets must
# drop it so the app re-pushes. Comments are stripped so a commented-out line does not count.
def code(text, opener, what):
    return re.sub(r"//[^\n]*", "", block(text, opener, what))
commit = code(ota, "static void otaFinalize() {", "ota_update.h")
lost = find(commit, 'if (g_config_pending) dropUnsavedPush("ota");',
            find(commit, "esp_ota_set_boot_partition(", what="otaFinalize"), "otaFinalize")
if find(commit, "datalogger.end();", what="otaFinalize") < lost:
    raise SystemExit("FAIL: otaFinalize must drop a pending push before the log closes and the reboot")
teardown = code(ble, "void bleDeinitForSleep() {", "ble.h")
find(teardown, 'if (g_config_pending) dropUnsavedPush("sleep");',
     find(teardown, "ble.deinit();", what="bleDeinitForSleep"), "bleDeinitForSleep")

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-profile-checks-") as temporary:
    build = Path(temporary)
    (build / "config_apply.inc").write_text(
        block(logger, "  if (g_config_pending && !g_ota_active && !g_logdump_active) {", "logger.ino"))
    start = find(ble, "static const char* PROFILE_PATH", what="ble.h")
    (build / "persisted_profile.inc").write_text(ble[start:find(ble, "\n#elif ENABLE_BLE", start, "ble.h")])
    executable = build / "profile_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", str(root / "tests/support"), "-I", str(root / "libraries/nlohmann"), "-I", str(build),
        str(root / "tests/profile_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    sys.exit(subprocess.run([str(executable)], env=environment).returncode)
