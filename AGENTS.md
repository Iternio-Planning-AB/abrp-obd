# AGENTS.md

Guidance for AI coding agents working in this repo. `CLAUDE.md` is a symlink to this file.

This is the **short** entry point: how to build, and the handful of mechanical rules that will bite you
before you get near the domain. The domain itself lives in [`DONGLE_CONTEXT.md`](DONGLE_CONTEXT.md) —
4200 lines of it — which is deliberately *not* auto-loaded. Read §0 there first (thirty seconds), then
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

**There is no CI; host regressions cover GPS parsing, selected GNSS freshness/parking paths and
heading/telemetry handling.** A green build proves it compiles and nothing more. See *Testing* below,
because that gap is the single biggest difference from a normal repo.

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
watchdog streak, the frozen parked fix) or LittleFS (the vehicle profile, the upload backlog). A plain
global reset to its initialiser on every wake is the most common way to get this wrong.

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
rejected replies do not create fresh movement, and fresh zero-speed fixes still use the frozen parked
position. Each group starts from a reset fixture. These are not full state-machine, OBD-profile,
power-gate or GNSS-hardware tests. The synthetic in-memory queue test verifies all 120 points arrive
once, in order, with battery data intact; it does not exercise LittleFS restoration or the cellular
modem.

Pass a group name (`link-range`, `link-failures`, `link-truncated`, `link-required`, `link-atomic`,
`link-valid`, `gps-replay`, `gps-rejected`, `gps-stationary`, `direct-missing`, `direct-range`, or
`queue`) to run just that group.

Run `python3 tests/run_release_checks.py` for the release tooling: version naming and selection, build-tag
resolution, signing through a fake `aws` with throwaway keys, and the workflow's safety properties. The
release-notes group needs `git-cliff` (or set `GIT_CLIFF`); without it, it is skipped, and with `CI` set it fails.

Hardware verification still requires a real dongle, and the loop is:

1. Build.
2. Flash, and reproduce on hardware — a bench supply and the desktop fake car cover the transport, but
   never the OBD profile, the power gates or anything GPS.
3. Read the flash log (`DUMP` over serial, or the in-app log dump over ABD7). `logEvent` lines are the
   ground truth; `Serial.printf` is lost the moment the cable is out.
4. **Only then commit.** Firmware changes stay in the working tree until they have run on a device.

A change that only a drive can exercise — power gating, upload cadence, unit conversions on real PIDs —
is not verified by a bench test, and saying so is part of reporting it.

## Where to look

| Question | Section of `DONGLE_CONTEXT.md` |
| --- | --- |
| What is built, what is staged off | §1 |
| BLE service/characteristic layout (ABD0–ABD8) | §2 |
| Auth, pairing, what a client may do | §3, Appendix A |
| OTA firmware update | §4, Appendices B and C |
| Log retrieval | §5 |
| Sleep, wake gates, battery protection | §6 |
| Cloud upload, profiles, where they come from | §6a |
| Co-processor command limits, cars it can't read (BMW i3 …) | §6b |
| The OBD-alarm gate (built, bypassed) | §7 |
| App-side rules, `abrpobd` vs `obdble` | §8 |
| Diagnosing a dongle that went quiet | §9 |
| Invariants to check before changing something | §11 |

In the source, the entry points are `logger.ino` (`setup()` is the wake ladder, `loop()` is the poll
and upload cycle), `Helpers/state_machine.h` (activity states and cadences), `Helpers/sleep_power.h`
(the one `enterDeepSleep` primitive every sleep path goes through) and `Helpers/auth_gate.h` (ABD8).

## Etiquette

- Do not commit, push or open PRs unless asked. Branch work off `main`.
- Commit messages are used to generate release notes. Use Conventional Commits:
  `type(scope): imperative summary` (scope optional). Describe the user-visible change, not just
  the implementation; use `feat` for features, `fix` for bug fixes, and `docs`, `refactor`, `test`
  or `chore` as appropriate. Mark breaking changes with `!` and a `BREAKING CHANGE:` footer that
  explains the impact and migration.
- Check `git remote -v` before reaching for a forge CLI: `gh` only works if the remote is GitHub.
- The app side lives in the ABRP app repository and has its own `AGENTS.md`. Protocol changes touch both repos
  and land together; a firmware change that moves a wire format without the app is a broken pair.

## Diagnostic host checks

Run `python3 tests/run_log_diagnostics_checks.py` for actual DataLogger, CSV, and GPS-selection checks.
The filesystem is in memory; tests cover schema rotation, failed opens, partial headers, serial dump,
BLE byte-stream reading, cached GPS fields, and native millis wrap. `--capture-helper <absolute-script-path>`
also verifies a synthetic dump through the updated capture helper, including actual historical
`eacc642` logger-begin and CSV-builder code simulating a firmware rollback; this does not open a device.
`CXX="clang++ -DNDEBUG"` keeps assertions checked.

Run `python3 tests/run_upload_checks.py` for real SIM7070 send/AT parsing and upload-helper checks with
ASan/UBSan and NDEBUG. UART is fake. Python stdlib hashlib independently supplies SHA256 test results.
Run `python3 tests/run_gzip_container_checks.py` for the actual gzip container wrapper, independent
Python decompression/CRC/ISIZE checks, and fallback paths. ROM deflate, ROM CRC, and PSRAM are shims;
these checks do not validate the real ROM output or modem transport.

GPS CSV `gps_read=ok` means parsing accepted a reply, including replies with zero coordinates.
`gps_fix` still means cached nonzero coordinates. Date/time, speed, and raw coordinates are the cached
accepted record, not fields from a rejected reply. `gps_field` is 0=date, 1=time, 2=latitude,
3=longitude, 4=altitude, 5=speed; -1 is not applicable. `gps_time_age_ms` measures elapsed time since
receiver date/time changed, not proven position-observation age; blank means unknown. Reply bytes
count only co-processor bytes, not direct UART bytes. `gps_source` records the GPS helper's actual
coordinate selection (gnss/frozen/omitted), not final wire eligibility. The freshness serializer can
subsequently omit selected coordinates when their observation time is unknown or invalid. Source
selection does not itself establish that a location was uploaded.
Raw coordinates intentionally include positions suppressed from uploads and increase sensitive data
collection. Use this investigative build and private captures accordingly; the existing circular flash
budget bounds storage size, not retention duration or at-rest encryption. GPS columns are an optional
tail after the exact legacy CSV prefix. Rollback rows require the updated capture helper, which reports
legacy-shaped rows and leaves absent diagnostics blank rather than guessing their origin or values.

Upload request IDs reset each wake. The 12-hex SHA256 is a truncated final-wire-payload fingerprint,
not an authentication or exactly-once guarantee. `src` counts source frames, `pts` serialized points,
and `sutc` spans source frame UTCs, including frames omitted by serialization. `p/a/r` is the observed
prompt/body-ACK/POST result; `rx` is declared/SHREAD/captured response length. `w=?` explicitly means
UART-written length is unknown. `cut=-1` means response completeness is unknown, 0 measured complete,
and 1 measured incomplete. Raw server bodies are not recorded: only fixed categories and length/
truncation metadata. The body-ACK outcome remains diagnostic; POST acceptance and retries are unchanged.

## Combined freshness verification and rollout

The combined heading/GPS runner now has 29 groups. Run it normally and with `CXX='clang++ -DNDEBUG'`.
It covers accepted-record observation UTC, strict calendars, independent location/heading times,
frozen-average contributor times, and valid GPS clock seeding before a coordinate fix. The backlog
oracle compiles the historical `eacc642` loader/serializer/resave path; it verifies numeric battery
FIFO preservation and prevents old firmware from restamping protected geo. A successful historical
battery upload deliberately discards protected geo history. Queue power-loss atomicity is unchanged.

`__geo_` is reserved, scrubbed from profile results and public serializers, and stored as numeric
shadow metadata only in the private backlog. Legacy/invalid geo provenance drops geo, not otherwise
eligible battery/state. Actual RTC retention, cross-image resets, ROM gzip and modem behavior remain
hardware gates. Server freshness/SSE ordering protection remains recommended before a broad firmware
canary, but sender-only geo eligibility does not depend on an ingest deployment or a backend expired-geo
exception. Existing backend clock-skew normalization is unchanged. Keep this investigative build and
its raw-location logs private.

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

Run `python3 tests/run_clock_range_checks.py` normally and with `CXX='clang++ -DNDEBUG'` for extracted
production predicates, sender snapshots, and formatter checks under both host-native `time_t` and a
signed32 target alias. Add `--target-cxx /path/to/xtensa-esp32-elf-g++` to compile native type/bound and
unchanged RTC-sync ABI probes using the actual target compiler. Host calendar bridging emulates signed32
writes but is not ESP32 runtime verification. The current ESP32 toolchain has signed32 `time_t`:
INT32_MAX (2038-01-19T03:14:07Z) is inclusive; INT32_MAX+1 and UINT32_MAX are untrusted. Native time64
hosts retain the uint32 ceiling. This patch does not upgrade existing time-source/GPS conversion or
battery/calendar formatting for dates after 2038; that broader firmware limitation remains unsupported.
