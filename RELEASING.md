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

Nothing below is automated. Do it once, in this order.

### 1. AWS: signing key

Create an asymmetric key that never leaves KMS:

```sh
aws kms create-key --key-spec ECC_NIST_P256 --key-usage SIGN_VERIFY \
  --description "abrp-obd OTA firmware signing" --policy file://key-policy.json
aws kms create-alias --alias-name alias/abrp-obd-ota-signing --target-key-id <key-id>
```

The key policy allows `kms:Sign` (digest, ECDSA_SHA_256 only) and `kms:GetPublicKey` only to the release role. It
denies deletion and disabling to everyone, admins included; lifting that takes a deliberate `PutKeyPolicy`. Losing
this key means every dongle needs a USB re-flash before it accepts another update. The account root keeps key
administration so the key can't be orphaned if the admin role is deleted:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Sid": "Root", "Effect": "Allow", "Principal": { "AWS": "arn:aws:iam::<account>:root" },
      "Action": ["kms:Describe*", "kms:Get*", "kms:List*", "kms:PutKeyPolicy"], "Resource": "*" },
    { "Sid": "Admin", "Effect": "Allow", "Principal": { "AWS": "arn:aws:iam::<account>:role/<admin-role>" },
      "Action": ["kms:Describe*", "kms:Get*", "kms:List*", "kms:PutKeyPolicy", "kms:TagResource",
                 "kms:CreateAlias", "kms:UpdateAlias", "kms:DeleteAlias"],
      "Resource": "*" },
    { "Sid": "ReleaseSigns", "Effect": "Allow", "Principal": { "AWS": "arn:aws:iam::<account>:role/abrp-obd-release" },
      "Action": "kms:Sign", "Resource": "*",
      "Condition": { "StringEquals": { "kms:SigningAlgorithm": "ECDSA_SHA_256", "kms:MessageType": "DIGEST" } } },
    { "Sid": "ReleaseReadsKey", "Effect": "Allow",
      "Principal": { "AWS": "arn:aws:iam::<account>:role/abrp-obd-release" },
      "Action": "kms:GetPublicKey", "Resource": "*" },
    { "Sid": "NoDeletion", "Effect": "Deny", "Principal": "*",
      "Action": ["kms:ScheduleKeyDeletion", "kms:DisableKey"], "Resource": "*" }
  ]
}
```

### 2. AWS: role for the workflow

Add `token.actions.githubusercontent.com` as an OIDC identity provider (audience `sts.amazonaws.com`) if the
account doesn't have it, then create role `abrp-obd-release` with this trust policy. The subject pins the repo
**and** the `release` environment, so no other repo, branch or job can assume it. Matching is case-sensitive.

```json
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": { "Federated": "arn:aws:iam::<account>:oidc-provider/token.actions.githubusercontent.com" },
    "Action": "sts:AssumeRoleWithWebIdentity",
    "Condition": { "StringEquals": {
      "token.actions.githubusercontent.com:aud": "sts.amazonaws.com",
      "token.actions.githubusercontent.com:sub": "repo:Iternio-Planning-AB/abrp-obd:environment:release"
    } }
  }]
}
```

It needs no permissions policy beyond what the key policy grants. Check the org's subject template first
(`gh api orgs/Iternio-Planning-AB/actions/oidc/customization/sub`); if it is customised, change `sub` to match. Keep
`StringEquals`, never `StringLike` with wildcards.

### 3. GitHub: environment and secrets

In *Settings → Environments*, create `release` with a deployment branch rule that allows only `main`. Add these
environment secrets (not repository secrets, so only that environment's jobs can read them):

| Secret | Value |
| --- | --- |
| `OTA_SIGNING_ROLE_ARN` | `arn:aws:iam::<account>:role/abrp-obd-release` |
| `OTA_SIGNING_REGION` | the key's region |
| `OTA_KMS_KEY_ID` | `alias/abrp-obd-ota-signing` |

There are no long-lived credentials: the job swaps a short-lived OIDC token for 15-minute role credentials, and the
private key never leaves KMS.

**Do before the first real release.** Until these exist, anyone who can push to this repo can produce firmware
every dongle trusts:

- a ruleset on `main`: PR required, no force-push, no deletion. Without it, a pushed commit gets signed, and a new
  workflow file that names `environment: release` gets the same OIDC subject;
- a tag ruleset restricting creation of `v*` tags to admins (the workflow's `GITHUB_TOKEN` can still create them).
  Versions are computed from existing tags, so a pushed `v9.0.0` would drag the next release, and the fleet's floor,
  up with it;
- optionally, a required reviewer on `release` with "prevent self-review" (a second pair of eyes, at the cost of the
  one press), and a custom OIDC subject that includes `job_workflow_ref`, pinned to `release.yml@refs/heads/main`.

### 4. Switch the firmware to the KMS key

The dongles trust the public key compiled into them. To make them trust the KMS key:

```sh
python3 baseLogger/tools/ota_keygen.py --from-kms alias/abrp-obd-ota-signing   # needs kms:GetPublicKey
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
