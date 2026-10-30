# Host-test guidance

These rules supplement the [root guidance](../AGENTS.md). The detailed coverage catalogue,
field scenarios, optional integrations and limitations are in [README.md](README.md); read only
the relevant section rather than loading it all.

## Running checks

Run from the repository root with Python 3 and `clang++` (or set `CXX`). Runners generally use
the Python standard library and compile real production code with host fakes and ASan/UBSan.
Run affected C++ checks normally and with `CXX='clang++ -DNDEBUG'`; assertions must remain effective.
Upload checks already force `NDEBUG`, so run them once. Compiler-independent release, protocol-fixture
and server-root checks also need only one run.

```sh
python3 tests/run_heading_checks.py
CXX='clang++ -DNDEBUG' python3 tests/run_heading_checks.py
```

To run every host runner, as CI does:

```sh
failed=0
for runner in tests/run_*.py; do
  python3 "$runner" || failed=1
done
test "$failed" -eq 0
```

Repeat with `CXX='clang++ -DNDEBUG'` exported, omitting the four once-only runners above as CI does.
Release-note checks need `git-cliff` (or `GIT_CLIFF`);
without it they skip locally and fail under `CI`. Live TLS-root checks may skip when the server
is unreachable; `--require-live` makes that a failure. Do not mistake a skip for verification.

## Runner map

| Area | `tests/` runner |
| --- | --- |
| [GNSS parsing, location/heading, geo buffering](README.md#headinggps-and-release-tooling) | `run_heading_checks.py` |
| [Clock trust, drift, NTP, target time bounds](README.md#send-once-geo-verification) | `run_clock_range_checks.py` |
| [Drive/energy/wake differential and safety properties](README.md#state-machine-differential-harness) | `run_sm_checks.py` |
| [Charge, DC hold, crash streak, telemetry](README.md#energy-and-temperature-host-checks) | `run_energy_checks.py` |
| [Upload queue/backlog](README.md#adaptive-upload-queue-checks-75), [signing and TLS](README.md#cellular-tls-trust-checks) | `run_upload_checks.py` |
| [Modem registration, recovery and sleep](README.md#cellular-registration-host-checks) | `run_modem_checks.py` |
| [CSV/events, DUMP, BLE write diagnostics](README.md#diagnostic-host-checks) | `run_log_diagnostics_checks.py` |
| [OBD parsing, retries and profile equations](README.md#obd-reply-checks) | `run_obd_reply_checks.py` |
| [Profile push/persistence/reset](README.md#profile-push-checks) | `run_profile_checks.py` |
| [BLE protocol negotiation and relay sessions](README.md#ble-protocol-and-relay-session-checks-90) | `run_relay_checks.py` |
| [Cross-repo wire fixtures and checksums](README.md#ble-protocol-and-relay-session-checks-90) | `run_protocol_fixture_checks.py` |
| [Real gzip/zlib, corruption and memory failure](README.md#diagnostic-host-checks) | `run_gzip_container_checks.py` |
| [Config macro grouping and variants](README.md#config-macro-checks) | `run_config_checks.py` |
| [Release/signing tooling and PSRAM linker map](README.md#headinggps-and-release-tooling) | `run_release_checks.py` |
| [Cellular server trust roots](README.md#cellular-tls-trust-checks) | `run_server_roots_checks.py` |

## Maintaining checks

- Extend the owning suite before creating another runner or fake. Diagnostics owns DataLogger
  schema/persistence behavior; energy owns charge decisions and energy/temperature field units.
- Keep overlap only for a distinct regression scenario or useful fault isolation. Consolidate
  fixtures without losing unique inputs/assertions; map removed coverage to its replacement and
  get approval before pruning existing checks. Do not weaken checks to make a refactor pass.
- Test observable behavior through real production code. Prefer direct includes over new textual
  extraction; retain structural safety guards until equivalent verification replaces them.
- Share small, repeated harness utilities only when needed; do not introduce a framework just to
  reduce runner count. Generated sequences, historical comparisons and absolute safety properties
  are complementary, not automatically duplicates.

- Extract/include production code rather than reimplementing it in the harness. Extraction
  anchors can include comments: comment-only changes still need relevant runner checks.
- Use independent oracles where possible; e.g. Python hashlib verifies signatures rather than
  trusting the same mbedTLS shim that produced them.
- Keep fixture resets, deterministic clocks/seeds, and assertions active under `NDEBUG`.
- Differential checks compare pinned OLD code with CUR, but shared context comes from the
  working tree on both sides. Equality cannot prove changes in that shared context.
- `SM_PIN` must remain a reachable main commit. Re-pin only after an intentional compared-region
  behaviour change lands on main, using the step's merge-base; never re-pin just to hide a failure.
  Read `run_sm_checks.py`'s header for exact regions and limitations.
- Keep the drive-region ownership checks: no outside assignments to its state and no drive
  state in `alarm_gate.h`. Do not weaken extraction/position guards to accommodate a refactor.
- Firmware feature variants and target-width semantics are not implied by host coverage.
  UART, clocks, filesystems and hardware remain fakes; state explicitly what needs a dongle.
- CI discovers every `run_*.py` in both CXX modes by default; only the four documented identical
  second runs are omitted. Keep this exception list aligned with each runner's compiler behavior.
  A new CI job must also be included in the required aggregate `ci` job's `needs`.
- Update the relevant section of `README.md` when coverage changes. Keep case-by-case histories
  there, not in this short instruction file.
