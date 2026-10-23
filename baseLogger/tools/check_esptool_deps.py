#!/usr/bin/env python3
"""
Check that tool-esptoolpy's post-install hook installed exactly the esptool dependencies locked in
.github/requirements/esptool.txt (release.yml runs this between `pio pkg install` and the build).

The hook pip-installs its own list into the package's _contrib. It reports a failure only by deleting the package's
manifests and exiting 0, a cached or preinstalled package never runs it, and pip installs from the fetched wheels
without checking their hashes. So the result is checked rather than trusted: both manifests present and one package
directory, the hook's list equal to esptool.in, one fetched wheel per locked package, each hashed against the lock,
and every file in _contrib byte-identical to what those wheels' RECORDs list, with nothing else beside them.

Usage:
    python3 baseLogger/tools/check_esptool_deps.py --wheels "$RUNNER_TEMP/wheels"
"""

import argparse
import ast
import base64
import csv
import hashlib
import os
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REQUIREMENTS = ROOT / ".github" / "requirements"
# Written by pip itself, so not in the wheel's RECORD; scripts in bin/ get their shebang rewritten and are never run.
PIP_WRITES = {"INSTALLER", "REQUESTED", "RECORD", "direct_url.json"}


def normalize(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def hook_deps(package):
    """The hook's `esptool_deps` list, read without running the hook."""
    tree = ast.parse((package / "package-postinstall.py").read_text())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", None) == "esptool_deps" for t in node.targets):
            return ast.literal_eval(node.value)


def locked_wheels(lock):
    """{package: (version, {sha256 hex})} from a `uv pip compile --generate-hashes` lock."""
    locked = {}
    for block in re.split(r"(?m)^(?=[A-Za-z])", lock.read_text())[1:]:
        m = re.match(r"([A-Za-z0-9_.-]+)==([^\s;]+)", block)
        if m:
            locked[normalize(m.group(1))] = (m.group(2), set(re.findall(r"--hash=sha256:([0-9a-f]{64})", block)))
    return locked


def record_digest(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def problems(package, wheels, spec, lock):
    errors = []
    for manifest in (".piopm", "package.json"):
        if not (package / manifest).is_file():
            errors.append(f"{package / manifest} is missing: the post-install hook failed (its pip error is above)")
    errors += [f"{other}: a second {package.name}, whose hook nothing checked" for other in
               sorted(package.parent.glob(f"{package.name}*")) if other != package]
    wanted = [line for line in spec.read_text().splitlines() if line and not line.startswith("#")]
    try:
        deps = hook_deps(package)
    except (OSError, SyntaxError, ValueError) as e:
        deps = f"unreadable ({e})"
    if deps != wanted:
        errors.append(f"the hook's esptool_deps {deps} differ from {spec.name} {wanted}: copy them there and "
                      f"regenerate {lock.name}")

    locked = locked_wheels(lock)
    expected, fetched = {}, []
    for whl in sorted(wheels.iterdir()):
        name, version = whl.name.split("-")[:2]
        if locked.get(normalize(name), ("", ()))[0] != version or \
                hashlib.sha256(whl.read_bytes()).hexdigest() not in locked[normalize(name)][1]:
            errors.append(f"{whl} is not a wheel {lock.name} locks")
            continue
        fetched.append(normalize(name))
        with zipfile.ZipFile(whl) as z:
            record = next(n for n in z.namelist() if n.endswith(".dist-info/RECORD"))
            for path, digest, *_ in csv.reader(z.read(record).decode().splitlines()):
                if digest and ".data/" not in path:
                    expected[path] = digest
    errors += [f"{wheels} holds {fetched.count(name)} wheels for {name}, not one" for name in sorted(locked)
               if fetched.count(name) != 1]

    contrib = package / "_contrib"
    for path in sorted(p for p in contrib.rglob("*") if p.is_file()):
        rel = path.relative_to(contrib).as_posix()
        if rel.startswith("bin/") or (rel.split("/")[0].endswith(".dist-info") and path.name in PIP_WRITES):
            continue
        if expected.pop(rel, None) != record_digest(path.read_bytes()):
            errors.append(f"{path} is not a file a locked wheel installs")
    errors += [f"{contrib / rel} is missing" for rel in sorted(expected)]
    return errors


def main(argv=None):
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--wheels", type=Path, required=True, help="the directory the hook's pip installed from")
    ap.add_argument("--package", type=Path, default=core / "packages" / "tool-esptoolpy")
    ap.add_argument("--spec", type=Path, default=REQUIREMENTS / "esptool.in")
    ap.add_argument("--lock", type=Path, default=REQUIREMENTS / "esptool.txt")
    args = ap.parse_args(argv)
    errors = problems(args.package, args.wheels, args.spec, args.lock)
    for error in errors:
        print(f"::error title=esptool dependencies::{error}")
    if not errors:
        print(f"{args.package}: the hook installed exactly {args.lock.name}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
