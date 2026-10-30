# Library guidance

These rules supplement the [root guidance](../AGENTS.md).

- `BLEWrapper`, `NetworkWrapper`, `OBDWrapper` and `DataLogger` connect the sketch to transports
  and storage. Trace their callers in `baseLogger/` before changing contracts.
- BLE callbacks run on Bluedroid, not the loop task. Keep NVS, crypto, flash and OBD work out
  of callbacks; use the existing hand-off to firmware services.
- Preserve the GATT table/handle constraints in `BLEWrapper`: bonded phones cache it.
  Wire changes require coordinated app changes, not just a firmware build.
- FreematicsPlus is modified vendor code, not a replaceable pristine copy. The STM32 firmware
  is closed and implements only part of ELM327; consult DONGLE_CONTEXT §6b before assuming an
  AT command works. GPS, modem and OBD host checks exercise different portions of this layer.
- Preserve upstream copyright/licence headers. See
  [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for ownership and licences.
- Leave unrelated vendored code alone during cleanup. `zlib/` is a verified, unmodified subset:
  its [README](zlib/README.md) documents provenance and `SHA256SUMS` drift checks.
- The PSRAM-safe runtime and compiler flags are required on ESP32 rev 1; do not remove them
  to make a host build pass.

## Verification

Use [tests/AGENTS.md](../tests/AGENTS.md) to select checks: BLE protocol/profile/relay,
OBD reply, heading/GPS, modem/upload, diagnostics/DataLogger, or gzip as appropriate.
Run both normal and `CXX='clang++ -DNDEBUG'` checks, then build the firmware for code changes.
Host fakes do not prove UART timing, LittleFS persistence, RF, RTC retention or vehicle behaviour;
the hardware verification requirement in the root guidance still applies.
