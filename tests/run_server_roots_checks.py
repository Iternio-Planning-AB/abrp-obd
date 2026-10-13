#!/usr/bin/env python3
"""Host checks for the cellular TLS trust bundle (baseLogger/Helpers/server_roots.h); see DONGLE_CONTEXT.md §6a.

Offline: the header is exactly what tools/server_roots.py generates from its own certificates, the
certificates are exactly the pinned roots below, and none expires within three years (one documented
exception). Live: the chain api.iternio.com presents today verifies against the bundle, and so does
its topmost presented certificate on its own - the SIM7070 needs the issuer of the last certificate the
server sends. The live part is skipped only when the server cannot be reached; --require-live fails then.

Needs the openssl CLI (OpenSSL or LibreSSL).
"""

import argparse
import datetime
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "baseLogger" / "tools"))
import server_roots  # noqa: E402

HOST = "api.iternio.com"
MIN_REMAINING = datetime.timedelta(days=3 * 365)
PEM = re.compile(rb"-----BEGIN CERTIFICATE-----.+?-----END CERTIFICATE-----\n", re.S)

# Changing the bundle means changing this list, on purpose: an added or dropped root is a trust decision.
PINNED = {
    "8ECDE6884F3D87B1125BA31AC3FCB13D7016DE7F57CC904FE1CB97C6AE98196E": "Amazon Root CA 1",
    "18CE6CFE7BF14E60B2E347B8DFE868CB31D02EBB3ADA271569F50343B46DB3A4": "Amazon Root CA 3",
    "E35D28419ED02025CFA69038CD623962458DA5C695FBDEA3C22B0BFB25897092": "Amazon Root CA 4",
    "568D6905A2C88708A4B3025190EDCFEDB1974A606A13C6E5290FCB2AE63EDAB5": "Starfield Services Root G2",
    "EBD41040E4BB3EC742C9E381D31EF2A41A48B6685C96E7CEF3C1DF6CD4331C99": "GlobalSign Root CA",
    "349DFA4058C5E263123B398AE795573C4E1313C83FE68F93556CD5E8031B3C7D": "GTS Root R4",
}
# GlobalSign Root CA (expires 2028-01-28) is a hedge only for as long as Cloudflare's GTS Root R4 is
# still served cross-signed by it; after that GTS Root R4 alone covers the chain. Let it lapse.
EXPIRY_EXEMPT = {"EBD41040E4BB3EC742C9E381D31EF2A41A48B6685C96E7CEF3C1DF6CD4331C99"}


def fail(message):
    sys.exit("FAIL: " + message)


def bundle_certificates():
    text = server_roots.HEADER.read_text()
    length = int(re.search(r"#define SERVER_ROOTS_LEN (\d+)", text).group(1))
    der = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", text.split("SERVER_ROOTS_P7B", 1)[1]))
    if len(der) != length:
        fail(f"SERVER_ROOTS_LEN {length} != {len(der)} array bytes")
    return text, der, PEM.findall(server_roots.openssl("pkcs7", "-inform", "DER", "-print_certs", data=der))


def verify(cafile_pems, cert, untrusted=()):
    with tempfile.TemporaryDirectory() as d:
        ca, leaf, chain = Path(d, "ca.pem"), Path(d, "cert.pem"), Path(d, "untrusted.pem")
        ca.write_bytes(b"".join(cafile_pems))
        leaf.write_bytes(cert)
        args = ["openssl", "verify", "-CApath", d, "-CAfile", str(ca), "-purpose", "sslserver"]
        if untrusted:
            chain.write_bytes(b"".join(untrusted))
            args += ["-untrusted", str(chain)]
        result = subprocess.run([*args, str(leaf)], capture_output=True, text=True)
        detail = (result.stdout + result.stderr).strip()
        # LibreSSL can print "error 20 ... unable to get local issuer certificate" and still exit 0 with "OK".
        return result.returncode == 0 and ": OK" in result.stdout and "error" not in detail, detail


def offline():
    text, der, pems = bundle_certificates()
    if server_roots.render(pems) != text:
        fail("server_roots.h is not the generator's output for its own certificates; regenerate it")
    print(f"PASS header regenerates byte-for-byte ({len(pems)} roots, {len(der)} bytes <= {server_roots.MAX_BYTES})")
    found = {server_roots.fingerprint(p): p for p in pems}
    if set(found) != set(PINNED):
        added = [server_roots.describe(found[f])[0] for f in set(found) - set(PINNED)]
        missing = [PINNED[f] for f in set(PINNED) - set(found)]
        fail(f"bundle differs from the pinned roots: added {added or 'none'}, missing {missing or 'none'}")
    print("PASS bundle is exactly the pinned roots")
    now = datetime.datetime.now(datetime.timezone.utc)
    soon = [PINNED[f] for f, p in found.items()
            if f not in EXPIRY_EXEMPT and server_roots.not_after_of(p) - now < MIN_REMAINING]
    if soon:
        fail("roots expire within three years - add their successors and ship firmware: " + ", ".join(soon))
    print("PASS no root expires within three years (GlobalSign Root CA exempt, see EXPIRY_EXEMPT)")
    return pems


def live(pems, require):
    try:
        result = subprocess.run(["openssl", "s_client", "-connect", f"{HOST}:443", "-servername", HOST, "-showcerts"],
                                input=b"", capture_output=True, timeout=20)
    except subprocess.TimeoutExpired:
        result = None
    chain = PEM.findall(result.stdout) if result else []
    if not chain:
        # Connected (CONNECTED in the output) but no chain is a failure; no connection at all is offline.
        if result and b"CONNECTED" in result.stdout:
            fail(f"connected to {HOST} but read no certificate chain")
        if require:
            fail(f"could not reach {HOST}")
        print(f"SKIP live chain: {HOST} unreachable")
        return
    ok, detail = verify(pems, chain[0], chain[1:])
    if not ok:
        fail(f"live {HOST} chain does not verify against the bundle: {detail}")
    print(f"PASS live {HOST} chain verifies against the bundle")
    ok, detail = verify(pems, chain[-1])
    if not ok:
        fail(f"the server's topmost certificate has no issuer in the bundle - the modem would refuse it: {detail}")
    print(f"PASS topmost presented certificate's issuer is bundled ({server_roots.field(chain[-1], '-issuer')})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--require-live", action="store_true", help="fail instead of skipping when unreachable")
    args = parser.parse_args()
    live(offline(), args.require_live)


if __name__ == "__main__":
    main()
