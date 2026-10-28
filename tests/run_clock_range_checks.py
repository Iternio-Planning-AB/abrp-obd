import argparse
import re
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("group", nargs="?", default="all", choices=["all", "formatter", "clock", "host", "drift", "resync", "bounds", "modem"])
parser.add_argument("--target-cxx")
args = parser.parse_args()
clock = (root / "baseLogger/Helpers/clock_sync.h").read_text()
upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
sleep = (root / "baseLogger/Helpers/sleep_power.h").read_text()
formatter = (root / "baseLogger/Helpers/telemetry_format.h").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)

logger = (root / "baseLogger/logger.ino").read_text()
# #13, and the only check of it: a post-OTA trial boot zeroes the clock like a cold boot.
assert "rtc.setTime(0)" in logger and not re.search(r"otaIsTrialBoot\(\)[^;\n]*rtc\.setTime\(0\)", logger), \
    "the trial boot must zero the clock"

def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

with tempfile.TemporaryDirectory(prefix="obd-clock-range-checks-") as temporary:
    build = Path(temporary)
    (build / "clock_quality.inc").write_text(section(clock, "bool clockValid(", "// UTC calendar time"))
    (build / "clock_live.inc").write_text(section(clock, "// The learned rate survives deep sleep", "// Calendar fields"))
    (build / "clock_parse.inc").write_text(section(clock, "// Calendar fields", "// Parse an RFC-1123"))
    (build / "clock_call_site.inc").write_text(section(logger, "clockNtpAfterUpload(", "\n") + "\n")
    (build / "clock_flush_site.inc").write_text(section(sleep, "if (ok) clockNtpAfterUpload(", ";\n") + ";\n")
    (build / "clock_loop_site.inc").write_text(section(logger, 'if (!clockResync("loop"))', "\n") + "\n")
    (build / "clock_resync.inc").write_text(section(clock, "// One clock resync from the modem", "/*\n  Bench-only clock diagnostic"))
    (build / "upload_snapshot.inc").write_text(section(upload, "static double uploadSendUtc()", "bool logNetwork("))
    (build / "target_formatter.inc").write_text(formatter.replace("#pragma once\n", "", 1))
    executable = build / "clock_range_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
        "-I", str(build), "-I", str(root / "libraries/nlohmann"), "-I", str(root / "baseLogger/Helpers"),
        str(root / "tests/clock_range_checks.cpp"), "-o", str(executable),
    ], check=True)
    cell_header = (root / "libraries/NetworkWrapper/cellwrapper.h").read_text()
    cell_source = (root / "libraries/NetworkWrapper/cellwrapper.cpp").read_text()
    (build / "cell_ntp_result.inc").write_text(section(cell_header, "struct NtpResult", "class CellWrapper"))
    (build / "cell_ntp.inc").write_text(section(cell_source, "bool CellWrapper::syncNtp(", "std::string CellWrapper::sendRawAt("))
    modem = build / "clock_modem_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
        "-I", str(build), str(root / "tests/clock_modem_checks.cpp"), "-o", str(modem),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    if args.group != "modem":
        subprocess.run([str(executable), args.group], env=environment, check=True)
    if args.group in {"all", "modem"}:
        subprocess.run([str(modem)], env=environment, check=True)
    if args.target_cxx:
        probe = build / "native_clock_range_probe.cpp"
        probe.write_text(
            '#include <cmath>\n#include <cstdint>\n#include <ctime>\n#include <limits>\n#include <type_traits>\n'
            f'#include "{root / "baseLogger/config.h"}"\n'
            '#define RTC_DATA_ATTR\n'
            'struct RTCStub { uint32_t getEpoch() { return UINT32_MAX; } } rtc;\n'
            '#include "clock_quality.inc"\n#include "upload_snapshot.inc"\n#include <cstdio>\n#include "clock_parse.inc"\n'
            + section(formatter, "bool geoValidUtc(", "bool geoValidLocation(")
            + 'static_assert(sizeof(time_t) == 4 && std::numeric_limits<time_t>::is_signed, "actual target native calendar is signed32");\n'
            'static_assert(sizeof(last_clock_sync_epoch) == 4 && std::is_same<decltype(last_clock_sync_epoch), time_t>::value, "RTC sync metadata ABI preserved");\n'
            'static_assert(CLOCK_MAX_EPOCH == uint64_t(INT32_MAX), "actual native positive bound is INT32_MAX");\n'
            'static_assert(uint64_t(INT32_MAX) <= CLOCK_MAX_EPOCH && uint64_t(INT32_MAX) + 1 > CLOCK_MAX_EPOCH && uint64_t(UINT32_MAX) > CLOCK_MAX_EPOCH, "native boundary rejects post2038 uint32 clocks");\n'
            'bool (*valid_raw)(uint64_t) = clockValid;\n'
            'bool (*stale_raw)(uint64_t) = clockStale;\n'
            'bool (*sync_raw)(uint64_t) = clockNeedsSync;\n'
        )
        subprocess.run([*shlex.split(args.target_cxx), "-std=gnu++11", "-fsyntax-only", "-I", str(build), str(probe)], check=True)
        print("PASS actual target signed32/native-bound/RTC-ABI/wide-predicate/int64-calendar compile probes", flush=True)
