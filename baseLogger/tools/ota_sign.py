#!/usr/bin/env python3
"""
Sign a built firmware image and produce the release artifacts the app downloads.

Outputs (default into ./release/):
    firmware.bin    the raw compiled image, byte-for-byte what gets streamed to the dongle
    manifest.json   version, size, sha256 and the signature the app sends in the BLE START command

SIGNATURE SCHEME - must stay identical to Helpers/ota_update.h (otaVerifySignature):

    blob   = b"ABRPOTA1" | uint32le(version) | uint32le(size) | sha256(image)     (48 bytes)
    digest = SHA-256(blob)
    sig    = ECDSA-P256(digest), raw r||s, 64 bytes

Version and size are signed alongside the image hash on purpose: signing only the image hash would let someone
replay a genuinely-signed OLD image with an inflated version number to get past the dongle's anti-downgrade
check. The dongle recomputes the hash by reading the staged image back out of flash, so a tampered image can
never satisfy a valid signature.

No Python packages required - uses the openssl CLI.

Usage:
    python3 tools/ota_sign.py --version 2
    python3 tools/ota_sign.py --version 2 --bin .pio/build/esp32dev/firmware.bin --out release/
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASELOGGER = HERE.parent
DEFAULT_BIN = BASELOGGER / ".pio" / "build" / "esp32dev" / "firmware.bin"
DEFAULT_KEY = Path.home() / ".abrp-ota" / "ota_private_key.pem"
MAGIC = b"ABRPOTA1"

# Keep in step with config.h
MIN_IMAGE = 256 * 1024
MAX_IMAGE = 6 * 1024 * 1024


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def der_sig_to_raw(der: bytes) -> bytes:
    """Convert a DER-encoded ECDSA signature to the raw r||s form (64 bytes) the dongle expects."""
    if der[0] != 0x30:
        raise ValueError("not a DER SEQUENCE")
    i = 2
    if der[1] & 0x80:                      # long-form length
        i = 2 + (der[1] & 0x7F)

    def read_int(pos):
        if der[pos] != 0x02:
            raise ValueError("expected DER INTEGER")
        ln = der[pos + 1]
        val = der[pos + 2: pos + 2 + ln]
        return val.lstrip(b"\x00").rjust(32, b"\x00"), pos + 2 + ln

    r, i = read_int(i)
    s, _ = read_int(i)
    if len(r) != 32 or len(s) != 32:
        raise ValueError("r/s longer than 32 bytes - wrong curve?")
    return r + s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", type=int, required=True,
                    help="firmware version; MUST match OTA_FW_VERSION in the built config.h and must be >= the "
                         "version already on the dongle (equal is allowed, older is refused)")
    ap.add_argument("--bin", type=Path, default=DEFAULT_BIN)
    ap.add_argument("--key", type=Path, default=DEFAULT_KEY)
    ap.add_argument("--out", type=Path, default=BASELOGGER / "release")
    args = ap.parse_args()

    if not args.bin.exists():
        sys.exit(f"ERROR: {args.bin} not found - build first (pio run -e esp32dev)")
    if not args.key.exists():
        sys.exit(f"ERROR: signing key {args.key} not found - run tools/ota_keygen.py first")

    image = args.bin.read_bytes()
    size = len(image)
    if not (MIN_IMAGE <= size <= MAX_IMAGE):
        sys.exit(f"ERROR: image is {size} B, outside the dongle's accepted range "
                 f"({MIN_IMAGE}..{MAX_IMAGE}) - it would be rejected at START")

    img_sha = hashlib.sha256(image).digest()
    blob = MAGIC + struct.pack("<II", args.version, size) + img_sha

    with tempfile.TemporaryDirectory() as td:
        blob_path = Path(td) / "blob.bin"
        der_path = Path(td) / "sig.der"
        pub_path = Path(td) / "pub.pem"
        blob_path.write_bytes(blob)

        # 'openssl dgst -sha256 -sign' hashes the file with SHA-256 then ECDSA-signs that digest - exactly what
        # the dongle verifies (it hashes the same blob and calls mbedtls_ecdsa_verify on the digest).
        run(["openssl", "dgst", "-sha256", "-sign", str(args.key), "-out", str(der_path), str(blob_path)])
        der = der_path.read_bytes()

        # Self-verify before publishing, so a broken artifact is caught here and not after a 3-minute transfer.
        run(["openssl", "ec", "-in", str(args.key), "-pubout", "-out", str(pub_path)])
        run(["openssl", "dgst", "-sha256", "-verify", str(pub_path), "-signature", str(der_path),
             str(blob_path)])

    sig = der_sig_to_raw(der)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "firmware.bin").write_bytes(image)
    manifest = {
        "version": args.version,
        "size": size,
        "sha256": img_sha.hex(),
        "signature": sig.hex(),          # 128 hex chars = raw r||s
        "sig_alg": "ecdsa-p256-sha256",
        "firmware": "firmware.bin",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"image      : {args.bin}")
    print(f"size       : {size} B")
    print(f"version    : {args.version}")
    print(f"sha256     : {img_sha.hex()}")
    print(f"signature  : {sig.hex()}")
    print("self-verify: OK (openssl dgst -verify)")
    print(f"\nRelease artifacts written to {args.out}/")
    print("  -> upload BOTH firmware.bin and manifest.json to the GitHub release")
    print("\nREMINDER: --version must equal OTA_FW_VERSION in the config.h this image was built from,")
    print("          or the dongle's anti-downgrade check will disagree with the signed manifest.")


if __name__ == "__main__":
    main()
