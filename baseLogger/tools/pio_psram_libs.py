"""
PlatformIO extra_script: link the toolchain's PSRAM-safe C/C++ runtime, and refuse an image without it.

The ONE+ is an ESP32 rev 1, whose cache can lose a store to PSRAM, and mallocs over 4 KB (smaller ones too once
internal RAM runs short) go to PSRAM. -mfix-esp32-psram-cache-issue avoids the code that triggers it, but the
Arduino core passes it to the compiler only, and gcc picks the runtime multilib (libc, libstdc++, libm, libgcc)
from the link line (#64).
Not covered: ROM code, such as the miniz tdefl gzip_body.h runs on a PSRAM compressor, and ROM libgcc.
"""

import os
import sys

Import("env")  # noqa: F821 - SCons builtin

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))
import psram_libs  # noqa: E402

_errors = []
if psram_libs.FLAG not in env.Flatten(env.get("CCFLAGS", [])):
    _errors.append(f"{psram_libs.FLAG} is not in the compile flags (platformio.ini build_flags)")
_errors += [f"links {script}, whose ROM functions have no PSRAM workaround"
            for script in psram_libs.rom_newlib_scripts(env.Flatten(env.get("LINKFLAGS", [])))]
if _errors:
    sys.stderr.write("ERROR: " + "\nERROR: ".join(_errors) + " (#64)\n")
    env.Exit(1)
env.Append(LINKFLAGS=[psram_libs.FLAG])


def _check_map(source, target, env):
    try:
        with open(env.subst("$BUILD_DIR/${PROGNAME}.map"), encoding="utf-8", errors="replace") as f:
            unsafe = psram_libs.unsafe_objects(f.read())
    except Exception as e:  # noqa: BLE001 - a check that cannot run is a failed check
        unsafe = [f"map check failed: {e}"]
    if not unsafe:
        print(f"PSRAM runtime: every toolchain object is from {psram_libs.MULTILIB}")
        return 0
    sys.stderr.write(f"ERROR: linked without the PSRAM workaround (want {psram_libs.MULTILIB}, #64):\n  "
                     + "\n  ".join(unsafe) + "\n")
    # SCons keeps a failed action's target: remove the images so none of them can be flashed by hand.
    for image in ("${PROGNAME}.elf", "${PROGNAME}.bin", "factory.bin"):
        path = env.subst("$BUILD_DIR/" + image)
        if os.path.exists(path):
            os.remove(path)
    return 1


def _remove_map(target, source, env):
    # a link that stopped writing the map must not be checked against the last one
    if os.path.exists(env.subst("$BUILD_DIR/${PROGNAME}.map")):
        os.remove(env.subst("$BUILD_DIR/${PROGNAME}.map"))


env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", _remove_map)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _check_map)
