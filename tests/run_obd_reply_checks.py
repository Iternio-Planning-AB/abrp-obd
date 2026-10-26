import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

arguments = argparse.ArgumentParser()
arguments.add_argument("group", nargs="?", default="all",
                       choices=["all", "good", "stray-prefix", "foreign-header", "wrong-did", "no-anchor",
                                "truncated", "header", "expected", "retry", "profile", "failures"])
args = arguments.parse_args()
root = Path(__file__).resolve().parents[1]
wrapper = (root / "libraries/OBDWrapper/obdwrapper.cpp").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))
# Before the frame is used: a profile's own is_parked (MEB's B==8) is dropped after obd_count counts it and before the
# drive latch appends the firmware's, so a frame never carries two (#84).
logger = (root / "baseLogger/logger.ino").read_text()
count = logger.index("    int obd_count = (int)current_frame.size();")
strip = logger.index('[](const std::pair<std::string, double>& r) { return r.first == "is_parked"; }), current_frame.end());')
if not count < strip < logger.index('current_frame.emplace_back("is_parked"') or logger.count('return r.first == "is_parked";') != 1:
    raise SystemExit("FAIL: logger.ino must drop the profile's is_parked once, after obd_count and before the latch's")
with tempfile.TemporaryDirectory(prefix="obd-reply-checks-") as temporary:
    build = Path(temporary)
    # The real reply parser, from the exception type through genericProtocol().
    start = wrapper.index("struct UnexpectedPidResponse")
    end = wrapper.index("bool OBDWrapper::readBuffer(", start)
    (build / "generic_protocol.inc").write_text(wrapper[start:end])
    flags = ["-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
             "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-sign-compare"]
    executable = build / "obd_reply_checks"
    subprocess.run([
        *compiler, *flags, "-I", str(build), str(root / "tests/obd_reply_checks.cpp"), "-o", str(executable),
    ], check=True)
    # The real readPIDs() for the retry: all of obdwrapper.cpp, with tests/obd/FreematicsOBD.h in place of the
    # co-processor library.
    retry = build / "obd_retry_checks"
    subprocess.run([
        *compiler, *flags, "-Wno-unused-function", "-Wno-format", "-Wno-deprecated-declarations", "-Wno-parentheses",
        "-Wno-string-plus-int", "-I", str(root / "tests/obd"),
        "-I", str(root / "tests/support"), "-I", str(root / "libraries/OBDWrapper"),
        "-I", str(root / "libraries/nlohmann"), "-I", str(root / "libraries/mathparser"),
        str(root / "tests/obd_retry_checks.cpp"), str(root / "libraries/OBDWrapper/obdwrapper.cpp"),
        str(root / "libraries/mathparser/parser.cpp"), str(root / "libraries/mathparser/expression_tree.cpp"),
        "-o", str(retry),
    ], check=True)
    # The #85 OBDFAIL block from loop(), from the profile check through the logging loop.
    logger = (root / "baseLogger/logger.ino").read_text()
    start = logger.index("      if (obd.pids_hash != obd_fail_profile) {")
    loop = logger.index("      for (const auto& f : obd.failures) {", start)
    depth, i = 0, logger.index("{", loop)
    while True:
        depth += {"{": 1, "}": -1}.get(logger[i], 0)
        i += 1
        if depth == 0:
            break
    (build / "obd_fail_log.inc").write_text(logger[start:i] + "\n")
    fail_log = build / "obd_fail_log_checks"
    subprocess.run([*compiler, *flags, "-I", str(build), str(root / "tests/obd_fail_log_checks.cpp"), "-o", str(fail_log)],
                   check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    if args.group not in ("retry", "profile", "failures"):
        subprocess.run([str(executable), args.group], env=environment, check=True)
    if args.group in ("all", "retry", "profile", "failures"):
        subprocess.run([str(retry), args.group], env=environment, check=True)
    if args.group in ("all", "failures"):
        subprocess.run([str(fail_log)], env=environment, check=True)
