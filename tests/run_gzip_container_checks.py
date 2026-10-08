import gzip
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib

root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "clang++"))
with tempfile.TemporaryDirectory(prefix="obd-gzip-container-") as temporary:
    build = Path(temporary)
    for name in ["esp_heap_caps.h", "esp32/rom/miniz.h", "esp32/rom/crc.h"]:
        stub = build / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("#pragma once\n")
    executable = build / "gzip_container_checks"
    subprocess.run([
        *compiler, "-std=c++17", "-DNDEBUG", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", "-I", str(build), "-I", str(root / "baseLogger/Helpers"),
        str(root / "tests/gzip_container_checks.cpp"), "-o", str(executable),
    ], check=True)
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    inputs = [b"", b"hello", b'[{"provider":"ABRP_OTA","soc":{"time":"2026-10-07T04:49:03Z","frac":0.5}}]',
              bytes(range(256)) * 16, b"repeated telemetry " * 2000]
    for raw in inputs:
        compressor = zlib.compressobj(wbits=-15)
        deflated = compressor.compress(raw) + compressor.flush()
        wire = subprocess.check_output([str(executable), raw.hex(), deflated.hex()], env=environment)
        if gzip.decompress(wire) != raw:
            raise AssertionError("Production gzip container failed independent decompression")
        if wire[:10] != b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff":
            raise AssertionError("Gzip header mismatch")
        if struct.unpack("<II", wire[-8:]) != (zlib.crc32(raw), len(raw)):
            raise AssertionError("Gzip CRC32/ISIZE trailer mismatch")
    for failure in ["alloc", "init", "deflate"]:
        subprocess.run([str(executable), "68656c6c6f", "cb48cdc9c90700", failure], env=environment, check=True)
    print("PASS gzip container: five independent Python round trips and three failure paths")
    print("ROM deflator, ROM CRC implementation, PSRAM and modem are host-shimmed, not hardware verified")
