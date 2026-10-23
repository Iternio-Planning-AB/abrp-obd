#!/usr/bin/env python3
"""Host checks for config.h feature macros: ENABLE_NETWORK must behave as one value inside any #if.

`#define ENABLE_NETWORK ENABLE_CELL || ENABLE_WIFI` made `#if ENABLE_NETWORK && ENABLE_LOGGING` read as
`ENABLE_CELL || (ENABLE_WIFI && ENABLE_LOGGING)`, true on the cellular build with logging off. Each probe
below is a #error that fires when the expression differs from the one computed here from the flags, for
the default build and every flag set tools/check_variants.sh builds, plus both transports off.
config.h includes only standard headers, so it is preprocessed as it stands.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "clang++"))

variants = [""] + re.findall(r'"(-D[^"]+)"', (ROOT / "baseLogger/tools/check_variants.sh").read_text())
variants += ["-DENABLE_CELL=0 -DENABLE_WIFI=0"]
assert len(variants) == 5, "check_variants.sh no longer lists its flag sets as quoted -D strings"

failed = 0
for flags in variants:
    defined = dict(re.findall(r"-D(\w+)=(\d)", flags))
    cell = int(defined.get("ENABLE_CELL", 1))
    wifi = int(defined.get("ENABLE_WIFI", 0))
    logging = int(defined.get("ENABLE_LOGGING", 1))
    if cell and wifi:
        continue  # config.h #errors on it, on purpose
    network = int(bool(cell or wifi))
    probes = {
        "ENABLE_NETWORK && ENABLE_LOGGING": int(network and logging),
        "!ENABLE_NETWORK": int(not network),
    }
    source = '#include "config.h"\n' + "".join(
        f"#if ({expr}) != {want}\n#error \"{expr} is not {want}\"\n#endif\n" for expr, want in probes.items())
    with tempfile.TemporaryDirectory(prefix="config-checks-") as temporary:
        probe = Path(temporary) / "probe.cpp"
        probe.write_text(source)
        run = subprocess.run([*compiler, "-std=c++17", "-fsyntax-only", "-I", str(ROOT / "baseLogger"),
                              *flags.split(), str(probe)], capture_output=True, text=True)
    name = flags or "default"
    if run.returncode:
        failed += 1
        print(f"FAIL: {name}\n{run.stderr.strip()}")
    else:
        print(f"ok: {name}")
sys.exit(1 if failed else 0)
