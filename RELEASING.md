# Releasing firmware

A release is one press: **Actions → Release → Run workflow** on `main`. The workflow
(`.github/workflows/release.yml`):

1. **build** job: picks the version from the Conventional Commits since the last release (`git cliff
   --bumped-version`: `feat` → minor, `fix`/`perf`/`revert` → patch, any `type!:` or `BREAKING CHANGE` → major),
   unless you type one, then builds the commit you dispatched on with that version injected;
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

> **The app does not read these releases yet.** It still fetches from the Bitbucket repo `abrp/firmware-update`
> (`src/state/firmwareUpdate/utils/firmwareRepository.ts` in the app). Until the app moves to the GitHub
> Releases API, OTA to testers goes through that path, or over USB.

## Versions

`X.Y.Z` packs into the uint32 the dongle compares: `major*1000000 + minor*1000 + patch` (`tools/fw_version.py`),
so minor and patch stop at 999. The BLE protocol, the signed blob and the anti-downgrade check are unchanged. The
pre-semver releases were the integers 1–7, and 1.0.0 (1000000) is above all of them. An rc and its final share a
code: the dongle accepts the final over its rc because it allows an equal version, but an app that only offers a
strictly newer `version` will not offer it (see Known gaps).

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

## One-time setup

The AWS and GitHub side lives in the Bitbucket repo `abrp/cloudops`, stack `tf/abrp-obd` (applied by
Flux in the `frankfurt` cluster, eu-central-1). Nothing here is created by hand. It holds:

- KMS key `alias/abrp-obd-ota-signing` (ECC_NIST_P256). Only role `abrp-obd-ota-release` may sign with it
  (digests, ECDSA_SHA_256), nobody can delete or disable it, and only the `admin-breakglass` SSO role can change
  its policy. Losing this key means every dongle needs a USB re-flash before it accepts another update.
- Role `abrp-obd-ota-release`, which trusts only `.github/workflows/release.yml` on `main` of this repo. If assuming
  it fails, a `dry_run` prints the OIDC claims the job got, to compare with the trust policy
  (`tf/abrp-obd/release-role.tf`).
- This repo's Actions **variables** (not secrets; neither grants anything) `OTA_SIGNING_ROLE_ARN` and
  `OTA_KMS_KEY_ID`, set through the GitHub App `iternio-cloudops-terraform`.

There are no long-lived credentials: the job swaps a short-lived OIDC token for 15-minute role credentials, and the
private key never leaves KMS.

### Accepted risks

The org is on GitHub Free: no environments, rulesets or branch protection on a private repo. So:

- **Anyone who can push to `main` can get firmware signed**, by pushing a change to `release.yml` and running it.
  Keep write access to this repo small.
- **A pushed `vX.Y.Z` tag drags the next version**, and with it the fleet's floor, up.
- **An `admin-breakglass` session can grant itself Sign** by rewriting the key policy.
- **Anyone who can rewrite the release role's trust can also sign**: the `tf-aws-infra` user and
  `tf-access-infra-assumable`. The cloudops infra and env tf-runners can too, after attaching more policy to their
  own roles, so a malicious cloudops merge to any env stack is enough. Fixing it means moving the key to a member
  account with an SCP; SCPs don't apply in the management account.
- Nothing alerts on any of this, or on Sign; CloudTrail's 90-day event history is the only record.

### Switch the firmware to the KMS key

The dongles trust the public key compiled into them. To make them trust the KMS key:

```sh
python3 baseLogger/tools/ota_keygen.py --from-kms alias/abrp-obd-ota-signing   # AWS admin, eu-central-1
```

Commit the regenerated `Helpers/ota_pubkey.h`, then **re-flash every existing dongle over USB**
(`pio run -e esp32dev -t upload`, which keeps NVS and pairing). Until a dongle has been re-flashed, it refuses
everything the workflow signs. The old local PEM is then unused; destroy it.

Then do a `dry_run`, and after that a `prerelease`, before the first real release.

## When a run fails

- **Before "Publish":** nothing exists outside the run. Fix and press again.
- **During "Publish":** at most a draft exists. A draft has no tag and the app's read-only token can't see it. A
  rerun refuses to stack a second draft on the same tag. Either finish it by hand with
  `gh release edit vX.Y.Z --draft=false --latest`, or delete it (`gh release delete vX.Y.Z --yes`) and run again.
- **A bad release is out:** don't delete it. Dongles that installed it keep its code as their floor. Release a fix
  as the next patch version.

## Known gaps

- The app reads Bitbucket, not these releases (see above). When it moves, it should:
  - order releases by `manifest.version`, not by tag;
  - break a tie on `version_name`, so a final is offered over its rc;
  - skip prereleases unless the user opts in;
  - show `version_name` rather than the integer `fw` from ABD5.
- `platform = espressif32` in `platformio.ini` is unpinned, so two releases of the same commit can use different
  toolchains. Pin it after confirming the version the current images are built with.
- The pip installs pin versions but not hashes.
- The publish job trusts what the build job built. It checks only the build tag and the embedded public key, so a
  compromised PyPI or PlatformIO dependency can't reach the key but could still get a backdoored image signed.
  Pinning the platform and hashing the pip installs narrows that.
- Nothing here has run on GitHub or AWS yet. `tests/run_release_checks.py` covers the tooling with a fake `aws`
  and throwaway keys.
