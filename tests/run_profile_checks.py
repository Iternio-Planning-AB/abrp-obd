"""Host checks for the BLE profile push hand-off: what reaches /vehicle_profile.json, and what a reset does to a push
still waiting for the loop, and what a key reset forgets. Compiles the real push stash (configCallback), config-apply
block (logger.ino), loadPersistedProfile (ble.h) and forgetProfile (auth_gate.h) against an in-memory LittleFS and fake OBD/BLE
objects. The BLE task, NVS and the reboot itself are not exercised;
the pre-reset drops, the wrapper's hand-over and the key-reset calls are checked for position only."""
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
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", block(text, opener, what), flags=re.S)
commit = code(ota, "static void otaFinalize() {", "ota_update.h")
lost = find(commit, 'if (g_config_pending) dropUnsavedPush("ota");',
            find(commit, "esp_ota_set_boot_partition(", what="otaFinalize"), "otaFinalize")
if find(commit, "datalogger.end();", what="otaFinalize") < lost:
    raise SystemExit("FAIL: otaFinalize must drop a pending push before the log closes and the reboot")
teardown = code(ble, "void bleDeinitForSleep() {", "ble.h")
find(teardown, 'if (g_config_pending) dropUnsavedPush("sleep");',
     find(teardown, "ble.deinit();", what="bleDeinitForSleep"), "bleDeinitForSleep")

# The hash is committed by the loop after the file write, never by the wrapper at SETUP OK. It travels with the push, and
# a session that sent no HASH (the wizard's CLEAN) carries 0 rather than an earlier session's value.
wrapper = (root / "libraries/BLEWrapper/blewrapper.cpp").read_text()
setup = code(wrapper, "void BLEWrapper::completeSetup() {", "blewrapper.cpp")
find(setup, 'data["hash"] = new_hash;', what="completeSetup")
if any(call in setup for call in ("confirmHash(", "commitHash(", "putULong(")):
    raise SystemExit("FAIL: completeSetup must not commit the hash; the loop does after the file write")
find(code(wrapper, "void ServerCallbacks::onConnect(", "blewrapper.cpp"), "wrapper->resetPushStaging();", what="onConnect")
find(setup, "new_hash = 0;", find(setup, 'data["hash"] = new_hash;', what="completeSetup"), "completeSetup")
apply = code(logger, "  if (g_config_pending && !g_ota_active && !g_logdump_active) {", "logger.ino")
if find(apply, "LittleFS.open(PROFILE_PATH", what="config apply") < find(apply, "ble.invalidateHash();", what="config apply"):
    raise SystemExit("FAIL: the stored hash must be invalidated before the profile file is rewritten")
if find(apply, "ble.commitHash(hash, true);", what="config apply") < find(apply, "pf.print(", what="config apply"):
    raise SystemExit("FAIL: the hash is committed only after the profile file is written")

# Both key resets forget the stored push; the BLE one after the session grant is revoked.
gate = code((root / "baseLogger/Helpers/auth_gate.h").read_text(), 'if (j.contains("authreset")) {', "auth_gate.h")
forget = find(gate, 'if (!forgetProfile("authreset") || (p.isKey("secret") && !p.remove("secret"))) {',
             find(gate, "ble.set_authenticated(false);", what="authreset"), "authreset")
if gate.count('p.remove("secret")') != 1:
    raise SystemExit("FAIL: the BLE key reset removes the key once, after the forget")
failed = find(gate, 'authNotifyJson("{\\"auth\\":\\"error\\"}");', forget, "authreset")
if failed > find(gate, 'authNotifyJson("{\\"auth\\":\\"reset\\"}");', forget, "authreset"):
    raise SystemExit("FAIL: a key reset whose forget or key removal fails must answer error, not reset")
console = (root / "baseLogger/Helpers/logging.h").read_text()
clear = code(console, 'else if (cmd.equalsIgnoreCase("AUTHCLEAR")) {', "logging.h")
forget = find(clear, 'if (!forgetProfile("authclear")) {', find(clear, "ble.set_authenticated(false);", what="AUTHCLEAR"), "AUTHCLEAR")
if find(clear, "ble.clearAuthSecrets();", what="AUTHCLEAR") < find(clear, "return;", forget, "AUTHCLEAR"):
    raise SystemExit("FAIL: AUTHCLEAR must forget (and erase the log) first, and keep the secret when that fails")
busy = code(console, "if ((g_ota_active || g_logdump_active) &&", "logging.h")
if busy.index('cmd.equalsIgnoreCase("AUTHCLEAR")') > busy.index("BUSY:"):
    raise SystemExit("FAIL: serial AUTHCLEAR must be refused while an update or a BLE log dump runs, like CLEAR")

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-profile-checks-") as temporary:
    build = Path(temporary)
    (build / "config_apply.inc").write_text(
        block(logger, "  if (g_config_pending && !g_ota_active && !g_logdump_active) {", "logger.ino"))
    start = find(ble, "std::string g_pending_config_str;", what="ble.h")
    (build / "config_callback.inc").write_text(
        ble[start:find(ble, "bool configCallback(json newConfig) {", start, "ble.h")]
        + block(ble, "bool configCallback(json newConfig) {", "ble.h"))
    start = find(ble, "static const char* PROFILE_PATH", what="ble.h")
    (build / "persisted_profile.inc").write_text(ble[start:find(ble, "\n#elif ENABLE_BLE", start, "ble.h")])
    upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
    backlog = find(upload, "static const char* UPLOAD_BACKLOG_PATH", what="network_upload.h")
    (build / "backlog_path.inc").write_text(upload[backlog:find(upload, "\n", backlog, "network_upload.h") + 1])
    ota = (root / "baseLogger/Helpers/ota_update.h").read_text()
    (build / "ota_codes.inc").write_text("".join(
        line + "\n" for line in ota.splitlines() if line.startswith(("#define OTA_PH_ABORTED ", "#define OTA_ERR_NOAUTH "))))
    (build / "forget_profile.inc").write_text(
        block((root / "baseLogger/Helpers/auth_gate.h").read_text(), "bool forgetProfile(const char* why) {", "auth_gate.h"))
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
