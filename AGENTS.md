# AGENTS.md

Guidance for AI coding agents working in this repo. `CLAUDE.md` is a symlink to this file.

This is the **short** entry point: how to build, and the handful of mechanical rules that will bite you
before you get near the domain. The domain itself lives in [`DONGLE_CONTEXT.md`](DONGLE_CONTEXT.md) —
over 4000 lines of it — which is deliberately *not* auto-loaded. Read §0 there first (thirty seconds), then
the section you need. There is a map at the bottom of this file.

## What this is

An ESP32 OBD logger that polls the car and uploads to ABRP over its **own LTE-M modem**, so a parked
car keeps reporting with no phone nearby. A phone is an occasional privileged client, not the data
path. Two consequences drive nearly every design decision in here: a client that can write to the
dongle can write to the **car**, and nobody is standing next to a car whose 12 V battery is being
flattened.

Hardware is a Freematics ONE+: our ESP32 alongside an **STM32 co-processor we cannot reflash** (closed
vendor blob; it owns CAN, K-line and pin-16 voltage, and we drive it with ELM327-style AT commands).

## Build

```sh
cd baseLogger && pio run -e esp32dev
```

`pio` may not be on `PATH` in a non-login shell — check before concluding the toolchain is missing.
If it is unavailable, this isolated invocation has been verified from `baseLogger/` and does not
install Python packages globally:

```sh
uv run --no-project --with platformio==6.1.18 pio run -e esp32dev
```

Flash and watch with `pio run -e esp32dev -t upload -t monitor` (115200 baud).

`OTA_FW_VERSION` and `FW_BUILD_TAG` are injected by `tools/pio_version.py` from the newest `vX.Y.Z` tag
(fetch tags from the GitHub remote), never edited by hand. Releases are cut by the manual *Release* workflow; see
`RELEASING.md`.

A clean build prints two pre-existing `"PRINT" redefined` warnings, from `FreematicsOBD.cpp` and
`mathparser/parser.cpp`. They are not yours; anything else probably is.

Every build checks its linker map and fails unless the C/C++ runtime came from the toolchain's PSRAM-safe
multilib (`tools/pio_psram_libs.py`, #64): this ESP32 rev 1 can lose stores to PSRAM, where mallocs over 4 KB
land.

**CI runs the host checks only.** `.github/workflows/host-tests.yml` runs every `tests/run_*.py` on each
pull request and on `main`, normally and with `CXX='clang++ -DNDEBUG'`. Apart from the one job below, nothing in CI builds
the firmware or touches a device, so a green PR proves the host checks and nothing more. Merging needs its `ci` job,
the one check the `main` ruleset requires: a new job goes in that job's `needs`. See *Testing* below,
because that gap is the single biggest difference from a normal repo.

The `variants` job compiles the non-default `config.h` variants with `sh tools/check_variants.sh` from `baseLogger/`;
it never builds the default image. `ENABLE_WIFI 1` (with `ENABLE_CELL 0`) only compiles, as it overflows IRAM, and its TLS is unverified: bench only.

## The rules that bite first

**`baseLogger/Helpers/*.h` are not translation units.** They are `#include`d into `logger.ino` to form
one sketch, and they share its globals and `config.h` flags. Every one says so in its header comment.
Do not add include guards that assume separate compilation, do not add `static` where a global is
expected, and do not expect a symbol to be visible before its `#include` line.

**Include order in `logger.ino` is load-bearing.** The list around line 320 is ordered by dependency,
not alphabetically — a helper that calls `logEvent` must come after `logging.h`, one that calls
`gzipBody` after `gzip_body.h`. Insert deliberately.

**`build_src_filter` excludes Freematics reference sources** (`stanleyhuangyc*`, `*telelogger*`,
`*libraries-FreematicsPlus*`). None are tracked, but a local copy dropped into `baseLogger/` to read
must never compile — the reference FreematicsPlus would collide with the real one in `libraries/`.

**Deep sleep is a reboot, not a resume.** RAM is wiped and `setup()` runs again from the top. Anything
that must survive is `RTC_DATA_ATTR` (wake/sleep counters, the charging budget, the taper latch, the
frozen parked fix) or LittleFS (the vehicle profile, the upload backlog). A plain global reset to its
initialiser on every wake is the most common way to get this wrong. `RTC_DATA_ATTR` survives deep sleep
only: a panic, WDT or software reset reloads it too (`DONGLE_CONTEXT.md` §11).

**BLE callbacks run on the Bluedroid task and must only copy bytes and set a flag.** All NVS, crypto
and OBD work happens on the loop task, in the matching `*Service()` function. This is stated as
load-bearing in `auth_gate.h` and holds equally for OTA, the log dump and the profile push.

**Tunables live in `config.h`**, each with the reasoning above it. Change the constant, and update the
comment when the reasoning changes — several of them encode a measured field result, not a preference.
Two are hard external limits rather than choices: the SIM7070 refuses a request body over
`SHCONF BODYLEN=1024`, and the key-reset window is timed from the dongle's own power-on.

**Comments here carry field history.** Many explain a failure that took a drive or a 71-hour log to
find. Do not delete one unless you are deleting the code it describes.

## Testing

Run `python3 tests/run_heading_checks.py` from the repo root for the heading/GPS regression checks
(Python standard library and `clang++`, or set `CXX`). The runner extracts the complete current
`gpsGetData()` method and the GNSS-unlatch block from `logger.ino` on every run, reads the production
movement-age limit, includes the real GPS types and location/telemetry/upload helpers, and compiles
the real TinyGPS implementation with ASan/UBSan. Serial and HTTP transports are fakes; co-processor
replies are synthetic and host `millis()` is controlled. Host date writes emulate the ESP32's 32-bit
`unsigned long`. Checks remain enabled regardless of `NDEBUG`.

Parser checks cover rejected/truncated replies preserving the accepted record, malformed and
32-bit-overflowing required integers, coordinate rejection/recovery and optional heading handling.
Selected GNSS integration checks follow the real sampling/frame/unlatch order: the first moving fix
emits live coordinates while still PARK, the next distinct moving fix unlatches DRIVE, replayed or
rejected replies do not create fresh movement, fresh zero-speed fixes still use the frozen parked
position, and `gps-relocate` moves that position only once `PARK_ACC_MIN` fresh stationary fixes average
beyond `PARK_RELOCATE_MIN_M`, and `gps-jump` covers the link-path jump filter (time-aware bound, re-anchor
after three consistent fresh fixes, replays and sparse fixes never re-anchor). Each group starts from a reset fixture. These are not full state-machine,
OBD-profile, power-gate or GNSS-hardware tests. The synthetic in-memory queue test verifies all 120 points arrive
once, in order, with battery data intact; it does not exercise LittleFS restoration or the cellular
modem.

Pass a group name (`link-range`, `link-failures`, `link-truncated`, `link-required`, `link-atomic`,
`link-valid`, `gps-replay`, `gps-rejected`, `gps-stationary`, `gps-relocate`, `gps-jump`, `direct-missing`,
`direct-range`, or `queue`) to run just that group.

Run `python3 tests/run_release_checks.py` for the release tooling: version naming and selection, build-tag
resolution, signing through a fake `aws` with throwaway keys, the workflow's safety properties, and the
linker-map check of `tools/psram_libs.py` (`PsramRuntimeLink`). The release-notes group needs `git-cliff` (or
set `GIT_CLIFF`); without it, it is skipped, and with `CI` set it fails.

Hardware verification still requires a real dongle, and the loop is:

1. Build.
2. Flash, and reproduce on hardware — a bench supply (and a fake car, an internal tool not in this repo)
   covers the transport, but never the OBD profile, the power gates or anything GPS.
3. Read the flash log (`DUMP 3` over serial for the newest three segments, `DUMP` for all of it, or the
   in-app log dump over ABD7). `logEvent` lines are the
   ground truth; `Serial.printf` is lost the moment the cable is out.
4. **Only then commit.** Firmware changes stay in the working tree until they have run on a device.

A change that only a drive can exercise — power gating, upload cadence, unit conversions on real PIDs —
is not verified by a bench test, and saying so is part of reporting it.

## Where to look

| Question | Section of `DONGLE_CONTEXT.md` |
| --- | --- |
| What is built, what is enabled | §1 |
| BLE service/characteristic layout (ABD0–ABD8) | §2 |
| Auth, pairing, what a client may do | §3, Appendix A |
| OTA firmware update | §4, Appendices B and C |
| Log retrieval | §5 |
| Sleep, wake gates, battery protection | §6 |
| Cloud upload, profiles, where they come from | §6a |
| Co-processor command limits, cars it can't read (BMW i3 …) | §6b |
| The OBD-alarm gate (per vehicle) | §7 |
| App-side rules, `abrpobd` vs `obdble` | §8 |
| Diagnosing a dongle that went quiet | §9 |
| Invariants to check before changing something | §11 |

In the source, the entry points are `logger.ino` (`setup()` is the wake ladder, `loop()` is the poll
and upload cycle), `Helpers/state_machine.h` (activity states and cadences), `Helpers/sleep_power.h`
(the one `enterDeepSleep` primitive every sleep path goes through) and `Helpers/auth_gate.h` (ABD8).

## Etiquette

- Do not commit, push or open PRs unless asked. Branch work off `main`.
- Commit messages generate the release notes: follow *Commits* in `CONTRIBUTING.md` (Conventional
  Commits, and which types reach the notes).
- User docs: `README.md` (what it is, what you need, supported cars), `docs/getting-started.md`
  (setup, step by step) and `docs/technical-overview.md` (code layout, settings, console). Keep the
  car table and the settings tables current when the code changes.
- Check `git remote -v` before reaching for a forge CLI: `gh` only works if the remote is GitHub.
- The app side lives in the ABRP app repository (closed source) and has its own `AGENTS.md`. Protocol changes
  touch both repos and land together; a firmware change that moves a wire format without the app is a broken pair.
  From outside, describe the app-side change a PR needs in its description.

## State-machine differential harness

Run `python3 tests/run_sm_checks.py` (groups `scenarios`, `random`, `enumerate`; `--depth N`, default 5) for
the park/drive decision. The differential covers the drive region; shared context comes from the working
tree. The blocks that write drive state (the poll-time unlatch and re-latch, the `is_parked` publish, the
OTA/dump hold, `changeState`, the buffer block and the pre-sleep marker) come from the pinned `main` commit
(`SM_PIN`, via `git show`) for OLD and from the working tree for CUR; the context around them (the wake
gate, the backstop) comes from the working tree for both, so a deliberate change there does not fail it. Each
side compiles into its own namespace around one harness (`tests/sm/`), and identical event sequences run
through both: every sequence of 5 events over a
15-symbol alphabet (about 760 k, a few seconds; `--depth 6` runs about 11 M in under a minute), 5000
fixed-seed sequences of 30, and named field cases (garage latch, smooth-road leash, re-latch boundary,
OTA and log-dump holds, a console session, both speed keys and a NaN speed, trip-end marker, the two
no-speed-PID drives). The fake clock advances inside every log line and freeze, as the device's does.
After every event the latch, leash, hold time, marker debt, published `is_parked`, buffered frames,
freezes, the GNSS age bound asked for, and the exact `logEvent` and Serial sequences must be equal; the
weak-rail gate and the max-awake backstop are checked as properties, on the working tree's code. Since step 1 (#24) the CUR side is the
drive region in `Helpers/drive_state.h`, and the runner fails if anything outside that header assigns
`is_currently_parked`, `g_last_speed_ms` or `park_marker_owed`, if `alarm_gate.h` names drive state, or if
a call into the drive region sits outside the blocks it extracts. The pin rule and what the harness
cannot prove (32-bit `millis()` wrap, other flag sets, code outside the extracted blocks) are in the
runner's header. Energy and wake surfaces come in step 0b (#28).

## Diagnostic host checks

Run `python3 tests/run_log_diagnostics_checks.py` for actual DataLogger, CSV, and GPS-selection checks.
The filesystem is in memory; tests cover schema rotation, failed opens, partial headers, serial dump,
BLE byte-stream reading, cached GPS fields, native millis wrap, and that BLE write logging (`ble-write`)
records keys, never values, and (`motion`) the per-minute `MOTION` line aggregation: percentages, peak/mean,
one line per minute, the partial-minute flush at sleep and uncounted OTA/log-dump windows. `fw-build` covers the `FW build=` line: top of every new segment (first boot, schema
rotation, size rotation inside `logEvent` with the segment size still exact, `CLEAR`), again on a
non-deep-sleep boot, never on a continued segment, and the `eacc642` loader not rotating on it. `--capture-helper <absolute-script-path>` (optional; the helper is an internal tool,
not in this repo) also verifies a synthetic dump through it, including actual historical
`eacc642` logger-begin and CSV-builder code simulating a firmware rollback; this does not open a device.
`CXX="clang++ -DNDEBUG"` keeps assertions checked.

Run `python3 tests/run_upload_checks.py` for real SIM7070 send/AT parsing and upload-helper checks with
ASan/UBSan and NDEBUG. UART is fake. Python hashlib/hmac/base64 independently verify the logged
fingerprint and every signature, so the host mbedTLS shims are never their own oracle.
Run `python3 tests/run_gzip_container_checks.py` for the actual gzip container wrapper, independent
Python decompression/CRC/ISIZE checks, and fallback paths. ROM deflate, ROM CRC, and PSRAM are shims;
these checks do not validate the real ROM output or modem transport.

GPS CSV `gps_read=ok` means parsing accepted a reply, including replies with zero coordinates.
`gps_fix` still means cached nonzero coordinates. Date/time and speed are the cached accepted record,
not fields from a rejected reply. `gps_field` is 0=date, 1=time, 2=latitude, 3=longitude,
4=altitude, 5=speed; -1 is not applicable. `gps_time_age_ms` measures elapsed time since
receiver date/time changed, not proven position-observation age; blank means unknown. Reply bytes
count only co-processor bytes, not direct UART bytes. `gps_source` records the GPS helper's actual
coordinate selection (gnss/frozen/omitted), not final wire eligibility. The freshness serializer can
subsequently omit selected coordinates when their observation time is unknown or invalid. Source
selection does not itself establish that a location was uploaded.
The log has no raw-fix columns: `lat`/`lon` (3 decimals) hold the position `gps_source` selected, which
is the receiver fix itself when `gnss`, and `PARKED_LOC` events log the frozen average. Both are written
before send-time freshness, so they include positions the uploader later omits; treat flash logs as
private. Segments from older firmware keep their `gps_lat`/`gps_lon` until rotated out. GPS columns are an optional
tail after the exact legacy CSV prefix. Rollback rows require the current capture helper, which reports
legacy-shaped rows and leaves absent diagnostics blank rather than guessing their origin or values.

Upload request IDs reset each wake. The 12-hex SHA256 is a truncated final-wire-payload fingerprint,
not an authentication or exactly-once guarantee. `src` counts source frames, `pts` serialized points,
and `sutc` spans source frame UTCs, including frames omitted by serialization. `loc`/`ldup` count locations sent
and omitted as already sent, appended only when both fit whole. `p/a/r` is the observed
prompt/body-ACK/POST result; `rx` is declared/SHREAD/captured response length. `w=?` explicitly means
UART-written length is unknown. `cut=-1` means response completeness is unknown, 0 measured complete,
and 1 measured incomplete. Raw server bodies are not recorded: only fixed categories and length/
truncation metadata. The body-ACK outcome remains diagnostic; POST acceptance and retries are unchanged.

## Combined freshness verification and rollout

The combined heading/GPS runner now has 32 groups; `upload-gate` covers which polls are buffered for upload. Run it normally and with `CXX='clang++ -DNDEBUG'`.
It covers accepted-record observation UTC, strict calendars, independent location/heading times,
frozen-average contributor times, and valid GPS clock seeding before a coordinate fix. The backlog
oracle compiles the historical `eacc642` loader/serializer/resave path; it verifies numeric battery
FIFO preservation and prevents old firmware from restamping protected geo. A successful historical
battery upload deliberately discards protected geo history. Queue power-loss atomicity is unchanged.

`__geo_` is reserved, scrubbed from profile results and public serializers, and stored as numeric
shadow metadata only in the private backlog. Legacy/invalid geo provenance drops geo, not otherwise
eligible battery/state. Actual RTC retention, cross-image resets, ROM gzip and modem behavior remain
hardware gates. Sender-side geo eligibility does not depend on any server-side change; the backend's existing
clock-skew normalization is unchanged.

## Send-time geo expiry verification

Upload groups `geo-expiry`, `geo-retry`, and `geo-sizing` exercise the real public upload path,
including in-memory LittleFS save/restore and failed-send resaves. The runner uses production
`config.h` constants, including the 1000-byte cap and `GEO_MAX_SEND_AGE_SECONDS` (29 * 24 * 60 * 60
seconds, inclusive). The one-day margin under the server's 30-day window mitigates clock/transport
error; it does not guarantee safety for a bad but apparently recently synchronized clock. Expiry,
clock-quality retry, and sizing checks also run with logging disabled. Compiling RED assertions
cover unknown clocks with valid fresh geo/SOC, unsynced retained RTC, the former 30-day boundary,
invalid explicit formatter time, and raw-RTC retry/sizing policy regressions.

`logNetwork(frames)` has no freshness-bypass parameter. Its internal `uploadSendUtc()` captures the
uint32 RTC once and validates that snapshot through the real `clockNeedsSync(now)` predicate. The
snapshot must be > 1e9 and at most `CLOCK_MAX_EPOCH`, the smaller of UINT32_MAX and the native
`time_t` positive maximum. `last_clock_sync_epoch` must also be valid, nonzero, not future, and at most
`CLOCK_RESYNC_SECONDS` (900) seconds old. Never-synced, stale, inconsistent, unrepresentable, and unknown
clocks omit geo, not eligible battery/driving/charging data. Snapshot-aware `uint64_t` overloads of
`clockValid`, `clockStale`, and `clockNeedsSync` validate raw uint32 epochs without signed narrowing;
the existing no-argument acquisition APIs remain. The retained `RTC_DATA_ATTR time_t` sync field and
its layout are unchanged.

Buffered uploads take one trust-aware UTC snapshot per attempted batch and use that exact eligible
policy for both sizing and final serialization, even if clock quality changes during compression.
Each retry and subsequent batch takes a new snapshot. The internal `logNetworkAt(frames, send_utc)`
is not a new public sender entry point. The shared `bulkStringV2(frames, send_utc = 0)` formatter
fails closed on missing, invalid, fractional, nonfinite, or out-of-native-calendar send times. Location
and heading observation UTCs must also fit the same native-calendar bound before their `time_t` casts.
An explicit valid formatter time represents caller-validated clock trust; offline mapping tests use
fixed explicit trusted times, never frame UTC as the actual send clock. Historical numeric backlog
provenance/calendar acceptance is not narrowed by this wire-only eligibility bound.

Eligible location and heading retain their independent original observation UTC. Filtering never
rewrites source frames or failed-send backlog provenance. Eligible battery/state retain original
frame UTC, with stale-battery handling unchanged. Geo-only omitted batches keep the successful
no-POST rule. Future-relative-to-corrected-send-clock normalization remains the backend's existing
policy. Both host runners extract the production clock-quality predicates instead of faking them;
UART, clocks, compression, and filesystems remain fakes. Real RTC retention, clock acquisition,
modem timing, ROM gzip and hardware remain unverified; no device was flashed by these checks.

## Send-once geo verification

A location or heading goes up once per new GPS observation; the design is in `DONGLE_CONTEXT.md` §6a.
Upload group `geo-dedup` covers repeats within a batch, accepted/failed/unanswered outcomes, new times or
coordinates, multi-batch partial failure, sizing against the mark, boot clearing and untrusted clocks; the
heading `upload-gate` group covers a geo-only poll buffered once while unaccepted, never again after, and
not again behind a restored backlog. RTC retention across deep sleep and what the backend shows between
sends need a parked night.

Run `python3 tests/run_clock_range_checks.py` normally and with `CXX='clang++ -DNDEBUG'` for extracted
production predicates, sender snapshots, and formatter checks under both host-native `time_t` and a
signed32 target alias. Add `--target-cxx /path/to/xtensa-esp32-elf-g++` to compile native type/bound and
unchanged RTC-sync ABI probes using the actual target compiler. Host calendar bridging emulates signed32
writes but is not ESP32 runtime verification. The current ESP32 toolchain has signed32 `time_t`:
INT32_MAX (2038-01-19T03:14:07Z) is inclusive; INT32_MAX+1 and UINT32_MAX are untrusted. Native time64
hosts retain the uint32 ceiling. This patch does not upgrade existing time-source/GPS conversion or
battery/calendar formatting for dates after 2038; that broader firmware limitation remains unsupported.

The `drift` group covers deep-sleep drift compensation by driving the real `clockCompensateSleep`, `setClock`
and NVS save/restore from `clock_sync.h` against a fake microsecond clock: a night of 30-minute wakes is
learned from its morning NITZ resync, the next night stays within seconds, GPS syncs never teach, one bad
source moves the rate only 1/`CLOCK_DRIFT_GAIN`, and a wrong stored rate converges back. The real RC rate,
RTC retention and NVS across a re-plug need a dongle and a parked night (look for `CLOCK drift` in the dump).

## Energy and temperature host checks

Run `python3 tests/run_energy_checks.py` for charge progress/corroboration, the `is_charging` verdict
and release latch (`signal`, `release`; replays field current sequences, no location data), the `setup()`
wake gate (taper and charge-end reads), the `SLEEP enter` reason labels (`sleep-reason`), the crash-loop
streak (`wdt-streak`: the real `wdt_streak.h` store across simulated resets, garbage and foreign stores,
the escalation's threshold and cadence, the OTA commit ending it, the retained "modem may be on" flag
every sleep acts on, and the weak-rail cold boot never reconnecting the modem), the ABD1 frame fitted to MTU−3 (`ble-frame`: compact 2 dp numbers, key filtering, priority trimming, MTU 23, and the `BLE tlm` log cadence), telemetry serialization, CSV columns, and schema-rotation checks under
ASan/UBSan. Add `--profiles /path/to/profiles` to exercise actual Scenic/iX3 production/developer
equations with the firmware parser (optional: the ABRP backend profiles are not public). The runner extracts the current charging and logging code;
filesystem and hardware are fakes, not on-device verification.

## OBD reply checks

Run `python3 tests/run_obd_reply_checks.py` for the real `genericProtocol()` on protocol-6 `22xxxx`
replies, extracted from `obdwrapper.cpp`: a clean reply decodes to fixed bytes, stray, blank and short lines
ahead of a single or first frame are dropped, a re-segmented reply under a foreign header and a wrong DID
are refused, replies with no anchor line and non-`22xxxx` requests are left to the plain parser, and
truncated replies decode their partial bytes. Fixtures are scrubbed co-processor replies; the co-processor itself is not exercised.

## Profile push checks

Run `python3 tests/run_profile_checks.py` for the BLE profile hand-off: the real config-apply block from `loop()`
and `loadPersistedProfile()` against an in-memory LittleFS. A full push writes `/vehicle_profile.json` even when
the profile is unchanged (a token-only re-push survives a reboot, with the re-init still skipped), and a failed
write, or a push that does not apply, invalidates the hash and removes the file, so the next boot cannot restore an
older token. The runner also checks that `otaFinalize` and `bleDeinitForSleep` drop a push still
pending, after the boot-partition commit and after `ble.deinit()`, by position only. The BLE task, NVS and the reset
itself are not exercised.

## Cellular TLS trust checks

Run `python3 tests/run_server_roots_checks.py` (add `--require-live` to fail rather than skip when
`api.iternio.com` is unreachable) and `python3 tests/run_upload_checks.py tls-ca`. Neither proves what
the modem accepts; that needs `TLSCHECK` on a dongle.

`python3 tests/run_upload_checks.py signing` covers the signed upload: the shared RFC 9421 vector byte for
byte, every signed POST recomputed by the runner from the captured wire bytes with Python's
hashlib/hmac (gzip on and off), no `X-TLM-TOKEN`, and the real `setHeaders()` AT output (quote escaping,
refused values, the header budget, abort on a refused `AT+SHAHEAD`) and the best-effort User-Agent.
