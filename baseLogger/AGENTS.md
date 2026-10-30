# Firmware guidance

These rules supplement the [root guidance](../AGENTS.md). Domain details are in
[DONGLE_CONTEXT.md](../DONGLE_CONTEXT.md), especially §6 (power) and §11 (invariants).

## Sketch structure

- `Helpers/*.h` are included into `logger.ino` as one translation unit, sharing globals and
  `config.h` flags. Do not treat them as independently compiled headers or add `static` where
  a global is expected.
- Include order is by dependency: users of `logEvent` follow `logging.h`; users of `gzipBody`
  follow `gzip_body.h`. A symbol is not visible before its declaration/include.
- `logger.ino`: `setup()` is the wake ladder, `loop()` is the poll/upload cycle.
  `Helpers/state_machine.h` owns activity/cadence, `Helpers/drive_state.h` the drive region,
  `Helpers/sleep_power.h` normal sleep, and `Helpers/auth_gate.h` ABD8 authentication.
- `build_src_filter` excludes Freematics reference copies (`stanleyhuangyc*`, `*telelogger*`,
  `*libraries-FreematicsPlus*`). Do not compile a reference copy beside the real library.

## Runtime constraints

- Deep sleep reboots through `setup()`. State that must survive belongs in `RTC_DATA_ATTR`
  or LittleFS, not ordinary globals. Panic, WDT and software resets reload RTC_DATA_ATTR too.
- At crash streak 3, setup's early timer-only sleep precedes peripheral initialisation:
  already-powered peripherals are deliberately left on and WOM is not armed. This is an
  exception to normal `enterDeepSleep`, not a template for new sleep paths.
- BLE callbacks run on Bluedroid: stash bytes/flags and do NVS, crypto and OBD work in loop-task
  services. ABD2's `StartupCallbacks::onWrite` and ABD9's `relaySessionWrite` parse small JSON;
  ABD9 accepts at most 64 B into a locked mailbox and never logs or calls `ble`.
- Tunables and their rationale belong in `config.h`. Preserve measured field constraints.
  The SIM7070's `SHCONF BODYLEN=1024` is a hard limit; the key-reset window starts at power-on.
- Drive-state ownership and extraction boundaries are checked by `tests/run_sm_checks.py`.
  Other runners also use source/comment anchors: run relevant checks after comment-only edits.

## Build and release

Build from this directory with `pio run -e esp32dev` (fallback in the root guidance).
Flash/monitor with `pio run -e esp32dev -t upload -t monitor`, 115200 baud.

- Two existing `"PRINT" redefined` warnings come from `FreematicsOBD.cpp` and
  `mathparser/parser.cpp`; investigate new warnings.
- Every link must pass `tools/pio_psram_libs.py`: the runtime must be the PSRAM-safe toolchain
  multilib. ESP32 rev 1 can lose PSRAM stores; allocations over 4 KB land there.
- `OTA_FW_VERSION` and `FW_BUILD_TAG` come from `tools/pio_version.py` and the newest `vX.Y.Z`
  tag. Fetch GitHub tags; never hand-edit these values. Use the manual Release workflow;
  see [RELEASING.md](../RELEASING.md).
- CI's `variants` job links the default image and logging-off, BLE-off, and network-off
  (`ENABLE_CELL 0`) variants using `tools/check_variants.sh`. WiFi-on/cell-off only compiles:
  it overflows IRAM and its TLS is unverified, so it remains bench-only.

## Hardware verification

1. Build and run the relevant [host checks](../tests/AGENTS.md).
2. Flash and reproduce on a dongle. A bench supply/fake car covers transport, not real
   OBD profiles, power gates or GPS.
3. Read flash logs: `DUMP 3` for the newest segments, `DUMP` for all, or ABD7 in the app.
   `logEvent` survives unplugging; `Serial.printf` does not.
4. Only then commit firmware changes, when asked. Report any drive/charge verification still
   needed; a linked binary or successful host test does not satisfy it.
