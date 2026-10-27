"""Host checks for the relay acks (#90): a phone counts as the transport only while the acks of an app that announced
them keep moving forward, and cellular carries every frame such an app never acked.

Compiles the real Helpers/ble_relay.h, telemetry_format.h, the upload queue and v2 body (network_upload.h), the
backlog encoder, ble.h's bleAuthenticated()/bleRelaying(), and logger.ino's poll pieces in their production order:
relayAckService() (from the top of loop()), the ABD1 send, the queue block and the cellular gate's condition with
the grace and handoff it reads. BLE, the HTTP transport, the clock and the heap are fakes; the gate's cadence term
is zero. Today's ABD1 frame (for apps that never announce) comes from SM_PIN's telemetry_format.h, the last commit
before the seq existed.

Not exercised, and so pinned structurally below: the Bluedroid ABD2 handler (the relay_ack branch sits behind the
authorization gate and returns before the Phase B lockdown and setJsonValue), the cellular block running for a
relayed-but-lapsed frame, which uses of bleRelaying() moved to bleAuthenticated(), and the ABD5 info JSON. The real
modem, a phone and the app are not: that needs a bench session with the app's forwarding stopped."""
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

PIN = "c61c081139f8021018458f882e4a73b7024316d0"  # main before #90: today's ABD1 frame

def code(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)

def fail(message):
    raise SystemExit("FAIL: " + message)

logger = (root / "baseLogger/logger.ino").read_text()
helpers = root / "baseLogger/Helpers"
ble = (helpers / "ble.h").read_text()
wrapper = (root / "libraries/BLEWrapper/blewrapper.cpp").read_text()

# The relay_ack branch: behind the ABD2 authorization gate, and back out before anything can fold it into the profile.
handler_start = find(wrapper, "void StartupCallbacks::onWrite(", what="blewrapper.cpp")
handler = code(wrapper[handler_start:find(wrapper, "void ServerCallbacks::onConnect(", handler_start, "blewrapper.cpp")])
branch = find(handler, 'else if (message_key == "relay_ack") {', what="ABD2 handler")
if not find(handler, "if (!wrapper->get_authenticated()) {", what="ABD2 handler") < branch:
    fail("the relay_ack branch must sit behind the ABD2 authorization gate")
relay_branch = block(handler[branch:], 'else if (message_key == "relay_ack") {', "ABD2 handler")
if "dispatchRelayAck(" not in relay_branch or not re.search(r"\n      return;\n    }\n$", relay_branch) or \
        re.search(r"logEvt|logError|notify", relay_branch):
    fail("the relay_ack branch must end in an unconditional return, and log and answer nothing on the Bluedroid task")
if 'message_key != "HASH" && message_key != "relay_ack"' not in handler:
    fail("a relay_ack must not get the generic ACK on ABD2")
if not branch < find(handler, "wrapper->profile_signature_required()", what="ABD2 handler") < \
        find(handler, "wrapper->setJsonValue(message_key", what="ABD2 handler"):
    fail("the relay_ack branch must return before the Phase B lockdown and setJsonValue")
if "bleRelayAckWrite(input)" not in handler[:find(handler, "ABD2 rx:", what="ABD2 handler")]:
    fail("the ABD2 rx log must leave relay acks out")
if "ble.set_relay_ack_hook(relayAckHook);" not in block(ble, "void bleInitForWake() {", "ble.h"):
    fail("bleInitForWake must wire the relay-ack hook")

if "void set_authenticated(bool auth) { if (auth && !authenticated) ++auth_grants; authenticated = auth; };" not in \
        (root / "libraries/BLEWrapper/blewrapper.h").read_text():
    fail("BLEWrapper must count a session grant on every false -> true, and only then")
relay = (helpers / "ble_relay.h").read_text()
if re.search(r"logEvent|Serial", block(relay, "void relayAckHook(", "ble_relay.h")):
    fail("relayAckHook runs on the Bluedroid task: stash only")

# The cellular block runs whenever the phone is not the transport, relayed frame or not.
relay_start = find(logger, "    if (ble_relayed) {", what="logger.ino")
relay_end = find(logger, "\n    }\n", relay_start, "logger.ino")
if not logger[relay_end:].startswith("\n    }\n    // No phone") or \
        "    if (!bleRelaying()) {\n" not in logger[relay_end:relay_end + 400]:
    fail("the cellular block must follow the relay branch under if (!bleRelaying()), not sit in its else")
loop_start = find(logger, "void loop() {", what="logger.ino")
if "relayAckService();" not in logger[loop_start:find(logger, "if (current_state == UNINITIALIZED)", loop_start, "logger.ino")]:
    fail("loop() must run relayAckService() every iteration, ahead of its early returns")

# Presence keys on authentication; the transport (and the session's relay latch) on relaying.
if "bleRelaying()" in code((helpers / "alarm_gate.h").read_text()):
    fail("the alarm gate's 'human is here' must key on bleAuthenticated(), not the transport")
if "if (bleAuthenticated()) {\n      last_active = now;" not in (helpers / "state_machine.h").read_text():
    fail("the BLE awake leash must key on bleAuthenticated()")
if "if (bleRelaying()) g_ble_session_relayed = true;" not in (helpers / "state_machine.h").read_text():
    fail("the session's relay latch must key on bleRelaying()")

# ABD5 advertises the acks, and the info JSON still fits its buffer at its worst.
ota = (helpers / "ota_update.h").read_text()
info = block(ota, "void otaPublishInfo() {", "ota_update.h")
size = int(re.search(r"char buf\[(\d+)\];", info).group(1))
fmt = re.search(r'snprintf\(buf, sizeof\(buf\),\s*"((?:[^"\\]|\\.)*)"', info).group(1).encode().decode("unicode_escape")
if '"relay_ack":1' not in fmt:
    fail('the ABD5 info JSON must carry "relay_ack":1')
worst = re.sub(r"%lu|%u", "4294967295", fmt).replace("%d", "-1").replace("%s", "resume_wait", 2)
worst = worst.replace('"build":"resume_wait"', '"build":"' + "v" * 48 + '"', 1)
if len(worst) + 1 > size:
    fail(f"the ABD5 info JSON can reach {len(worst) + 1} bytes, over its {size}-byte buffer")

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-relay-checks-") as temporary:
    build = Path(temporary)
    (build / "ble_predicates.inc").write_text(block(ble, "bool bleAuthenticated() {", "ble.h")
                                              + block(ble, "uint32_t bleAuthGrant() {", "ble.h")
                                              + block(ble, "bool bleRelaying() {", "ble.h"))
    send_start = find(logger, "    const uint16_t ble_mtu = ble.mtu();", what="logger.ino")
    (build / "ble_send.inc").write_text(logger[send_start:find(logger, "#else\n    const bool ble_relayed = false;",
                                                               send_start, "logger.ino")])
    (build / "upload_buffer.inc").write_text(block(logger, "    if (frame_has_data) {", "logger.ino"))
    grace = find(logger, "      bool ble_upload_grace = ", what="logger.ino")
    handoff = find(logger, "      bool ble_handoff_fire = ", grace, "logger.ino")
    (build / "upload_grace.inc").write_text(logger[grace:find(logger, "\n", grace) + 1]
                                            + logger[handoff:find(logger, "\n", handoff) + 1])
    gate = find(logger, "      if (!bleRelaying() && !g_ota_active && !ble_upload_grace", what="logger.ino")
    (build / "upload_gate.inc").write_text(logger[gate + len("      if ("):find(logger, ") {\n", gate, "logger.ino")])
    historical = subprocess.check_output(
        ["git", "show", f"{PIN}:baseLogger/Helpers/telemetry_format.h"], cwd=root, text=True)
    (build / "historical_ble_frame.inc").write_text(block(historical, "BleTlmFrame bleTlmFrame(", "telemetry_format.h"))
    clock = (helpers / "clock_sync.h").read_text()
    quality = find(clock, "bool clockValid(", what="clock_sync.h")
    (build / "clock_quality.inc").write_text(clock[quality:find(clock, "// UTC calendar time", quality, "clock_sync.h")])
    network = (root / "libraries/FreematicsPlus/FreematicsNetwork.h").read_text()
    budget = find(network, "// Request headers the SIM7070", what="FreematicsNetwork.h")
    (build / "header_budget.inc").write_text(network[budget:find(network, "typedef enum {", budget)])
    gzip_body = (helpers / "gzip_body.h").read_text()
    outcome = find(gzip_body, "enum GzipOutcome", what="gzip_body.h")
    (build / "gzip_outcome.inc").write_text(gzip_body[outcome:find(gzip_body, "\n};\n", outcome) + 4])
    upload = (helpers / "network_upload.h").read_text()
    backlog = find(upload, "static const char* UPLOAD_BACKLOG_PATH", what="network_upload.h")
    (build / "backlog.inc").write_text(upload[backlog:find(upload, "\n#endif", backlog)])
    executable = build / "relay_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
        "-I", str(root / "tests/support"), "-I", str(root / "libraries/nlohmann"),
        "-I", str(root / "libraries/BLEWrapper"), "-I", str(helpers), "-I", str(build),
        str(root / "tests/relay_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    sys.exit(subprocess.run([str(executable), *sys.argv[1:]], env=environment).returncode)
