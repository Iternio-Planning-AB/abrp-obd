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
header = (root / "libraries/FreematicsPlus/FreematicsPlus.h").read_text()
diagnostics_api = next(line for line in header.splitlines() if "gpsReadDiagnostics() const" in line)
diagnostics_member = next(line for line in header.splitlines() if "GPSReadDiagnostics m_gps_read_diagnostics" in line)
logger = (root / "baseLogger/logger.ino").read_text()
unlatch_start = logger.index("    if (gpsMovingFresh(GPS_MOVING_MAX_AGE_MS)) {")
unlatch_end = logger.index("\n    }\n#endif", unlatch_start) + len("\n    }")
config = (root / "baseLogger/config.h").read_text()
moving_age = next(line for line in config.splitlines() if line.startswith("#define GPS_MOVING_MAX_AGE_MS "))
clock = (root / "baseLogger/Helpers/clock_sync.h").read_text()
clock_start = clock.index("time_t utcToEpoch(")
clock_end = clock.index("\n}", clock_start) + 2
compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)
with tempfile.TemporaryDirectory(prefix="obd-heading-checks-") as temporary:
    build = Path(temporary)
    (build / "gps_get_data.inc").write_text(source[start:end])
    (build / "gps_read_diagnostics_public.inc").write_text(diagnostics_api + "\n")
    (build / "gps_read_diagnostics_private.inc").write_text(diagnostics_member + "\n")
    (build / "gps_unlatch.inc").write_text(moving_age + "\n" + logger[unlatch_start:unlatch_end])
    (build / "utc_converter.inc").write_text(clock[clock_start:clock_end])
    quality_start = clock.index("bool clockValid(")
    quality_end = clock.index("// UTC calendar time", quality_start)
    (build / "clock_quality.inc").write_text(clock[quality_start:quality_end])
    reset_line = next(line for line in logger.splitlines() if "resetFrozenGpsProvenance(" in line)
    (build / "gps_reset.inc").write_text(reset_line)
    if "sys.gpsGetData(&gd)" in logger or logger.count("pollGpsData();") != 4:
        raise RuntimeError("all four logger GPS polls must use provenance wrapper")
    frame_start = logger.index("    scrubGeoMetadata(current_frame);")
    frame_end = logger.index("    if (rtc.getEpoch() > 1e9)", frame_start)
    (build / "gps_frame.inc").write_text(logger[frame_start:frame_end])
    upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
    backlog_start = upload.index('static const char* UPLOAD_BACKLOG_PATH')
    backlog_end = upload.index('\n#endif', backlog_start)
    (build / "backlog.inc").write_text(upload[backlog_start:backlog_end])
    historical_upload = subprocess.check_output(
        ["git", "show", "eacc642:baseLogger/Helpers/network_upload.h"], cwd=root, text=True)
    historical_format = subprocess.check_output(
        ["git", "show", "eacc642:baseLogger/Helpers/telemetry_format.h"], cwd=root, text=True)
    historical_start = historical_upload.index('static const char* UPLOAD_BACKLOG_PATH')
    historical_end = historical_upload.index('\n#endif', historical_start)
    (build / "historical_backlog.inc").write_text(historical_upload[historical_start:historical_end])
    (build / "historical_format.inc").write_text(historical_format)
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
