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
Flash and watch with `pio run -e esp32dev -t upload -t monitor` (115200 baud).

A clean build prints two pre-existing `"PRINT" redefined` warnings, from `FreematicsOBD.cpp` and
`mathparser/parser.cpp`. They are not yours; anything else probably is.

**Nothing here is unit tested and there is no CI.** A green build proves it compiles and nothing more.
See *Testing* below, because that gap is the single biggest difference from a normal repo.

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

There are no tests. Verification is a real dongle, and the loop is:

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
