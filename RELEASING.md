# Releasing firmware

A release is one press: **Actions → Release → Run workflow** on `main`. The workflow
(`.github/workflows/release.yml`):

1. **build** job: picks the version from the Conventional Commits since the last release (`git cliff
   --bumped-version`: `feat` → minor, `fix`/`perf`/`revert` → patch, any `type!:` or `BREAKING CHANGE` → major, or
   minor before 1.0.0), unless you type one, then builds the commit you dispatched on with that version injected;
2. **publish** job: refuses an image that wasn't built as that version or doesn't carry the trusted public key,
   signs it with the AWS KMS key, checks the signature against `Helpers/ota_pubkey.h`, then stages a draft GitHub
   Release with every asset and publishes it. Publishing creates tag `vX.Y.Z` on that commit.

It never commits. Merges that land while it runs don't change what gets built. The jobs are split so that nothing
downloaded from PyPI or the PlatformIO registry runs where the OIDC token that can ask KMS for a signature exists.
The build toolchain is still trusted for *what it builds*: a compromised compiler's output would be signed.

| Input | Meaning |
| --- | --- |
| `version` | `X.Y.Z` to use instead of the computed one. Must be newer than the last release, and may raise major by at most one. |
| `prerelease` | Tags `vX.Y.Z-rc.N` (N counts up) and marks it as a prerelease, not "latest". |
| `dry_run` | Builds, signs and verifies through KMS, then publishes nothing and keeps no signed image (one would install on any dongle). Use it for the first run after any AWS change. |

Assets: `firmware.bin` and `manifest.json` (what OTA uses), `firmware.elf` (decodes crash backtraces for this exact
build), `factory.bin` (bootloader, partitions and app in one file for `esptool.py write_flash 0x0` on a blank unit;
**it erases NVS**, so pairing and the OTA floor are lost), and `SHA256SUMS`. The release body is the git-cliff notes.

## Versions

`X.Y.Z` packs into the uint32 the dongle compares: `major*1000000 + minor*1000 + patch` (`tools/fw_version.py`), so
minor and patch stop at 999. The BLE protocol, the signed blob and the anti-downgrade check are unchanged. The
pre-semver releases were the integers 1–7, and the first release, 0.1.0 (1000), is above all of them. Before 1.0.0 a
breaking change bumps minor, not major; type `1.0.0` as the `version` input when it's time. An rc and its final
share a code: the dongle accepts the final over its rc because it allows an equal version, but an app that only
offers a strictly newer `version` will not offer it, so OTA clients should break a tie on `version_name`.

The version is never edited by hand. `tools/pio_version.py` sets `OTA_FW_VERSION` and `FW_BUILD_TAG` at build time:

- release build: `RELEASE_VERSION` from the workflow, tag `v1.2.0`;
- any other build: the newest `vX.Y.Z` tag reachable from HEAD, so it reports the same code as the release it
  came from (bench re-flash and OTA of that release still work), with tag `v1.2.0+3.gabc1234.dirty`;
- no tag (or a shallow clone): `0.0.0`. Tags are created on GitHub; fetch them from the GitHub remote first
  (`git fetch <github-remote> --tags`).

Building and USB-flashing locally never needs the signing key; only BLE OTA checks signatures.

## Notes and commits

Only commits that change the image appear in the notes and count toward a release: breaking changes of any type,
`feat`, `fix`, `perf` and `revert`. `docs`, `chore`, non-breaking `refactor`, `test` and anything that isn't a Conventional
Commit don't. Every release raises the floor on each dongle that installs it, so a docs-only history doesn't mint
one: the run stops with "nothing to release". Give `version` to release anyway. Preview locally:

```sh
git cliff --unreleased --tag v1.2.0      # notes
git cliff --bumped-version               # what the next version would be
```

## Signing

Release images are signed with an AWS KMS key (ECC_NIST_P256, ECDSA over SHA-256) whose private half never leaves
KMS. The publish job swaps a short-lived GitHub OIDC token for 15-minute credentials on a role that trusts only
`.github/workflows/release.yml` on `main` of this repo; there are no long-lived credentials. The role ARN and key
id are the Actions **variables** `OTA_SIGNING_ROLE_ARN` and `OTA_KMS_KEY_ID` (not secrets; neither grants
anything on its own). If assuming the role fails, a `dry_run` prints the OIDC claims the job got.

Dongles trust the public key compiled into `Helpers/ota_pubkey.h`, regenerated from KMS with:

```sh
python3 baseLogger/tools/ota_keygen.py --from-kms <key-alias>
```

A dongle refuses any update not signed by the key it was built with, so changing the key means re-flashing
every dongle over USB (`pio run -e esp32dev -t upload`, which keeps NVS and pairing). After any change on the
signing side, do a `dry_run` first.

### Forks

The workflow needs the official signing role and will not sign anything in a fork. Generate your own key pair
with `python3 baseLogger/tools/ota_keygen.py` (the private key is written outside the repo), build with the
resulting `Helpers/ota_pubkey.h`, flash over USB once, and sign later images with `baseLogger/tools/ota_sign.py`. Official images then refuse to install on your dongles and
yours on official ones; that is the point.

## When a run fails

- **Before "Publish":** nothing exists outside the run. Fix and press again.
- **During "Publish":** at most a draft exists. A draft has no tag and isn't visible to OTA clients. A
  rerun refuses to stack a second draft on the same tag. Either finish it by hand with
  `gh release edit vX.Y.Z --draft=false --latest`, or delete it (`gh release delete vX.Y.Z --yes`) and run again.
- **A bad release is out:** don't delete it. Dongles that installed it keep its code as their floor. Release a fix
  as the next patch version.

## Known gaps

- CI never links the firmware, so the linker-map check (`tools/pio_psram_libs.py`, which fails any image that
  links a runtime without the PSRAM workaround) first runs in the release build. After a platform, framework or
  toolchain bump, build the merge commit with `pio run -e esp32dev` before tagging.
- The publish job trusts what the build job built. It checks only the build tag and the embedded public key, so a
  compromised PlatformIO dependency can't reach the key but could still get a backdoored image signed. The PyPI tools
  are pinned by hash (`.github/requirements/release.txt`), except starlette, which only `pio home` needs (see
  `release.in`), and so are esptool's own dependencies, which `tool-esptoolpy`'s post-install script pip-installs
  while PlatformIO installs its packages (`esptool.txt`; from the fetch on, pip has no index, only those wheels). The
  platform, the Arduino framework, esptool (which writes the signed `firmware.bin`) and SCons (which runs every build
  step) are pinned by version in `platformio.ini`, with no hashes, so they hold only while the registry doesn't
  republish a version; a pin without a `+` suffix also accepts a `+` rebuild of it. The toolchain is pinned by the
  platform's own manifest. Bump them together, and compare images built before and after from the same checkout path,
  core directory and `SOURCE_DATE_EPOCH`: `__FILE__` and `__DATE__` end up in the image.
- esptool's dependency list is copied by hand from the package's `package-postinstall.py` (`esptool_deps`) into
  `esptool.in`. On an esptool bump, copy the new list and regenerate `esptool.txt` with the command in `esptool.in`.
  *Check esptool dependencies* fails the run when the installed hook's list differs from `esptool.in`, or when
  `_contrib` holds anything but the files the hash-checked wheels list, byte for byte. pip doesn't re-check a fetched
  wheel's hash when it installs it; a hashed `PIP_CONSTRAINT` would, but pip 25.0.1 (the one CPython 3.12.15 bundles)
  then refuses the hook's `~=` ranges. If PyPI deletes a locked file, *Fetch esptool dependencies* fails and nothing
  can be released until the lock is regenerated. pip itself is not pinned: setup-python's bundled pip checks the
  hashes and runs the hook's install (the run prints its version).
