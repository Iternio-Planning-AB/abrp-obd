"""Was the toolchain runtime linked from its ESP32 PSRAM-workaround multilib? Used by pio_psram_libs.py, and by
the release workflow as `python3 psram_libs.py <firmware.map>`."""

import re
import sys

FLAG = "-mfix-esp32-psram-cache-issue"
MULTILIB = "esp32-psram"  # IDF's esp-13 toolchains spell it esp32_psram, under xtensa-esp-elf

# Every toolchain object or archive in the map; framework and project paths never contain the target triple.
_TOOLCHAIN_OBJECT = re.compile(r"[^\s()]*/xtensa-esp32-elf/[^\s()]*\.[ao]\b(?:\([^\s()]+\))?")
# The multilib directory itself, under the toolchain's lib or gcc/<version>, not a directory above the toolchain.
_PSRAM_MULTILIB = re.compile(r"/(?:xtensa-esp32-elf/lib|gcc/xtensa-esp32-elf/[^/]+)/" + MULTILIB + "/")
# Must come from the PSRAM libc: a ROM newlib linker script would resolve them to ROM, which has no workaround.
_STRING_MEMBERS = ("lib_a-memcpy.o", "lib_a-memset.o")
# ESP-IDF leaves these out when the workaround is on.
_ROM_NEWLIB = re.compile(r"esp32\.rom\.newlib-(?!data\.ld$)[\w-]+\.ld$")


def unsafe_objects(map_text):
    """Toolchain objects in a GNU ld map that are not from the PSRAM multilib, in map order, then each string
    function the PSRAM libc did not supply. Empty when safe; ValueError when the map has no toolchain object."""
    lines = "\n".join(line for line in map_text.splitlines() if "/xtensa-esp32-elf/" in line)
    matches = _TOOLCHAIN_OBJECT.findall(lines)
    if not matches:
        raise ValueError("no xtensa-esp32-elf toolchain object in the linker map")
    unsafe, members = [], set()
    for match in matches:
        archive, _, member = match.partition("(")
        if not _PSRAM_MULTILIB.search(archive):
            if archive not in unsafe:
                unsafe.append(archive)
        elif archive.endswith("/libc.a"):
            members.add(member.rstrip(")"))
    return unsafe + [f"{name} not from the PSRAM libc" for name in _STRING_MEMBERS if name not in members]


def rom_newlib_scripts(linkflags):
    """ROM newlib linker scripts on the link line: their functions run without the workaround."""
    return [flag for flag in linkflags if _ROM_NEWLIB.search(str(flag))]


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        problems = unsafe_objects(f.read())
    print("\n".join(problems) or f"PSRAM runtime: every toolchain object is from {MULTILIB}")
    sys.exit(1 if problems else 0)
