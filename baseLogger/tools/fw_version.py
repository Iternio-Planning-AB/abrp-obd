#!/usr/bin/env python3
"""
Firmware version naming: one place that turns "X.Y.Z[-rc.N]" into the uint32 OTA_FW_VERSION.

The dongle only ever compares integers (anti-downgrade floor, the signed START blob, the rollback marker), so
the semver name is packed as major*1_000_000 + minor*1_000 + patch. Minor and patch must stay below 1000 or two
names would share a code; major stops where the code would overflow uint32. A release candidate shares its
final's code on purpose - the dongle allows an equal version, so rc -> final installs.
"""

import argparse
import os
import re
import subprocess
import sys

_NAME = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-rc\.([1-9]\d*))?")
UINT32_MAX = 0xFFFFFFFF


def parse(name):
    """(major, minor, patch, rc or None) for a release name; ValueError for anything else."""
    m = _NAME.fullmatch(name) if isinstance(name, str) else None
    if not m:
        raise ValueError(f"not a release version (X.Y.Z or X.Y.Z-rc.N): {name!r}")
    major, minor, patch = (int(g) for g in m.groups()[:3])
    rc = int(m.group(4)) if m.group(4) else None
    if minor > 999 or patch > 999 or _pack(major, minor, patch) > UINT32_MAX:
        raise ValueError(f"{name!r} does not fit the uint32 OTA_FW_VERSION (minor/patch <= 999)")
    return major, minor, patch, rc


def _pack(major, minor, patch):
    return major * 1_000_000 + minor * 1_000 + patch


def version_code(name):
    return _pack(*parse(name)[:3])


def _git(repo, *args):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def build_version(repo, environ):
    """
    (name, code, build_tag) for the image being compiled in `repo`.

    CI passes RELEASE_VERSION and gets exactly that. Any other build takes the newest stable v* tag reachable
    from HEAD, so a bench flash reports the same code as the release it grew from (the dongle allows equal) and
    the build tag carries how far it has drifted: v1.2.0+2.gabc1234.dirty. No tag, or no git, is 0.0.0.
    """
    release = environ.get("RELEASE_VERSION")
    if release:
        return release, version_code(release), "v" + release
    reachable = [n for n, v in _released(repo, "--merged", "HEAD").items() if v[3] is None]
    if reachable:
        name = max(reachable, key=parse)
        described = _git(repo, "describe", "--tags", "--long", "--dirty", "--abbrev=7", "--match", "v" + name)
        ahead, sha, dirty = re.fullmatch(r"v.+-(\d+)-(g[0-9a-f]+)(-dirty)?", described).groups()
        if ahead == "0" and not dirty:
            return name, version_code(name), "v" + name
        return name, version_code(name), f"v{name}+{ahead}.{sha}" + (".dirty" if dirty else "")
    head = _git(repo, "describe", "--always", "--dirty", "--abbrev=7")
    if not head:
        return "0.0.0", 0, "v0.0.0+unknown"
    sha, _, dirty = head.partition("-")
    return "0.0.0", 0, f"v0.0.0+{_git(repo, 'rev-list', '--count', 'HEAD')}.g{sha}" + (".dirty" if dirty else "")


def _released(repo, *filters):
    """Every tag that parses as a release name, as {name: (major, minor, patch, rc)}."""
    out = {}
    for tag in (_git(repo, "tag", "--list", *filters, "v*") or "").splitlines():
        try:
            out[tag[1:]] = parse(tag[1:])
        except ValueError:
            pass                                    # v7 and other pre-semver tags are history, not releases
    return out


def next_release(repo, bumped, override=None, prerelease=False):
    """
    The name the next release gets. `bumped` is git-cliff's --bumped-version suggestion; `override` (X.Y.Z)
    replaces it. Either must be newer than the last stable release and may advance major by at most one: the
    dongle's anti-downgrade floor only ever rises, so a mistyped 9.0.0 would strand every dongle that installs it
    until each is reset over USB.
    """
    target = override if override else bumped.removeprefix("v")
    major, minor, patch, rc = parse(target)
    if rc is not None:
        raise ValueError(f"{target!r}: give X.Y.Z, release candidates are numbered by the prerelease option")
    released = _released(repo)
    stable = [v[:3] for v in released.values() if v[3] is None]
    last = max(stable, default=(0, 0, 0))
    if (major, minor, patch) <= last:
        what = "nothing to release" if not override else "not newer than"
        raise ValueError(f"{what} v{'.'.join(map(str, last))} (suggested {target}); nothing since changes "
                         "the image (only docs/chore/test commits), or pass an explicit newer version")
    if major > last[0] + 1:
        raise ValueError(f"{target!r} skips a major version after v{'.'.join(map(str, last))}")
    if not prerelease:
        return target
    rcs = [v[3] for v in released.values() if v[:3] == (major, minor, patch) and v[3] is not None]
    return f"{target}-rc.{max(rcs, default=0) + 1}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    nxt = sub.add_parser("next", help="print the name the next release gets")
    nxt.add_argument("--repo", default=".")
    nxt.add_argument("--bumped", required=True, help="git cliff --bumped-version output")
    nxt.add_argument("--override", default="", help="explicit X.Y.Z; empty means use --bumped")
    nxt.add_argument("--prerelease", action="store_true")
    bld = sub.add_parser("build", help="print name, code and build tag a build of this checkout would get")
    bld.add_argument("--repo", default=".")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "next":
            print(next_release(args.repo, args.bumped, args.override or None, args.prerelease))
        else:
            print(*build_version(args.repo, os.environ))
    except ValueError as e:
        sys.exit(f"ERROR: {e}")


if __name__ == "__main__":
    main()
