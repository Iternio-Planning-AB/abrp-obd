import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

arguments = argparse.ArgumentParser()
arguments.add_argument("group", nargs="?", default="all",
                       choices=["all", "good", "stray-prefix", "foreign-header", "wrong-did", "no-anchor",
                                "truncated"])
args = arguments.parse_args()
root = Path(__file__).resolve().parents[1]
wrapper = (root / "libraries/OBDWrapper/obdwrapper.cpp").read_text()
compiler = shlex.split(os.environ.get("CXX", "clang++"))
with tempfile.TemporaryDirectory(prefix="obd-reply-checks-") as temporary:
    build = Path(temporary)
    # The real reply parser, from the exception type through genericProtocol().
    start = wrapper.index("struct UnexpectedPidResponse")
    end = wrapper.index("bool OBDWrapper::readBuffer(", start)
    (build / "generic_protocol.inc").write_text(wrapper[start:end])
    executable = build / "obd_reply_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-sign-compare",
        "-I", str(build), str(root / "tests/obd_reply_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    subprocess.run([str(executable), args.group], env=environment, check=True)
