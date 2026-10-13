# Contributing

Thanks for helping. Start with the [README](README.md) for what this is and the
[Getting started guide](docs/getting-started.md) to get a dongle running; the
[Technical overview](docs/technical-overview.md) covers the code layout and settings.

Security problems go through [SECURITY.md](SECURITY.md), never a public issue.

## Build

```sh
cd baseLogger && pio run -e esp32dev
```

A clean build prints two `"PRINT" redefined` warnings from vendored code. Anything else is new.

## Test

The host checks live in `tests/` and need Python 3 and `clang++`. Run the ones that touch your
change, for example:

```sh
python3 tests/run_heading_checks.py
python3 tests/run_upload_checks.py
python3 tests/run_energy_checks.py
```

They cover parsing, uploads and charging logic on a host with fake hardware. They do not replace a
real dongle: a firmware change should be flashed and run on a device before it is merged, and the
pull request should say what was tested on hardware and what was not. A change that only a drive or
a charge can exercise (power gating, upload cadence, real PID values) is not verified by a bench
test; say so.

## Fixes must hold for every car

One car's log is evidence, not the scope of the fix. Do not add behaviour keyed to a single make or
model; fix the general case so it holds for all supported cars.

## Commits

Commit messages generate the release notes, so use
[Conventional Commits](https://www.conventionalcommits.org/): `type(scope): imperative summary`,
describing the user-visible change, not just the implementation (scope optional). Only `feat`,
`fix`, `perf`, `revert` and breaking changes reach the notes, via [`cliff.toml`](cliff.toml); `docs`,
`ci`, `test`, `refactor` and `chore` are left out and do not bump the version. Mark a breaking change
with `!` and a `BREAKING CHANGE:` footer that explains the impact and migration; a `!` on an
otherwise excluded type still ships.

Protocol changes (BLE characteristics, wire formats) need a matching change in the ABRP app; mention
it in the pull request.

## Logs and private data

Never attach dongle logs, `DUMP` output or ABRP tokens to a public issue or pull request; see
[SECURITY.md](SECURITY.md).
