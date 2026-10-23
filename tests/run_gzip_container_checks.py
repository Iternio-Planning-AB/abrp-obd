import gzip
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import struct
import subprocess
import tempfile
import time
import zlib

root = Path(__file__).resolve().parents[1]
vendor = root / "libraries/zlib"
compiler = shlex.split(os.environ.get("CXX", "clang++"))
c_compiler = shlex.split(os.environ.get("CC", "clang"))
# The C build takes the same NDEBUG choice as the C++ one, so CXX='clang++ -DNDEBUG' covers both.
c_compiler += [token for token in compiler[1:] if token == "-DNDEBUG" and token not in c_compiler]
sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]

# Vendored zlib must be exactly the verified 1.3.2 files: no drift, nothing extra that could compile.
manifest = {}
for line in (vendor / "SHA256SUMS").read_text().splitlines():
    digest, name = line.split()
    manifest[name] = digest
local = {"README.md", "library.json", "SHA256SUMS"}
present = {path.name for path in vendor.iterdir()} - local
if present != set(manifest):
    raise AssertionError(f"libraries/zlib does not match SHA256SUMS: extra {present - set(manifest)}, missing {set(manifest) - present}")
for name, digest in manifest.items():
    if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != digest:
        raise AssertionError(f"libraries/zlib/{name} drifted from SHA256SUMS")
sources = sorted(name for name in manifest if name.endswith(".c"))
# library.json is ours, not zlib's, but it decides how zlib is built: pin it exactly.
expected_library = {
    "name": "zlib",
    "version": "1.3.2",
    "description": "zlib deflate/inflate, vendored unmodified for the gzip upload body (baseLogger/Helpers/gzip_body.h).",
    "license": "Zlib",
    "frameworks": "arduino",
    "platforms": "espressif32",
    "build": {"flags": ["-DNO_GZIP", "-mfix-esp32-psram-cache-issue"]},
}
if json.loads((vendor / "library.json").read_text()) != expected_library:
    raise AssertionError("libraries/zlib/library.json changed: the zlib build flags are pinned")
print(f"PASS vendored zlib matches SHA256SUMS ({len(manifest)} files, {len(sources)} sources)")

config = (root / "baseLogger/config.h").read_text()
cap = int(re.search(r"#define CELL_MAX_POST_BYTES (\d+)", config).group(1))

def synthetic_body(points, seed):
    # Driving points in the v2 shape with slowly varying values; seeded, no real data.
    r = random.Random(seed)
    t0 = 1791111000 + r.randrange(100000)
    soc, odo, el = r.uniform(20, 90), r.uniform(1e4, 9e4), r.uniform(0, 400)
    lat, lon = r.uniform(-60, 60), r.uniform(-170, 170)
    out = []
    for i in range(points):
        ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t0 + 5 * i))
        spd = r.uniform(30, 120)
        soc -= r.uniform(0, .05); odo += spd / 720; el += r.uniform(-2, 2)
        lat += r.uniform(-1e-3, 1e-3); lon += r.uniform(-1e-3, 1e-3)
        p = {"provider": "ABRP_OTA"}
        def put(field, unit, value): p[field] = {"time": ts, unit: value}
        put("soc", "frac", round(soc, 3) / 100); put("soh", "frac", 0.97)
        put("power", "w", round(r.uniform(-40000, 60000), -1)); put("voltage", "v", round(370 + r.uniform(-3, 3), 1))
        put("current", "a", round(r.uniform(-100, 160), 1)); put("batteryTemperature", "c", 22)
        put("externalTemperature", "c", 11); put("odometer", "m", round(odo * 1000))
        put("speed", "ms", round(spd / 3.6, 2)); put("elevation", "m", round(el, 1)); put("heading", "degrees", round(r.uniform(0, 359)))
        p["drivingState"] = {"time": ts, "state": "DRIVE"}
        p["location"] = {"time": ts, "lat": round(lat, 6), "long": round(lon, 6)}
        out.append(p)
    return json.dumps(out, separators=(",", ":")).encode()

environment = dict(os.environ)
environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
with tempfile.TemporaryDirectory(prefix="obd-gzip-container-") as temporary:
    build = Path(temporary)
    (build / "esp_heap_caps.h").write_text("#pragma once\n")
    objects = []
    for source in sources:
        obj = build / (source[:-2] + ".o")
        subprocess.run([*c_compiler, "-x", "c", "-O1", *sanitize, "-DNO_GZIP", "-I", str(vendor),
                        "-c", str(vendor / source), "-o", str(obj)], check=True)
        objects.append(str(obj))
    executable = build / "gzip_container_checks"
    subprocess.run([
        *compiler, "-std=c++17", *sanitize, "-Wall", "-Wextra",
        "-I", str(build), "-I", str(vendor), "-I", str(root / "baseLogger/Helpers"),
        str(root / "tests/gzip_container_checks.cpp"), *objects, "-o", str(executable),
    ], check=True)

    def wires(inputs):
        result = subprocess.run([str(executable), "batch"], input="".join(raw.hex() + "\n" for raw in inputs),
                                env=environment, text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return [bytes.fromhex(line) for line in result.stdout.split()]

    seeds = range(40)
    inputs = [b"", b"hello", b'[{"provider":"ABRP_OTA","soc":{"time":"2026-10-07T04:49:03Z","frac":0.5}}]',
              bytes(range(256)) * 16, b"repeated telemetry " * 2000, synthetic_body(60, 99)]
    inputs += [synthetic_body(12, seed) for seed in range(8)]
    for raw, wire in zip(inputs, wires(inputs), strict=True):
        if gzip.decompress(wire) != raw:
            raise AssertionError("Production gzip member failed independent decompression")
        if wire[:10] != b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff":
            raise AssertionError("Gzip header mismatch")
        if struct.unpack("<II", wire[-8:]) != (zlib.crc32(raw), len(raw)):
            raise AssertionError("Gzip CRC32/ISIZE trailer mismatch")
    print(f"PASS gzip member: {len(inputs)} independent Python round trips, header and trailer exact")

    # Points per POST is what the ratio buys: a regression here is fewer points under the modem's cap.
    nine = wires([synthetic_body(9, seed) for seed in seeds])
    worst = max(len(wire) for wire in nine)
    if worst > cap:
        raise AssertionError(f"9 synthetic points no longer fit {cap} B for every seed (worst {worst} B)")
    print(f"PASS ratio: 9 synthetic points fit {cap} B for all {len(seeds)} seeds "
          f"(worst {worst} B, mean {sum(map(len, nine)) / len(nine):.0f} B)")

    for mode in ["alloc-deflate", "alloc-inflate", "alloc-verify", "tamper", "crash", "steady", "bench"]:
        subprocess.run([str(executable), mode], env=environment, check=True)
    print("Real vendored zlib; PSRAM allocation, RTC memory and the clock are host shims, not hardware verified")
