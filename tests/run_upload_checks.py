import hashlib
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

def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
group = sys.argv[1] if len(sys.argv) > 1 else "all"
if group not in {"all", "body-ack", "stages", "responses", "unknown", "fingerprint", "errors", "queue", "logging-off"}:
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
    (build / "upload_command.inc").write_text(section(source, "commandResponse CellSIMCOM::sendCommand", "float CellSIMCOM::parseDegree"))
    (build / "upload_log_event.inc").write_text(section(logging, "void logEvent(const char* fmt", "// Defined in ota_update.h"))
    (build / "FreematicsPlus.h").write_text('#pragma once\n#include "upload_transport_fixture.h"\nstruct FreematicsESP32 {};\n')
    (build / "mbedtls").mkdir()
    (build / "mbedtls/sha256.h").write_text('#include "upload_sha256_support.h"\n')
    flags = [
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DNDEBUG",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function", "-Wno-deprecated-declarations",
        "-I", str(build), "-I", str(root / "tests"), "-I", str(root / "libraries/nlohmann"),
        "-I", str(root / "libraries/NetworkWrapper"), "-I", str(root / "baseLogger/Helpers"),
    ]
    targets = []
    if group in {"all", "body-ack", "stages", "responses"}:
        targets.append(("upload_checks", "upload_checks.cpp", [], group))
    if group in {"all", "fingerprint", "errors", "queue"}:
        targets.append(("upload_helpers_checks", "upload_helpers_checks.cpp", [], group))
    if group in {"all", "logging-off"}:
        targets.append(("upload_logging_off_checks", "upload_helpers_checks.cpp", ["-DENABLE_LOGGING=0"], "queue"))
    for name, filename, defines, selected_group in targets:
        executable = build / name
        subprocess.run([*flags, *defines, str(root / "tests" / filename), "-o", str(executable)], check=True)
        with subprocess.Popen([str(executable), selected_group], env=environment, text=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                if line.startswith("SHA256 "):
                    digest = hashlib.sha256(bytes.fromhex(line[7:].strip())).hexdigest()
                    process.stdin.write(digest + "\n")
                    process.stdin.flush()
                elif line.startswith("HASH "):
                    _, digest, payload = line.split()
                    expected = hashlib.sha256(bytes.fromhex(payload)).hexdigest()[:12]
                    if digest != expected:
                        process.kill()
                        sys.exit(f"FAIL: final payload SHA256 expected {expected}, got {digest}")
                    print("PASS Python hashlib final-payload oracle", flush=True)
                else:
                    print(line, end="", flush=True)
            if process.wait():
                sys.exit(process.returncode)
