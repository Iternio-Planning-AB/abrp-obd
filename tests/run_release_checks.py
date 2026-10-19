#!/usr/bin/env python3
"""Host checks for the release tooling in baseLogger/tools (versioning, signing, workflow)."""

import base64
import hashlib
import http.server
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "baseLogger" / "tools"
sys.path.insert(0, str(TOOLS))

import fw_version  # noqa: E402
import ota_keygen  # noqa: E402
import ota_sign  # noqa: E402


class VersionCode(unittest.TestCase):
    def test_semver_packs_into_the_uint32_the_dongle_compares(self):
        self.assertEqual(fw_version.version_code("1.2.3"), 1_002_003)

    def test_release_candidate_shares_its_final_release_code(self):
        self.assertEqual(fw_version.version_code("1.2.3-rc.4"), 1_002_003)

    def test_rejects_versions_that_cannot_round_trip_through_the_uint32(self):
        for bad in ["1.1000.0", "1.0.1000", "4295.0.0", "1.2", "v1.2.3", "01.2.3", "1.2.3-beta", "1.2.3\n", ""]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                fw_version.version_code(bad)

    def test_largest_encodable_version_fits_in_uint32(self):
        self.assertEqual(fw_version.version_code("4294.967.295"), 0xFFFFFFFF)


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", *args],
                          check=True, capture_output=True, text=True).stdout.strip()


class Repo:
    """A throwaway git repo with conventional-commit history."""

    def __init__(self):
        self._td = tempfile.TemporaryDirectory()
        self.path = Path(self._td.name)
        git(self.path, "init", "-q", "-b", "main")

    def commit(self, subject, *paragraphs):
        git(self.path, "commit", "-q", "--allow-empty", "-m", subject, *(a for p in paragraphs for a in ("-m", p)))
        return git(self.path, "rev-parse", "--short=7", "HEAD")

    def tag(self, name):
        git(self.path, "tag", name)

    def dirty(self):
        (self.path / "scratch.txt").write_text("x")
        git(self.path, "add", "scratch.txt")

    def close(self):
        self._td.cleanup()


class BuildVersion(unittest.TestCase):
    def setUp(self):
        self.repo = Repo()
        self.addCleanup(self.repo.close)

    def test_release_build_uses_the_version_ci_passes_in(self):
        self.repo.commit("feat: a")
        self.assertEqual(fw_version.build_version(self.repo.path, {"RELEASE_VERSION": "1.4.0-rc.2"}),
                         ("1.4.0-rc.2", 1_004_000, "v1.4.0-rc.2"))

    def test_release_build_refuses_a_malformed_version(self):
        self.repo.commit("feat: a")
        with self.assertRaises(ValueError):
            fw_version.build_version(self.repo.path, {"RELEASE_VERSION": "1.4"})

    def test_build_at_a_release_tag_reports_that_release(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.2.0")
        self.assertEqual(fw_version.build_version(self.repo.path, {}), ("1.2.0", 1_002_000, "v1.2.0"))

    def test_dev_build_after_a_tag_keeps_its_code_and_says_where_it_came_from(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.2.0")
        self.repo.commit("fix: b")
        sha = self.repo.commit("fix: c")
        self.repo.dirty()
        self.assertEqual(fw_version.build_version(self.repo.path, {}),
                         ("1.2.0", 1_002_000, f"v1.2.0+2.g{sha}.dirty"))

    def test_dev_build_ignores_release_candidate_tags(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.2.0")
        self.repo.commit("feat: b")
        self.repo.tag("v1.3.0-rc.1")
        sha = self.repo.commit("fix: c")
        self.assertEqual(fw_version.build_version(self.repo.path, {})[2], f"v1.2.0+2.g{sha}")

    def test_tags_that_are_not_release_versions_are_skipped(self):
        for stray in ["v7", "v1.3.0.1", "v1.3.0-beta"]:
            with self.subTest(stray=stray):
                repo = Repo()
                self.addCleanup(repo.close)
                repo.commit("feat: a")
                repo.tag("v1.2.0")
                repo.commit("feat: b")
                repo.tag(stray)
                self.assertEqual(fw_version.build_version(repo.path, {})[:2], ("1.2.0", 1_002_000))

    def test_only_stray_tags_builds_as_version_zero(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.2.0.1")
        self.assertEqual(fw_version.build_version(self.repo.path, {})[:2], ("0.0.0", 0))

    def test_untagged_history_builds_as_version_zero(self):
        sha = self.repo.commit("feat: a")
        self.assertEqual(fw_version.build_version(self.repo.path, {}), ("0.0.0", 0, f"v0.0.0+1.g{sha}"))

    def test_outside_git_builds_as_version_zero(self):
        with tempfile.TemporaryDirectory() as plain:
            os.environ["GIT_CEILING_DIRECTORIES"] = plain
            self.addCleanup(os.environ.pop, "GIT_CEILING_DIRECTORIES")
            self.assertEqual(fw_version.build_version(Path(plain), {}), ("0.0.0", 0, "v0.0.0+unknown"))


class NextRelease(unittest.TestCase):
    """`bumped` is what git-cliff --bumped-version suggests from the conventional commits since the last tag."""

    def setUp(self):
        self.repo = Repo()
        self.addCleanup(self.repo.close)
        self.repo.commit("feat: first")

    def released(self, *tags):
        for t in tags:
            self.repo.tag(t)
            self.repo.commit("fix: more")

    def test_first_release_takes_the_suggested_version(self):
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.0.0"), "1.0.0")

    def test_takes_the_bump_suggested_by_the_commits(self):
        self.released("v1.0.0")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.1.0"), "1.1.0")

    def test_refuses_when_no_commit_warrants_a_release(self):
        self.released("v1.0.0")
        with self.assertRaisesRegex(ValueError, "nothing to release"):
            fw_version.next_release(self.repo.path, "v1.0.0")

    def test_override_wins_over_the_suggestion(self):
        self.released("v1.0.0")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.0.1", override="1.2.0"), "1.2.0")

    def test_override_must_move_forward(self):
        self.released("v1.0.0", "v1.1.0")
        for stale in ["1.1.0", "1.0.5"]:
            with self.subTest(stale=stale), self.assertRaisesRegex(ValueError, "not newer"):
                fw_version.next_release(self.repo.path, "v1.1.1", override=stale)

    def test_override_must_be_a_plain_release_version(self):
        for bad in ["1.2", "v1.2.0", "1.2.0-rc.1", "1.2.0; rm -rf /"]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                fw_version.next_release(self.repo.path, "v1.0.0", override=bad)

    def test_refuses_skipping_a_major_version(self):
        self.released("v1.0.0")
        with self.assertRaisesRegex(ValueError, "major"):
            fw_version.next_release(self.repo.path, "v1.0.1", override="3.0.0")

    def test_legacy_integer_tags_do_not_count_as_releases(self):
        self.released("v7")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.0.0"), "1.0.0")

    def test_prerelease_numbers_candidates_for_the_next_version(self):
        self.released("v1.0.0")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.1.0", prerelease=True), "1.1.0-rc.1")
        self.released("v1.1.0-rc.1", "v1.1.0-rc.2")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.1.0", prerelease=True), "1.1.0-rc.3")

    def test_final_release_follows_its_candidates(self):
        self.released("v1.0.0", "v1.1.0-rc.1")
        self.assertEqual(fw_version.next_release(self.repo.path, "v1.1.0"), "1.1.0")


class NextReleaseCli(unittest.TestCase):
    """What the workflow runs: prints the name on success, a reason and non-zero on refusal."""

    def setUp(self):
        self.repo = Repo()
        self.addCleanup(self.repo.close)
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("fix: b")

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(TOOLS / "fw_version.py"), "next", "--repo", str(self.repo.path),
                               *args], capture_output=True, text=True)

    def test_prints_the_release_name(self):
        r = self.run_cli("--bumped", "v1.0.1", "--prerelease")
        self.assertEqual((r.returncode, r.stdout), (0, "1.0.1-rc.1\n"))

    def test_empty_override_means_no_override(self):
        r = self.run_cli("--bumped", "v1.0.1", "--override", "")
        self.assertEqual((r.returncode, r.stdout), (0, "1.0.1\n"))

    def test_refusal_is_a_readable_error(self):
        r = self.run_cli("--bumped", "v1.0.0")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(r.stdout, "")
        self.assertIn("nothing to release", r.stderr)


def openssl(*args, **kw):
    return subprocess.run(["openssl", *args], check=True, capture_output=True, **kw).stdout


# Stands in for the AWS CLI's two KMS calls. It refuses anything but a 32-byte digest, so a signer that sent the
# 48-byte blob instead of its hash fails here exactly as the real Sign call would produce a wrong signature.
FAKE_AWS = r"""#!/usr/bin/env python3
import base64, os, subprocess, sys
a = sys.argv[1:]
opt = lambda name: a[a.index(name) + 1]
key = os.environ["FAKE_KMS_PEM"]
assert opt("--key-id") == os.environ["FAKE_KMS_KEY_ID"], "wrong key id"
if a[:2] == ["kms", "sign"]:
    assert opt("--message-type") == "DIGEST" and opt("--signing-algorithm") == "ECDSA_SHA_256"
    digest = open(opt("--message").removeprefix("fileb://"), "rb").read()
    assert len(digest) == 32, f"KMS DIGEST must be 32 bytes, got {len(digest)}"
    der = subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", key], input=digest, check=True,
                         capture_output=True).stdout
    print(base64.b64encode(der).decode())
elif a[:2] == ["kms", "get-public-key"]:
    der = subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-outform", "DER"], check=True,
                         capture_output=True).stdout
    print(base64.b64encode(der).decode())
else:
    sys.exit("unexpected aws call: " + " ".join(a))
"""


def der_signature(r: bytes, s: bytes) -> bytes:
    body = b"\x02" + bytes([len(r)]) + r + b"\x02" + bytes([len(s)]) + s
    return b"\x30" + bytes([len(body)]) + body


class RawSignature(unittest.TestCase):
    """The dongle wants r||s as two 32-byte big-endian halves; DER integers are minimal-length and signed."""

    def test_short_integers_are_left_padded(self):
        r, s = b"\x01" * 31, b"\x02" * 30
        self.assertEqual(ota_sign.der_sig_to_raw(der_signature(r, s)), b"\x00" + r + b"\x00\x00" + s)

    def test_sign_padding_byte_is_dropped(self):
        r, s = b"\x80" + b"\x11" * 31, b"\xff" * 32
        self.assertEqual(ota_sign.der_sig_to_raw(der_signature(b"\x00" + r, b"\x00" + s)), r + s)

    def test_rejects_a_truncated_signature(self):
        sig = der_signature(b"\x01" * 32, b"\x02" * 32)
        for cut in (1, 5, 40, len(sig) - 1):
            with self.subTest(cut=cut), self.assertRaises(ValueError):
                ota_sign.der_sig_to_raw(sig[:-cut])

    def test_rejects_what_is_not_a_p256_signature(self):
        for bad in [b"\x31" + der_signature(b"\x01", b"\x01")[1:], der_signature(b"\x01" * 33, b"\x01"),
                    b"\x30\x06\x04\x01\x01\x02\x01\x01", b"\x30\x06\x02\x04\x01\x01\x01\x01"]:
            with self.subTest(bad=bad.hex()), self.assertRaises(ValueError):
                ota_sign.der_sig_to_raw(bad)


class Signing(unittest.TestCase):
    """tools/ota_sign.py end to end: the artifacts it writes must satisfy the dongle's otaVerifySignature."""

    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.dir = Path(td.name)
        self.key = self.make_key("trusted")
        self.point = ota_keygen.public_point_from_key(self.key)
        self.header = self.dir / "ota_pubkey.h"
        ota_keygen.write_pubkey_header(self.point, "test key", self.header)
        self.image = self.dir / "firmware.bin"
        self.build_image(b"v1.2.3\x00")
        self.out = self.dir / "dist"
        bindir = self.dir / "bin"
        bindir.mkdir()
        (bindir / "aws").write_text(FAKE_AWS)
        (bindir / "aws").chmod(0o755)
        self.env = dict(os.environ, PATH=f"{bindir}{os.pathsep}{os.environ['PATH']}",
                        FAKE_KMS_PEM=str(self.key), FAKE_KMS_KEY_ID="alias/abrp-ota-test")

    def build_image(self, build_tag, point=None):
        """Stand-in firmware: like the real image it carries its build tag and the trusted public key."""
        filler = bytes(range(256)) * 600
        body = filler + build_tag + bytes(100) + (point or self.point) + filler
        self.image.write_bytes(body)

    def make_key(self, name):
        path = self.dir / f"{name}.pem"
        openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(path))
        return path

    def sign(self, *args):
        return subprocess.run([sys.executable, str(TOOLS / "ota_sign.py"), "--bin", str(self.image),
                               "--pubkey-header", str(self.header), "--out", str(self.out), *args],
                              capture_output=True, text=True, env=self.env)

    def assert_dongle_accepts(self, version_code):
        manifest = json.loads((self.out / "manifest.json").read_text())
        image = (self.out / "firmware.bin").read_bytes()
        self.assertEqual(image, self.image.read_bytes())
        self.assertEqual(manifest["version"], version_code)
        self.assertEqual(manifest["size"], len(image))
        self.assertEqual(manifest["sha256"], hashlib.sha256(image).hexdigest())
        blob = b"ABRPOTA1" + struct.pack("<II", version_code, len(image)) + hashlib.sha256(image).digest()
        raw = bytes.fromhex(manifest["signature"])
        self.assertEqual(len(raw), 64)
        minimal = lambda half: (b"\x00" + half.lstrip(b"\x00")) if half.lstrip(b"\x00")[0] & 0x80 else half.lstrip(b"\x00")
        (self.dir / "sig.der").write_bytes(der_signature(minimal(raw[:32]), minimal(raw[32:])))
        (self.dir / "blob.bin").write_bytes(blob)
        openssl("ec", "-in", str(self.key), "-pubout", "-out", str(self.dir / "pub.pem"))
        openssl("dgst", "-sha256", "-verify", str(self.dir / "pub.pem"), "-signature", str(self.dir / "sig.der"),
                str(self.dir / "blob.bin"))
        return manifest

    def test_pem_signed_release_verifies_like_the_dongle_does(self):
        r = self.sign("--version-name", "1.2.3", "--key", str(self.key))
        self.assertEqual(r.returncode, 0, r.stderr)
        manifest = self.assert_dongle_accepts(1_002_003)
        self.assertEqual(manifest["version_name"], "1.2.3")
        self.assertEqual(manifest["sig_alg"], "ecdsa-p256-sha256")
        self.assertEqual(manifest["firmware"], "firmware.bin")

    def test_kms_signed_release_verifies_like_the_dongle_does(self):
        self.build_image(b"v1.3.0-rc.2\x00")
        r = self.sign("--version-name", "1.3.0-rc.2", "--kms-key-id", "alias/abrp-ota-test")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.assert_dongle_accepts(1_003_000)["version_name"], "1.3.0-rc.2")

    def test_key_the_firmware_does_not_trust_publishes_nothing(self):
        other = self.make_key("untrusted")
        for args in (["--key", str(other)], ["--kms-key-id", "alias/abrp-ota-test"]):
            self.env["FAKE_KMS_PEM"] = str(other)
            with self.subTest(args=args[0]):
                r = self.sign("--version-name", "1.2.3", *args)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("ota_pubkey.h", r.stderr)
                self.assertFalse(self.out.exists())

    def test_refuses_an_image_built_as_another_version(self):
        for tag in [b"v1.2.4\x00", b"v1.2.30\x00", b"v1.2.3-rc.1\x00", b"unversioned\x00"]:
            with self.subTest(tag=tag):
                self.build_image(tag)
                r = self.sign("--version-name", "1.2.3", "--key", str(self.key))
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("built as", r.stderr)
                self.assertFalse(self.out.exists())

    def test_dev_build_of_the_same_version_can_be_bench_signed(self):
        self.build_image(b"v1.2.3+4.gabc1234.dirty\x00")
        r = self.sign("--version-name", "1.2.3", "--key", str(self.key))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_refuses_an_image_that_does_not_carry_the_trusted_key(self):
        self.build_image(b"v1.2.3\x00", point=ota_keygen.public_point_from_key(self.make_key("other")))
        r = self.sign("--version-name", "1.2.3", "--key", str(self.key))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not embed", r.stderr)
        self.assertFalse(self.out.exists())

    def test_refuses_an_image_the_dongle_would_reject_by_size(self):
        self.image.write_bytes(b"v1.2.3\x00" + self.point + bytes(1000))
        r = self.sign("--version-name", "1.2.3", "--key", str(self.key))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("accepted range", r.stderr)
        self.assertFalse(self.out.exists())

    def test_refuses_an_unencodable_version(self):
        r = self.sign("--version-name", "1.2", "--key", str(self.key))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not a release version", r.stderr)
        self.assertFalse(self.out.exists())

    def test_keygen_writes_the_kms_public_key_the_signer_trusts(self):
        self.header.unlink()
        r = subprocess.run([sys.executable, str(TOOLS / "ota_keygen.py"), "--from-kms", "alias/abrp-ota-test",
                            "--header", str(self.header)], capture_output=True, text=True, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("AWS KMS alias/abrp-ota-test", self.header.read_text())
        self.assertEqual(self.sign("--version-name", "1.2.3", "--key", str(self.key)).returncode, 0)


WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"


def _blocks(lines, header):
    """Split `lines` at every line matching `header` (a compiled regex): [(match, [lines...]), ...]."""
    out = []
    for line in lines:
        m = header.match(line)
        if m:
            out.append((m, []))
        elif out:
            out[-1][1].append(line)
    return out


class Workflow:
    """Just enough structure of release.yml to check it: jobs, their steps, and each step's run script."""

    def __init__(self, text):
        self.text = text
        lines = text.splitlines()
        self.triggers = []
        if "on:" in lines:
            for line in lines[lines.index("on:") + 1:]:
                if line and not line.startswith(" "):
                    break
                m = re.match(r"^  ([a-z_]+):", line)
                if m:
                    self.triggers.append(m.group(1))
        body = lines[lines.index("jobs:") + 1:] if "jobs:" in lines else []
        self.jobs = {m.group(1): "\n".join(job) for m, job in _blocks(body, re.compile(r"^  ([^\s:#]+):"))}

    def steps(self, job):
        return ["\n".join([m.group(0), *step]) for m, step in
                _blocks(self.jobs[job].splitlines(), re.compile(r"^      - (.*)$"))]

    @staticmethod
    def run_script(step):
        m = re.search(r"(?m)^(?:      - |        )run: *(.*)$", step)
        if not m:
            return None
        script = [m.group(1)]
        for line in step[m.end():].splitlines()[1:]:
            if line.strip() and not line.startswith("          "):
                break
            script.append(line)
        return "\n".join(script)

    def step(self, job, name):
        return next(s for s in self.steps(job) if s.splitlines()[0] == f"      - name: {name}")


class WorkflowGuards(unittest.TestCase):
    """The publish job can sign firmware every dongle trusts; the properties that keep it safe are pinned here."""

    def setUp(self):
        self.assertTrue(WORKFLOW.exists(), f"{WORKFLOW} missing")
        self.wf = Workflow(WORKFLOW.read_text())

    def all_steps(self):
        return [s for job in self.wf.jobs for s in self.wf.steps(job)]

    def test_only_a_manual_dispatch_can_start_it(self):
        self.assertEqual(self.wf.triggers, ["workflow_dispatch"])

    def test_every_job_runs_only_from_main(self):
        self.assertEqual(set(self.wf.jobs), {"build", "publish"})
        for name, job in self.wf.jobs.items():
            with self.subTest(job=name):
                self.assertRegex(job, r"(?m)^    if: github\.ref == 'refs/heads/main'$")

    def test_no_expression_is_spliced_into_a_shell_script(self):
        scripts = [self.wf.run_script(s) for s in self.all_steps()]
        self.assertTrue(any(scripts))
        for script in filter(None, scripts):
            self.assertNotIn("${{", script, "pass values through env:, never ${{ }} inside run:")

    def test_token_scopes_are_granted_per_job(self):
        self.assertRegex(self.wf.text, r"(?m)^permissions: \{\}$")

    def test_build_job_cannot_reach_the_signing_key(self):
        build = self.wf.jobs["build"]
        self.assertNotIn("id-token", build)
        self.assertNotIn("environment:", build)
        self.assertNotIn("secrets.", build)
        self.assertIn("contents: read", build)

    def test_build_tools_install_only_by_hash(self):
        # A hash-less install would let a republished PyPI version run with the release's checkout.
        install = self.wf.run_script(self.wf.step("build", "Install tools"))
        self.assertIn("--require-hashes", install)
        self.assertIn("-r .github/requirements/release.txt", install)
        lock = (ROOT / ".github/requirements/release.txt").read_text()
        reqs = re.split(r"(?m)^(?=[a-z])", lock)[1:]
        self.assertTrue(reqs)
        for req in reqs:
            with self.subTest(req=req.split()[0]):
                self.assertRegex(req, r"^[a-z0-9_.-]+==[^\s]+")
                self.assertIn("--hash=sha256:", req)
        pins = re.findall(r"(?m)^([a-z0-9_.-]+==\S+)$", (ROOT / ".github/requirements/release.in").read_text())
        self.assertTrue(pins)
        for pin in pins:
            self.assertIsNotNone(re.search(rf"(?m)^{re.escape(pin)} ", lock),
                                 f"{pin} not in release.txt: regenerate it from release.in")

    def test_signing_job_runs_no_downloaded_build_tooling(self):
        publish = self.wf.jobs["publish"]
        self.assertIn("id-token: write", publish)
        actions = {u.split("@")[0] for u in re.findall(r"uses: *([^\s#]+)", publish)}
        self.assertEqual(actions, {"actions/checkout", "actions/download-artifact",
                                   "aws-actions/configure-aws-credentials"})
        for step in self.wf.steps("publish"):
            script = self.wf.run_script(step) or ""
            with self.subTest(step=step.splitlines()[0]):
                for tool in ("pip", "pio ", "uv", "git cliff", "npm", "npx", "curl ", "wget "):
                    self.assertNotIn(tool, script)

    def test_signing_identity_comes_from_repo_variables(self):
        # The AWS role trusts only release.yml on main; these are repo variables, not secrets.
        publish = self.wf.jobs["publish"]
        self.assertNotIn("secrets.", publish)
        aws = self.wf.step("publish", "AWS credentials")
        self.assertIn("role-to-assume: ${{ vars.OTA_SIGNING_ROLE_ARN }}", aws)
        self.assertIn("aws-region: eu-central-1", aws)
        sign = self.wf.step("publish", "Sign")
        self.assertIn("KMS_KEY_ID: ${{ vars.OTA_KMS_KEY_ID }}", sign)
        self.assertIn('--kms-key-id "$KMS_KEY_ID"', self.wf.run_script(sign))

    def test_dry_run_shows_the_oidc_claims_but_never_the_token(self):
        steps = self.wf.steps("publish")
        claims = self.wf.step("publish", "Show OIDC claims")
        aws = self.wf.step("publish", "AWS credentials")
        self.assertLess(steps.index(claims), steps.index(aws), "shown before an assume-role failure can stop the job")
        self.assertRegex(claims, r"(?m)^        if: \$\{\{ inputs\.dry_run \}\}$")

        # Run it against a fake token endpoint: the output must be the listed claims and nothing else.
        def b64(data):
            return base64.urlsafe_b64encode(json.dumps(data).encode()).rstrip(b"=").decode()
        claims_in = {"aud": "sts.amazonaws.com", "sub": "repo:o@1/r@2:ref:refs/heads/main", "ref": "refs/heads/main",
                     "job_workflow_ref": "o/r/.github/workflows/release.yml@refs/heads/main",
                     "repository_id": "2", "repository_owner_id": "1", "actor": "not-printed"}
        token = f"{b64({'alg': 'RS256'})}.{b64(claims_in)}.signature-not-printed"

        class TokenEndpoint(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                ok = self.headers["Authorization"] == "bearer request-token" and "audience=sts.amazonaws.com" in self.path
                self.send_response(200 if ok else 403)
                self.end_headers()
                self.wfile.write(json.dumps({"value": token}).encode())

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), TokenEndpoint)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.shutdown)
        env = {"PATH": os.environ["PATH"], "ACTIONS_ID_TOKEN_REQUEST_TOKEN": "request-token",
               "ACTIONS_ID_TOKEN_REQUEST_URL": f"http://127.0.0.1:{server.server_port}/token?api-version=2.0"}
        block = self.wf.run_script(claims)
        self.assertTrue(block.startswith("|"))
        script = textwrap.dedent(block.split("\n", 1)[1])
        # stderr too: the run log shows both.
        out = subprocess.run(["bash", "-e", "-c", script], env=env, check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True).stdout
        expected = {k: v for k, v in claims_in.items() if k != "actor"}
        self.assertEqual(json.loads(out), expected)
        for secret in ("request-token", "signature-not-printed", token.split(".")[1]):
            self.assertNotIn(secret, out)

    def test_build_runs_the_tooling_checks_from_full_history_before_building(self):
        names = [s.splitlines()[0].strip() for s in self.wf.steps("build")]
        self.assertLess(names.index("- name: Check release tooling"), names.index("- name: Build"))
        self.assertIn("fetch-depth: 0", self.wf.jobs["build"])
        self.assertIn("RELEASE_VERSION:", self.wf.step("build", "Build"))

    def test_checkouts_keep_no_credentials(self):
        checkouts = [s for s in self.all_steps() if "actions/checkout@" in s]
        self.assertTrue(checkouts)
        for step in checkouts:
            self.assertIn("persist-credentials: false", step)

    def test_every_action_is_pinned_to_a_commit(self):
        uses = re.findall(r"uses: *([^\s#]+)", self.wf.text)
        self.assertTrue(uses)
        for ref in uses:
            self.assertRegex(ref, r"@[0-9a-f]{40}$")

    def test_dry_run_publishes_nothing_and_keeps_no_signed_image(self):
        steps = self.wf.steps("publish")
        sign = steps.index(self.wf.step("publish", "Sign"))
        self.assertTrue(steps[sign + 1:])
        for step in steps[sign + 1:]:
            self.assertRegex(step, r"(?m)^        if: \$\{\{ !inputs\.dry_run \}\}$")
        self.assertNotIn("upload-artifact", self.wf.jobs["publish"])
        self.assertIn("--kms-key-id", self.wf.run_script(self.wf.step("publish", "Sign")))

    def test_release_is_staged_as_a_draft_and_never_reuses_a_tag(self):
        script = self.wf.run_script(self.wf.step("publish", "Publish"))
        self.assertLess(script.index("gh release view"), script.index("gh release create"))
        self.assertLess(script.index("git/matching-refs/tags/"), script.index("gh release create"))
        self.assertIn("release not found", script, "only a definite not-found may count as absent")
        self.assertLess(script.index("--draft"), script.index("--draft=false"))


GIT_CLIFF = shlex.split(os.environ.get("GIT_CLIFF", "git-cliff"))
CLIFF_TOML = ROOT / "cliff.toml"


def cliff(repo, *args):
    return subprocess.run([*GIT_CLIFF, "--config", str(CLIFF_TOML), *args], cwd=repo, check=True,
                          capture_output=True, text=True).stdout


@unittest.skipUnless(shutil.which(GIT_CLIFF[0]) or os.environ.get("CI"), "git-cliff not installed (set GIT_CLIFF)")
class ReleaseNotes(unittest.TestCase):
    """The repo's cliff.toml driving the real git-cliff, as the workflow does."""

    def setUp(self):
        self.repo = Repo()
        self.addCleanup(self.repo.close)

    def test_first_release_is_0_1_0_even_over_breaking_or_pre_convention_history(self):
        self.repo.commit("Initial commit")
        self.repo.commit("feat(upload): send cell temperatures")
        self.repo.commit("feat(release): cut releases", "BREAKING CHANGE: versions are semver now")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v0.1.0")

    def test_before_1_0_0_a_breaking_change_bumps_minor_not_major(self):
        self.repo.commit("feat: a")
        self.repo.tag("v0.1.0")
        self.repo.commit("refactor(upload)!: drop the v1 wire format")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v0.2.0")
        self.repo.tag("v0.2.0")
        self.repo.commit("fix: b")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v0.2.1")

    def test_bump_follows_conventional_commits_and_skips_candidates(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("feat(gps): b")
        self.repo.tag("v1.1.0-rc.1")
        self.repo.commit("fix: c")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v1.1.0")
        self.repo.commit("feat!: d")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v2.0.0")

    def test_breaking_change_of_any_type_bumps_major_and_is_listed(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("refactor(upload)!: drop the v1 wire format")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v2.0.0")
        self.assertIn("Drop the v1 wire format", cliff(self.repo.path, "--unreleased", "--tag", "v2.0.0", "--strip", "all"))

    def test_breaking_change_footer_is_listed_as_breaking(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("refactor(ble): renumber the OTA status frame", "BREAKING CHANGE: apps before 4.2 cannot update")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v2.0.0")
        notes = cliff(self.repo.path, "--unreleased", "--tag", "v2.0.0", "--strip", "all")
        self.assertIn("### Breaking Changes\n\n- **ble:** Renumber the OTA status frame", notes)
        self.assertIn("apps before 4.2 cannot update", notes)

    def test_bang_breaking_change_is_not_described_twice(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("refactor(upload)!: drop the v1 wire format")
        notes = cliff(self.repo.path, "--unreleased", "--tag", "v2.0.0", "--strip", "all")
        self.assertIn("### Breaking Changes\n\n- **upload:** Drop the v1 wire format\n", notes)

    def test_the_first_release_files_breaking_commits_under_their_type(self):
        # No semver release came before it, but pre-semver dongles did: a migration footer still matters.
        self.repo.commit("feat(release): cut releases", "BREAKING CHANGE: ota_sign.py takes --version-name")
        self.repo.commit("fix(ble)!: renumber the OTA status frame")
        notes = cliff(self.repo.path, "--unreleased", "--tag", "v0.1.0", "--strip", "all")
        self.assertNotIn("Breaking Changes", notes)
        self.assertIn("### Features\n\n- **release:** Cut releases - ota_sign.py takes --version-name\n", notes)
        self.assertIn("### Bug Fixes\n\n- **ble:** Renumber the OTA status frame\n", notes)

    def test_commits_that_do_not_change_the_image_do_not_release(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("docs: explain the wake ladder")
        self.repo.commit("chore: tidy")
        self.repo.commit("test(gps): cover replays")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v1.0.0")

    def test_ci_changes_never_reach_the_notes(self):
        self.repo.commit("feat: a")
        self.repo.tag("v1.0.0")
        self.repo.commit("ci(release): pin the build tools by hash")
        self.repo.commit("fix(ci): retry the PyPI download")
        self.repo.commit("feat(ci): publish a dry-run summary")
        self.assertEqual(cliff(self.repo.path, "--bumped-version").strip(), "v1.0.0")
        self.repo.commit("fix(upload): reopen idle sessions")
        notes = cliff(self.repo.path, "--unreleased", "--tag", "v1.0.1", "--strip", "all")
        self.assertEqual(notes.strip(), "### Bug Fixes\n\n- **upload:** Reopen idle sessions")

    def test_notes_group_the_whole_cycle_since_the_last_stable_release(self):
        self.repo.commit("feat: old")
        self.repo.tag("v1.0.0")
        self.repo.commit("feat(gps): keep fixes coherent")
        self.repo.tag("v1.1.0-rc.1")
        self.repo.commit("fix(upload): reopen idle sessions")
        self.repo.commit("chore: tidy")
        notes = cliff(self.repo.path, "--unreleased", "--tag", "v1.1.0", "--strip", "all")
        self.assertIn("Keep fixes coherent", notes)
        self.assertIn("Reopen idle sessions", notes)
        self.assertNotIn("old", notes)
        self.assertNotIn("tidy", notes)
        self.assertLess(notes.index("Features"), notes.index("Bug Fixes"))


if __name__ == "__main__":
    unittest.main()
