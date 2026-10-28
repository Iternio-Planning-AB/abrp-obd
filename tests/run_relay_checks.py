"""Host checks for the BLE protocol versions and the relay session (#90): a phone counts as the transport only while
its session's protocol says so (always in v0, while its acks keep moving forward in v1, never with no protocol), and
cellular carries every frame a v1 app never acked.

Compiles the real Helpers/ble_relay.h (the session state machine and the ABD9 session channel), ble_protocol.h (the
negotiation and the per-version tables), auth_gate.h's authSendNonce, telemetry_format.h, the upload queue and v2 body
(network_upload.h), the backlog encoder, ble.h's predicates, and logger.ino's poll pieces in their production order:
relayAckService() and sessionChannelService() (from the top of loop()), the ABD1 send, the queue block and the
cellular gate's condition with the grace and handoff it reads. BLE, the HTTP transport, the clock and the heap are
fakes; the gate's cadence term is zero. Today's ABD1 frame (for v0 sessions) comes from PIN's telemetry_format.h, the
last commit before the seq existed. A second build raises BLE_PROTO_MIN to 1 for the no-protocol session. The
`enumerate` group runs every sequence of `--depth` events (default 4: 15 events, 151875 sequences, ~50 s with the
compile) from three starting points with the session invariants checked after each, the ack timeout among them in both
directions.

Not exercised, and so pinned structurally below: the Bluedroid ABD2 handler (its PROTO refusal, and no relay_ack case
left), the GATT table (ABD1-ABD8 as in PIN, ABD9/ABDA with an encrypted CCCD, 30 of 32 handles, the CCCDs cleared on
disconnect), auth_gate.h around bleProtoAccept, the cellular block running for a relayed-but-lapsed frame, which uses
of bleRelaying() moved to bleAuthenticated(), and the ABD5 info JSON. The real modem, a phone and the app are not:
that needs a bench session (docs/design/bench-protocol-v1.md, local)."""
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

PIN = "c61c081139f8021018458f882e4a73b7024316d0"  # main before #90: today's ABD1 frame and GATT table
MIN1_GROUPS = {"no-protocol", "fixtures"}  # groups the -DBLE_PROTO_MIN=1 build runs (relay_checks.cpp main())
MIN1_ONLY_GROUPS = {"no-protocol"}
NO_SUCH_GROUP = 3                          # relay_checks.cpp's exit code when the requested group is not in its build

def code(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)

def fail(message):
    raise SystemExit("FAIL: " + message)

logger = (root / "baseLogger/logger.ino").read_text()
helpers = root / "baseLogger/Helpers"
ble = (helpers / "ble.h").read_text()
wrapper = (root / "libraries/BLEWrapper/blewrapper.cpp").read_text()
wrapper_h = (root / "libraries/BLEWrapper/blewrapper.h").read_text()

# ABD2 has no relay_ack case any more: acks are ABD9's, so a relay_ack written to ABD2 is an ordinary unknown setup key,
# as it is on main (ACKed, and folded into the profile).
handler_start = find(wrapper, "void StartupCallbacks::onWrite(", what="blewrapper.cpp")
handler = code(wrapper[handler_start:find(wrapper, "void ServerCallbacks::onConnect(", handler_start, "blewrapper.cpp")])
if "relay_ack" in handler or 'message_key != "HASH") {' not in handler:
    fail("the ABD2 handler must have no relay_ack case: no branch, no ACK exception, no rx-log exception")
for path in (root / "libraries/BLEWrapper").iterdir():
    if re.search(r"relay_ack|RelayAck", code(path.read_text())):
        fail(f"{path.name} still knows relay_ack: the session channel is ABD9's, decoded in Helpers/ble_relay.h")
if "ble.set_session_hooks(relaySessionWrite, relaySessionRead);" not in block(ble, "void bleInitForWake() {", "ble.h"):
    fail("bleInitForWake must wire the ABD9 session hooks")

if "void set_authenticated(bool auth) { if (auth && !authenticated) ++auth_grants; if (!auth) setup_allowed = false; " \
        "authenticated = auth; };" not in wrapper_h:
    fail("BLEWrapper must count a session grant on every false -> true, and only then, and end setup with the grant")
relay = (helpers / "ble_relay.h").read_text()
for hook in ("void relaySessionWrite(", "size_t relaySessionRead("):
    if re.search(r"logEvent|logEvt|Serial|\bble\.|Preferences|nvs_|mbedtls|LittleFS|datalogger|delay\(", code(block(relay, hook, "ble_relay.h"))):
        fail(f"{hook[:-1]} runs on the Bluedroid task: no NVS, crypto, flash, logging or ble calls, only bytes under the lock")
session_write = code(block(relay, "void relaySessionWrite(", "ble_relay.h"))
if not find(session_write, "len > RELAY_SESSION_WRITE_MAX", what="relaySessionWrite") < find(session_write, "json::parse(", what="relaySessionWrite"):
    fail("relaySessionWrite must refuse an oversized write before it parses anything on the Bluedroid task")
if not re.search(r"static_assert\(sizeof\(\"\{\\\"relay\\\":\\\"lapsed\\\"[^;]*<= 93,", relay):
    fail("ble_relay.h must hold the worst-case ABD9 state under the 93-byte read at compile time")

# The GATT table: ABD1-ABD8 exactly as the released app 7.1.13 knows them (PIN), ABD9 and ABDA gain notify and an
# encrypted CCCD, 30 of 32 handles. Handle order follows the library's pointer-keyed map, not creation order, so the
# handles themselves are logged at boot and compared on the bench.
old_wrapper = subprocess.check_output(["git", "show", f"{PIN}:libraries/BLEWrapper/blewrapper.cpp"], cwd=root, text=True)
def table(source):
    init = code(block(source, "void BLEWrapper::init() {", "blewrapper.cpp"))
    chars = re.findall(r"(p\w+Characteristic) = pService->createCharacteristic\(\s*(\w+),([^;]*)\);", init)
    return init, [(name, uuid, re.sub(r"\s+", "", props)) for name, uuid, props in chars]
old_init, old_chars = table(old_wrapper)
new_init, new_chars = table(wrapper)
if new_chars[:8] != old_chars[:8] or len(new_chars) != 10:
    fail("ABD1-ABD8 must keep their UUIDs, properties and creation order, and nothing may be added after ABDA")
def permissions(init, name):
    return re.findall(name + r"->(?:setAccessPermissions|setCallbacks|addDescriptor)\(([^;]*)\);", init)
for name, _, _ in old_chars[:8]:
    if permissions(new_init, name) != permissions(old_init, name):
        fail(f"{name} must keep its permissions, callbacks and descriptors")
old_wrapper_h = subprocess.check_output(["git", "show", f"{PIN}:libraries/BLEWrapper/blewrapper.h"], cwd=root, text=True)
def uuid_values(header):
    return dict(re.findall(r'#define (\w+_UUID)\s+"(\w+)"', header))
old_uuids, new_uuids = uuid_values(old_wrapper_h), uuid_values(wrapper_h)
if any(new_uuids.get(uuid) != old_uuids[uuid] for _, uuid, _ in old_chars[:8]) or new_uuids.get("SERVICE_UUID") != "ABD0" \
        or new_uuids.get("SESSION_CHARACTERISTIC_UUID") != "ABD9" or new_uuids.get("RESERVED_B_CHARACTERISTIC_UUID") != "ABDA":
    fail("the service and characteristic UUIDs must stay ABD0-ABDA")
uuids = [uuid for _, uuid, _ in new_chars]
if uuids[8:] != ["SESSION_CHARACTERISTIC_UUID", "RESERVED_B_CHARACTERISTIC_UUID"] or \
        any("PROPERTY_NOTIFY" not in props for _, _, props in new_chars[8:]):
    fail("ABD9 (the session channel) and ABDA (the spare) must be read, write and notify")
for name in ("pSessionCharacteristic", "pReservedBCharacteristic"):
    cccd = re.search(r"(\w+) = new BLE2902\(\);\s*" + name + r"->addDescriptor\(\1\);", new_init)
    if not cccd or f"{cccd.group(1)}->setAccessPermissions(enc_cccd);" not in new_init:
        fail(f"{name} must get a CCCD, encryption-gated like every other")
cccds = new_init.count("new BLE2902()")
if cccds != old_init.count("new BLE2902()") + 2 or 1 + 2 * len(new_chars) + cccds != 30 or \
        "pService = pServer->createService(BLEUUID(SERVICE_UUID), 32);" not in new_init:
    fail("the table is 1 + 10 x 2 + 9 CCCDs = 30 handles of the 32 the service reserves")
connect = code(block(wrapper, "void ServerCallbacks::onConnect(", "blewrapper.cpp"))
if not find(connect, "wrapper->clearAuthentication();", what="onConnect") < find(connect, "wrapper->set_device_connected(true);", what="onConnect") \
        or not re.search(r"volatile bool authenticated\b", wrapper_h) or not re.search(r"volatile bool setup_allowed\b", wrapper_h):
    fail("a connection must start with no grant (onConnect clears it before marking the link up), and authenticated and "
         "setup_allowed are read across tasks (volatile)")
disconnect = code(block(wrapper, "void ServerCallbacks::onDisconnect(", "blewrapper.cpp"))
if "wrapper->clearAuthSubscription();" not in disconnect or "wrapper->clearRelaySubscriptions();" not in disconnect:
    fail("a disconnect must clear the ABD8, ABD1 and ABD9 CCCDs: each is per-connection state")
clear = code(block(wrapper, "void BLEWrapper::clearRelaySubscriptions() {", "blewrapper.cpp"))
if "pTelemetryCccd" not in clear or "pSessionCharacteristic" not in clear:
    fail("clearRelaySubscriptions must clear the ABD1 and ABD9 CCCDs")
if not re.search(r'logEvt\("GATT handles', new_init[new_init.index("pService->start();"):]):
    fail("init() must log every characteristic's handle once the service has started")

# The cellular block runs whenever the phone is not the transport, relayed frame or not.
relay_start = find(logger, "    if (ble_relayed) {", what="logger.ino")
relay_end = find(logger, "\n    }\n", relay_start, "logger.ino")
if not logger[relay_end:].startswith("\n    }\n    // No phone") or \
        "    if (!bleRelaying()) {\n" not in logger[relay_end:relay_end + 400]:
    fail("the cellular block must follow the relay branch under if (!bleRelaying()), not sit in its else")
loop_start = find(logger, "void loop() {", what="logger.ino")
loop_head = logger[loop_start:find(logger, "if (current_state == UNINITIALIZED)", loop_start, "logger.ino")]
if not find(loop_head, "authGateService();", what="loop()") < find(loop_head, "relayAckService();", what="loop()") < \
        find(loop_head, "sessionChannelService();", what="loop()"):
    fail("loop() must run authGateService(), relayAckService() and sessionChannelService() in that order every "
         "iteration, ahead of its early returns")

# Presence keys on authentication; the transport (and the session's relay latch) on relaying.
if "bleRelaying()" in code((helpers / "alarm_gate.h").read_text()):
    fail("the alarm gate's 'human is here' must key on bleAuthenticated(), not the transport")
if "if (bleAuthenticated()) {\n      last_active = now;" not in (helpers / "state_machine.h").read_text():
    fail("the BLE awake leash must key on bleAuthenticated()")
if "if (bleRelaying()) g_ble_session_relayed = true;" not in (helpers / "state_machine.h").read_text():
    fail("the session's relay latch must key on bleRelaying()")

# ABD5 carries no relay_ack (versions are negotiated in the handshake, never advertised there), and the info JSON
# still fits its buffer at its worst.
ota = (helpers / "ota_update.h").read_text()
info = block(ota, "void otaPublishInfo() {", "ota_update.h")
size = int(re.search(r"char buf\[(\d+)\];", info).group(1))
fmt = re.search(r'snprintf\(buf, sizeof\(buf\),\s*"((?:[^"\\]|\\.)*)"', info).group(1).encode().decode("unicode_escape")
if "relay_ack" in fmt or "proto" in fmt:
    fail("the ABD5 info JSON must carry neither relay_ack nor a protocol version")
worst = re.sub(r"%lu|%u", "4294967295", fmt).replace("%d", "-1").replace("%s", "resume_wait", 2)
worst = worst.replace('"build":"resume_wait"', '"build":"' + "v" * 48 + '"', 1)
if len(worst) + 1 > size:
    fail(f"the ABD5 info JSON can reach {len(worst) + 1} bytes, over its {size}-byte buffer")

# Negotiation (Helpers/ble_protocol.h). auth_gate.h does not compile on a host, so what it does around bleProtoAccept is
# pinned here: the version is chosen only after the HMAC check, and nothing else in it grants a session.
auth = code((helpers / "auth_gate.h").read_text())
proof_branch = block(auth, 'if (j.contains("proof") && j["proof"].is_string()) {', "auth_gate.h")
diff_at = find(proof_branch, "if (diff != 0) {", what="the proof branch")
if find(proof_branch, "bleProtoAccept(", what="the proof branch") < diff_at + len(block(proof_branch[diff_at:], "if (diff != 0) {")):
    fail("the protocol must be selected after the HMAC check, never before it")
if not re.search(r"bleProtoAccept\(j, reply, sizeof\(reply\)\);\s*authNotifyJson\(reply\);", proof_branch):
    fail("auth_gate.h must notify bleProtoAccept's reply straight after it: the echo the app adopts is that reply")
if "set_authenticated(true)" in auth:
    fail("auth_gate.h must grant a session only through bleProtoAccept, which allows setup before the grant")
if "bleProtoRangeField(" not in block(auth, "static void authSendNonce() {", "auth_gate.h"):
    fail("authSendNonce must send the range in every nonce frame")
if not re.search(r"static_assert\([^;]*< 93,", auth):
    fail("auth_gate.h must hold the worst-case nonce frame under the 93-byte notify at compile time")
protocol = code((helpers / "ble_protocol.h").read_text())
table = block(protocol, "struct BleProtocol {", "ble_protocol.h")
if "bool (*sessionApply)(uint32_t ack, RelayVerb verb);" not in table or re.search(r"sessionMessage|relayNeedsSeq|json", table):
    fail("the protocol table takes ABD9's typed values (sessionApply), and whether it tags frames is nextSeq's alone")
if "relayStateRefresh()" in code(block((helpers / "ble_relay.h").read_text(), "void relayAckService() {", "ble_relay.h")):
    fail("relayAckService must leave ABD9's state to sessionChannelService, which refreshes it every pass")
accept = block(protocol, "void bleProtoAccept(", "ble_protocol.h")
if not find(accept, "ble.set_setup_allowed(", what="bleProtoAccept") < find(accept, "ble.set_authenticated(true)", what="bleProtoAccept"):
    fail("bleProtoAccept must allow or refuse setup before it sets the grant")
for path in [*sorted(helpers.glob("*.h")), root / "baseLogger/logger.ino"]:
    if path.name != "ble_protocol.h" and re.search(r"->version\b|\bkProtoV\d", code(path.read_text())):
        fail(f"{path.name} tests a protocol version: versions live in ble_protocol.h and its selection only")
if "return bleAuthenticated() && bleProtoPhoneIsTransport();" not in block(ble, "bool bleRelaying() {", "ble.h"):
    fail("bleRelaying() must ask the session's protocol table whether the phone is the transport")
gate_start = find(handler, "if (!wrapper->get_authenticated()) {", what="ABD2 handler")
gate_end = gate_start + len(block(handler[gate_start:], "if (!wrapper->get_authenticated()) {"))
proto_gate = find(handler, "if (!wrapper->get_setup_allowed()) {", what="ABD2 handler")
if not gate_end <= proto_gate < find(handler, "bool is_auth_write", what="ABD2 handler") or \
        '"PROTO"' not in block(handler[proto_gate:], "if (!wrapper->get_setup_allowed()) {"):
    fail("ABD2 must answer PROTO to every write in a no-protocol session, right after the NOAUTH gate")
for name in ("ota_update.h", "log_dump.h", "auth_gate.h"):
    if "setup_allowed" in code((helpers / name).read_text()):
        fail(f"{name} must not look at setup_allowed: OTA, the log dump and ABD8 work in a no-protocol session")

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-relay-checks-") as temporary:
    build = Path(temporary)
    # authSendNonce and what it needs, from this tree and from PIN (main before negotiation: the fw-none fixtures).
    def nonce_pieces(source, what):
        constants = find(source, "static const uint32_t AUTH_NONCE_TTL_MS", what=what)
        state = find(source, "static uint8_t g_auth_nonce[AUTH_NONCE_LEN];", what=what)
        return (source[constants:find(source, "\n", find(source, "static const size_t   AUTH_NONCE_LEN", what=what)) + 1]
                + source[state:find(source, "static bool g_auth_nonce_valid = false;", what=what)]
                + "static bool g_auth_nonce_valid = false;\n"
                + "".join(block(source, opener, what) for opener in (
                    "static void authNotifyJson(", "static const char* authDongleId() {",
                    "static bool authResetWindowOpen() {", "static void authSendNonce() {")))
    (build / "auth_nonce.inc").write_text(nonce_pieces((helpers / "auth_gate.h").read_text(), "auth_gate.h"))
    (build / "main_auth_nonce.inc").write_text(nonce_pieces(subprocess.check_output(
        ["git", "show", f"{PIN}:baseLogger/Helpers/auth_gate.h"], cwd=root, text=True), "PIN auth_gate.h"))
    (build / "ble_predicates.inc").write_text(block(ble, "bool bleAuthenticated() {", "ble.h")
                                              + block(ble, "uint32_t bleAuthGrant() {", "ble.h")
                                              + block(ble, "bool bleTelemetrySubscribed() {", "ble.h")
                                              + block(ble, "bool bleNotifySession(", "ble.h")
                                              + block(ble, "bool bleRelaying() {", "ble.h"))
    send_start = find(logger, "    const uint16_t ble_mtu = ble.mtu();", what="logger.ino")
    (build / "ble_send.inc").write_text(logger[send_start:find(logger, "#else\n    const bool ble_relayed = false;",
                                                               send_start, "logger.ino")])
    (build / "upload_buffer.inc").write_text(block(logger, "    if (frame_has_data) {", "logger.ino"))
    grace = find(logger, "      bool ble_upload_grace = ", what="logger.ino")
    handoff = find(logger, "      bool ble_handoff_fire = ", grace, "logger.ino")
    (build / "upload_grace.inc").write_text(logger[grace:find(logger, "\n", grace) + 1]
                                            + logger[handoff:find(logger, "\n", handoff) + 1])
    reconnect = find(logger, "  if (!g_weak_rail_cold_boot && !bleRelaying() && !g_ota_active", what="logger.ino")
    (build / "reconnect_gate.inc").write_text(logger[reconnect + len("  if ("):find(logger, ") {\n", reconnect, "logger.ino")])
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
    # Two builds: the production BLE_PROTO_MIN, and a raised minimum for the no-protocol session (design §3.4).
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    # A group name runs that group in whichever build has it, and fails when neither does (exit 3 from a build means
    # "no such group here"). The BLE_PROTO_MIN=1 build is compiled only when it has something to run.
    args = sys.argv[1:]
    group = next((arg for i, arg in enumerate(args) if not arg.startswith("--") and (i == 0 or args[i - 1] != "--depth")), None)
    builds = []
    if group is None or group not in MIN1_ONLY_GROUPS:
        builds.append(("relay_checks", []))
    if group is None or group in MIN1_GROUPS:
        builds.append(("relay_checks_min1", ["-DBLE_PROTO_MIN=1"]))
    found = False
    for name, defines in builds:
        executable = build / name
        subprocess.run([
            *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
            *defines, f'-DFIXTURE_DIR="{root / "protocol/fixtures"}"',
            "-I", str(root / "tests/support"), "-I", str(root / "libraries/nlohmann"),
            "-I", str(root / "libraries/BLEWrapper"), "-I", str(helpers), "-I", str(build),
            str(root / "tests/relay_checks.cpp"), "-o", str(executable),
        ], check=True)
        result = subprocess.run([str(executable), *args], env=environment).returncode
        if result == NO_SUCH_GROUP:
            continue
        if result:
            sys.exit(result)
        found = True
    if not found:
        fail(f"no group named {group!r}")
