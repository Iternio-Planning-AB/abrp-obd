#!/usr/bin/env python3
"""Host checks for the release tooling in baseLogger/tools (versioning, signing, workflow)."""

import base64
import configparser
import contextlib
import hashlib
import http.server
import io
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
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "baseLogger" / "tools"
sys.path.insert(0, str(TOOLS))

import check_esptool_deps  # noqa: E402
import fw_version  # noqa: E402
import ota_keygen  # noqa: E402
import ota_sign  # noqa: E402
import psram_libs  # noqa: E402


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
REQUIREMENTS = ROOT / ".github" / "requirements"


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

    def test_plain_run_scripts_parse_as_yaml(self):
        # `--only-binary :all: -r` in a plain scalar is a YAML syntax error: GitHub rejects the whole file.
        # A ` #` there is no error, but silently cuts the command short.
        for script in filter(None, (self.wf.run_script(s) for s in self.all_steps())):
            if script.startswith(("|", ">", '"', "'")):
                continue
            self.assertNotRegex(script, r"^([-?:] |[\[\]{}#&*!%@`,])", "use run: |")
            for line in map(str.rstrip, script.splitlines()):
                with self.subTest(line=line):
                    self.assertNotRegex(line, r":\s|:$| #", "use run: |")

    def test_token_scopes_are_granted_per_job(self):
        self.assertRegex(self.wf.text, r"(?m)^permissions: \{\}$")

    def test_build_job_cannot_reach_the_signing_key(self):
        build = self.wf.jobs["build"]
        self.assertNotIn("id-token", build)
        self.assertNotIn("environment:", build)
        self.assertNotIn("secrets.", build)
        self.assertIn("contents: read", build)

    def assert_hash_lock(self, name):
        """`name`.txt pins every package by version and hash and was generated by `name`.in's command.
        Returns {package: locked version} and the .in text."""
        lock = (REQUIREMENTS / f"{name}.txt").read_text()
        reqs = re.split(r"(?m)^(?=[a-z])", lock)[1:]
        self.assertTrue(reqs)
        for req in reqs:
            with self.subTest(req=req.split()[0]):
                self.assertRegex(req, r"^[a-z0-9_.-]+==[^\s]+")
                self.assertIn("--hash=sha256:", req)
        spec = (REQUIREMENTS / f"{name}.in").read_text()
        regen = re.search(r"(?m)^#   (uv pip compile .*)$", spec)
        self.assertIsNotNone(regen)
        self.assertIn(f"#    {regen.group(1)}\n", lock, f"{name}.txt was not generated by {name}.in's command")
        return dict(re.findall(r"(?m)^([a-z0-9_.-]+)==(\S+)", lock)), spec

    def test_build_tools_install_only_by_hash(self):
        # A hash-less install would let a republished PyPI version run with the release's checkout.
        install = self.wf.run_script(self.wf.step("build", "Install tools"))
        self.assertEqual(len(re.findall(r"\bpip3? install\b", install)), 1, "every install goes through the lock")
        self.assertIn("--require-hashes", install)
        self.assertIn("--only-binary :all:", install)
        self.assertIn("-r .github/requirements/release.txt", install)
        locked, spec = self.assert_hash_lock("release")
        pins = re.findall(r"(?m)^([a-z0-9_.-]+)==(\S+)$", spec)
        self.assertTrue(pins)
        for pkg, version in pins:
            self.assertEqual(locked.get(pkg), version, f"{pkg}=={version} not in release.txt: regenerate it")
        self.assertNotIn("starlette", locked)
        # 2026.10.18 and older bundle the emSign roots with compromised intermediates (GHSA-6q48-6c7w-9gw2).
        self.assertGreaterEqual(tuple(map(int, locked["certifi"].split("."))), (2026, 10, 19))

    def test_build_tools_install_fails_on_any_gap_but_starlette(self):
        block = self.wf.run_script(self.wf.step("build", "Install tools"))
        self.assertTrue(block.startswith("|"))
        pio = re.search(r"(?m)^platformio==(\S+)$", (ROOT / ".github/requirements/release.in").read_text())
        self.assertIsNotNone(pio)
        starlette = f"platformio {pio.group(1)} requires starlette, which is not installed."
        self.assertIn(starlette, block, "the tolerated line must name the pinned platformio")
        script = textwrap.dedent(block.split("\n", 1)[1])
        with tempfile.TemporaryDirectory() as td:
            # pip install succeeds only with --no-deps; pip check prints $CHECK and exits $CODE.
            (Path(td) / "python").write_text(textwrap.dedent("""\
                #!/bin/sh
                case "$*" in
                  *"pip install"*--no-deps*) exit 0 ;;
                  *"pip check"*) [ -z "$CHECK" ] || echo "$CHECK" >&$FD; exit $CODE ;;
                esac
                exit 9
                """))
            (Path(td) / "python").chmod(0o755)
            for check, code, fd, ok in [
                (starlette, 1, 1, True),
                # Clean once starlette is back: the workaround must go with it.
                ("No broken requirements found.", 0, 1, False),
                (f"{starlette}\nfoo 1.0 requires bar, which is not installed.", 1, 1, False),
                (f"fastapi-{starlette}", 1, 1, False),
                ("ERROR: Exception:", 2, 2, False),
                ("", 137, 1, False),
            ]:
                with self.subTest(check=check):
                    env = dict(os.environ, PATH=f"{td}{os.pathsep}{os.environ['PATH']}",
                               CHECK=check, CODE=str(code), FD=str(fd))
                    # GitHub runs a run: step that sets no shell: as `bash -e {0}`.
                    run = subprocess.run(["bash", "-e", "-c", script], env=env, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
                    self.assertEqual(run.returncode == 0, ok)

    def test_esptool_dependencies_come_only_from_their_lock(self):
        # tool-esptoolpy's post-install hook pip-installs esptool's dependencies by version range. From the fetch on,
        # pip has no index, so the only wheels it can find are the ones fetched here by hash.
        steps = self.wf.steps("build")
        names = [s.splitlines()[0][len("      - name: "):] for s in steps]
        order = ["Fetch esptool dependencies", "Install PlatformIO packages", "Check esptool dependencies", "Build"]
        self.assertEqual([n for n in names if n in order], order)
        for step in steps[:names.index(order[0])]:
            self.assertNotRegex(self.wf.run_script(step) or "", r"\bpio\b")
        for step in steps:
            self.assertNotRegex(step, r"PIP_\w+:", "a step must not override the fetch's pip environment")
        block = self.wf.run_script(self.wf.step("build", order[0]))
        self.assertNotRegex(block, r"\bpip3? install\b")
        for arg in ("pip download", "--require-hashes", "--only-binary :all:", '-d "$RUNNER_TEMP/wheels"',
                    "-r .github/requirements/esptool.txt"):
            self.assertIn(arg, block)
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "python").write_text("#!/bin/sh\n")
            (Path(td) / "python").chmod(0o755)
            env = dict(os.environ, PATH=f"{td}{os.pathsep}{os.environ['PATH']}", RUNNER_TEMP=f"{td}/rt",
                       GITHUB_ENV=f"{td}/github_env")
            subprocess.run(["bash", "-e", "-c", textwrap.dedent(block.split("\n", 1)[1])], env=env, check=True,
                           stdout=subprocess.DEVNULL)
            self.assertEqual((Path(td) / "github_env").read_text(),
                             f"PIP_NO_INDEX=1\nPIP_FIND_LINKS={td}/rt/wheels\n")
        self.assertIn('--wheels "$RUNNER_TEMP/wheels"', self.wf.run_script(self.wf.step("build", order[2])))

    def test_esptool_lock_covers_the_pinned_hook(self):
        locked, spec = self.assert_hash_lock("esptool")
        reqs = [line for line in spec.splitlines() if line and not line.startswith("#")]
        self.assertTrue(reqs)
        for req in reqs:
            with self.subTest(req=req):
                # The hook's own operator; another one needs this check taught first.
                name, want = re.fullmatch(r"([a-z0-9_.-]+) ~= ([\d.]+)", req).groups()
                have = locked.get(name, "0").split(".")
                self.assertEqual(have[:want.count(".")], want.split(".")[:-1], f"locked {have}")
                self.assertGreaterEqual(tuple(map(int, have)), tuple(map(int, want.split("."))))
        # The top-level requirements are exactly esptool.in's: a lock not regenerated after an edit fails here.
        lock = (REQUIREMENTS / "esptool.txt").read_text()
        top = {b.split("==")[0] for b in re.split(r"(?m)^(?=[a-z])", lock)[1:]
               if re.search(r"(?m)^    #( via|  ) -r \.github/requirements/esptool\.in$", b)}
        self.assertEqual(top, {req.split()[0] for req in reqs})
        # The list belongs to one esptool package: bumping it in platformio.ini means copying the new hook's list.
        ini = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=("#", ";"))
        ini.read(ROOT / "baseLogger" / "platformio.ini")
        esptool = re.search(r"(?m)^platformio/tool-esptoolpy@\S+$", ini["env:esp32dev"]["platform_packages"])
        self.assertIsNotNone(esptool)
        self.assertIn(f"{esptool.group(0)};", spec)

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
        self.addCleanup(server.server_close)
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


class BuildInputs(unittest.TestCase):
    def test_platform_and_the_packages_that_shape_the_image_are_pinned(self):
        # Each env must pin for itself: [env] must not set these, and extends is not modelled.
        pinned = r"\s*@\s*\d+\.\d+\.\d+(\+[0-9A-Za-z.-]+)?"
        required = {"platformio/framework-arduinoespressif32", "platformio/tool-esptoolpy", "platformio/tool-scons"}
        for ini_path in (ROOT / "baseLogger" / "platformio.ini", ROOT / "MEMS_test" / "platformio.ini"):
            ini = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=("#", ";"))
            self.assertTrue(ini.read(ini_path), f"{ini_path} missing")
            if ini.has_section("env"):
                self.assertNotIn("platform", ini["env"])
                self.assertNotIn("platform_packages", ini["env"])
            envs = [s for s in ini.sections() if s.startswith("env:")]
            self.assertTrue(envs)
            for env in envs:
                with self.subTest(ini=ini_path.parent.name, env=env):
                    self.assertRegex(ini[env].get("platform", ""), rf"^platformio/espressif32{pinned}$")
                    # Every line pinned and listed once: a later line for the same package overrides the pin.
                    packages = [p.strip() for p in ini[env].get("platform_packages", "").splitlines() if p.strip()]
                    for pkg in packages:
                        self.assertRegex(pkg, rf"^platformio/[a-z0-9-]+{pinned}$")
                    names = [pkg.split("@")[0].strip() for pkg in packages]
                    self.assertEqual(len(names), len(set(names)), "a package is listed twice")
                    self.assertLessEqual(required, set(names))


class EsptoolDepsCheck(unittest.TestCase):
    """check_esptool_deps.py on a package whose hook installed two locked wheels, laid out as pip does."""

    DEPS = ["alpha ~= 1.0.0", "beta ~= 2.0.0"]

    def fixture(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        root = Path(td.name)
        fx = {"root": root, "wheels": root / "wheels", "package": root / "packages" / "tool-esptoolpy",
              "spec": root / "esptool.in", "lock": root / "esptool.txt"}
        contrib = fx["contrib"] = fx["package"] / "_contrib"
        fx["wheels"].mkdir()
        contrib.mkdir(parents=True)
        for manifest in (".piopm", "package.json"):
            (fx["package"] / manifest).write_text("{}")
        self.hook(fx, self.DEPS)
        fx["spec"].write_text("# copied from the hook\n" + "".join(f"{dep}\n" for dep in self.DEPS))
        lock = ["# uv pip compile\n"]
        for name, version in (("alpha", "1.0.1"), ("beta", "2.0.0")):
            dist = f"{name}-{version}.dist-info"
            files = {f"{name}/__init__.py": f"VERSION = {version!r}\n",
                     f"{dist}/METADATA": f"Metadata-Version: 2.1\nName: {name}\nVersion: {version}\n"}
            record = "".join(f"{path},{check_esptool_deps.record_digest(text.encode())},{len(text)}\n"
                             for path, text in files.items()) + f"{dist}/RECORD,,\n"
            wheel = fx["wheels"] / f"{name}-{version}-py3-none-any.whl"
            with zipfile.ZipFile(wheel, "w") as whl:
                for path, text in {**files, f"{dist}/RECORD": record}.items():
                    whl.writestr(path, text)
            for path, text in {**files, f"{dist}/RECORD": "rewritten by pip", f"{dist}/INSTALLER": "pip"}.items():
                (contrib / path).parent.mkdir(parents=True, exist_ok=True)
                (contrib / path).write_text(text)
            lock.append(f"{name}=={version} \\\n    --hash=sha256:{hashlib.sha256(wheel.read_bytes()).hexdigest()}\n")
        fx["lock"].write_text("".join(lock))
        (contrib / "bin").mkdir()
        (contrib / "bin" / "alpha").write_text("#!/usr/bin/python3\n")
        return fx

    @staticmethod
    def hook(fx, deps):
        (fx["package"] / "package-postinstall.py").write_text(
            f"import os\n\nesptool_deps = {deps!r}\n\nraise SystemExit('never run')\n")

    def check(self, fx):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = check_esptool_deps.main([f"--{k}={fx[k]}" for k in ("wheels", "package", "spec", "lock")])
        return code, out.getvalue()

    def test_what_the_locked_wheels_install_passes(self):
        code, out = self.check(self.fixture())
        self.assertEqual(code, 0, out)
        self.assertNotIn("::error", out)

    def test_anything_else_is_refused(self):
        def append(path, text):
            with open(path, "a") as f:
                f.write(text)

        def uninstall_beta(fx):
            (fx["wheels"] / "beta-2.0.0-py3-none-any.whl").unlink()
            for path in ("beta", "beta-2.0.0.dist-info"):
                shutil.rmtree(fx["contrib"] / path)

        cases = {
            # What a failed hook leaves: it deletes the manifests and _contrib, and exits 0.
            "no .piopm": lambda fx: (fx["package"] / ".piopm").unlink(),
            "no package.json": lambda fx: (fx["package"] / "package.json").unlink(),
            "no _contrib": lambda fx: shutil.rmtree(fx["contrib"]),
            "a second package": lambda fx: (fx["package"].parent / "tool-esptoolpy@2.41100.0").mkdir(),
            "a second package, renamed": lambda fx: (fx["package"].parent / "tool-esptoolpy-old").mkdir(),
            "hook list shrank": lambda fx: self.hook(fx, self.DEPS[:-1]),
            "hook list grew": lambda fx: self.hook(fx, [*self.DEPS, "gamma ~= 1.0"]),
            "no hook": lambda fx: (fx["package"] / "package-postinstall.py").unlink(),
            "wheel swapped": lambda fx: append(fx["wheels"] / "beta-2.0.0-py3-none-any.whl", "x"),
            "wheel extra": lambda fx: (fx["wheels"] / "gamma-1.0-py3-none-any.whl").write_text(""),
            "wheel twice": lambda fx: shutil.copy(fx["wheels"] / "beta-2.0.0-py3-none-any.whl",
                                                  fx["wheels"] / "beta-2.0.0-py2.py3-none-any.whl"),
            "wheel and its files missing": uninstall_beta,
            "file changed": lambda fx: append(fx["contrib"] / "alpha" / "__init__.py", "import os\n"),
            "file missing": lambda fx: (fx["contrib"] / "beta" / "__init__.py").unlink(),
            "file extra": lambda fx: (fx["contrib"] / "evil.pth").write_text("import os\n"),
        }
        for case, change in cases.items():
            with self.subTest(case=case):
                fx = self.fixture()
                change(fx)
                code, out = self.check(fx)
                self.assertEqual(code, 1, out)
                self.assertIn("::error title=esptool dependencies::", out)


class PsramRuntimeLink(unittest.TestCase):
    """The ESP32 rev 1 PSRAM workaround must reach the link: the plain libc/libstdc++ lost stores (#64)."""

    LIB = "/p/packages/toolchain-xtensa-esp32/bin/../lib/gcc/xtensa-esp32-elf/8.4.0/../../../../xtensa-esp32-elf/lib"
    GCC = "/p/packages/toolchain-xtensa-esp32/bin/../lib/gcc/xtensa-esp32-elf/8.4.0"

    def map_text(self, multilib):
        # Shapes copied from a real firmware.map: archive members, a wrapped member line, LOAD lines, crt objects.
        return textwrap.dedent(f"""\
            Archive member included to satisfy reference by file (symbol)

            {self.LIB}/{multilib}/libc.a(lib_a-memcpy.o)
                                          .pio/build/esp32dev/src/logger.ino.cpp.o (memcpy)
            {self.LIB}/{multilib}/libc.a(lib_a-memset.o)
                                          .pio/build/esp32dev/src/logger.ino.cpp.o (memset)
            {self.LIB}/{multilib}/libstdc++.a(eh_catch.o)
                                          {self.LIB}/{multilib}/libstdc++.a(del_opvnt.o) (__cxa_begin_catch)
            .pio/build/esp32dev/lib5a0/libFreematicsPlus.a(FreematicsOBD.cpp.o)
                                          .pio/build/esp32dev/src/logger.ino.cpp.o (_ZN7COBD4initEv)
            LOAD {self.GCC}/{multilib}/crti.o
            LOAD {self.LIB}/{multilib}/crt0.o
            LOAD {self.LIB}/{multilib}/libm.a
            LOAD {self.GCC}/{multilib}/libgcc.a
            LOAD /p/packages/framework-arduinoespressif32/tools/sdk/esp32/lib/libheap.a
            """)

    def test_plain_multilib_names_every_toolchain_object(self):
        unsafe = psram_libs.unsafe_objects(self.map_text("no-rtti"))
        self.assertCountEqual([path.rsplit("/", 1)[-1] for path in unsafe],
                         ["libc.a", "libstdc++.a", "crti.o", "crt0.o", "libm.a", "libgcc.a",
                          "lib_a-memcpy.o not from the PSRAM libc", "lib_a-memset.o not from the PSRAM libc"])

    def test_one_plain_archive_among_psram_ones_fails(self):
        text = self.map_text("esp32-psram/no-rtti")
        self.assertEqual(psram_libs.unsafe_objects(text), [])
        text += f"LOAD {self.LIB}/no-rtti/libnosys.a\n"
        self.assertEqual(psram_libs.unsafe_objects(text), [f"{self.LIB}/no-rtti/libnosys.a"])

    def test_string_functions_from_rom_fail_though_libc_is_psram(self):
        # A ROM newlib linker script resolves memcpy to ROM: libc.a then supplies other members, never memcpy.
        text = self.map_text("esp32-psram/no-rtti").replace("libc.a(lib_a-memcpy.o)", "libc.a(lib_a-strlen.o)")
        self.assertEqual(psram_libs.unsafe_objects(text), ["lib_a-memcpy.o not from the PSRAM libc"])

    def test_psram_must_be_the_multilib_not_any_directory_above_it(self):
        lib = self.LIB.replace("/p/packages/", "/esp32-psram/packages/")
        text = self.map_text("no-rtti").replace(self.LIB, lib).replace(self.GCC, self.GCC.replace("/p/", "/esp32-psram/"))
        self.assertIn(f"{lib}/no-rtti/libc.a", psram_libs.unsafe_objects(text))

    def test_rom_newlib_linker_scripts_are_refused(self):
        # ESP-IDF leaves these out when the workaround is on: ROM string and printf code has none.
        core = ["-T", "esp32.rom.ld", "-T", "esp32.rom.libgcc.ld", "-T", "esp32.rom.newlib-data.ld"]
        self.assertEqual(psram_libs.rom_newlib_scripts(core), [])
        for script in ["esp32.rom.newlib-funcs.ld", "esp32.rom.newlib-nano.ld", "esp32.rom.newlib-time.ld"]:
            with self.subTest(script=script):
                self.assertEqual(psram_libs.rom_newlib_scripts([*core, "-T", script]), [script])

    def test_command_line_check_for_the_release_workflow(self):
        with tempfile.TemporaryDirectory() as td:
            for multilib, code in [("esp32-psram/no-rtti", 0), ("no-rtti", 1)]:
                with self.subTest(multilib=multilib):
                    path = Path(td) / "firmware.map"
                    path.write_text(self.map_text(multilib))
                    result = subprocess.run([sys.executable, str(TOOLS / "psram_libs.py"), str(path)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        steps = Workflow((ROOT / ".github/workflows/release.yml").read_text()).steps("build")
        names = [step.splitlines()[0] for step in steps]
        check = next(i for i, step in enumerate(steps) if "tools/psram_libs.py" in (Workflow.run_script(step) or ""))
        self.assertGreater(check, next(i for i, name in enumerate(names) if "name: Build" in name),
                           "the release checks the map the build wrote")

    def test_map_without_toolchain_objects_is_an_error_not_a_pass(self):
        # e.g. a toolchain that spells its target differently: fail, do not wave it through
        text = self.map_text("esp32-psram/no-rtti").replace("xtensa-esp32-elf", "xtensa-esp-elf")
        with self.assertRaises(ValueError):
            psram_libs.unsafe_objects(text)

    def test_platformio_ini_wires_the_link_check(self):
        ini = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=("#", ";"))
        ini.read(ROOT / "baseLogger" / "platformio.ini")
        scripts = [s.strip() for s in ini["env:esp32dev"]["extra_scripts"].splitlines() if s.strip()]
        self.assertIn("tools/pio_psram_libs.py", scripts)
        self.assertIn(psram_libs.FLAG, ini["env:esp32dev"]["build_flags"].split())


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
