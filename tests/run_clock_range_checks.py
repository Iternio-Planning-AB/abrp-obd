import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("group", nargs="?", default="all", choices=["all", "formatter", "clock", "host", "drift"])
parser.add_argument("--target-cxx")
args = parser.parse_args()
clock = (root / "baseLogger/Helpers/clock_sync.h").read_text()
upload = (root / "baseLogger/Helpers/network_upload.h").read_text()
formatter = (root / "baseLogger/Helpers/telemetry_format.h").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))
print(subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0], flush=True)

def section(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

with tempfile.TemporaryDirectory(prefix="obd-clock-range-checks-") as temporary:
    build = Path(temporary)
    (build / "clock_quality.inc").write_text(section(clock, "bool clockValid(", "// UTC calendar time"))
    (build / "clock_live.inc").write_text(section(clock, "// The learned rate survives deep sleep", "// Parse a SIM modem"))
    (build / "upload_snapshot.inc").write_text(section(upload, "static double uploadSendUtc()", "bool logNetwork("))
    (build / "target_formatter.inc").write_text(formatter.replace("#pragma once\n", "", 1))
    executable = build / "clock_range_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
        "-I", str(build), "-I", str(root / "libraries/nlohmann"), "-I", str(root / "baseLogger/Helpers"),
        str(root / "tests/clock_range_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    subprocess.run([str(executable), args.group], env=environment, check=True)
    if args.target_cxx:
        probe = build / "native_clock_range_probe.cpp"
        probe.write_text(
            '#include <cmath>\n#include <cstdint>\n#include <ctime>\n#include <limits>\n#include <type_traits>\n'
            f'#include "{root / "baseLogger/config.h"}"\n'
            '#define RTC_DATA_ATTR\n'
            'struct RTCStub { uint32_t getEpoch() { return UINT32_MAX; } } rtc;\n'
            '#include "clock_quality.inc"\n#include "upload_snapshot.inc"\n'
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
        print("PASS actual target signed32/native-bound/RTC-ABI/wide-predicate compile probes", flush=True)
