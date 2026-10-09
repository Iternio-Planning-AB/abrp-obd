"""
PlatformIO extra_script: inject OTA_FW_VERSION and FW_BUILD_TAG (tools/fw_version.py), and add a `factory`
target that merges bootloader + partitions + boot_app0 + app into one image for esptool on a blank unit.

Defines go to projenv only, so a new commit rebuilds the sketch and not the Arduino core and libraries.
"""

import os
import sys

Import("env", "projenv")  # noqa: F821 - SCons builtin

sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "tools"))
import fw_version  # noqa: E402

try:
    _name, _code, _tag = fw_version.build_version(env.subst("$PROJECT_DIR"), os.environ)
except ValueError as e:
    sys.stderr.write(f"ERROR: RELEASE_VERSION: {e}\n")
    env.Exit(1)

projenv.Append(CPPDEFINES=[("OTA_FW_VERSION", _code), ("FW_BUILD_TAG", env.StringifyMacro(_tag))])
print(f"Firmware version: {_tag} (OTA_FW_VERSION {_code})")


def _merge_factory(source, target, env):
    # Offsets come from the platform's own flash layout for this board/partition table, so they always match what
    # `pio run -t upload` writes. The image is 0xFF-padded, so flashing it erases NVS (pairing, OTA floor).
    images = [(env.subst(off), env.subst(path)) for off, path in env.get("FLASH_EXTRA_IMAGES", [])]
    images.append((env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")))
    out = env.subst("$BUILD_DIR/factory.bin")
    cmd = ['"$PYTHONEXE"', '"$UPLOADER"', "--chip", "esp32", "merge_bin", "-o", f'"{out}"',
           "--flash_mode", env.subst("${__get_board_flash_mode(__env__)}"),
           "--flash_size", env.subst("$BOARD_UPLOAD_FLASH_SIZE") or env.BoardConfig().get("upload.flash_size")]
    for off, path in images:
        cmd += [off, f'"{path}"']
    return env.Execute(" ".join(cmd))


env.AddCustomTarget(
    name="factory",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=[_merge_factory],
    title="Factory image",
    description="Merge bootloader, partitions, boot_app0 and the app into factory.bin",
)
