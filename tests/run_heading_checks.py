import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "libraries/FreematicsPlus/FreematicsPlus.cpp").read_text()
start = source.index("bool FreematicsESP32::gpsGetData(GPS_DATA** pgd) {")
end = source.index("\nint FreematicsESP32::gpsGetNMEA(", start)
logger = (root / "baseLogger/logger.ino").read_text()
unlatch_start = logger.index("    if (gpsMovingFresh(GPS_MOVING_MAX_AGE_MS)) {")
unlatch_end = logger.index("\n    }\n#endif", unlatch_start) + len("\n    }")
config = (root / "baseLogger/config.h").read_text()
moving_age = next(line for line in config.splitlines() if line.startswith("#define GPS_MOVING_MAX_AGE_MS "))
compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-heading-checks-") as temporary:
    build = Path(temporary)
    (build / "gps_get_data.inc").write_text(source[start:end])
    (build / "gps_unlatch.inc").write_text(moving_age + "\n" + logger[unlatch_start:unlatch_end])
    executable = build / "heading_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
        "-I", str(root / "tests/support"), "-I", str(root / "libraries/FreematicsPlus"),
        "-I", str(root / "libraries/nlohmann"), "-I", str(root / "baseLogger/Helpers"),
        "-I", str(build), str(root / "tests/heading_checks.cpp"),
        str(root / "libraries/FreematicsPlus/FreematicsGPS.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    result = subprocess.run([str(executable), *sys.argv[1:]], env=environment)
    sys.exit(result.returncode)
