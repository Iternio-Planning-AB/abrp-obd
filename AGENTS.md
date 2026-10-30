# Repository guidance

An ESP32 OBD logger on Freematics ONE+ hardware, uploading to ABRP over its own LTE-M modem.
A phone is an occasional privileged client, not the data path. A client that can write to the
dongle can write to the car, and nobody may be nearby to notice a flattened 12 V battery.
The closed STM32 co-processor owns CAN, K-line and pin-16 voltage; we cannot reflash it.

Read §0 of [DONGLE_CONTEXT.md](DONGLE_CONTEXT.md), then only the relevant section. Code is
authoritative; historical design documents may describe superseded behaviour.

## Working rules

- Branch from `main`. Put worktrees in `.worktrees/` at the project root.
- Do not commit, push or open PRs unless asked. Follow the Conventional Commit rules in
  [CONTRIBUTING.md](CONTRIBUTING.md); commit types control release notes and version bumps.
- Check `git remote -v` before using a forge CLI.
- Preserve safety invariants, hardware constraints and field history in comments. Trim obvious
  narration and repetition, not the reasons a workaround exists.
- Fix the general case, not behaviour keyed to one car's make or model.
- Firmware changes must run on a device before committing. A bench test does not verify power
  gates, real PIDs or GPS; report hardware and drive verification separately from host checks.
- Protocol changes must land with the matching closed-source ABRP app change. Describe that
  app-side dependency in the PR; a firmware-only wire-format change breaks the pair.
- Keep the supported-car and settings tables in the user docs current.
- Never publish dongle logs, DUMP output or tokens; see [SECURITY.md](SECURITY.md).

## Build and test

```sh
cd baseLogger && pio run -e esp32dev
```

`pio` may be absent from a non-login shell's PATH. If unavailable, run from `baseLogger/`:

```sh
uv run --no-project --with platformio==6.1.18 pio run -e esp32dev
```

Host checks use Python 3 and `clang++` (or `CXX`), from the repository root:

```sh
python3 tests/run_heading_checks.py
CXX='clang++ -DNDEBUG' python3 tests/run_heading_checks.py
```

Choose runners using [tests/AGENTS.md](tests/AGENTS.md); detailed coverage is in
[tests/README.md](tests/README.md). **CI proves host checks and firmware builds, not device behaviour.**
The required `ci` job in `.github/workflows/host-tests.yml` aggregates checks; add new jobs to its `needs`.

## Scoped guidance

Nested instructions supplement this file. Each `CLAUDE.md` is a relative symlink to its sibling
`AGENTS.md`; edit the target, not a second copy.

| Scope | Guidance |
| --- | --- |
| Sketch, helpers, configuration and firmware tooling | [baseLogger/AGENTS.md](baseLogger/AGENTS.md) |
| Wrappers, drivers and vendored dependencies | [libraries/AGENTS.md](libraries/AGENTS.md) |
| Host harnesses and verification | [tests/AGENTS.md](tests/AGENTS.md) |

Keep these files short and actionable. Detailed coverage and field narratives belong in the
linked references, not in automatically loaded instructions.

## Reference map

| Question | Reference |
| --- | --- |
| Product, hardware and supported cars | [README.md](README.md) |
| Setup and flashing | [docs/getting-started.md](docs/getting-started.md) |
| Code layout, settings and console | [docs/technical-overview.md](docs/technical-overview.md) |
| Release process and signing | [RELEASING.md](RELEASING.md) |
| Enabled features / BLE layout / auth | DONGLE_CONTEXT §1 / §2 / §3, Appendix A |
| OTA / log retrieval | DONGLE_CONTEXT §4, Appendices B–C / §5 |
| Sleep, wake gates and battery protection | DONGLE_CONTEXT §6 |
| Cloud uploads and profiles / co-processor limits | DONGLE_CONTEXT §6a / §6b |
| OBD-alarm gate / app obligations | DONGLE_CONTEXT §7 / §8 |
| Diagnostics / invariants | DONGLE_CONTEXT §9 / §11 |
