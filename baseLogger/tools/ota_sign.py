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

The signature is checked against the public key compiled into the firmware (Helpers/ota_pubkey.h) before anything
is written, so an image signed by a key the dongles don't trust can never be published.

Two signers. Releases use --kms-key-id: an AWS KMS ECC_NIST_P256 key signs sha256(blob) (MessageType DIGEST), so the
private key never exists outside KMS. --key signs with a local PEM (tests, or a bench key you generated yourself).

No Python packages required - uses the openssl CLI, and the aws CLI for KMS.

Usage:
    python3 tools/ota_sign.py --version-name 1.2.0 --kms-key-id alias/abrp-ota-signing --out dist/
    python3 tools/ota_sign.py --version-name 1.2.0 --key ~/.abrp-ota/ota_private_key.pem
"""

import argparse
import base64
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import fw_version
from ota_keygen import P256_SPKI_PREFIX

HERE = Path(__file__).resolve().parent
BASELOGGER = HERE.parent
DEFAULT_BIN = BASELOGGER / ".pio" / "build" / "esp32dev" / "firmware.bin"
DEFAULT_KEY = Path.home() / ".abrp-ota" / "ota_private_key.pem"
PUBKEY_HEADER = BASELOGGER / "Helpers" / "ota_pubkey.h"
MAGIC = b"ABRPOTA1"

# Keep in step with config.h
MIN_IMAGE = 256 * 1024
MAX_IMAGE = 6 * 1024 * 1024


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def der_sig_to_raw(der: bytes) -> bytes:
    """Convert a DER-encoded ECDSA signature to the raw r||s form (64 bytes) the dongle expects."""
    # A P-256 signature is at most 72 bytes, so the SEQUENCE length is always short-form.
    if len(der) < 8 or der[0] != 0x30 or der[1] != len(der) - 2:
        raise ValueError("not a complete DER SEQUENCE")

    def read_int(pos):
        if pos + 2 > len(der) or der[pos] != 0x02 or pos + 2 + der[pos + 1] > len(der):
            raise ValueError("expected a complete DER INTEGER")
        end = pos + 2 + der[pos + 1]
        return der[pos + 2:end].lstrip(b"\x00").rjust(32, b"\x00"), end

    r, i = read_int(2)
    s, end = read_int(i)
    if end != len(der) or len(r) != 32 or len(s) != 32:
        raise ValueError("not a P-256 signature (r/s longer than 32 bytes, or trailing data)")
    return r + s


def trusted_point(header: Path) -> bytes:
    """The 65-byte point the firmware will verify with, read from the generated ota_pubkey.h."""
    body = header.read_text().split("{", 1)[1].split("}", 1)[0]
    point = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body))
    if len(point) != 65 or point[0] != 0x04:
        sys.exit(f"ERROR: {header} does not hold a 65-byte uncompressed P-256 point")
    return point


def check_image_identity(image: bytes, version_name: str, point: bytes):
    """
    Refuse an image that is not the build being released. A release image embeds FW_BUILD_TAG "v<name>"
    (tools/pio_version.py), or "v<name>+<n>.g<sha>[.dirty]" for a bench build of the same version, and the public
    key it will verify updates with. Without this a stale or mis-built binary is only caught on the dongle, after the
    transfer, as a version mismatch on trial boot.
    """
    if not re.search(rb"v" + re.escape(version_name.encode()) + rb"[\x00+]", image):
        sys.exit(f"ERROR: image was not built as v{version_name} (no matching FW_BUILD_TAG) - built as another "
                 "version, or without tools/pio_version.py. Nothing was written.")
    if point not in image:
        sys.exit("ERROR: image does not embed the public key in ota_pubkey.h - it was built against a different "
                 "key and would refuse every update signed for it. Nothing was written.")


def sign_pem(key: Path, blob_path: Path, td: Path) -> bytes:
    # 'openssl dgst -sha256 -sign' hashes the file with SHA-256 then ECDSA-signs that digest - exactly what
    # the dongle verifies (it hashes the same blob and calls mbedtls_ecdsa_verify on the digest).
    der_path = td / "sig.der"
    run(["openssl", "dgst", "-sha256", "-sign", str(key), "-out", str(der_path), str(blob_path)])
    return der_path.read_bytes()


def sign_kms(key_id: str, blob: bytes, td: Path) -> bytes:
    # KMS is handed the digest, not the blob: MessageType DIGEST signs these 32 bytes as-is, the same digest the
    # dongle computes. The CLI returns the DER signature base64-encoded.
    digest_path = td / "digest.bin"
    digest_path.write_bytes(hashlib.sha256(blob).digest())
    out = run(["aws", "kms", "sign", "--key-id", key_id, "--message", f"fileb://{digest_path}",
               "--message-type", "DIGEST", "--signing-algorithm", "ECDSA_SHA_256",
               "--output", "text", "--query", "Signature"], text=True).stdout
    return base64.b64decode(out.strip())


def verifies(point: bytes, der_sig: bytes, blob_path: Path, td: Path) -> bool:
    spki = base64.encodebytes(P256_SPKI_PREFIX + point).decode()
    pub = td / "trusted.pem"
    pub.write_text(f"-----BEGIN PUBLIC KEY-----\n{spki}-----END PUBLIC KEY-----\n")
    (td / "check.der").write_bytes(der_sig)
    r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature", str(td / "check.der"),
                        str(blob_path)], capture_output=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version-name", required=True,
                    help="release version X.Y.Z[-rc.N]; MUST be the version the image was built with "
                         "(RELEASE_VERSION). Its uint32 code is what the dongle compares and what gets signed")
    ap.add_argument("--bin", type=Path, default=DEFAULT_BIN)
    signer = ap.add_mutually_exclusive_group()
    signer.add_argument("--kms-key-id", help="AWS KMS key id/alias/ARN to sign with (release signing)")
    signer.add_argument("--key", type=Path, default=DEFAULT_KEY, help="local PEM private key")
    ap.add_argument("--pubkey-header", type=Path, default=PUBKEY_HEADER)
    ap.add_argument("--out", type=Path, default=BASELOGGER / "release")
    args = ap.parse_args()

    try:
        version = fw_version.version_code(args.version_name)
    except ValueError as e:
        sys.exit(f"ERROR: {e}")
    if not args.bin.exists():
        sys.exit(f"ERROR: {args.bin} not found - build first (pio run -e esp32dev)")
    if not args.kms_key_id and not args.key.exists():
        sys.exit(f"ERROR: signing key {args.key} not found")
    point = trusted_point(args.pubkey_header)

    image = args.bin.read_bytes()
    size = len(image)
    if not (MIN_IMAGE <= size <= MAX_IMAGE):
        sys.exit(f"ERROR: image is {size} B, outside the dongle's accepted range "
                 f"({MIN_IMAGE}..{MAX_IMAGE}) - it would be rejected at START")

    check_image_identity(image, args.version_name, point)

    img_sha = hashlib.sha256(image).digest()
    blob = MAGIC + struct.pack("<II", version, size) + img_sha

    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        blob_path = td / "blob.bin"
        blob_path.write_bytes(blob)
        try:
            der = sign_kms(args.kms_key_id, blob, td) if args.kms_key_id else sign_pem(args.key, blob_path, td)
        except subprocess.CalledProcessError as e:
            err = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
            sys.exit(f"ERROR: signing failed ({e.cmd[0]} exit {e.returncode}): {err.strip()}")

        # Verify against the key the firmware actually trusts, not the signer's own public half: a wrong KMS key
        # or a stale header is caught here and not after a 3-minute transfer to a dongle that refuses it.
        if not verifies(point, der, blob_path, td):
            sys.exit(f"ERROR: signature does not verify against {args.pubkey_header.name} - the signing key is not "
                     "the one compiled into this firmware. Nothing was written.")

    sig = der_sig_to_raw(der)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "firmware.bin").write_bytes(image)
    manifest = {
        "version": version,
        "version_name": args.version_name,
        "size": size,
        "sha256": img_sha.hex(),
        "signature": sig.hex(),          # 128 hex chars = raw r||s
        "sig_alg": "ecdsa-p256-sha256",
        "firmware": "firmware.bin",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"image      : {args.bin}")
    print(f"size       : {size} B")
    print(f"version    : {args.version_name} (code {version})")
    print(f"sha256     : {img_sha.hex()}")
    print(f"signature  : {sig.hex()}")
    print(f"self-verify: OK against {args.pubkey_header.name}")
    print(f"\nRelease artifacts written to {args.out}/")


if __name__ == "__main__":
    main()
