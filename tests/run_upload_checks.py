import base64
import hashlib
import hmac
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "libraries/FreematicsPlus/FreematicsNetwork.cpp").read_text()
header = (root / "libraries/FreematicsPlus/FreematicsNetwork.h").read_text()
logging = (root / "baseLogger/Helpers/logging.h").read_text()
clock = (root / "baseLogger/Helpers/clock_sync.h").read_text()
logger = (root / "baseLogger/logger.ino").read_text()
streak_line = next(line for line in logger.splitlines() if "if (!g_upload_local_fail)" in line)
geo_reset_line = next(line for line in logger.splitlines() if "resetGeoAccepted(" in line)
open_streak_line = next(line for line in logger.splitlines() if "if (!network.trustRefused())" in line)

def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
group = sys.argv[1] if len(sys.argv) > 1 else "all"
if group not in {"all", "body-ack", "stages", "responses", "unknown", "fingerprint", "errors", "queue", "geo-expiry", "geo-retry", "geo-sizing", "geo-dedup", "logging-off", "tls-ca", "signing"}:
    sys.exit("Unknown upload test group: " + group)
environment = dict(os.environ)
environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
with tempfile.TemporaryDirectory(prefix="obd-upload-checks-") as temporary:
    build = Path(temporary)
    (build / "upload_types.inc").write_text(section(header, "struct commandResponse", "class HTTPClientClass"))
    (build / "upload_getter.inc").write_text(next(line for line in header.splitlines() if "const HTTPTransferDiagnostics& transferDiagnostics()" in line))
    (build / "upload_member.inc").write_text(next(line for line in header.splitlines() if "HTTPTransferDiagnostics m_transfer_diagnostics;" in line))
    (build / "upload_send.inc").write_text(section(source, "commandResponse HTTPClientSIM::send(const char* api_page", "bool HTTPClientSIM::send(HTTP_METHOD"))
    (build / "upload_parse.inc").write_text(section(source, "std::pair<int, int> handlePair", "commandResponse HTTPClientSIM::getEpoch"))
    (build / "header_budget.inc").write_text(section(header, "// Request headers the SIM7070", "typedef enum {"))
    (build / "upload_streak.inc").write_text(streak_line + "\n")
    (build / "geo_reset.inc").write_text(geo_reset_line + "\n")
    (build / "open_streak.inc").write_text(open_streak_line + "\n")
    (build / "tls_defines.inc").write_text(section(header, "// Root-CA install (HTTPClientSIM::ensureCa)", "#define RECV_BUF_SIZE"))
    (build / "tls_members.inc").write_text(section(header, "    enum CaState", "    HTTPTransferDiagnostics m_transfer_diagnostics;") +
                                           section(header, "    char m_open_trace[", "\n};"))
    (build / "tls_open.inc").write_text(section(source, "int HTTPClientSIM::bearerState", "bool HTTPClientSIM::close()"))
    (build / "upload_command.inc").write_text(section(source, "commandResponse CellSIMCOM::sendCommand", "float CellSIMCOM::parseDegree"))
    (build / "upload_log_event.inc").write_text(section(logging, "void logEvent(const char* fmt", "// Defined in ota_update.h"))
    (build / "clock_quality.inc").write_text(section(clock, "bool clockValid(", "// UTC calendar time"))
    (build / "FreematicsPlus.h").write_text('#pragma once\n#include "upload_transport_fixture.h"\nstruct FreematicsESP32 {};\n')
    flags = [
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DNDEBUG",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function", "-Wno-deprecated-declarations",
        "-I", str(build), "-I", str(root / "tests"), "-I", str(root / "tests/support"), "-I", str(root / "libraries/nlohmann"),
        "-I", str(root / "libraries/NetworkWrapper"), "-I", str(root / "baseLogger/Helpers"),
    ]
    targets = []
    if group in {"all", "body-ack", "stages", "responses"}:
        targets.append(("upload_checks", "upload_checks.cpp", [], group))
    if group in {"all", "fingerprint", "errors", "queue", "geo-expiry", "geo-retry", "geo-sizing", "geo-dedup", "signing"}:
        targets.append(("upload_helpers_checks", "upload_helpers_checks.cpp", [], group))
    if group in {"all", "tls-ca", "signing"}:
        targets.append(("tls_ca_checks", "tls_ca_checks.cpp", [], group))
    if group in {"all", "logging-off"}:
        targets.append(("upload_logging_off_checks", "upload_helpers_checks.cpp", ["-DENABLE_LOGGING=0"], "all"))
    for name, filename, defines, selected_group in targets:
        # The signing group's recomputes are counted, so a check that stops emitting them fails here.
        expected_sigchecks = 3 if filename == "upload_helpers_checks.cpp" and selected_group in {"all", "signing"} else 0
        expected_hashes = 2 if (filename == "upload_helpers_checks.cpp" and selected_group in {"all", "fingerprint"}
                                and "-DENABLE_LOGGING=0" not in defines) else 0
        sigchecks = hashes = 0
        executable = build / name
        subprocess.run([*flags, *defines, str(root / "tests" / filename), "-o", str(executable)], check=True)
        with subprocess.Popen([str(executable), selected_group], env=environment, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                if line.startswith("SIGCHECK "):
                    token, wire, digest, sig_input, signature = (bytes.fromhex(f) for f in line.split()[1:])
                    keyid = hashlib.sha256(token).hexdigest()[:32]
                    want_digest = b"sha-256=:" + base64.b64encode(hashlib.sha256(wire).digest()) + b":"
                    want_input = b's=("content-digest");keyid="' + keyid.encode() + b'"'
                    base = b'"content-digest": ' + want_digest + b'\n"@signature-params": ' + want_input[len(b"s="):]
                    want_signature = b"s=:" + base64.b64encode(hmac.new(token, base, hashlib.sha256).digest()) + b":"
                    for field, got, want in (("Content-Digest", digest, want_digest), ("Signature-Input", sig_input, want_input),
                                            ("Signature", signature, want_signature)):
                        if got != want:
                            process.kill()
                            sys.exit(f"FAIL: {field} expected {want!r}, got {got!r}")
                    sigchecks += 1
                    print(f"PASS Python hashlib/hmac recomputes the signature headers ({len(wire)}-byte body)", flush=True)
                elif line.startswith("HASH "):
                    _, digest, payload = line.split()
                    expected = hashlib.sha256(bytes.fromhex(payload)).hexdigest()[:12]
                    if digest != expected:
                        process.kill()
                        sys.exit(f"FAIL: logged payload fingerprint expected {expected}, got {digest}")
                    hashes += 1
                    print("PASS Python hashlib matches the logged payload fingerprint", flush=True)
                else:
                    print(line, end="", flush=True)
            if process.wait():
                sys.exit(process.returncode)
        if sigchecks != expected_sigchecks or hashes != expected_hashes:
            sys.exit(f"FAIL: {name} ran {sigchecks} signature recomputes and {hashes} fingerprint checks, "
                     f"expected {expected_sigchecks} and {expected_hashes}")
