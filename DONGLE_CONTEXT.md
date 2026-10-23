# DONGLE_CONTEXT — everything the next person needs to know

> ## ⚠️ READ THIS FIRST
>
> **This file was generated with Claude Code, deliberately dense.** It exists to carry *context*
> forward, not to be read cover to cover by a human. It will look overwhelming — that is the point:
> it holds the reasoning, the dead ends, the field measurements and the gotchas that would otherwise
> be lost when whoever wrote this code moves on.
>
> **How to use it:** open it in an AI-assisted editor (Claude Code, Cursor, …) alongside the repo and
> *ask questions*. "Why is X done this way?", "what breaks if I change Y?", "where do I start to add
> Z?" — the answers are in here. Reading it linearly is not the intended use.
>
> **Trust order:** the **code** is authoritative, then this file, then the older design docs. Where
> this file and the code disagree, the code won. Some sections describe things that were built, then
> disabled, then partially fixed — the status banners say which.
>
> **This file is self-contained.** Part I (below) is the distilled context; **Part II embeds the
> full source documents verbatim** — the BLE security plan, the OTA architecture/protocol/app spec,
> the power docs, and the alarm-gate architecture and field findings. Named references like
> `ALARM_GATE_FIELD_FINDINGS.md` point to the matching appendix in Part II. Nothing outside this
> repo is required.

**Last substantive update:** 2026-08-28.

---

## 0. Thirty-second orientation

The dongle is **not a phone accessory**. It polls the car over OBD and uploads to ABRP over its own
LTE-M modem, so a parked car keeps reporting with no phone nearby. When a phone *is* present and
authorised, BLE becomes the transport and cellular stands down.

Everything else follows from that: the phone is an **occasional privileged client**, not the data
path. That's why access control matters (a client that can write to the dongle can write to the car),
and why the power logic matters (nobody is around to notice a flattened 12 V battery).

**Hardware:** Freematics ONE+ — ESP32 (our firmware) + **STM32 co-processor** (closed vendor blob,
owns the vehicle interface: CAN, K-line, pin-16 voltage). We talk to the STM32 with ELM327-style AT
commands over a UART link. We cannot change the STM32 firmware — it was never published (verified:
absent from the Freematics GitHub org, no source or image anywhere in this repo, no flash path
without an ST-LINK and a real brick risk if readout protection is set). It implements only part of
the ELM327 command set, and that rules some cars out entirely (BMW i3/iX3 among them): see §6b.

**Two repos:**
- Firmware: `abrp-obd` (this one).
- App: the ABRP React Native app — `tlm_type: 'abrpobd'` identifies this dongle; the older generic
  dongle lane is `'obdble'`. **The two lanes must stay separate** — see §8.

---

## 1. Current state of every staged feature

| Feature | Built? | Enabled? | Where |
|---|---|---|---|
| Cellular telemetry | ✅ | ✅ | `network_upload.h` |
| BLE telemetry | ✅ | ✅ | `ble.h`, ABD1 |
| Flash CSV logging | ✅ | ✅ | `DataLogger`, `logging.h` |
| BLE log retrieval | ✅ | ✅ | `log_dump.h`, ABD7 |
| BLE OTA firmware update | ✅ | ✅ (signing ON) | `ota_update.h`, ABD5/ABD6 |
| Deep-sleep power management | ✅ | ✅ | `sleep_power.h`, `state_machine.h` |
| Link encryption / bonding | ✅ | ✅ `BLE_ENABLE_BONDING 1` | `blewrapper.cpp` |
| HMAC access handshake | ✅ | ✅ unconditional (no flag) | `auth_gate.h`, ABD8 |
| Signed vehicle profiles (JWS) | ✅ | ❌ `PROFILE_REQUIRE_SIGNATURE 0` | `jws_verify.h`, `profile_sig.h` |
| OBD-alarm gate | ✅ | ✅ per vehicle (app toggle → NVS flag) | `alarm_gate.h` |

**Why bonding and the handshake are now on:** the staging that kept them off existed to speak the
*older* app protocol, which had no ABD8 handshake and no JWS. That app no longer exists — the dongle
had no users outside the development fleet when it was retired — so the audit mode was removed outright rather than
flipped, and `AUTH_ENFORCE` is gone as a concept. An ABD8 proof is now the only session grant.

**Why `PROFILE_REQUIRE_SIGNATURE` is still 0:** different reason, and not back-compat. The gate has
not yet been watched succeed in the field, and a gate nobody has seen work should not be the only
way in. It flips once there is evidence, not once the old app is gone.

**The alarm gate is per vehicle:** the app's toggle writes the NVS flag, and there is no compile-time
bypass any more. It was bypassed while three enforcement-breaking bugs were open; two are fixed. What
remains affects only an opted-in car: see §7 and `ALARM_GATE_FIELD_FINDINGS.md` (Appendix J).

---

## 2. The BLE protocol (GATT service ABD0)

**FROZEN TABLE.** The service is created with a fixed handle budget. Characteristics are assigned
handles in creation order, so **appending at the end** keeps ABD1–ABD8 at their existing handles.
ABD9/ABDA exist as reserved placeholders for exactly that reason. iOS caches the GATT table and the
Arduino BLE library implements neither Service Changed nor Database Hash, so a table that shifts
handles silently breaks already-paired iPhones.

| UUID | Purpose |
|---|---|
| `ABD0` | The service |
| `ABD1` | Telemetry (notify + read) — the live frame |
| `ABD2` | Setup/config: token handshake, profile push, `obd_alarm` flag |
| `ABD3` | Developer |
| `ABD4` | Error |
| `ABD5` | OTA control + the readable info JSON (installed fw, state, `alarm`) |
| `ABD6` | OTA data (write-only firmware payload) |
| `ABD7` | Log dump |
| `ABD8` | Auth handshake (nonce + HMAC) |
| `ABD9`, `ABDA` | **Reserved — do not repurpose casually** (see FROZEN TABLE) |

### Hard-won BLE facts

- **MTU 247 is a ceiling, not a preference.** It's the largest value where a notification still fits
  one radio packet (251-byte DLE). At 517 the small frames arrive and every large chunk is
  *deterministically lost*. Measured, after being wrong about it.
- **A notify carries at most MTU−3 bytes and is never fragmented.** Chunk sizes are derived from the
  negotiated MTU for this reason.
- **Android stays at MTU 23** until the app calls `requestMTU`. iOS negotiates ~185 itself.
- **ABD1's notify is fitted to MTU−3; its readable value is the full frame.** Numbers are `%.2f` with
  trailing zeros dropped (same value as the 2 dp contract, shorter text). If the frame is still too
  long, the notify drops fields by `bleTlmRank` (SoC, power/current/voltage, speed and the three flags
  last to go; `kwh_charged` and the battery temperature bounds first), so it always parses. The
  follow-up read is a long read, so an app that reads after the notify (idle Android at MTU 23) still
  gets every field. A real driving frame (~220 B at `%.2f`) is ~200 B compact: it fits 247, and at
  iOS's 185 the notify loses the temperature bounds. The notify goes straight to
  `esp_ble_gatts_send_indicate`; the value is set once per poll and never swapped for the notify, since
  the library's blob-read handler copies it without a lock. Keys outside `[A-Za-z0-9_]` are left out.
- **The GATT attribute budget fails silently when exceeded.** The default is 15 handles; the table
  needed 18. The stack reported the overflow only in an event the Arduino core ignores, so two
  attributes were dropped with no error — which killed OTA and took days to find. The budget is now
  32 with the arithmetic written down at the `createService` call.
- **Callbacks copy bytes; the loop task does the work.** Flash, crypto and OBD work must never run on
  the Bluedroid callback task. Violating this caused reboots during profile pushes. Every subsystem
  (OTA, log dump, auth, profile signature, alarm flag) follows the same pattern: the callback
  memcpy's into a buffer and sets a flag; a `…Service()` function on the loop task does the rest.
- **Clearing a bond on one side only looks like a hardware fault.** The phone keeps its half of the
  key, offers it, and the link drops before pairing runs — a connect flap with no error anywhere.
  `BONDCLEAR` prints the "forget the device on the phone too" reminder for this reason.

---

## 3. The security stack (pairing and handshake on; signed profiles not yet required)

Three layers, in order. Each answers a different question and none substitutes for another. The order
is enforced — you cannot reach layer 2 without clearing layer 1.

### Layer 1 — Pair: can a bystander read the traffic?
Every value **and** every CCCD in the service requires an encrypted link (`*_ENCRYPTED`
permissions). An unpaired phone can connect and read/write **nothing**.

- LE Secure Connections + bonding, **Just Works** (`ESP_IO_CAP_NONE`).
- Critically: `setInitEncryptionKey`/`setRespEncryptionKey` with `ESP_BLE_ENC_KEY_MASK |
  ESP_BLE_ID_KEY_MASK`. **The IRK matters** — without distributing it, bonds grow once per RPA
  rotation and pairing eventually fails with `SEC failed`.
- Never `_ENC_MITM`: Just Works cannot satisfy it.
- **Pairing must be central-driven.** A peripheral-initiated security request makes Android ≥12 show
  *two* system dialogs for one procedure. Gating an attribute instead returns ATT `0x05` and the
  phone pairs and retries by itself — one dialog. **Do not reintroduce a peripheral-side push to
  "help" a phone pair; gate an attribute instead.**
- Every `*_ENCRYPTED` assignment is inside `if (bonding_enabled)`, so `BLE_ENABLE_BONDING 0` cleanly
  removes the gating rather than leaving orphaned permissions.

### Layer 2 — Prove: is this client permitted?
A challenge–response on ABD8 grants the session. Without it: no telemetry, no OTA, no log dump, no
profile write.

- Dongle sends a 16-byte `esp_random` nonce (30 s TTL); the app answers
  `HMAC-SHA256(secret, nonce)`, compared in constant time.
- 32-byte secret, **TOFU**: provisioned on first use, stored on the dongle, held per-dongle in the
  app's keyring keyed by the **dongle's own eFuse-MAC id** (not the BLE device id, which differs per
  phone and could never be shared).
- Sharing between phones is by **QR code** — MITM-free without any key exchange or backend.
- `AUTHCLEAR` is the **revocation** mechanism: wipe the secret, reconnect with the phone you want to
  keep, and every other holder is locked out. (It must always mint fresh — re-pushing known secrets
  destroys revocation.)
- **Greet on the CCCD rising edge.** The dongle speaks first, when the app's subscription is armed,
  which removes the subscribe-vs-write race entirely — no retries, no timeouts.
- **Two flags, deliberately:** `setup_unlocked` (the ABD2 state-machine latch) vs `authenticated`
  (the session grant). Conflating them made enforcement impossible.
- Why it exists: the old handshake sent the vehicle's **cloud push token in plaintext on every
  connect**. Sniff it once and you could push fabricated telemetry for that vehicle forever.

### Layer 3 — Sign: is this payload legitimate?
A vehicle profile is only applied if it carries a valid backend RS256 JWS signature.

- The backend already returns the profile as a JWS; the app used to verify it and throw the signature
  away. It now forwards the JWS **verbatim** in ~180-byte chunks; the dongle verifies RSA-2048
  itself, **before parsing**, in ~54 ms.
- **Verify before parse, on the verbatim bytes.** The app coerces numbers after verifying and the
  firmware used to re-serialise the pushed key/value pairs — neither form is what was signed. A
  signature over any "canonical" re-serialisation would break on number formatting alone.
- `alg` is pinned to RS256 (`strstr(header, "\"RS256\"")`) — accepting whatever `alg` claims is the
  classic JWS confusion bug.
- base64url handling: translate `-`/`_`, re-pad (mbedTLS won't accept unpadded URL-safe).
- Why it exists: a profile contains `init_commands` — **raw AT including `ATSH`**, i.e. arbitrary CAN
  headers. Anything that could write a profile could inject arbitrary CAN frames using the dongle as
  transport. This is the oldest hole in the protocol, older than every other feature.
- **`JWSTEST`** re-verifies the last real profile intact → tampered → intact. A happy-path push looks
  identical whether the signature is checked or blindly accepted, so that log proves nothing; the
  *middle* result is what matters.

### The unfiltered channel worth remembering
Profile `init_commands` reach the co-processor **verbatim and unfiltered**. Any command the closed
STM32 blob implements is invokable at runtime from a profile — which is exactly what the JWS gate
stands in front of.

---

## 4. OTA firmware update (ABD5/ABD6)

- 4096-byte sectors, stop-and-wait ACKs, CRC16-CCITT frames.
- START payload carries size + version + **raw** SHA-256 (32 bytes) + ECDSA-P256 signature (64
  bytes) — hex-decode the manifest values, don't send hex strings.
- **Subscribe to ABD5 notifications *before* writing START**, or you get no application-level
  response at all.
- Two frame types on ABD5: the sector ACK **and** a status frame (START accept/reject, verify/commit
  progress). Early docs only described the first.
- Trial boot + rollback; a 90 s resume window if the link drops mid-transfer.
- `OTA_FW_VERSION` is injected from the release version at build time (`X.Y.Z` → `major*1000000 +
  minor*1000 + patch`), and `ota_sign.py` refuses an image not built as the version it signs. Three
  stale-binary releases happened back when it was hand-bumped; the boot trip-wire stays as a backstop.
  See `RELEASING.md`.
- `OTARESET` clears the persisted anti-downgrade floor after a bench flash to a lower version.

### Distribution — GitHub Releases

Signed releases are GitHub Releases of this repo, cut by the manual *Release* workflow, which signs with
an AWS KMS key (`RELEASING.md`). OTA reads `firmware.bin` and `manifest.json`
(`{version, size, sha256, signature, sig_alg: "ecdsa-p256-sha256", firmware, version_name}`); the app
re-reads the manifest on every check, downloads the `.bin` to a phone-side in-memory cache, then
streams it over BLE.

- **The dongle's trust is the ECDSA signature, not where the image came from.** Only `ota_pubkey.h`
  (the public half) lives in firmware.
- **Signing keypair:** `tools/ota_keygen.py` writes the header (`--from-kms` for the release key, or a
  local keypair for a fork), `tools/ota_sign.py` signs a build. If the key is ever rotated, firmware and
  signer change together and the new firmware must reach devices *before* anything signed with the new
  key.

---

## 5. Log retrieval (ABD7)

- App-driven pull; the dongle never streams unprompted.
- `{"LOGDUMP":"START"}` → size/chunk/window header; `{"LOGDUMP":<offset>}` → a window of binary
  chunks then `{"eow","eof"}`; `{"LOGDUMP":"END"}`.
- Frame: `[0xD7][u32 offset LE][payload]` — the magic byte disambiguates chunks from the JSON
  replies, and each chunk carries its own offset so a dropped notification just means re-request from
  the last contiguous offset. **Reads are idempotent**, so a re-request can never corrupt anything.
- One `readAt()` per ~8 KB window, sliced from RAM. Paying that per chunk throttled the whole
  transfer to 1–2 KB/s (the "minutes for 10%" bug).
- **Measured throughput ≈ 9.6 KB/s** (2.7 MB in 278 s). The limit is the radio granting roughly one
  notification per connection event (~20–24 ms/chunk), *not* the inter-chunk pacing — 4 ms and 1 ms
  measured identical.
- Known cosmetic issue: Bluedroid returns `ESP_FAIL` when its TX queue is full; the retry loop
  handles it, but the Arduino library `log_e`s **before** reporting it, so a healthy dump prints
  hundreds of scary red lines. Analysis and the fix (bypass the wrapper, count the retries) are in
  `DEFERRED_POWER_WORK.md` (Part II) §8 — deliberately not applied.
- **Which firmware wrote the log:** `EVT,…,FW build=<FW_BUILD_TAG> v=<OTA_FW_VERSION> elf=<16 hex>
  part=<app0|app1>`. `elf` is the app ELF SHA-256 prefix esp_coredump checks (match it to
  `sha256sum firmware.elf`); `part` tells an OTA'd slot from a USB flash. It is written right after the
  header of every new segment (including after `CLEAR`), so even `DUMP 1` names the build, and after
  `WAKE` on every boot that is not a deep-sleep wake. A segment-top line written by `begin()` comes
  before `WAKE`, so its envelope reads `INIT,0.00,0.00` and, on a cold boot, utc 0: those are not
  measurements. A non-deep-sleep `WAKE` with no `FW` line straight after it means older firmware is
  running (a rollback or an old serial flash) — the newest `FW` line above it is then stale.

---

## 6. Power management (the top constraint)

**The governing insight: drain is set by how often you poke the car, not by how long the ESP32 is
awake.** Polling a sleeping EV wakes its ECUs, which then stay awake on the *car's* timer.

- **The ATRV/rail gate runs first, before anything is powered.** Reading OBD pin-16 voltage is a
  *local* co-processor measurement (`ATRV`) — no bus traffic — so the dongle can tell whether the car
  is awake without poking it. Car asleep + nothing due ⇒ straight back to deep sleep with no modem,
  no GPS, no BLE.
- **Two independent rail sense paths:** `R29/R31 → ESP32 IO36` (ADC1_CH0, readable without waking the
  co-processor) and `R35/R37 → STM32 PB0` (surfaced as `ATRV`). ATRV is unreliable (diverges up to
  3.6 V); **IO36 is the trustworthy one**.
- **Weak-battery lockout band** (`IO36_LOCKOUT_LOW/HIGH` = 8.0/11.0 V): suppress *all* polling so a
  recoverable 12 V isn't pushed over the edge. Below 8 V = bench/no-car (allowed, so USB DUMP works).
- `RAIL_AWAKE_VOLTS 13.3` is **calibrated to one car with almost no margin.** Measured over 1016 field
  samples: moving 13.25–15.2 V (mode 13.4), switched-off 12.5–13.2 V. The bands nearly touch, so
  there's no room for a lower release threshold and **no hysteresis is possible**. Measure any new
  car before trusting anything that keys on it.
- **Wake sources:** wake-on-motion (ICM-42627 interrupt, threshold ~0.158 g) and a timer
  (`WAKE_PARKED_MINUTES 30` / `WAKE_CHARGING_MINUTES 3`).
- **Following a charge on a car that answers intermittently** (issue #7). Every extra poll is bounded, and
  a low-rail read needs proven charging. Three rules cover it:
  - A latched charge that goes silent at a rail ≥ `RAIL_AWAKE_VOLTS` never releases. After
    `CHARGE_SILENT_HOLD_POLLS` it waits at the parked cadence with the latch up, and the rail drop
    still gets its one charge-end read. The hold, a release at a sample poke, the gate's give-up and a
    skip wake all keep the charge's fast-cadence budget, cap and corroboration until the last charge
    evidence is `CHARGE_STALE_RESET_MINUTES` (6 h) old. So a stuck current that answers between
    silences, or at every 12 V top-up, still relaxes. A drive (a streaming wake) always resets.
  - When a charge latch clears on a stationary wake, one follow-up is armed. The first silent
    rail-high poke after it comes back in 3 min instead of 30. It is spent there, and cleared by
    motion (a power-on zeroes it with the rest, below).
  - The post-drive window (`POST_DRIVE_WINDOW_MINUTES 15`, at most `POST_DRIVE_MAX_POLLS 5`) is
    opened at the awake-ladder end only, with a fresh rail read that is high, and never over a
    latched or capped charge. A NET-recovery reboot opens it too, keeping its 1-min timer. A silent rail-high poke
    inside it comes back in 3 min, so a DC charge started right after parking is not missed for a
    parked cadence. It is timed on the RTC microsecond clock from the last motion. OBD answers play
    no part, because a drive may have none.
  - The follow-up and the window only shorten the cadence after a rail-high poll that happens anyway.
    None of the three apply to a car with the alarm flag set (§7).
  - The follow-up, window and last-evidence RTC state is zeroed on any boot that is not a deep-sleep
    wake.
- **BLE must come up early** — right after the gate, before the ~12 s cellular init — so the dongle
  advertises within ~2 s of wake. Behind cellular it took ~14 s and blew the app's 5 s reconnect
  window, churning every post-sleep reconnect.
- `bleRelaying()` = connected **AND** authenticated. **This, not `bleConnected()`, is what the
  cellular stand-down keys on.** Gating on mere presence meant a phone that never authorised silently
  starved cellular — data flowed *nowhere* live while frames piled up. Found in the field.
- Deep sleep tears BLE fully down (`BLEDevice::deinit`) — a left-initialised controller keeps
  deep-sleep current high and holds tens of KB of heap.
- Substantial deferred power work (STM32 reset-hold, modem power-down hardening, gate restructure)
  is scoped and costed in `DEFERRED_POWER_WORK.md` (Part II) — **read it before touching sleep**.

---

## 6a. The cloud path, the data model, and where profiles come from

**Cellular upload** (`network_upload.h`, `NetworkWrapper`): APN `hologram`, host `api.iternio.com`.
Buffered frames → `POST /2/app/tlm`, the v2 endpoint, as a **gzipped** typed `InputPointList` built by
`bulkStringV2` (`provider: ABRP_OTA`, every value its own object with an RFC-3339 time and an SI unit).
`ABRP_OTA` marks the dongle's own cellular uploads (the phone posts relayed BLE frames as `APP_OBD`); it
is self-declared, not proof of origin. A backend whose provider enum lacks it answers every cellular
POST with a 400.
Auth is a request signature keyed by the vehicle's telemetry token (which is what routes the data; see
*Signed uploads* below), plus `X-API-KEY` (`API_KEY`, which attributes the integration). The token
itself is not sent. HTTP 200 *is* the acceptance — v2 has no `status` field in the body.

Compression is the reason for v2: the SIM7070 refuses a request body over `SHCONF BODYLEN=1024`, so
body bytes are what limit points per POST — measured, 1000 bytes holds 5 points raw and ~21 gzipped.
Deflate is the ESP32 boot ROM's miniz, driven from `gzip_body.h` with the ~300 KB compressor in PSRAM.

Time sync has **three** sources, not four: GNSS (`CLK_GPS`, wins), the modem's `AT+CCLK` (`CLK_NITZ`),
`AT+CNTP` (`CLK_NTP`), plus the `Date` header on each telemetry POST (`CLK_HTTP`) as a coarse anchor.
There is no `GET /2/utc` call — `CellWrapper::getEpoch()` exists but nothing invokes it, and `UTC_KEY`
/ `UTC_PATH` are gone.

There is **no built-in token and no factory profile.** Both arrive together in the ABD2 push and
persist together in `/vehicle_profile.json`. Until then `g_no_profile` stops the poll and the empty
token stops the upload, so an unpaired dongle advertises on BLE and does nothing else. (`v1
/1/tlm/send` and `/1/tlm/bulk` are no longer used; `bulkString` and `CELL_BULK_PATH` survive only as
reference.)

**Telemetry keys** the frame can carry: `soc, soh, voltage, current, power, is_charging, is_parked,
speed, kwh_charged, soe, ext_temp, batt_temp, batt_temp_min, batt_temp_max, lat, lon, heading, utc` (exact
set is per-vehicle-profile). `is_parked` and `is_charging` are derived on-device; `lat/lon/heading`
come from GNSS. Note `bleTlmString()` **trims `lat`/`lon`/`utc`/`heading` from the BLE frame** — the
phone supplies its own position — so a BLE frame is smaller than the cellular body, and a frame that is
*only* position (no PID answered) serialises to `{"tlm":{"is_parked":…,"is_charging":…}}`.

**A location or heading goes up once per GPS observation** (#14). `bulkStringV2` omits a `location` equal to
the last one sent — same `lat`, `lon` and observation time, the whole tuple, since the single-fix fallback
can move coordinates without moving their time — and a `heading` equal in degrees and observation time.
"Last sent" is first the accepted mark, then each earlier point of the same body, so frames must stay in FIFO
order. The marks (`g_geo_accepted`, `RTC_DATA_ATTR` in `network_upload.h`) move only on an HTTP 200, never on
a sizing pass, `ok_empty`, a body that was not sent or an HTTP error. A deep-sleep wake keeps them;
every other boot (power-on, OTA, rollback, watchdog, panic) clears them, costing at most one resend and never
depending on another image's RTC layout. The poll gate asks the same question against the marks as of the
newest buffered frame (`g_geo_queued`; after a backlog restore it is recomputed over the restored frames at the first poll with a
trusted clock, since the restore itself runs before NITZ/NTP), so a geo-only
poll is buffered once while its fix is unsent, not once per poll, and not at all once accepted. Frames and the backlog are untouched, so rolling back to
older firmware just resends as before. There is deliberately no periodic refresh: a parked car whose spot
the backend has aged out stays off the map until it relocates, drives or reboots. A 200 whose location the
backend silently dropped counts as sent until then too. `UPLOAD body` logs `loc=` (locations in the body) and
`ldup=` (suppressed repeats).

**Where the PID list / vehicle profile comes from:** the app fetches it from the ABRP backend by
`typecode` (`getObdBlePids`), gets back `{ obdPids, jws }`, and pushes it to the dongle over ABD2.
The `obdPids` is the per-model map of PIDs → CAN commands + equations that `OBDWrapper` evaluates; the
`jws` is the same profile signed (used only when `PROFILE_REQUIRE_SIGNATURE` is on). So a new vehicle
is supported by adding its profile **on the backend**, not by reflashing — the dongle is generic and
carries no profile of its own at all. It used to: a dozen `init*()` PID blobs in `obdwrapper.cpp`, one
of which ran as a pre-pairing stand-in. Removed, because a profile for the wrong car does not merely
return nothing — `init_commands` is arbitrary CAN, so it puts wrong frames on a stranger's bus.
The one thing a profile cannot add is a new telemetry key: BLE forwards every key, but the cellular
upload drops one until `bulkStringV2` has a line mapping it to a v2 field.

**TLS trust on the cellular path** (`HTTPClientSIM::configureTls`/`ensureCa`/`probeTls`,
`Helpers/server_roots.h`, generated by `tools/server_roots.py`, checked by
`tests/run_server_roots_checks.py`). Uploads go only over a chain that verifies against the bundle;
`AT+SHSSL=1,""` (no verification) is never sent, and no bundle or a failed install means no TLS at
all. Cellular only: the bench WiFi transport still uses `setInsecure()`.

*The roots, and why each.* Roots only, because ACM renews the leaf yearly and moves between
intermediates (Amazon RSA 2048 M01..M04); none of that needs firmware.
- Amazon Root CA 1, 3, 4: the roots of the API's ACM certificate (RSA and ECDSA chains).
- Starfield Services Root G2: **load-bearing today.** The server sends Root CA 1 cross-signed by it,
  and the modem needs the issuer of the *last* certificate sent: Root CA 1 alone is refused (measured).
- GTS Root R4 and GlobalSign Root CA: a hedge for moving the API behind Cloudflare, whose chain today
  is WE1 ← GTS Root R4 cross-signed by GlobalSign Root CA - so GlobalSign is the one needed now
  (measured: without it www.cloudflare.com is refused), and GTS R4 once the cross-sign is dropped.
  GlobalSign expires 2028-01-28 and is exempt from the three-year expiry check; let it lapse.
- Amazon Root CA 2 (RSA 4096) is left out for size. Let's Encrypt (ISRG Root X1) was left out by choice.

*Changing CA vendor:* ship firmware trusting the new root first, then switch the server. Run `TLSCHECK`
on a bench dongle and `run_server_roots_checks.py --require-live` after shipping and again right after
the switch. A dongle that missed the firmware stops uploading (frames kept) until updated over BLE, which
does not need cellular; the first failed open of each boot logs a trace starting `untrusted`.

*Measured on `1951B08SIM7070`:*
- `CONVERT` accepts a DER PKCS#7 of up to 5120 bytes and refuses 5121 or more (two root sets either
  side). Multi-certificate PEM converts too, but costs ~40 % more bytes. The bundle is 4255 bytes.
- The certificate's name is not checked by the SH or the CA stack (an IP URL and a mismatching SNI
  both connect), and the AT interface has no option to enable it. So anyone able to redirect DNS or
  traffic - a fake 2G base station included, since `CNMP=51` allows GSM - can present a free ACM
  certificate for their own domain and read the uploads (positions, battery). Signing keeps the token
  itself off the wire (below), so such a listener cannot post as the car. `ignorertctime` stays 1, because the modem
  judges dates by its own RTC, which is not guaranteed set before the first upload; expired leaves are
  therefore accepted.
- `AT+SHSSL` accepts a CA name the store does not hold; `SHCONN` then fails (it does not connect
  unverified). Deleting the `/customer/` source after `CONVERT` does not affect the converted entry,
  and nothing lists or deletes converted entries.
- `+CAOPEN` results with TLS on: 27 for a chain the roots refuse, 1 for a CA name not in the store,
  0 when it verifies.

*Install.* The bundle is written as `/customer/rc<FNV-1a>.p7b`, converted, then `<name>.ok` is written -
only after `CONVERT` succeeds, so a power cut mid-install retries. A present marker means no write, so
the modem's flash is written once per bundle. Write and filesystem steps are retried up to 3 times per
boot (a missed prompt is waited out and purged); a `CONVERT` refusal (`ERROR`) latches for the boot, and
after two (an RTC count for that bundle) it is retried only every 48 boots. A `CONVERT` that times out
(`FAIL:convert_to`) is a write-step failure, not a refusal. An unanswered `CFSGFIS` is not "absent".

*Failures.* `SHCONN` only says `ERROR`, so on a refused open `probeTls()` asks the CA stack: plain TCP,
then TLS on the same roots. The open trace's verdict becomes `untrusted` (27), `noca` (1) or `tlsfail`
(no answer); the first trust verdict then stands for the boot's later refused opens, while a network
failure or `tlsfail` is probed again on the next one. After `noca` (or an `SHSSL` error) with a present marker, the marker is
deleted once per boot so the next open reinstalls - the recovery for a modem whose certificate store was
reset. `untrusted` keeps it: the store has the roots and the server does not chain to them.
Trust failures (no or refused bundle, `untrusted`, `noca`) do not count toward the stale-bearer restart
and reboot ladder; transient install steps and `tlsfail` do.

**Signed uploads** (`Helpers/tlm_signature.h`, used by `logNetworkAt`). The token signs each upload and
is never sent: one fixed RFC 9421 / RFC 9530 profile, defined in that file's header comment and pinned
byte for byte by the shared vector in `tests/upload_helpers_checks.cpp` (the backend hard-codes the same
vector). The signature base, and why only the body is covered, are in that header comment.

With no token nothing is sent (`UPLOAD not sent: no_token`). The modem sends at most
`SIM7070_HEADER_BUDGET` bytes of headers (the measurement is in its `FreematicsNetwork.h` comment). `setHeaders()`
sends only the caller's headers, checks every name (RFC 7230 token) and value (printable ASCII, no
backslash) and the budget before sending anything, and fails the request (`UPLOAD not sent: hdr_fail`)
on any refusal rather than POST it unsigned. `User-Agent: abrp-obd/<x.y.z>` (from `OTA_FW_VERSION`) is
best-effort: added after the mandatory headers only if it still fits the budget, never a reason not to
send. Quotes go out as `\"` inside the quoted AT value (measured
to arrive as `"`; `\22` arrives literally). A batch not sent keeps its frames in the backlog, which is
bounded by `MAX_UPLOAD_BUFFER` (the oldest go first).

| Who | Sees | Can do |
|---|---|---|
| Passive listener on the cellular path | TLS only | nothing |
| Active MITM with any certificate under the bundled roots (name and dates are not checked) | uploads: positions, battery, the api key, keyid, signatures | read; replay an exact earlier request, at any later time (no `created`); swallow uploads by answering 200, so the frames are dropped; set the clock through unauthenticated NTP when a resync runs and GPS is stale (pre-existing; the HTTP `Date` header is not read on cellular), and if that resync ends an hour or more of sleep it also teaches a skewed drift rate (bounded by `CLOCK_DRIFT_MAX_PPM`, kept in NVS across a re-plug, halved toward the truth by each honest window after); not forge a new request or learn the token |
| Anyone with the token | — | post as the car. The token reaches the dongle over BLE at pairing (ABD2 profile push; LESC Just Works: encrypted, not MITM-authenticated) and is held by the phone app; cellular signing does not change either |

This assumes a high-entropy token (a UUIDv4, ~122 random bits): the keyid is an unsalted SHA-256 prefix
of it, which only an unguessable token keeps from being reversed. The bench WiFi lane (off in this build)
would sign the same way, over a TLS session that does not verify the server (`setInsecure()`); before
it is turned back on it needs `https.end()` (or its headers cleared) per POST, because `addHeader()`
accumulates.

---

## 6b. What the co-processor can do on the bus, and the cars it rules out

The STM32 reports `OBD2USART V1.4` and answers `ATZ` with `ELM327 v1.5`, but it is a partial
emulation. Bench results (2026-09-24, each command sent raw to the co-processor):

| Reply | Commands |
|---|---|
| `OK` | `ATZ ATE0 ATS0 ATH1 ATSP6 ATSP7 ATM1 ATM0`; `ATSH ATCF ATCM` once a protocol is set |
| `?` | `ATCEA` (extended addressing), `ATFCSH ATFCSD ATFCSM` (flow control), `ATCAF0/1` (raw mode), `ATPB`, `ATCRA` (the firmware rewrites it to `ATCF`/`ATCM`), plus harmless defaults (`ATD ATAL ATBI ATAT ATST`) |
| `ERROR` | `ATSPB` (user-defined protocol) |

- **Every request gets the co-processor's own length byte.** There is no raw mode, so a profile
  cannot control the first data byte of a frame. That rules out extended addressing and hand-built
  frames.
- **`NO DATA` proves nothing.** It also comes back with no bus attached at all. Only a reply shows the
  dongle is on the bus.
- **Monitor mode works.** Lines arrive as `$MON,<4 ID bytes, big-endian>,<DLC>,<data...>` (verified on
  an i3 by filtering on the ID). A request sent while in `ATM1` is processed and monitoring continues.
- **A reply is not always the ECU's frames verbatim.** Seen on an Ioniq 5 (issue #10): a stray
  headerless tail ahead of a clean 7EC reply, or the whole reply re-segmented behind 21 stale bytes
  under a 5-digit "header". For protocol 6 and `22xxxx`, `genericProtocol` drops whole lines ahead of
  the first single/first frame answering `62<DID>` and logs `OBD<pid> reason=skipped_prefix lines=<n>`
  (no bytes; its own 3-per-power-on cap, 15 min apart). With no such line the re-segmented kind is refused
  as `wrong_did`. Headers-off (`ATH0`) and 29-bit headers on protocol 6 are not parsed. Host checks: `tests/run_obd_reply_checks.py`.

**Backend profiles this hardware cannot run** (checked against all 50 ABRP backend profiles, which are not public, as of 2026-09-24):

| Profile | Why |
|---|---|
| `i3`, `ix3`, `ix60`, `i3_test` | extended addressing (`ATCEA07`), protocol B (`ATSPB`), flow control carrying the address byte |
| `eGolf`, `MEB_new` | hand-built raw frames, which need `ATCAF0`. `MEB`/`MEB_audi` read the same values without it |
| Mazda MX-30 (uncertain) | flow control goes out on `72B`, not the request ID; depends on which ID the co-processor picks |

21 profiles set `ATFCSH`, which the co-processor ignores. They work only if its default flow-control
ID matches; the Scenic (29-bit) has field evidence, the 11-bit profiles do not. A check against this
list before pushing a profile would tell an owner up front instead of leaving them with silence.

**The BMW i3 in detail** (car tests 2026-10-01, listen-only build `pio run -e esp32dev_i3listen` on
branch `i3-testing`, `Helpers/can_listen.h`):
- **The OBD port carries one broadcast frame: `0x130`**, BMW terminal status, 10 Hz, DLC 5. With the
  filter wide open nothing else arrives on 11-bit or 29-bit at 500 kbps. The gateway forwards nothing
  else unrequested, and the i3's Ethernet pins (3/8/11/12/13) are not wired on this board (Appendix I
  §3). Passive SoC is ruled out.
- **`0x130` data byte 0 is the ignition state:** `00` off, `01` accessory (100 ms on the way through,
  once 7 s), `05` ignition on. Locking and unlocking change nothing, and the frame keeps coming with
  the car off. It matches a published F30 sniff, so it likely holds for other BMWs.
- **Active reads got no reply.** A 7-byte payload `03 22 <DID> 00 00 00` should go out as
  `07 03 22 <DID> ...`, a correctly addressed request to the SME (ext address 07), because the
  co-processor's length byte becomes `07`. `DD68`, `DDBC` and `DDB4` got no reply, in normal mode or
  sent from monitor mode with every ID visible. Standard `7DF 0100` got none either. We have never
  seen a reply to any transmission from this dongle on an i3, so a wrong frame and no frame at all
  look the same from here.
- **Next:** an OBD Y-splitter with a second dongle in `ATM1` shows the exact bytes on the wire. Until
  then an i3 on this dongle gets location and ignition state, no battery data.

---

## 7. The OBD-alarm gate (enforced per vehicle — read before changing)

**The problem:** some cars (BMW i3 confirmed, with a positive control) sound their anti-theft alarm
when a device **transmits** on the OBD bus while the car is locked and armed. Passive presence is
fine. So silence-while-armed is a complete solution — no CAN isolation, no BimmerCode.

**What was built:** a per-vehicle flag (app toggle → ABD2 `obd_alarm` → dongle NVS → published in the
ABD5 info JSON for read-back), plus a permit predicate gating all bus traffic on a flagged car.

**History:** it was bypassed with a compile-time switch (`OBD_ALARM_ENFORCE`, now gone) after three
field-breaking bugs, all documented with evidence in `ALARM_GATE_FIELD_FINDINGS.md` (Part II). Bugs 1
and 3 are fixed (`config.h` above `ALARM_SETUP_PERMIT_MINUTES`); bug 2 and the per-vehicle rail margin
remain, and affect only a flagged car:

1. **`clean()` doesn't reset `setup_unlocked`** → a re-AUTH on a live connection is misparsed as a
   profile value and refused `UNSIGNED` → the push never completes → no setup permit → the wizard's
   PID test is denied → **and the flag becomes unrecoverable from the app** (Save needs the PID test).
   It was independent of the gate and predated it. **Fixed.**
2. **Suppression sits upstream of BLE bring-up** → every timer wake on a stationary flagged car is
   radio-silent, so the dongle becomes invisible until physical motion. A *polling* decision
   accidentally became a *connectability* decision.
3. **`gpsBegin()` is a one-shot that powers GNSS back down on failure**, while `g_gps_powered` is set
   `true` regardless → no fix all wake → permit structurally unreachable → a whole blind drive.
   **Fixed.**

**Design lessons worth keeping even if the feature never returns:**
- The failure asymmetry: a false permit (transmit while armed) is unrecoverable; a false deny is
  invisible and recoverable. Buy false denies cheaply.
- **No single signal is sufficient.** A car on a flatbed has ignition off, alarm armed, and GNSS
  reporting 80 km/h. "Rail energised" alone polls a parked charging car. Only the conjunction holds.
- **The permit must be instantaneous — no latch, no grace.** An early version held the permit while
  the rail stayed energised, which an EV keeps up while charging: park, plug in, lock, and the latch
  never releases. Any grace period for "stopped at a light" is the same grace period for "parked and
  locking up", and nothing worth measuring happens while stationary anyway.
- **The accelerometer is inadmissible** as a hold signal: getting out, closing the door and locking
  all produce motion, so it would extend the permit through exactly the arming sequence.
- **Asymmetric debounce:** slow to permit, instant to deny.
- **Gate the requests, not the link** — an initialised-but-idle OBD link transmits nothing.
- **`ALARM_GNSS_MAX_AGE_MS` is currently inert**: `gd->ts` is stamped at *call* time, not fix time,
  on the co-processor GNSS path, so a re-echoed stale fix stays "fresh" forever. Fails toward
  permitting. Fix requires keying on the `$GNIFO` date/time fields changing.

**A passive ignition signal on BMWs (2026-10-01):** `0x130` data byte 0 reads `05` with the ignition
on, and reading it transmits nothing (§6b). That is positive evidence of "in use", which the gate
lacks. It says nothing about locked/armed, and the frame keeps running with the car off, so only the
value counts, never the frame's presence.

---

## 8. App side — the rules that matter

The app is the ABRP React Native app (Redux Toolkit, redux-persist + MMKV, `react-native-ble-plx`) —
a **separate, closed-source repository**; app paths in this document refer to it. This dongle's code lives under `src/state/ble/abrpObd/`,
`src/state/firmwareUpdate/`, and the `ObdBleSetup` screen; the OBD-BLE test screens are behind
developer options. Build/run is standard for that repo (`yarn`, its own README) — not covered here.
Note the protocol level of the app you test against matters: the firmware requires the ABD8
handshake unconditionally and bonding is on (§1), so an app build without the handshake cannot get a
session.

- **`tlm_type: 'abrpobd'`** = this dongle. **`'obdble'`** = the older generic dongle lane.
  **Everything added for this dongle must be an add-on that leaves the obdble lane byte-identical.**
  This was verified repeatedly and is the single most important app-side rule.
- **The vehicle↔dongle binding is server-side** (stored on the ABRP account). Consequence: a second phone signed into the same account syncs
  the binding and can connect **without ever opening the setup wizard**. Any per-vehicle setting that
  rides the account is therefore applied by phones that were never asked.
- **The dongle secret is device-local by design** (no backend). That was a deliberate decision — the
  backend-auth phases were struck. Device policy should stay consistent with it.
- **Keep dispatch counts low.** Telemetry frames are buffered and drained into a reducer rather than
  dispatched per frame; every dispatch runs the whole middleware chain.
- **i18n everything** user-facing; developer-only screens use plain strings with `disableTranslation`.
- **Timeouts are not house style.** Prefer making the protocol deterministic (e.g. the dongle greeting
  on the CCCD rising edge) over adding a timeout or a retry, both of which add failure modes.
- **`awaitingPairing`** means a *confirmed decline*, detected by the sustained telemetry subscription.
  Never latch it from a one-shot operation: the first gated access is what **triggers** pairing, so it
  races the procedure it started. Mapping that to "user declined" made every first-ever connect fail
  and succeed only on retry.
- **Redux-vs-radio desync is a recurring bug class.** Several paths null `state.ble.deviceId` without
  calling `cancelDeviceConnection`; the next connect then decides from Redux that it should connect,
  against a device the radio still holds → `DeviceAlreadyConnected` until the OS drops the link.
  Any path that drops the device from app state must also cancel the radio connection.

---

## 9. Diagnostics: how to actually find things

- **`DUMP` over serial** is the primary tool, and the BLE log dump exists so you can get it without
  unplugging the dongle from the car. A full dump takes about 4 minutes; `DUMP n` streams only the
  newest `n` segments (about 25 s each), and `DUMP 3` usually covers the last drive. It is genuinely "the tool that made every other bug in this
  project findable".
- **Clock lines.** The board has no 32 kHz crystal, so deep sleep keeps time on the internal RC, which runs
  ~0.6 % fast. Each wake takes that back at a learned rate. `CLOCK drift <raw> raw, <residual> residual over
  <s> asleep -> rate <ppm>` is a NITZ/NTP resync teaching the rate. `NET clock resynced ... (residual)` is
  what compensation left over. `NET clock resync FAILED` means the wake runs on the compensated RTC. After each
  deep-sleep `WAKE`, `CLOCK wake slept <s>, adjusted <ms> at <ppm>` says what that wake corrected.
- **`MOTION act=<%> n=<windows> s=<span> peak=<g> mean=<g> axmax=<g> obd parked chg`** is one line per awake
  minute (and the partial minute before `SLEEP enter`; one in progress when an OTA reboots is lost). `act` is the
  share of observed time whose `detectMovement` window saw motion: each window stands for the time
  since the previous one, capped at 2 s, because a moving window ends on its first sample; `axmax` is the largest single-axis value, which is what
  that 0.1 g threshold compares; `peak` is the largest bias-removed |acc| and `mean` the mean of per-window peaks.
  The bias is a short running average, so these show vibration, not sustained acceleration. `s` is the span the
  windows cover: a blocking upload between windows stretches a "minute". Rail and time are on the EVT prefix.
  Diagnostic only, gathered to judge accelerometer drive detection for cars with no OBD speed PID; nothing decides
  on it, and OTA/log-dump windows (which bypass `detectMovement`) are not counted.
- **`SLEEP enter ... reason=<r>`** names why the dongle slept. `dc-charge`, `charging` and `parked` are the
  cadence tiers, read back from the armed timer (1, 3, 30 min). `net-recover` is the modem-recovery
  reboot (1 min), `wdt-streak` the WDT-streak escalation (below), `backstop` the parked MAX_AWAKE ceiling. Charge
  cadence decisions the timer can't tell apart name themselves: `parked-held`, `parked-cap`,
  `parked-uncorroborated`, `pdw` and `followup` (below). An explicit sleep reason wins over a cadence
  label, which wins over the timer.
- **`<reset> reset recovered (streak=<n> retained=<0|1> owed=<0|1>)`** follows the `WAKE` of a panic, WDT
  or brownout reset (`<reset>` is the `WAKE` line's `reset=`). `streak` counts consecutive ones, brownouts
  included (#50: a battery that sags out on every modem start loops like a crash); a deep-sleep wake or any
  other reset ends it. `retained=0` means the count did not survive the reset and restarted at 1; on a
  `BROWNOUT` line it answers whether RTC memory survives a brownout on this board. `owed=1`: this boot owes
  the trip-end `is_parked:1` marker, restored from the same store (#49). At `streak=2` the boot logs
  `WDT streak=2 -> skip init, sleep <m>min` and sleeps with `reason=wdt-streak`: 30 min, or 3 min when the
  rail says the car is awake. A crash boot zeroes the clock, so these lines and the sleep after them usually
  carry no UTC; read them in order against the next `WAKE`. Nothing is reachable over BLE during that
  sleep; re-plugging the dongle is a power-on, which never escalates. The same store remembers whether
  the modem may still be powered (set by every modem init, cleared only by an acknowledged `AT+CPOWD`),
  and a sleep that did not power it this wake sends it an `AT` while that is set: an answer gets `AT+CPOWD`
  (so `SLEEP enter -> modem+GPS off` can follow a skip wake after a crash), silence counts as off.
- **Log the *reason*, not just the outcome.** A subsystem that goes quiet with an empty log is the
  shape that gets misattributed to the cellular stack for weeks — this has happened more than once
  in this project (the transport-handoff bug; a stored hash claiming a profile that wasn't running).
  Prefer one line per wake with the inputs *and* the verdict over transition-only logging.
- **A happy path proves nothing about a gate.** Both the signature check and the HMAC gate were only
  believed after watching them *refuse*, which is why `JWSTEST` shipped as a permanent diagnostic.
- **`BLE tlm mtu=<m> bytes=<full> sent=<n> fit|trimmed drop=<k>`** is the ABD1 frame against the
  negotiated MTU: the first frame of a wake, then again only when `drop` exceeds every line so far or the
  MTU changes. `sent` is the fitted notify size handed to the stack, not a confirmed delivery. `trimmed`
  means the notify left out `drop` fields (the readable value kept them); at `mtu=23` that is expected
  for an idle Android connection, and `sent=10` is the bare `{"tlm":{}}` sent as the cue to read.
- **`UPLOAD not sent: <reason>`** (`no_token`, `sign_fail`, `hdr_fail`) is a batch that never left the
  dongle. It keeps its frames and deliberately does not advance the http=0 streak that drives the
  stale-bearer restart and reboot: the bearer is not what failed, and restarting the modem cannot fix
  a missing token or an oversized header set. So a run of these never ends in `NET recover`. A session
  that refuses `HEADERLEN` (open trace `hdrlen`) does count toward it; that ladder is bounded by
  `net_reboot_count` (at most two reboots per outage).
- **Charge cadence lines.**
  - The `SLEEP enter` cadence labels: `parked-held` is latched and silent at a high rail, `parked-cap`
    has spent `WAKE_CHARGING_MAX_MINUTES`, `pdw` is the post-drive window, and `followup` the one
    follow-up after a charge.
  - `logEvent` keeps 119 characters of a message, so new fields go on short lines; the energy runner
    checks `SAMPLE_POLL` and `ATRVGATE` at worst-case values.
  - `SAMPLE_POLL pids is_charging prog rel taper stall rail=<before>-><after> hold sil fu pdw`:
    - `hold=1` is a latched silent poll at a high rail;
    - `sil` is the silent-hold count;
    - `fu=1` means a follow-up is armed;
    - `pdw` is the minutes left in the post-drive window.
    The running poll tallies are gone: count the lines, and `pids=0` for the unanswered ones.
  - `ATRVGATE atrv rail <verdict> <action> since_poll skips polls rc <ENFORCE|observe>`. `rc` is the
    charge latch after the gate. `rail_awake` and `weak_rail` follow from `rail`.
  - The events are `FOLLOWUP armed|spent`, `FOLLOWUP cleared reason=motion`,
    `POSTDRIVE window opened|closed wake=<wake reason>` at the ladder end, and
    `POSTDRIVE window closed reason=answered|expired|spent` from a poll.
- **`tx_count`** on `OBDWrapper` counts co-processor commands — a **superset** of CAN frames, since
  most AT commands only configure the ELM locally. A nonzero delta is therefore not proof of bus
  traffic, but the counter must not advance while a gate says it's denying.

---

## 10. Where to look for what

Everything below except `README.md` and `docs/` is embedded verbatim in **Part II** of this file.

| Question | File |
|---|---|
| Setup, supported cars | `README.md`, `docs/getting-started.md` (in repo) |
| Flags, file map, state machine | `docs/technical-overview.md` (in repo) |
| BLE security design + phase history | Appendix A |
| OTA wire protocol (authoritative) | Appendices C, B |
| OTA app-side spec | Appendix D |
| Signed firmware binaries + manifest | Private release repository (token entered in-app) |
| Power measurements + deferred work | Appendices H, G, F |
| OBD-alarm architecture + decisions | Appendix I |
| Why the alarm gate is off + fix plan | Appendix J |
| Backend expectations | Appendix E |
| What the co-processor can't do, cars it rules out (BMW i3 …) | §6b |

---

## 11. If you change one thing, know this

1. **Release through the workflow** (`RELEASING.md`); `OTA_FW_VERSION` comes from the tag, not an edit.
2. **Don't reorder the GATT table** — append only (FROZEN TABLE).
3. **Don't do flash/crypto/OBD work on a BLE callback** — stash and let the loop task handle it.
4. **Don't add a peripheral-initiated pairing request** — gate an attribute instead.
5. **Measure `RAIL_AWAKE_VOLTS` per car** before trusting any rail-based decision.
6. **Keep the obdble lane untouched** — everything for this dongle is an add-on.
7. **Read `DEFERRED_POWER_WORK.md` before touching sleep** — several obvious "improvements" there are
   documented as net-negative or brick-risk.
8. **Keep `tools/pio_psram_libs.py` in `extra_scripts`** — mallocs over 4 KB land in PSRAM, and on this
   rev 1 chip only the `esp32-psram` runtime writes them safely (#64). A PSRAM test needs buffers over 4 KB.
9. **`RTC_DATA_ATTR` survives deep sleep only.** The bootloader reloads it on every other reset, panic,
   WDT and software reset included (#30: 20 panics in the field, every one `boot=1 sleeps=0`). State that
   must survive a crash goes in `RTC_NOINIT_ATTR`, which is garbage after a power-on and may hold another
   image's bytes after a rollback, so it needs a check that rejects both (`wdt_streak.h`: a magic and an
   inverted copy). That store holds the crash-loop streak, the "modem may be on" flag and a copy of the
   park-marker debt that is cleared only once the marker is sent, since the backlog is deleted when it loads.


---
---

# PART II — FULL SOURCE DOCUMENTS (verbatim)

> Everything below is embedded **unmodified** from the original single-topic documents, so no detail
> was lost to summarisation. They were written at different times — where one contradicts Part I or
> the code, **the code wins, then Part I** (which is newer). Internal cross-references between these
> documents (e.g. "see DEFERRED_POWER_WORK.md §1c") resolve to the matching appendix here.
> App-side paths (`src/…`) refer to the closed-source ABRP app repository, not to this one.

| Appendix | Document | Original filename |
|---|---|---|
| A | BLE security plan (design + phase history) | `BLE_SECURITY_PLAN.md` |
| B | OTA update architecture (v1) | `ota-update-architecture-v1.md` |
| C | OTA wire-protocol reference (authoritative) | `ota-protocol-reference.md` |
| D | OTA app-side spec | `OTA_UPDATE_APP_SPEC.md` |
| E | Backend expectations | `backend_implements.md` |
| F | Power management | `POWER_MANAGEMENT.md` |
| G | Power budget (bench measurements) | `POWER_BUDGET.md` |
| H | Deferred power work (read before touching sleep) | `DEFERRED_POWER_WORK.md` |
| I | OBD-alarm / ignition-sense architecture (final) | `IGNITION_SENSE_ARCHITECTURE.md` |
| J | Alarm-gate field findings & fix plan | `ALARM_GATE_FIELD_FINDINGS.md` |



---

# Appendix A — BLE security plan (design + phase history)

> Original file: `BLE_SECURITY_PLAN.md`

# ABRP OBD dongle — BLE security & access plan

Design plan spanning the app and the firmware (`baseLogger`). Nothing here is
implemented yet except where marked. Written 2026-08-18.

Companion docs: `ota-protocol-reference.md` (ABD5/ABD6 contract), `ABRPOBD_REIMPLEMENTATION.md` (telemetry
lane), `baseLogger/DEFERRED_POWER_WORK.md` (power work).

---

## 0. What we are protecting, and from whom

| Channel | Today's control | Worst case if abused |
|---|---|---|
| ABD1 telemetry (notify) | token handshake | read live SoC/voltage/speed. **No location** — `bleTlmString` omits lat/lon/utc by design |
| ABD2 setup / profile push | token handshake | **arbitrary CAN injection** into the vehicle (`ATSH` + arbitrary hex) |
| ABD5/ABD6 OTA | none beyond auth flag | **replace firmware** on a device wired to the CAN bus |
| ABD7 log dump | auth flag | exfiltrate the CSV log: **GPS traces**, timestamps, vehicle history |
| — | | sniff the handshake → obtain `tlm_push_token` = the vehicle's **cloud upload credential** |

Three distinct problems, needing three different tools:

1. **Is this payload legitimate?** → digital signature (Phases B).
2. **Is this user permitted?** → challenge-response against the backend (Phases C, D).
3. **Can a bystander read the traffic?** → link-layer encryption (Phase E).

None substitutes for another. A bonded encrypted link still lets a compromised app push a malicious profile;
a signature does not hide anything; an ownership check does not stop eavesdropping.

**Not solvable by anything in this plan:** a rooted phone running a legitimate, authorized, bonded session can
do everything a legitimate user can. That is the boundary of the design.

---

## 1. Note on firmware-image confidentiality

Releases are public; the firmware image is not secret, and the BLE link only protects it in flight.
For protecting what is *on the device* (someone with physical access dumping flash), the answer is
**ESP32 flash encryption + secure boot**, which is a separate, larger topic and not part of this plan.

---

## 2. Phase A — developer gating (app only, no firmware) — READY

Purpose: the Update-firmware and Dongle-logs screens are currently visible to every user. They are internal
tools. This is **product gating, not security** — the characteristics still exist and still accept writes.

The app already has the scaffolding:
- `hasDeveloperRoleSelector` — reads `state.session.user?.roles` for `'devel'`, **server-driven**, a normal
  user cannot flip it.
- `isDeveloperUserSelector` — that role, minus a local kill-switch (`state.device.isDeveloperDisabled`); it
  already gates the "Developer options" row in Settings.
- `state.device` is **persisted** and already holds this class of local preference (`quickBarEnabled`).

| Change | File |
|---|---|
| `obdDongleToolsEnabled?: boolean` + setter (mirror `quickBarEnabled`) | `src/state/device/deviceSlice.ts` |
| Toggle row, default off | `src/domain/DeveloperOptions/index.tsx` (existing `DeveloperOptionRow`) |
| Gate the two `ListItem`s | `src/components/vehicleSettings/ConnectLiveData.tsx` |
| Early guard (deep-link / stale nav protection) | `flashFirmwareThunk`, `fetchDongleLogsThunk` |
| New keys | `translations/en.json` |

Gate expression: `isDeveloperUserSelector(state) && state.device.obdDongleToolsEnabled === true`.

Deliberately leaving `obd_pid_editor` ungated — it shipped before this work, and changing its visibility is a
behaviour change nobody asked for. Trivial to add to the same gate later.

---

## 3. Phase B — signed vehicle profiles (firmware + app, NO backend work)

> **STATUS — IMPLEMENTED end to end (2026-08-25), shipping as an ACCEPT-BOTH add-on. Untested on
> hardware.** The dongle takes a signed profile and still accepts the historical per-key packages, so
> an app that knows nothing about JWS behaves byte-for-byte as before.
>
> | Piece | Where |
> | --- | --- |
> | RS256 verify (RSA-2048, mbedTLS), base64url, `alg` pinned | `Helpers/jws_verify.h` |
> | Chunk buffer, verify-then-apply on the LOOP task | `Helpers/profile_sig.h` |
> | ABD2 `{"JWS":{"i","n","d"}}` branch + `notifySetup` | `blewrapper.cpp/.h` |
> | JWS kept instead of discarded | `getObdBlePids.ts` |
> | `obdPidsJws` map (separate from `obdPids`) | `obdbleSlice.ts` |
> | Chunked push, `JWS OK`/`JWS BAD` handling | `abrpObd/setupThunk.ts` |
>
> **Wire protocol:** after `CLEAN OK` the app writes `{"JWS":{"i":<idx>,"n":<total>,"d":"<180-char
> slice>"}}` in order (each ACKed), the dongle assembles, then verifies on the loop task and answers
> `JWS OK` / `JWS BAD`. Only on `JWS OK` does the app send `tlm_push_token` + `utc`, which complete the
> push exactly as before. `keys` is unchanged, so the dongle's `keys == config` completion check still
> balances: the signed payload supplies the PID values, the two packages supply the rest.
>
> **Isolation, per the add-on rule.** ABD2 is abrpobd's own characteristic — the generic obdble lane
> (FFF0/18F0/FFE0) cannot reach any of it, and `setupThunk` already early-rejects `!isAbrpObd`. The one
> genuinely shared thing is `obdPids`, which `bleWriteThunk` reads through `obdVehicleJsonSelector` for
> the obdble lane — so the JWS is stored in its OWN map and `obdPids` keeps its exact shape and bytes.
> That also preserves the CRC version tag (Trap 1): folding the JWS into `obdPids` would have changed
> the tag's input and dropped every reconnect onto the slow full-repush + `ATZ` path. Verified: zero
> diff in `bleWriteThunk.ts` and `src/state/dataSource/`.
>
> **On `JWS BAD` the app does NOT fall back to an unsigned push** — that would hand the dongle the very
> profile it just refused to trust, defeating the phase. It rejects and says so in the event log.
>
> **HARDWARE-VALIDATED (2026-08-25), happy path.** A real signed profile went end to end:
> `HASH NOT OK` → `clean` → 10 ordered chunks (each ACKed) → `PROFILE JWS OK (1660 B, 9 keys, verify
> 54ms)` → `SETUP OK (push complete)` → `CONFIG applied from BLE push (941 B, on loop task)` →
> `PROFILE persisted to flash`. RSA-2048 verification costs **~54 ms**, once per profile push, on the
> loop task where it belongs.
>
> **Trap 1 confirmed intact on device:** later boots reported `HASH OK` → `SETUP OK (fast path, hash
> matched)`, so the version tag still matches and the JWS did not change what it is computed over.
> Note the JWS rides the SLOW path only — on `HASH OK` the app sends just `utc`, so a normal reconnect
> transfers no chunks at all and costs nothing.
>
> **Latent sharp edge, pre-existing, worth knowing:** `clean()` does `preferences.clear()` on the
> `"BLE"` namespace, which deletes the persisted hash, and restores only `auth_token`. So a push that
> is cleaned and then ABORTS leaves the dongle with no stored hash and it will fully repush on the
> next connect. That is arguably correct (the config really was cleared, so claiming a hash would be a
> lie) but it means an interrupted setup always costs one extra full push + `ATZ`.
>
> **NEGATIVE TEST PASSED (2026-08-25) — the security property is proven, not just the plumbing.**
> Two independent tests, because a happy-path push looks identical whether the signature is enforced
> or blindly accepted:
>
> 1. **Crypto discriminates** — `JWSTEST` (serial, physical access only) re-verifies the last real
>    signed profile intact / one-character-altered / intact again:
>    `intact=ACCEPT tampered=reject restored=ACCEPT -> PASS`. The restore proves it is not a stuck
>    `false`. The alteration maps between two valid base64url characters and skips any `.`, so the
>    rejection is a genuine signature mismatch rather than malformed input.
> 2. **The verdict is honoured end to end** — the app pushed a JWS with its last 6 characters
>    overwritten (inside the signature segment, so same length and alphabet). Result:
>    `PROFILE JWS REJECTED (bad signature, 1660 B)`, and **zero** occurrences of `PROFILE JWS OK`,
>    `SETUP OK (push complete)` or `CONFIG applied from BLE push`. That last absence is the real
>    assertion — the untrusted profile never reached the apply path or the OBD init sequence.
>
> **Failure mode is fail-closed WITHOUT going dark**, verified in the same capture: after the
> rejection the dongle kept polling its previous PIDs. `clean()` clears the wrapper's staging config
> (`configJson`/`config_keys`/the NVS hash), not the profile already applied to the OBD layer — so a
> refused profile leaves the dongle running the last one it *did* trust, rather than running nothing.
>
> `JWSTEST` was kept as a permanent diagnostic rather than scaffolding: it is read-only, physical
> access only, and is exactly what confirms a dongle still trusts the right key after a key rotation,
> without pushing anything.
>
> **SIGNATURES NOW REQUIRED (`PROFILE_REQUIRE_SIGNATURE 1`).** A raw profile value is refused with
> `UNSIGNED`; only a signature-verified JWS can change what this dongle runs on the CAN bus. The
> refusal is scoped to profile CONTENT - `keys`, `HASH`, `tlm_push_token` and `utc` still pass, since
> they are handshake data and the push could not complete without them. The app treats `UNSIGNED` as a
> clear failure rather than waiting out a timeout.
>
> (The flag had been described in a comment before it existed - the same doc/implementation mismatch
> made once with `AUTH_ENFORCE`. Both are now real.)
>
> **One lockout scenario, by design:** the app falls back to a package push when it holds no JWS for the
> typecode (unsigned fetch, or a profile cached before `obdPidsJws` existed). That push is now refused -
> but the poller refetches on the next app start, because the store transform clears `obdPidsRefreshed`.
> A stale cache costs one app restart, not a bricked dongle. Reverting is one line in `config.h`.
>
> ---
> *Step-1 detail, retained:* the firmware verifier
> (`Helpers/jws_verify.h`). It embeds the SAME RSA-2048 public key the app already verifies against
> (`getObdBlePids.ts`), does base64url (URL alphabet, unpadded — mbedTLS needs it translated and
> re-padded), rejects any header not declaring `RS256`, and verifies RSASSA-PKCS1-v1_5/SHA-256 over
> the verbatim `<header>.<payload>` ASCII. It hands back the payload's LOCATION rather than its
> contents, so a caller cannot accidentally parse before the signature is proved.
>
> **The `alg` check is not decoration.** `alg` is attacker-controlled until the signature verifies, so
> trusting whatever it names is the classic JWS confusion bug (`alg:none`, or an HMAC alg verified
> against the public key as though it were a shared secret). It is pinned to RS256.
>
> **No serial escape hatch here, unlike the auth gate, and the dependency graph is the reason:** this
> gate does not stand in front of OTA (`otaService` checks `get_authenticated`, never the profile). A
> broken verify — rotated key, backend format change — still leaves the dongle reachable and a fix
> deliverable over the air, so it needs no physical override. (Contrast the auth gate, which *does*
> gate OTA — and where `AUTHCLEAR` already serves as that override, which is why the separate
> `AUTHOFF` idea was dropped as redundant.)
>
> **Remaining, in order:**
> 1. App: stop discarding the JWS in `getObdBlePids.ts` `transformResponse` (it verifies at line 71,
>    then splits the payload off and drops the signature) — carry it through obdble state to setup.
> 2. Transport: a chunked verbatim push on ABD2 (~1.8 KB for a typical profile: the persisted profile
>    is ~1 KB, base64url ≈1.4 KB, plus a 342-char signature — so ~10-20 writes at MTU 247).
> 3. Firmware: assemble, verify, then decode the JWS's OWN payload into `configJson` — the profile
>    source becomes the signed payload, NOT the reassembled key/value packages.
> 4. App: send the JWS instead of the per-key PID packages (`tlm_push_token` and `utc` still go
>    separately — the token is not in the profile and is not the CAN-injection risk).
>
> **Both traps still apply.** Do not change what the CRC version tag is computed over (still
> `CRC32(JSON.stringify({...sanitizedVehicleJson, tlm_push_token}))`, app-side, dongle never
> recomputes it) or every reconnect drops to the slow path with a full `ATZ` re-init. And change only
> ACQUISITION: `completeSetup` / `configCallback`, the loop-task apply, the identical-profile ATZ skip,
> the LittleFS persist and `invalidateHash()` all stay untouched.

**The hole:** a pushed profile carries `init_commands` (raw AT, including `ATSH` to set arbitrary CAN headers
and `ATSP` to choose the protocol) plus `data_commands` as arbitrary hex. That is arbitrary CAN injection with
the dongle as transport. On a gatewayed port that is mostly UDS — which is exactly where routine-control and
I/O-control-by-identifier live. **This hole predates every feature in this plan.**

**The fix already exists, half-built.** `src/api/vehicles/getObdBlePids.ts` requests `signed=true` and receives
a **JWS compact string, RS256**, verified in-app against a hardcoded RSA-2048 public key. Today the app
verifies it and then **throws the signature away**, pushing only the parsed object. Forward the JWS instead and
let the dongle verify it.

**Critical constraint discovered while designing this:** the app *mutates* the payload after verifying
(`value.maxValue = Number(value.maxValue)`), and the dongle reassembles the pushed key/value packages into
key-sorted nlohmann JSON. The bytes the dongle holds are therefore **not** the bytes that were signed. So a
detached signature over any "canonical" form is a trap — it would break on number formatting alone. The signed
artifact must be transported **verbatim** and verified *before* parsing, exactly like OTA hashes the bytes it
actually stored.

### Wire format (ABD2, additive)

```
app → {"JWSBEGIN":<total_chunks>}
app → {"JWS":{"i":0,"d":"<up to ~200 chars of the compact JWS>"}}   … i = 0..n-1
app → {"JWSEND":1}
dongle: reassemble → verify RS256 over  base64url(header) "." base64url(payload)
        → on success parse payload, apply as today, persist to /vehicle_profile.json
        → on failure  {"ERROR":"badsig"}, keep the running profile, invalidateHash()
```

JWS size ≈ 1.5 KB (≈1 KB payload + 342-char RSA-2048 signature + header) → ~7 chunks at MTU 247.

### File map

| Change | Where |
|---|---|
| Keep the raw JWS beside the parsed object, per typecode | `src/state/obdble/obdbleSlice.ts`, `src/api/vehicles/getObdBlePids.ts` |
| Send JWS chunks instead of per-key packages | `src/state/ble/abrpObd/setupThunk.ts` |
| Reassembly + verify + apply | `libraries/BLEWrapper/blewrapper.cpp` (`StartupCallbacks::onWrite`), `baseLogger/Helpers/ble.h` |
| Pinned key | new `baseLogger/Helpers/pid_pubkey.h` (RSA-2048, same key the app pins) |
| RS256 verify | mbedTLS RSA, ~10–30 ms on ESP32. Alternative: have the backend also emit ECDSA-P256 so the dongle reuses the OTA verify path — but that *is* backend work, and the JWS route needs none |

### The escape hatch this needs

The **PID editor authors unsigned profiles** (`customObdPids`, plus the `FreematicsEmu` dev profile). Strict
enforcement would break it. So the dongle needs an unsigned-accept mode that is **physical-access gated**: a
serial console command (`UNSIGNED ON`), **RAM-only** so a reboot clears it. Never an app-side toggle — an
app-flippable bypass is not a security boundary.

### Rollout without lockout (three firmware/app steps)

1. Firmware accepts **both** the legacy per-key push and the new JWS push (prefers JWS).
2. App switches to sending the JWS.
3. Firmware flips to **requiring** a valid JWS (unless `UNSIGNED ON`).

---

## 4. Phase B2 — replace the bearer token with nonce + HMAC

> **STATUS — ENFORCED, unconditionally** (`Helpers/auth_gate.h`; there is no `AUTH_ENFORCE` flag any
> more). A valid ABD8 proof is the ONLY thing that grants a session — telemetry notify, OTA, the log
> dump and the cellular stand-down all stay closed until it lands. The legacy
> `{"AUTH":"<tlm_push_token>"}` write unlocks the ABD2 profile push and nothing else.
>
> ABD8 is live: `kex` (ECDH TOFU, one-shot, refused with `{"auth":"locked"}` once provisioned),
> `challenge` (16-byte `esp_random` nonce, base64, single-use, 30 s TTL, `{"auth":"unprovisioned"}`
> when no secret — the app's TOFU cue), `proof` (HMAC-SHA256 via mbedTLS, constant-time compare,
> nonce consumed before judging so a proof is only ever judged once per challenge). Same task
> discipline as OTA/log dump: the ABD8 callback copies bytes on the Bluedroid task; all NVS + HMAC
> runs in `authGateService()` on the loop task. Outcomes land in the flash log as `AUTH2 ok/fail`.
>
> The cleartext `provision` verb that shipped alongside `kex` has been **deleted** — it took the
> shared secret as a plaintext BLE write, which is the exact thing `kex` exists to avoid, and it was
> reachable by any peer that reached an unprovisioned dongle. A secret now only ever gets *derived*.
> **App side shipped too:** `abrpObdAuthThunk` runs fire-and-forget after a *fulfilled* setup (gated on
> `isFulfilled` — a `.then` alone also runs for rejected thunks, and after a declined pairing a write to
> encryption-gated ABD8 would re-prompt; the thunk also early-rejects on `awaitingPairing`). HMAC-SHA256
> is built on expo-crypto's SHA-256 (`hmacSha256.ts`, validated against the RFC 4231 vectors — no new
> native dependency), TOFU generates the 32-byte secret with `getRandomBytesAsync`, and the keyring is
> `obdble.dongleSecrets` (redux-persist/MMKV — decided: same at-rest model as the account access token;
> note the tlm_push_token itself is NOT at rest on the phone, only in memory). The keyring entry is
> persisted the moment the dongle confirms `provisioned` — deliberately NOT after the proof round, so a
> BLE drop between the two cannot strand a provisioned dongle with a secret the app never saved.
> **Android greet loss — fixed (2026-08-21).** The greet fires on the ABD8 subscribe, which on Android
> happens BEFORE MTU negotiation. A notify carries at most MTU-3 bytes and is never fragmented, so at
> Android's unnegotiated default of 23 the 20-byte ceiling silently dropped both greet payloads
> (`{"auth":"unprovisioned"}` is 24 B, a nonce frame ~36 B) and the app never answered — while iOS,
> which negotiates ~247 on its own before the greet, provisioned first time. Same trap the log dump
> already guarded with `{"err":"mtu"}`; the auth gate had no such guard. Two-sided fix: the firmware
> folds `mtu() >= 64` into the greet's edge condition, so a too-narrow pipe DEFERS the greet and it
> fires the moment MTU rises (no extra state, no timer), and the app requests `PREFERRED_MTU` **before**
> subscribing — subscribing is what triggers the greet, so widening afterwards would be too late.
>
> **Keyring is keyed by the DONGLE's own reported id.** The dongle now sends a stable identity
> (`authDongleId()`, eFuse-MAC derived, 12 hex) in every ABD8 greet, and the app keys its keyring by
> that. Neither obvious alternative works: the **BLE device id** differs per phone (iOS hands out a
> per-central UUID, Android an address — which is why a vehicle stores an *array* in
> `obd_ble_device_ids`), so it could never be shared phone-to-phone and a QR code carrying one would
> hand the receiver a key it cannot match; and the **vehicle id** is wrong in principle — the secret
> belongs to the dongle, not the car, so pushing a new vehicle profile must not change who may talk to
> the dongle, and moving a dongle between cars must not invalidate anyone's access. (Vehicle-keying was
> tried and rejected on exactly that reasoning before the QR flow was built on it.) The eFuse MAC
> survives reflashing, NVS erase, `AUTHCLEAR` and deep sleep.
>
> **`AUTHCLEAR` is the REVOCATION mechanism, and the app must never undermine it.** On
> `unprovisioned` the app ALWAYS mints a fresh 32-byte secret — never re-pushes one it already holds,
> even for the same dongle id. Re-pushing would make revocation a no-op: the old secret would come
> straight back and every previous holder would keep working. (Tried and reverted — it looked like a
> kindness and silently destroyed the security property.)
>
> **Revocation procedure:** `AUTHCLEAR` + `BONDCLEAR`, then connect the phone that should keep access.
> It provisions a brand-new secret and every other holder is locked out; re-share by QR to whoever
> should still have it. First connect after the clear wins, with no confirmation prompt — accepted
> deliberately, because `BONDCLEAR` means no other phone can even reach ABD8 without pairing first
> (the whole table is encryption-gated) and a phone whose bond was just cleared hits the one-sided-bond
> flap rather than reconnecting silently. Claiming happens in a controlled environment.
>
> **Handshake verified end to end on both platforms (2026-08-21).** Android TOFU-provisioned first
> (`greet: unprovisioned` → `provisioned`) with MTU 247 preceding every greet — the MTU fix holds, 4/4.
> The iPhone then connected and was **refused** (`AUTH2 fail (bad proof)`), and after an `AUTHCLEAR` +
> iPhone provisioning the Android was refused in turn. Those refusals are the gate working: each phone
> holds only its own secret, and the dongle computed and compared an HMAC to reject the other. Note the
> *success* path (`AUTH2 ok`) is still unwitnessed — every capture so far provisioned with one phone and
> then connected the other; a reconnect with the SAME phone is the missing one-line confirmation.
>
> **The enforce path is now real.** `authenticated` used to be one flag doing two jobs — the ABD2
> state-machine latch (while false, the next write is parsed as an AUTH token) *and* the session grant
> (telemetry / OTA / log dump / cellular stand-down). That conflation made enforcement impossible:
> withholding the grant would have re-parsed every profile packet as an AUTH attempt and broken the
> push. They are now split — `setup_unlocked` (latch) and `authenticated` (grant). Audit mode sets
> both from the legacy write, so behaviour is unchanged; under `AUTH_ENFORCE 1` the legacy write sets
> only the latch and **only a valid ABD8 HMAC proof authorizes**. Both paths compile; audit is the
> shipping default. `auth_enforce` reaches the library through `set_auth_enforce` (the runtime-setter
> pattern — the library never sees config.h).
>
> **Audit mode grants NO access control.** Anyone signed into the account holds the vehicle's
> `tlm_push_token`, which the legacy write accepts — so a second phone with an old app build still gets
> telemetry, OTA and the log dump. Verified in the field 2026-08-21. That is the hole this phase exists
> to close, and it stays open until `AUTH_ENFORCE` flips.
>
> **Prerequisites before flipping (both are lockout risks, not polish):** the audit logs must show
> `AUTH2 ok` on every reconnect on both platforms — enforce turns a silent audit failure into no
> telemetry at all — and the QR give/receive flow must exist, because the secret lives in **that
> phone's keyring, not on the account**: a second phone, or a reinstall, otherwise has no in-app way to
> obtain it (only `AUTHCLEAR` over serial).
>
> **Handshake verified end to end (2026-08-24):** `greet: unprovisioned` → `provisioned` →
> `AUTH2 ok (ENFORCED - session granted)`, all in ONE session. Two fixes got it there. First, the
> keyring was written by a matcher reading the thunk's *fulfilled payload* — three things had to line
> up, and when any failed the dongle had committed a secret to NVS that the app never saved
> (unrecoverable except by `AUTHCLEAR`, with no symptom but an unanswered nonce). The keyring now has
> exactly ONE writer, `setDongleSecret`, dispatched explicitly the moment the dongle confirms, shared
> with the QR receive path. Second, provisioning alone proved nothing, so under enforcement no session
> was ever granted and telemetry stayed dark; the app now sends `{"challenge":1}` straight after
> provisioning and proves in the same session.
>
> **ABD2 (the vehicle profile) is now authorization-gated, and the connect order was inverted for it.**
> ABD2 carries the PID list and the tlm push token, so a peer that can write there can inject arbitrary
> PIDs at the CAN bus — the worst case in §0's table, and worse than reading telemetry. Pairing alone
> does not earn that: it proves the link is private, not that the peer is allowed. Under
> `AUTH_ENFORCE` the dongle now refuses ABD2 with `NOAUTH` until the ABD8 proof lands, and the app
> dispatches the auth handshake **before** `abrpObdSetupThunk` (it used to push the profile first and
> prove afterwards, which would fail on every first connect under enforcement). Correct layering:
> pair → prove → data. The handshake stays non-fatal so old firmware and declined pairings still
> complete setup; under enforcement the *dongle* is what says no, not the app.
>
> **QR give/receive shipped.** `ObdBleShareSecretScreen`, third row under Advanced OBD settings behind
> the same developer toggle (whose description names all three screens). Receive is the first control;
> below it a list of held secrets, each row showing a user-set name with the dongle id beneath (the id
> always stays visible — it is what matches the dongle's own `AUTH2 greet … id=` line). An edit view
> per row renames or deletes; it is a dedicated view rather than the shared `InputOptionsDialog`, which
> is used by unrelated flows, seeds its input from `description` rather than a real prefill, and puts
> its buttons under the keyboard — the last of which silently swallowed the first Save tap.
> `dongleNames` is a separate map from `dongleSecrets` (a name is cosmetic, the secret is not, and that
> shape is already persisted); deleting a secret drops its name too. The payload codec validates hard —
> version, 12-hex id, secret decoding to exactly 32 bytes — because a camera hands the app any QR in
> view and a malformed keyring entry produces a dongle that silently refuses this phone.
>
> **`AUTH_ENFORCE` is 1** — flipped and field-confirmed (`AUTH2 ok (ENFORCED - session granted)`).
>
> **The vestibule/bouncer is STRUCK, and replaced by something better.** Dropping a connection that
> never authorizes cannot work here: the dongle cannot distinguish a camping stranger from a user
> mid-pairing-decision, and dropping the link makes the app's poller reconnect and re-raise the OS
> pairing prompt every cycle — precisely the churn `awaitingPairing` was built to prevent.
>
> What the vestibule was actually for was denying a camper the two things it could still cost us:
> holding the dongle awake, and occupying a connection slot. So instead the **awake-hold now keys on
> `bleRelaying()` (connected AND authorized) rather than `bleConnected()`**. A peer that never
> authorizes no longer refreshes the activity leash, so the device progresses ACTIVE → CHARGING →
> STANDBY → deep sleep on its own: **~10 min (`ACTIVE_TIMER` + `CHARGING_TIMER`) instead of the 60 min
> `MAX_AWAKE_MINUTES` backstop.** Sleeping also stops advertising entirely, so the worst case for a
> declining user becomes one prompt per wake instead of one per poll tick — strictly better than the
> vestibule on both counts.
>
> **The trap that made this non-trivial:** gating the leash on relaying alone would let a legitimate
> phone that connects while the leash is nearly expired fall out of the BLE state DURING its handshake,
> and `CHARGING` deliberately never revives on a connection alone — so a real session would have been
> slept out from under the user, the exact failure `BLE_CONNECTED_HOLDS_AWAKE` exists to prevent. Both
> connect transitions therefore stamp `last_active`, giving every new session a full `ACTIVE_TIMER` to
> authorize before the relaying check starts governing it.

**The hole:** `{"AUTH":"<tlm_push_token>"}` is a reusable secret sent in plaintext on **every connect**, and
the dongle logs prove it is literally the vehicle's cloud upload credential:

```
ABD2 rx: {"AUTH":"<tlm_push_token>"}
ABD2 rx: {"tlm_push_token":"<tlm_push_token>"}   ← same value
```

(Firmware since then logs only the length and keys of each write, e.g. `ABD2 rx: len=48 keys=AUTH`.)

Sniff it once → push fabricated telemetry for that vehicle forever.

### Handshake (replaces the AUTH write)

```
dongle → {"nonce":"<16 random bytes, base64>"}      // esp_random(), single-use, 30 s TTL
app    → {"proof":"<HMAC-SHA256(secret, nonce), base64>"}
dongle: recompute, constant-time compare → authenticated
```

No backend call. HMAC-SHA256 is microseconds on the ESP32 and mbedTLS is already linked.

**Secret provisioning — SETTLED as TOFU + QR, no backend** (decided in review; supersedes the
backend-stored-secret idea above):

- **TOFU (trust on first use):** an unprovisioned dongle (empty `auth` NVS namespace) accepts the first
  secret written to ABD8 and locks to it. **No factory window / timeout** — dongles are claimed and
  provisioned by their owner before they ever reach a vehicle, so a countdown adds firmware complexity for
  a parking-lot threat that cannot occur. Rescue from a lost/mismatched secret is the `AUTHCLEAR` serial
  command (physical access; already shipped; does not revoke a live session).
- **One secret per dongle, no per-person slots** — slots mean chasing every holder down for rotation.
  Sharing access = the app shows the secret as a **QR code** ("give") and scans it ("receive"), phone to
  phone, in person. No MITM surface: you show a QR in a controlled space. The app keeps its per-dongle
  secrets in the OS keystore.
- The provisioning write crosses BLE **encrypted** (Phase E gated ABD8 before this phase started), so the
  original "provisioning crosses in the clear" caveat is retired.

**Rollout in audit mode first:** `AUTH_ENFORCE 0` — the ABD8 handshake runs and its outcome is logged
(`AUTH2 ok/fail`), but the legacy `{"AUTH":"<token>"}` on ABD2 keeps granting the session, so an app
without the new handshake loses nothing. Enforcement (legacy AUTH stops granting; vestibule/bouncer
drops never-authenticating connections) is a later, deliberate flip — same pattern that made the
encryption rollout safe.

**Honest limit that remains:** B2 removes the token from *every connect*, but the token is still part of a
**full profile push** (it is one of the pushed keys and is inside the CRC), so it still crosses the air —
now encrypted by Phase E — whenever the profile is re-pushed.

---

## 5. Phase C — authorization for the log dump

> **SUPERSEDED — struck by decision (2026-08-25).** This phase specified a backend ECDSA challenge to
> answer "which USER may pull this dongle's logs". Phase B2's HMAC gate already gates the log dump
> (its `noauth` check tests the session grant), so a second backend authorization layer would be
> redundant complexity for a goal already met. Kept for the record, not to be built.
>
> Known consequence of the substitution: HMAC answers "who holds the secret", not "which user is
> permitted". With QR sharing a secret propagates to whoever the holder chooses and there is no
> per-person revocation — rotation is all-or-nothing (`AUTHCLEAR` → re-provision → re-share).

Log fetch exfiltrates GPS traces, so "which user is allowed to pull this dongle's logs" is a live question that
only the backend can answer.

```
app    → {"CHALLENGE":1}
dongle → {"nonce":"<16 bytes, base64>"}
app    → HTTPS POST {dongle_id, nonce}   → backend: authenticate user
                                          → check ownership/permission table
                                          → sign, return signature
app    → {"AUTHSIG":"<base64, 64 bytes raw r‖s>"}
dongle: verify ECDSA-P256 over  "ABRPAUTH1" | dongle_id | nonce  → privileged = true (session-scoped)
```

- **Bind `dongle_id` into the signed blob.** Without it, a user legitimately authorized for *their* dongle can
  obtain a signature and replay it against someone else's.
- **Separate key pair from OTA signing** — different purpose, different rotation cadence, smaller blast radius.
  New `baseLogger/Helpers/auth_pubkey.h`, new `tools/auth_keygen.py`.
- **Extract, don't duplicate:** factor the existing mbedTLS ECDSA verify out of `Helpers/ota_update.h` into a
  shared `Helpers/ecdsa_verify.h` so OTA and auth share one proven implementation.
- Privileged flag is **RAM-only**, cleared on disconnect and after a timeout.
- Enforcement: `log_dump.h` START gates on `ble.privileged()`.
- App: new `src/state/ble/abrpObd/authorizeDongleThunk.ts`, called by `fetchDongleLogsThunk`; new endpoint
  client next to the existing pairing call, which is where the ownership record already lives.

Backend: one endpoint, an ownership check, and private-key storage (KMS/HSM rather than a config file).

---

## 6. Phase D — same authorization for OTA

> **SUPERSEDED — struck by decision (2026-08-25).** OTA START already requires the session grant
> (`ota_update.h`, the `OTA_ERR_NOAUTH` check), which under `AUTH_ENFORCE` means a valid HMAC proof.
> The prerequisite this phase demanded — a serial-only `AUTHOFF` override before enforcing — was also
> dropped, on the reasoning that `AUTHCLEAR` already restores access for every *state* failure, and
> the only case it cannot cover (the auth code itself being broken) needs new firmware regardless,
> which needs physical access either way. `AUTHOFF` would have saved the build toolchain on a visit,
> not the visit.

Identical mechanism, gating `ota_update.h`'s START.

**Prerequisite:** a serial-only override must exist *first* (`AUTHOFF`, RAM-only, physical access). OTA is the
delivery mechanism for every future fix — if the auth path breaks and OTA is behind it, the fleet cannot be
repaired. Same lesson as the anti-downgrade floor.

Note the resume path: an OTA that drops mid-transfer re-authenticates within the 90 s grace window. With the
network assumed available, that is simply a second backend call — no cached ticket needed.

---

## 7. Phase E — link-layer encryption (bonding / SMP)

> **STATUS — E1 + step 3 (ABD7 only) shipped and VALIDATED on Android and iOS** (2026-08-20, real dongle,
> both phones). Confirmed on both platforms: exactly one pairing prompt, and only at the first gated access
> (the log-dump fetch); silent LTK reuse on reconnect (Android re-encrypts eagerly at connect, iOS lazily at
> the first gated access — both correct, just changes where `SEC bonded` lands in a log); full 2.86 MB dumps
> over the encrypted link at ~22 KB/s with `retries=0`; clean re-pair after a both-sides unpair; unpaired peers
> still read telemetry and fetch fails for them — the gate working as scoped.
>
> **Field failure mode, observed live — the stale one-sided bond.** `BONDCLEAR` (or NVS loss, or a
> replacement dongle) leaves previously-paired phones holding their half of the key. iOS then re-encrypts
> at connect with its stale LTK, gets key-missing, and DROPS the link rather than continuing plaintext.
> Log fingerprint: `CONNECT` → `DISCONNECT reason=0x13` within ~2 s, **no MTU line, no AUTH, and no `SEC
> failed`** — iOS aborts at the link-layer encryption restart, before SMP runs, so `onAuthenticationComplete`
> never fires and the connect-flap pattern is the only trace. Cellular uploads continue throughout, so
> telemetry survives; the phone link stays dead until the user does Settings → Bluetooth → **Forget This
> Device** (Android: Unpair). This is the support-playbook answer for "my phone stopped connecting after
> the dongle was reset". Android's variant of the same state logs `SEC failed reason=0x63` instead.
>
> | Prereq | How it was met |
> | --- | --- |
> | 1. Database Hash / Service Changed | **Substituted, by decision:** the GATT table is FROZEN instead, with `ABD9`/`ABDA` created unused so a future need reuses a handle rather than reshaping the table. Service Changed without Database Hash would not reliably invalidate an iOS cache, and the library provides neither. See the FROZEN TABLE note in `blewrapper.h`. |
> | 2. Handle budget | `createService(..., 32)` — 28 used, 4 spare, arithmetic in `blewrapper.cpp`. |
> | 3. One batched table change | Better than planned: the table change shipped **before** bonding existed, so the cache flush was paid while still unbonded (a BT toggle recovers it). E1 itself changes no handles. |
> | 4. `BONDCLEAR` | Serial console, plus `AUTHCLEAR`. Both shipped before the thing they recover from. |
>
> **Rollout step 3 has begun, with ABD7 only.** A short-lived `requestEncryption()` push from `onConnect`
> (added so that *something* would trigger pairing while no attribute required it) has been **removed** — it
> caused a real bug: a peripheral-initiated security request makes **Android ≥12 show two system pairing
> dialogs for one procedure**, one on the request and one when its stack re-evaluates the encryption state.
> iOS bundles them, which is why it only reproduced on Android. Measured: a single `SEC req` and a single
> `SEC bonded` ~8 s apart, both dialogs inside that window. **Do not reintroduce a peripheral-side push to
> help a phone pair — gate an attribute instead** and let the central drive, which is the normal pattern
> (unpaired access → ATT `0x05` → the phone pairs and retries itself, one dialog).
>
> **Gated attributes: the ENTIRE service — every value and every CCCD (`ABD1`–`ABDA`)** — deliberately
> never the `_ENC_MITM` variants: Just Works cannot satisfy an MITM requirement, so those would make the
> attributes permanently unreachable rather than merely encrypted. Permissions are per-attribute metadata,
> so this changes no handles and the FROZEN TABLE invariant holds. An unpaired phone can connect but can
> read/write **nothing**; pairing triggers at the first attribute touched (normally ABD2's CCCD, the setup
> subscribe). ABD1/ABD2 gating is the **"cellular until paired"** behaviour, validated in the field
> (2026-08-21): a 21 s unanswered prompt showed zero BLE telemetry, zero setup traffic, frames buffered
> for cellular — then `SEC bonded` → `AUTH OK` one second later (Android holds the gated write pending
> during the dialog and retries it transparently).
>
> **Companion fix, field-found on the decline path:** the dongle's three cellular gates (upload, modem
> re-init, NTP retry) keyed on `bleConnected()` under the pre-gating assumption "a connected phone relays
> to the cloud" — so a phone that declined pairing (connected, relaying nothing) silently starved cellular
> and the data flowed **nowhere** live (frames only buffered). They now key on `bleRelaying()` =
> connected **and** authenticated (`Helpers/ble.h`) — AUTH on ABD2 is only possible over a paired link, so
> it's exactly "the phone is a working transport", and it is the dongle-side mirror of the app's
> `awaitingPairing` (both flip on the same AUTH event). The state machine's awake-hold deliberately stays
> on `bleConnected()`: sleeping mid-pairing-prompt would drop the link and cause reconnect→re-prompt
> churn; `MAX_AWAKE` bounds a camping phone. Accepted cost: a POST may now overlap the few seconds
> between CONNECT and AUTH OK. Residual, by design: each NEW connection session prompts once — an
> unpaired phone that keeps auto-reconnecting keeps getting one prompt per session; the cure is pairing
> or unlinking the dongle, not suppression.
>
> The rollout went ABD7 → ABD1/ABD2 → everything, each step field-validated before the next. The original
> plan kept ABD5/6 (OTA) open until last, fearing a permission mistake would brick updates — that caution
> was retired on evidence: the identical pattern moved full 2.8 MB log dumps on both platforms with
> `retries=0`, comfortably above OTA's 1.7 MB stop-and-wait transfer. Gating OTA buys something real: an
> unpaired stranger could otherwise write `START` to ABD5 and spam the staging slot. ABD6 is WRITE_NR —
> ATT silently drops write commands to an encryption-required attribute on a plaintext link (no error
> response), which is fine since ABD6 is only written after START is ACKed on ABD5, which forced
> encryption. ABD8 gated = pairing-before-authorization, the intended layering for the phase-2 HMAC
> handshake. App impact of gating ABD3–ABDA: none — ABD3 is declared but unused, ABD4 is only read via a
> `.catch`-guarded error path, and ABD5–7 live in dev-gated one-shot engines reached only by already-paired
> sessions.
>
> **The former app-side blocker is resolved.** The app now maps ATT security refusals (insufficient
> authentication/authorization/encryption/key-size, detected via `attErrorCode` in `isBleAuthError.ts`)
> to an `awaitingPairing` state instead of a lost connection: the listener and setup thunks reject with a
> sentinel, `bleSlice` matches it and **keeps the connection** (a teardown would reconnect and re-prompt
> every poll cycle), and the data-source status shows "Pairing required – data via cellular"
> (`obd_pairing_required`). The flag clears on disconnect/reconnect or when a frame arrives. Detection
> must happen where the original `BleError` still exists — the thunks' wrap-into-plain-Error paths drop
> `attErrorCode`. **Ship dongle and app together**: an old app against ABD1/2-gated firmware loops
> reconnect→prompt on a declined dialog.
>
> Two diagnostic dead ends recorded so they are not repeated: `esp_ble_auth_cmpl_t.key_present` cannot
> distinguish a new pairing from a reused LTK on LE (it reports a BR/EDR classic link key, absent for every
> LE pairing) — use the bond count delta. And `fail_reason` is BTA-offset into ~`0x53`–`0x6B`, not a raw SMP
> status.
>
> Bond-table pressure — prereq 4's underlying problem — is now handled proactively too: `BLE_MAX_BONDS 10`
> against Bluedroid's hard `CONFIG_BT_SMP_MAX_BONDS=15`, pruned at BLE bring-up (loop task, nothing
> connected). Eviction is arbitrary and frees a single slot; an evicted phone re-pairs silently.

Gives confidentiality: the profile (and its PID list), the provisioning
secret, the firmware image in flight, and the telemetry values all stop being readable by a bystander.

Fully supported by the installed Arduino BLE library — no forking:
`BLECharacteristic::setAccessPermissions(esp_gatt_perm_t)`, and a `BLESecurity` class with
`setAuthenticationMode`, `setCapability`, `setKeySize`, `setStaticPIN`.

```c
BLESecurity sec;
sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);   // LE Secure Connections + bonding
sec.setCapability(ESP_IO_CAP_NONE);                    // Just Works (no display/keypad)
sec.setKeySize(16);                                    // AES-128
// then, per characteristic:
pChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
```

### Hard prerequisites — do NOT skip these

1. **GATT Database Hash / Service Changed.** This project has already lost days to iOS serving a stale cached
   GATT table (`notify change failed`, ABD6 "not found"), with the remedy being a Bluetooth toggle. **Bonded**
   devices cache harder, and the user remedy escalates to "Forget This Device" in system settings. Implement
   the Database Hash characteristic (or at minimum Service Changed) so a firmware update that changes the GATT
   table triggers re-discovery.
2. **Raise the service handle budget first.** Currently **21 of 24** used (7 characteristics + 6 CCCDs + the
   service declaration). Database Hash and any auth characteristic will not fit. Raise `createService(...,
   24)` to 32 — a few bytes of RAM.
3. **Batch every GATT-table change into ONE firmware update** (handle count, Database Hash, any new
   characteristic), so users pay the cache-flush disruption once instead of three times.
4. **A serial `BONDCLEAR` command.** Bluedroid's bond list is small (single digits) and stored in NVS; between
   dev, support and family phones it will fill and evict. There must be a recovery that is not a reflash.

### Rollout order (this ordering is the safety property)

1. Firmware **supports** SMP but requires it nowhere (permissions unchanged). Ship, verify nothing regressed.
2. App-side bonding support. Verify on **both** iOS and Android, explicitly across a firmware update that
   changes the GATT table — that is this project's known failure mode.
3. Flip `*_ENCRYPTED` per characteristic, in this order:
   **ABD7 (log dump, lowest blast radius) → ABD3/ABD4 → ABD1/ABD2 → ABD5/ABD6 LAST.**

### Known limits

- With `ESP_IO_CAP_NONE` (Just Works) the link is encrypted but **MITM-vulnerable during pairing itself** —
  the same one-time window as provisioning the B2 secret. A **per-dongle passkey delivered through the app**
  would close it; a firmware-compiled `setStaticPIN` would not, since the `.bin` is distributed.
- Bonding authenticates the *link*, not the *payload*: it does not replace Phase B, C or D.

---

## 7b. First-time pairing — the bootstrap path

There are TWO connect sequences in the app, not one, and both must authorize before touching ABD2:

| Path | Where | Order |
|---|---|---|
| Steady state | `ObdBlePoller.connect()` | connect → **auth** → setup → listener |
| First-time pairing | `ObdBleSetupConnectionScreen` | connect → **auth** → setup → listener |

The wizard was missed when the poller was reordered, and it does not inherit the fix: the poller
deliberately stands down while `isObdBleSetupOngoing` is set, so nothing else can authorize on the
wizard's behalf. The symptom was that pairing a NEW dongle to a vehicle always failed at "error writing
init commands" (ABD2 answering `NOAUTH`), while already-paired dongles kept working because their
secret predated enforcement — so the regression was invisible until someone claimed a fresh dongle.

The wizard is also the natural home for TOFU provisioning: the phone claiming a dongle is the phone
pairing it to a vehicle, so first-connect is exactly when the secret should be minted.

**A gated-channel refusal is NOT proof the user declined.** Whichever operation is FIRST to touch a
gated attribute is the one that TRIGGERS pairing, so it races the procedure it started — "pairing still
in flight" and "user declined" look identical at that instant. Latching `awaitingPairing` off such a
one-shot made every first-ever connect fail: the flag was set, `setupThunk` refused on it, the wizard
reported "OBD connection failed", and a retry then worked because the bond existed by then. Field-
reported and fixed.

So the rule is: **only the SUSTAINED subscription latches.** `listenerThunk` keeps a long-lived
telemetry monitor, so a refusal there really does mean access is being denied. `authThunk` and
`setupThunk` are one-shots — they log the refusal and reject plainly. `setupThunk` still *reads* the
flag on entry (a decline the listener already detected should short-circuit the next cycle), it just
no longer sets it.

In both paths the handshake is dispatched WITHOUT `unwrap()`. A rejection must not abort the flow —
firmware without ABD8 rejects it and that firmware's ABD2 accepts the push regardless. If
authorization was genuinely required and did not happen, the setup step is what reports it.

---

## 8. Ordering summary and the one invariant

```
A (dev gating)  →  B (signed profiles)  →  B2 (nonce+HMAC)  →  E1/E2 (SMP support + app bonding)
                                        →  C (log-fetch auth)
                                        →  E3 (flip encryption, ABD5/6 last)
                                        →  D (OTA auth, needs AUTHOFF override)
```

**The invariant, stated once and respected everywhere:** OTA (ABD5/ABD6) is the delivery mechanism for every
future fix, so it is the **last** thing locked in every dimension — authorization *and* encryption — and every
gate gets a physical-access serial override before it is enforced.

Practical grouping: A ships alone today. B+B2 are one firmware/app cycle (both touch ABD2 and the setup
handshake). E1 rides along in that same firmware so SMP support and the batched GATT changes land together. C
then D last, once the override exists.


---

# Appendix B — OTA update architecture (v1)

> Original file: `ota-update-architecture-v1.md`

# Firmware OTA over BLE — v1 build specification

End-to-end specification for the ESP32 dongle firmware update system, covering **both
sides** — the dongle (ESP32, PlatformIO/arduino-esp32) and the mobile app (React
Native / RTK) — and how they interact over BLE. This is a v1-only build spec: it
describes exactly what gets built now. Deferred ideas are deliberately omitted to keep
the implementation focused.

The two sides share one contract: a protocol over two GATT characteristics
(`ABD5`/`ABD6`) on the existing `ABD0` service, fixed sector/ACK framing, and a strict
division of responsibility where the **dongle owns all trust and safety** and the
**app is a dumb, untrusted courier + status screen**.

---

## 0. Division of responsibility (organizing principle)

| Concern | Owner |
|---|---|
| Trust boundary (signature verify) | **Dongle** |
| Safety (dual-slot, rollback, power gate) | **Dongle** |
| Commit decision (`esp_ota_set_boot_partition`) | **Dongle** |
| Discover newer version | App |
| Move bytes to the dongle | App (BLE) |
| Progress / result UI | App |
| Stay out of the transfer's way (poller back-off) | App |

**Failure invariant:** if the app is buggy, killed, or malicious, the worst case is a
*failed/aborted* update — never a bricked dongle and never an unsigned image running.
This falls out of the dongle design; the app's only obligation is not to undermine it.

---

## 1. Settled decisions (v1)

1. **Distribution:** signed `.bin` files + a manifest as GitHub Releases of this repo. The
   app fetches them, then transfers over BLE.
2. **Repo access:** none. The signature is the trust boundary, so authenticated
   distribution buys nothing.
3. **Signing trust root:** central vendor signing — one private key, kept offline/secure
   (ideally HSM/hardware token); the corresponding public key is compiled into the dongle
   firmware.
4. **Transport:** BLE only. The app downloads the image and streams it to the dongle.
   Byte-transfer is isolated behind one module on each side so the OTA backend does not
   hard-code the byte source (clean seam; no alternative transport is built in v1).
5. **Encryption:** dropped. Authenticity + integrity come from the signature, which is
   the trust boundary; app-level encryption with a plain-flash key adds complexity for no
   real confidentiality.
6. **Crypto:** **ECDSA-P256 + SHA-256**, using the bundled mbedTLS (`mbedtls/ecdsa.h` +
   `mbedtls/sha256.h` ship in-framework; Ed25519 would need an extra library, so it is
   not used).
7. **Version scheme:** monotonic integer (1, 2, 3, …) stored in the dongle's NVS. The
   dongle rejects any incoming image whose version ≤ the stored value. (Integer format
   chosen so it stays compatible with a hardware secure-version counter if that is ever
   adopted; none is enabled in v1.)

---

## 2. Shared BLE contract (the interface both sides implement)

One service reused so the existing connection/bonding is untouched. Two new
characteristics; `ABD1`..`ABD4` (telemetry/startup/developer/error) are **not** touched.

### 2.1 GATT layout — two new chars on the existing `ABD0` service
| Char | UUID | Properties | Role |
|---|---|---|---|
| OTA control | `ABD5` | WRITE + NOTIFY | client→device commands (START/END/ABORT) + metadata; device→client status/ACK notifications |
| OTA data | `ABD6` | WRITE_NO_RESPONSE | client→device firmware packets (throughput path) |

### 2.2 Sectoring & framing (Espressif `esp_ble_ota` design)
- Firmware split into **4096-byte sectors** (one flash page, one `esp_ota_write` per sector).
- Each sector split into MTU-sized packets. **Data packet on `ABD6`** = 3-byte header + payload:
  ```
  [0] sector_index LSB
  [1] sector_index MSB      (0xFFFF in [0..1] = "this is the last sector")
  [2] packet_seq            (0,1,2,...; 0xFF = "last packet of this sector")
  [3..] firmware payload    (MTU − 3 ATT − 3 header bytes)
  ```
- **Control/metadata on `ABD5`** (client→device), first 2 bytes = opcode LE:
  - `0x0001` START + `uint32 image_size` + `uint32 version` + `sha256[32]` + `signature[64]`
    (~106 B, one long write — reuse the config-push long-write path used on `ABD2`).
  - `0x0002` END/commit-request. `0x0003` ABORT.
- **ACK/status frame (device→client, NOTIFY on `ABD5`)** = 20 bytes, CRC16-CCITT
  (`poly=0x1021, init=0x0000`) in the last 2 bytes:
  ```
  [0..1] sector_index echoed   [2..3] status   [4..5] cur_sector (resume anchor)   [18..19] CRC16
  status: 0x0000 success, 0x0001 CRC/verify err, 0x0002 sector-index err, 0x0003 length err
  ```

### 2.3 Flow control — stop-and-wait per sector
Client sends all packets of sector N → device assembles 4 KB → loop task `esp_ota_write`s
it → device NOTIFYs the sector ACK (with `cur_sector`) → client sends sector N+1. Paces
the transfer to flash speed; can't overrun. Port the **NimBLE retry logic** (not the
weaker Bluedroid one): on `sector_index`/`packet_seq` mismatch, set a sticky sector-error,
reset the sector to packet 0, NACK with `0x0002`, and swallow the stale `0xFF` end-marker
until the sector retries cleanly.

### 2.4 Resume anchor
The ACK carries `cur_sector` — the last successfully-written sector. On reconnect the
device reports it so the app can resume mid-transfer rather than restart a multi-minute
upload. During OTA the app's OTA screen owns reconnect (the poller is suppressed, §4.5).

### 2.5 MTU / connection interval (throughput)
- **Dongle:** request a large MTU for the OTA session (up to 517; the existing `setMTU(185)`
  telemetry path still works at a higher ceiling). Payload/packet = `MTU − 6`. Request the
  fastest connection interval **only during OTA** (7.5 ms; min=max=0x06, latency=0,
  supervision=4 s) and revert afterward.
- **App reality check — this bounds the negotiation:** the app does **not** call `requestMTU`
  today. On **Android** it can raise it (`connectToDevice({ requestMTU })` / `requestMTUForDevice`)
  — a real lever. On **iOS**, MTU is OS-negotiated and **cannot be forced to 517** (you get
  ~185). So the dongle's request-517 is honored on Android and capped by iOS otherwise; plan
  iOS transfer time around the lower MTU. Raise MTU only for the OTA session; don't disturb
  the telemetry path.

### 2.6 Throughput expectation (shared reality)
Closest reference (fbiego, NimBLE, optimized): ~1,038,544 B in 85 s ≈ **12 KB/s** →
~1.72 MB best-case ~2.4 min; with stop-and-wait on Bluedroid plan **~2.5–5 min**.
Acceptable for a human-present, parked, engine-off operation *if* power/sleep gating holds
for the whole window.

---

## 3. DONGLE SIDE

### 3.1 Partition table & flash (already OTA-ready)
`default_16MB.csv`: `otadata` selector, `app0`/`ota_0` 6.5 MB, `app1`/`ota_1` 6.5 MB,
`spiffs` 3.5 MB (LittleFS), `coredump` 64 KB. Current image **1.72 MB** → ~3.8× headroom
per slot; the "a failed transfer only touches the inactive slot" property is already in
place. **Freeze the table before v1** — `esp_ota_ops` updates only `ota_0`/`ota_1`, never
the bootloader or the table; don't shrink slots. **USB/serial (`esptool`) full-flash
recovery must always work** and is the ultimate backstop.

### 3.2 OTA core — native `esp_ota_ops` (NOT Arduino `Update.h`)
The Arduino reference repos all use `Update.h` and none implement rollback/verify — they
`ESP.restart()` blind. `esp_ota_ops` exposes the safety APIs. Exact staging sequence
(runs on the loop task, §3.6):
```c
#include "esp_ota_ops.h"
const esp_partition_t* tgt = esp_ota_get_next_update_partition(NULL); // inactive slot
esp_ota_handle_t h;
esp_ota_begin(tgt, image_size, &h);          // image_size from START (§2.2)
// per 4 KB sector, flow-controlled:
esp_ota_write(h, sector_buf, sector_len);    // unverified bytes → inactive slot
// on last sector:
esp_ota_end(h);                              // validates header/format/size
// READ-BACK VERIFY before committing (§3.4): re-read tgt block-by-block → SHA-256 → ECDSA verify
esp_ota_set_boot_partition(tgt);             // ONLY commit point; nothing before this is live
esp_restart();
```
**Never verify in RAM** (image ≫ heap): write unverified chunks to flash, then read the
partition back to hash+verify. Commit = `esp_ota_set_boot_partition`; nothing is bootable
until the staged image both `esp_ota_end`-validates AND signature-verifies.

### 3.3 Rollback (native, already enabled)
Stock arduino-esp32 `sdkconfig` has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, so this is
**not** a framework-rebuild risk. The new image boots `ESP_OTA_IMG_PENDING_VERIFY`; it must
run its self-test (§3.5) and call `esp_ota_mark_app_valid_cancel_rollback()` within the boot
window, or the bootloader auto-reverts to the last-good slot on the next reset. On a failed
self-test, call `esp_ota_mark_app_invalid_rollback_and_reboot()` to revert immediately. Read
state with `esp_ota_get_state_partition(running, &state)`. A crash-before-self-test image is
also rolled back natively — the strongest reason to use `esp_ota_ops`.
**Phase-1 unknown to nail on the bench:** confirm the stock arduino startup doesn't already
call `esp_ota_mark_app_valid_*` for us; if it does, the trial window closes before our
self-test runs, and we must take over mark-valid timing so the self-test is meaningful.

### 3.4 Signature verification — ECDSA-P256 + SHA-256 (bundled mbedTLS)
- **Algorithm:** ECDSA over NIST P-256 (secp256r1), message = SHA-256 of the full image.
- **Signing (post-build, off-device):** a PlatformIO `extra_scripts` post-build step hashes
  `firmware.bin`, signs the hash (openssl / python-ecdsa) with the offline private key, and
  emits a detached signature (64 B r||s) + metadata (version, size, sha256). **Private key
  never in repo/project.**
- **On-device verify:** after `esp_ota_end`, read the staged partition back in ~4 KB blocks
  into an `mbedtls_sha256` context, then `mbedtls_ecdsa_read_signature`/verify against the
  public key compiled into firmware. Only on success → `esp_ota_set_boot_partition`.
- **Bind version into the signature:** hash over the image **plus** version+size metadata,
  and keep that metadata *inside* the signed blob, so a valid signature can't be replayed
  against a different declared version.

### 3.5 Health check / self-test — hardware init, NOT connectivity
Separate *firmware* failures from *environmental* ones; a healthy update in a no-signal
garage must NOT roll back. Pass conditions on this device: cellular modem answers `AT`
(existing `CellWrapper`/`check()`), BLE stack initializes (`bleInitForWake()`/advertising
up), OBD/CAN transceiver initializes (`obd.begin`/profile init), filesystem mounts
(`datalogger.begin`). **Do NOT require** network registration, an upload 200, or a paired
phone. Run early in `setup()` when `state == PENDING_VERIFY`, then mark valid/invalid.

### 3.6 Task / threading model (reuse the config-push hand-off)
**All flash/`esp_ota_*` work runs on the loop task, never in the BLE `onWrite` callback** —
the exact discipline that fixed the config-push PANICs (`configCallback` stashes
`g_pending_config_str` + `g_config_pending`; `loop()` applies it; BLE events marshaled via
`bleLogQueue`). For OTA:
- `ABD6` `onWrite` (Bluedroid task): copy the packet into the current sector buffer, update
  seq/offset; when a sector completes, hand the 4 KB buffer to the loop task via a FreeRTOS
  queue (or double buffer + flag). **Do not** call `esp_ota_write` here.
- `loop()`: drain the queue, `esp_ota_write` the sector, then send the sector ACK notify.
  Stop-and-wait means one sector buffer suffices.
- On Bluedroid, `ABD6` writes larger than one MTU arrive as prepared writes — reassemble on
  `EXEC_WRITE` (mirror the Espressif Bluedroid impl buffering up to 600 B).

### 3.7 Power & activity-state integration (reuse existing subsystems)
- **Power gate:** begin/continue a flash only when parked on a healthy RESTING rail. Reuse
  IO36 rail sense (`readIO36Rail`, `g_last_io36`) + ATRV; require > ~12.2 V resting and
  `is_currently_parked`. Refuse/abort on `weak_rail` or driving. Flash-write current spikes
  can trip the hardware brownout detector on a weak battery; OBD-II power can cut with the
  ignition off — the window is narrow. Dual-slot contains a mid-write brownout (only the
  inactive slot corrupts; the old image still boots).
- **Suppress deep sleep for the whole OTA:** hold `maintenance_mode = true` (already blocks
  the sleep state machine for USB sessions) from START until commit/abort, with the existing
  idle auto-release as backstop.
- **New `UPDATING` activity state** (or an `ota_in_progress` flag checked in
  `handleActivityState` and the loop's upload/reconnect blocks): while updating, the OBD
  poller and cellular reconnect/upload back off so nothing contends for CPU/heap or the
  shared modem UART, and the state machine cannot transition to sleep. OTA is pinned to a
  BLE-connected, parked session.
- **Inactivity timeout** (device-side): if no packet arrives for N seconds, abort cleanly
  (free buffers, release `maintenance_mode`) so an app crash can't wedge the device.

### 3.8 Anti-downgrade
Soft NVS monotonic counter (Preferences, already used): store the committed firmware version;
reject a START whose `version` ≤ stored value. `esp_app_desc_t` (via `esp_app_get_description()`)
carries the build version for cross-checks. Enforced on the dongle; app-side version selection
is UX only. **Documented boundary:** NVS is erased by a USB reflash / NVS wipe, so this stops a
*remote* downgrade (an app or attacker pushing an old signed image over BLE — the real threat)
but not a *physical* one by someone who can reflash over USB. That physical case is out of scope
for v1 by design.

---

## 4. APP SIDE

House rules: RTK slices in `src/state`, Biome + TS-strict, i18n for every string
(`no-hardcoded-jsx-strings` fails the build otherwise), unit tests from
`src/state`/`src/util`, additive-only changes that leave the telemetry core untouched.

### 4.1 Navigation + UI
- **New screen** `src/screens/FirmwareUpdateScreen.tsx`, registered in
  `VehicleSettingsModalNavigator.tsx` with route `FirmwareUpdate` (add to `navigation/types.tsx`
  param list). No new navigator, no change to existing routes.
- **Entry point:** one row under the same advanced-OBD area as the PID editor, reusing existing
  List/ListItem components.
- **Gating:** show only when the selected vehicle is OBD-paired (`tlm_type` `obdble`/`abrpobd`)
  and BLE-connected — mirror the OBD-screen checks. Keep it **developer-gated for v1**
  (`isDeveloperUserSelector`); open up once proven.

### 4.2 State — dedicated `firmwareUpdate` RTK slice
Add `src/state/firmwareUpdate/firmwareUpdateSlice.ts` and `SliceNames.FIRMWARE_UPDATE`
(`src/state/sliceNames.ts`). A slice (not component state) because the flow spans navigation,
survives re-render, must be observable by the poller (§4.5), and needs unit tests.
```ts
type FirmwareUpdatePhase =
  | 'idle' | 'checking' | 'available' | 'upToDate'
  | 'downloading'   // fetching the signed blob to the phone
  | 'transferring'  // streaming to the dongle
  | 'verifying'     // dongle verifying + committing (from status notifications)
  | 'rebooting' | 'success'
  | 'failed'        // verify-fail, power-refusal, timeout
  | 'rolledBack';   // rebooted but version did not advance

interface FirmwareUpdateState {
  phase: FirmwareUpdatePhase;
  currentVersion: number | null;    // read from the dongle
  availableVersion: number | null;  // from the release source
  progress: number;                 // 0..1, from ACK cur_sector / total sectors
  error: string | null;             // i18n key/code, never a raw secret
}
```
Actions: `setPhase`, `setVersions`, `setProgress`, `setError`, `reset`. Async in thunks via
`createAppAsyncThunk`. **Throttle progress** before Redux (per-sector / a few Hz) so a
400-sector transfer doesn't dispatch hundreds of actions/sec. **Blacklist the slice from
redux-persist** — it's transient.

### 4.3 Distribution access (public GitHub Releases)
- The signed `.bin` + manifest are public GitHub Releases; no token. HTTP via the
  already-present `axios`; the blob is saved with the already-present `expo-file-system`.
- Select the newest image by `manifest.version` (see Appendix C §2.2), never by tag order.

### 4.4 BLE transfer layer — reuse, don't rebuild
Additive on the **existing** connection. **Do not open a second BLE connection** — reuse
`state.ble.deviceId`.
- Talk to `ABD5`/`ABD6` **directly via `bleManager`** by UUID — the exact pattern
  `abrpObd/setupThunk.ts` uses (`writeCharacteristicWithResponseForDevice`,
  `monitorCharacteristicForDevice`, `readCharacteristicForDevice`). **`bleConnectThunk` does
  not change** (keeps early-returning for `ABD0`); OTA thunks address `ABD5`/`ABD6` by UUID like
  `setupThunk` addresses `ABD2`.
- Control frames (START/END/ABORT + metadata + signature) →
  `writeCharacteristicWithResponseForDevice(deviceId, ABD0, ABD5, base64)`; the ~106 B START uses
  the existing long-write path. Data packets →
  `writeCharacteristicWithoutResponseForDevice(…, ABD6, …)`.
- ACK/status → `monitorCharacteristicForDevice(deviceId, ABD0, ABD5, cb)`; parse the 20-byte
  frame; pace the next sector on ACK (stop-and-wait). Remove the subscription on done/abort.
- **Packetization / framing / CRC16 are pure functions** in `src/state/firmwareUpdate/utils/`
  (`buildSectorPackets`, `parseAckFrame`, `crc16Ccitt`, `compareVersions`) — unit-testable
  without a device.
- Keep the byte-transfer logic in this one `transfer` module so the slice/UI don't depend on how
  bytes move (clean seam; nothing beyond BLE is built in v1).

### 4.5 Coordination with the poller — the one place the app must touch core (safely)
During an update the dongle is `UPDATING` and stops telemetry; the app must not fight it.
**Mirror the proven `isObdBleSetupOngoing` flag — do not invent a new mechanism:**
- Add `isFirmwareUpdateOngoing: boolean` + `setFirmwareUpdateOngoing` to `bleSlice` (next to
  `isObdBleSetupOngoing`). `FirmwareUpdateScreen` sets it true on mount / false on unmount; the
  OTA thunks own it across the transfer.
- `ObdBlePoller` already reads `isObdBleSetupOngoing` in two spots (connected zombie-teardown
  guard, `connect()` wizard guard). Extend **those same two conditions** to also honor
  `isFirmwareUpdateOngoing` — one line each — so the poller neither tears down nor reconnects/
  streams mid-update.
- **Reconnect ownership during OTA is the OTA screen, not the poller.** On a mid-transfer drop,
  the OTA thunk reconnects to `deviceId`, reads `cur_sector` from `ABD5`, resumes. On the final
  reboot, the OTA screen reconnects and reads the version.
- **Do not touch** `getTelemetryUpdate`, merge/attribution, `updateVehicleTelemetry`, the generic
  `obdble` lane, `bleConnectThunk` discovery, or `ABD1`–`ABD4`.

### 4.6 UX & platform considerations
- **iOS backgrounding:** a 2.5–5 min transfer must survive foregrounding; `bluetooth-central`
  background mode keeps the link but throttles background transfers and can suspend a long-idle
  app. Keep the screen active (disable the idle timer during OTA), warn "keep the app open," and
  **rely on the `cur_sector` resume anchor** if the OS suspends.
- **Interruptions** (back-nav, call, low battery) → treat as abort: send ABORT on `ABD5`, clean
  up, release the flag. The dongle inactivity timeout is the backstop if the app vanishes.
- **Progress honesty:** drive the bar from `cur_sector / total` in the ACK, not local bytes
  written (local writes lead what the dongle has committed).
- **No silent failure:** every terminal state maps to a distinct i18n message (verify-failed,
  power-refused, timed-out, rolled-back).

### 4.7 Testing (coverage from `src/state`/`src/util`)
Unit-test (jest-expo) every pure protocol/security function: `compareVersions` (incl. malformed),
`crc16Ccitt` (known vectors), `buildSectorPackets` (4 KB sectoring, headers, last-packet/last-
sector markers, MTU sizing), `parseAckFrame` (status decode, `cur_sector`, CRC validation,
reject corrupt), `firmwareUpdateSlice` reducers (phase transitions, progress throttle, error
set/reset). The live BLE transfer is **not** unit-tested (needs the real `bleManager`) — covered
by bench testing. Gate loop before commit: `yarn format` → `yarn lint` → `yarn type:check` →
`yarn circular` → `yarn test`.

### 4.8 Additive vs. changed files
- **New (additive, isolated):** `FirmwareUpdateScreen.tsx`; `src/state/firmwareUpdate/` (slice,
  thunks, `utils/`); route entry in `VehicleSettingsModalNavigator.tsx` + `types.tsx`;
  `SliceNames.FIRMWARE_UPDATE`, translations, store wiring; one advanced-OBD list row.
- **Touched (minimal, mirror the flag pattern):** `bleSlice.ts` (+`isFirmwareUpdateOngoing`);
  `ObdBlePoller.ts` (extend the two existing guards).
- **Explicitly NOT touched:** telemetry merge/attribution, `updateVehicleTelemetry`, the generic
  `obdble` lane, `bleConnectThunk` discovery, `ABD1`–`ABD4`, redux-persist migrations.

---

## 5. End-to-end flow (both sides, mapped to code)

1. **Enter** `FirmwareUpdateScreen` → app `setFirmwareUpdateOngoing(true)`; read the dongle's
   current version. Poller backs off.
2. **Check** → `checkFirmwareUpdateThunk`: `axios.get` the manifest (GitHub
   Releases) → `compareVersions` → `available` / `upToDate`.
3. **Confirm** (user taps Update) → app shows a precondition pre-warning (park, engine off, keep
   the app open, ~N min). The **dongle** enforces the real gate and refuses START if driving /
   weak rail.
4. **Download** → `axios` → `expo-file-system` temp file (`phase: downloading`).
5. **START** → app writes `0x0001` on `ABD5` (`image_size`+`version`+`sha256`+`signature`).
   Dongle checks preconditions + anti-downgrade, `esp_ota_begin` on the inactive slot, arms.
6. **Transfer** → app streams 4 KB sectors on `ABD6`, stop-and-wait on `ABD5` ACKs; dongle hands
   each sector to the loop task, `esp_ota_write`s, ACKs with `cur_sector`. App updates throttled
   `progress` (`phase: transferring`).
7. **END / verify / commit** → app writes `0x0002`; dongle `esp_ota_end` → read-back SHA-256 →
   ECDSA verify → `esp_ota_set_boot_partition` → `esp_restart` (`phase: verifying` → `rebooting`).
8. **Reboot → reconnect → confirm** → connection drops; the OTA screen reconnects and reads the
   version: advanced → `success`; unchanged → `rolledBack`. Meanwhile the dongle boots
   `PENDING_VERIFY`, runs the hardware self-test, and marks valid or invalid.
9. **Exit / cleanup** → app deletes the temp file, removes the notify subscription,
   `setFirmwareUpdateOngoing(false)`, `reset`. Dongle releases `maintenance_mode`; poller resumes.

---

## 6. Non-negotiables (binding on both sides)
- **Physical USB/serial recovery always works** (`esp_ota_ops` never touches the bootloader or
  partition table). OTA is never the only way back.
- **Partition table sized generously and frozen before v1.**
- **Signing key is the entire trust root:** offline, backed up, access-controlled, ideally HSM.
- **App adds nothing the system's security depends on** — the worst app-side failure is an
  aborted update, never a brick or an unsigned image.

---

## 7. Phasing (highest-risk-first, both sides in lockstep)

**Phase 1 — bench core, unsigned, over BLE (de-risk the mechanics)**
- Dongle: `esp_ota_ops` staging + `set_boot_partition` + `PENDING_VERIFY` self-test +
  mark-valid/rollback. **First task: confirm the stock arduino mark-valid timing** (§3.3) — the
  one genuine unknown. Two GATT chars + sector framing + stop-and-wait ACK + loop-task hand-off
  (§3.6) + power/sleep gating (§3.7) + USB recovery proven. Measure real BLE transfer time.
- App: `firmwareUpdate` slice + `FirmwareUpdateScreen` + poller back-off flag + read dongle
  version + `ABD5`/`ABD6` write/notify wrappers. Prove it drives an *unsigned* bench transfer and
  the poller backs off cleanly (no contention, no zombie teardown, clean reconnect after reboot).

**Phase 2 — real update (security + full flow)**
- Dongle: ECDSA-P256 signature + read-back verify before commit (§3.4); NVS anti-downgrade (§3.8);
  PlatformIO post-build signing step.
- App: manifest check + `compareVersions` + download + full sector protocol with stop-and-wait +
  `cur_sector` resume + throttled progress + all terminal states + i18n. Android `requestMTU`
  raise as the throughput lever. Signature verified **on the dongle**; the app just carries bytes.


---

# Appendix C — OTA wire-protocol reference (authoritative)

> Original file: `ota-protocol-reference.md`

# OTA protocol reference — ground truth as implemented

This is the AUTHORITATIVE contract for the app team, generated from the actual firmware
(`Helpers/ota_update.h`, `Helpers/ota_pubkey.h`, `tools/ota_sign.py`), not from the design
plan. **`ota-update-architecture-v1.md` is the build plan and is mostly still correct**
(GATT layout, sectoring, MTU/throughput sections are accurate) — but three things changed
during implementation and that doc has NOT been updated for them. If you're building
against v1, read §0 first.

---

## 0. Corrections to `ota-update-architecture-v1.md`

1. **§3.7 "require `is_currently_parked`" is WRONG — updates are allowed while driving.**
   Per an explicit product decision, the parked requirement was removed: an update is
   always user-initiated in the app, so the user can choose to run one on the road. The
   dongle does NOT check `is_currently_parked` at START. `OTA_ERR_NOTPARKED` (reject code
   3) still exists in the protocol for forward compatibility but **is never sent** by the
   current firmware. Fix any UI copy that says "you must be parked."
2. **§3.7 power gate is understated.** It's not a flat ">12.2V" floor — it's the same
   three-band logic as the ATRV sleep gate: `rail < 8.0V` (bench/USB, no car) → **allowed**;
   `rail >= 12.2V` (healthy battery) → **allowed**; in between (weak-but-alive battery) →
   **refused**. This is what makes bench testing over USB possible.
3. **v1 documents only the sector ACK frame. There is a SECOND frame type it doesn't
   mention: the status frame**, used for START accept/reject, verify/commit progress, and
   an `INFO` query. See §3.3 below — this is the frame your rejection/error/success UI
   should actually be built against.

Everything else in v1 (GATT UUIDs, sector/packet framing on ABD6, CRC16-CCITT parameters,
MTU/throughput expectations, task/threading model, rollback mechanics) is accurate as
built.

---

## 1. `manifest.json` — exact schema

Produced by `tools/ota_sign.py`. One manifest per firmware version, always paired with
that version's `firmware.bin`.

```json
{
  "version": 2,
  "size": 1763664,
  "sha256": "1b1a3d5c1cf94e2a2536f8724b65b99b40ba9887136f3ff2d7935dc7a20dbad1",
  "signature": "d8d7661753c9be897ad61584b540e6cdce1a5cfd3d0a57dd11007a2434ce91b544d06cea6c3c32b605c73f3abd2883bcdec75573a147961d3c31b8f937356339",
  "sig_alg": "ecdsa-p256-sha256",
  "firmware": "firmware.bin"
}
```

| Field | Type | Meaning | Encoding gotcha |
|---|---|---|---|
| `version` | integer | Monotonic firmware version: the `OTA_FW_VERSION` the image was built with, `major*1000000 + minor*1000 + patch` of `version_name`. Compare this, never the tag. | plain JSON number, not a string |
| `version_name` | string | Release name `X.Y.Z` or `X.Y.Z-rc.N`, for display. Absent from the pre-semver integer releases | an rc and its final share `version` |
| `size` | integer | Exact byte length of `firmware.bin`. | **Verify this against the downloaded file's actual length before doing anything else** — a truncated download is the single most likely bench-test failure, and catching it here costs nothing vs. discovering it after a 3-minute BLE transfer |
| `sha256` | string | SHA-256 of the raw image bytes, hex, **lowercase**, 64 chars = 32 bytes | Must become **32 raw bytes** for the BLE START payload — hex-decode it, don't send the hex string |
| `signature` | string | ECDSA-P256 signature, hex, lowercase, 128 chars = 64 bytes | This is **raw `r‖s`** (32 bytes r, then 32 bytes s, both big-endian) — **NOT DER-encoded**. If you use a crypto library that hands you a DER signature, this is not that; `tools/ota_sign.py` already converts DER→raw before writing this field, so just hex-decode it directly into 64 bytes |
| `sig_alg` | string | Fixed `"ecdsa-p256-sha256"` today. Present so a future algorithm change is detectable; the app doesn't need to branch on it yet, just don't hardcode assumptions elsewhere | |
| `firmware` | string | Filename of the paired binary, always `"firmware.bin"` | informational; you already know the filename from the release layout (§2) |

### What the app must do with these fields
1. Download `firmware.bin`, check `downloaded.length == manifest.size`. Reject and re-download if not — don't hand a truncated file to the dongle.
2. *(Optional, recommended)* Recompute SHA-256 of the downloaded file locally and compare to `manifest.sha256`. This is a fast client-side sanity check, **not** a substitute for the dongle's own verification (which remains the real trust boundary) — it just avoids wasting a BLE transfer on a corrupt download.
3. Hex-decode `sha256` → 32 bytes, hex-decode `signature` → 64 bytes. Build the START payload (§3.2) with those raw bytes, `manifest.size` and `manifest.version` as little-endian integers.

---

## 2. GitHub repo / release layout

The **layout convention** is what the app's fetch code depends on.

### 2.1 Use GitHub Releases, not the repo's file tree
Binary assets belong on **Releases**, not committed into the git history (no bloat, no
LFS needed, up to 2 GB/asset). Keep the repo's actual tree minimal — a `README.md`
explaining what the repo is, optionally a `CHANGELOG.md`. Don't put `.bin` files in commits.

### 2.2 One release per version, tag = `v<N>`

> **Superseded from 0.1.0 onwards:** tags are `vX.Y.Z` / `vX.Y.Z-rc.N` (`RELEASING.md`), so
> `parseInt(tag.slice(1))` no longer orders them. Read every release's `manifest.json` and take the
> max `version`, which is still the integer below.
```
tag: v1, v2, v3, ...     (N = the integer OTA_FW_VERSION, no other format)
```
Trivial to parse (`parseInt(tag.slice(1))`) and matches `manifest.json`'s `version` field
after stripping the `v`. **Do not rely on GitHub's "latest release" flag or on list
order** — parse every tag yourself and take the numeric max, exactly as the earlier app-
side plan already assumed. This also means a release published slightly out of order, or
one you mark `prerelease` for a beta channel, still sorts correctly.

### 2.3 OTA uses two assets, exact filenames
```
firmware.bin
manifest.json
```
Releases also carry `firmware.elf`, `factory.bin` and `SHA256SUMS`; select the two above by name and
ignore the rest. No other naming, no versioned filenames (`firmware-v2.bin` etc.) — the version lives in
the tag and inside `manifest.json`, not in the filename, so fetch code never needs to
string-match a filename pattern.

### 2.4 Fetch sequence (GitHub REST API)
```
GET  https://api.github.com/repos/{owner}/{repo}/releases
     Accept: application/vnd.github+json
```
Returns an array; each entry has `tag_name` and `assets: [{ id, name, size, ... }]`.
Skip `prerelease: true` entries unless the user opted into test firmware, read each remaining
release's `manifest.json`, take the max `version` (break a tie on `version_name`, so a final beats
its rc), and fetch that release's `firmware.bin` by `name`.

```
GET  https://api.github.com/repos/{owner}/{repo}/releases/assets/{asset_id}
     Accept: application/octet-stream
```
Fetch `manifest.json`'s asset this way first (tiny, safe to buffer fully and `JSON.parse`),
then `firmware.bin`'s asset the same way (streamed to disk).

Unauthenticated requests share GitHub's per-IP rate limit (60/hour), which a check-on-open
flow stays well under.

---

## 3. BLE protocol — complete byte-level reference

Service `ABD0` (unchanged, existing pairing/bonding/auth flow untouched). Two chars:

| Char | UUID | Properties |
|---|---|---|
| OTA control | `ABD5` | READ + WRITE + NOTIFY |
| OTA data | `ABD6` | WRITE_NO_RESPONSE |

### 3.1 IMPORTANT: subscribe to ABD5 notifications BEFORE writing START
A GATT write to `ABD5` only gets an ATT-layer ack (the write succeeded as a write) —
the *logical* accept/reject arrives asynchronously as a notification. If you haven't
subscribed to `ABD5` yet, you write START and get no application-level response at all
and no error. Always call your notify-subscribe (`monitorCharacteristicForDevice` /
equivalent) on `ABD5` first, then write.

### 3.2 Control writes on ABD5 (app → dongle)

First 2 bytes = opcode, little-endian:

| Opcode | Value | Payload | Notes |
|---|---|---|---|
| START | `0x0001` | see below | |
| END | `0x0002` | (none) | request commit after all sectors sent |
| ABORT | `0x0003` | (none) | |
| INFO | `0x0004` | (none) | ask for current state without starting anything |

**START payload** (bytes after the opcode), all integers little-endian:

| Offset | Len | Field |
|---|---|---|
| 0..1 | 2 | opcode = `0x0001` |
| 2..5 | 4 | `image_size` (uint32, = `manifest.size`) |
| 6..9 | 4 | `version` (uint32, = `manifest.version`) |
| 10..41 | 32 | `sha256` — raw bytes, hex-decoded from `manifest.sha256` |
| 42..105 | 64 | `signature` — raw `r‖s`, hex-decoded from `manifest.signature` |

Total: **106 bytes**, one write (reuse the long-write path already used for `ABD2`).
sha256 and signature are each **optional at the parser level** (the dongle accepts a
shorter write and just treats them as absent) but **signature is currently required in
practice** — the dongle has a real signing key installed and `OTA_REQUIRE_SIGNATURE` is
on, so any START without all 106 bytes gets rejected with `NOSIG` or `NOKEY` (§3.5). Always
send the full 106 bytes.

**One command in flight at a time**: don't write a second control command until you've
received the notification answering the first one.

### 3.3 Notifications on ABD5 (dongle → app) — TWO frame types, always 20 bytes, always CRC16 in the last 2 bytes

Read `u16LE` at bytes `[0..1]` first to tell them apart:

- If it equals **`0xFFFE`** → **status frame** (§3.3.1)
- Otherwise → **sector ACK frame** (§3.3.2), and the value IS the sector index being acknowledged

Both frames: CRC16-CCITT (`poly=0x1021, init=0x0000`) over bytes `[0..17]`, stored at
`[18..19]` little-endian. Verify it; a corrupt notification should be ignored, not acted on.

#### 3.3.1 Status frame — the one to build your UI against

```
[0..1]  0xFFFE (sentinel)
[2]     phase
[3]     (unused, 0)
[4..5]  detail   (u16 LE, meaning depends on phase - see table)
[6..9]  extra    (u32 LE, meaning depends on phase - see table)
[10..17] (unused, 0)
[18..19] CRC16
```

| Phase | Value | Sent when | `detail` means | `extra` means |
|---|---|---|---|---|
| IDLE | `0x00` | ABORT ack while nothing was running; INFO reply when idle | 0 | current installed version |
| ARMED | `0x01` | START accepted (fresh, `detail=0`) or resumed (`detail`=sector to resume from); INFO reply mid-transfer | sector to (re)start sending from | bytes already staged (0 on fresh start) |
| VERIFYING | `0x03` | after END, while hashing/verifying the staged image | 0 | total bytes written |
| COMMITTED | `0x04` | signature verified, boot pointer moved — **device reboots ~600ms after this** | 0 | the version being committed to |
| ERROR | `0x05` | a step failed after acceptance (write/verify/commit failure) | `OTA_ERR_*` code (§3.4) | bytes written so far |
| ABORTED | `0x06` | ABORT received, or a guard fired (inactivity/session-cap/link-lost/weak-power) | `OTA_ERR_*` code (§3.4) | bytes written so far |
| REJECTED | `0x07` | START refused before anything was staged (no erase happened) | `OTA_ERR_*` code (§3.4) | 0 |
| TRIAL | `0x08` | *(reserved, not currently emitted)* | — | — |
| ROLLEDBACK | `0x09` | INFO reply, when this boot detected the previous update was reverted by the bootloader | current sector (irrelevant here) | current installed version |

**Sequence for a normal successful update:** `ARMED(0,0)` → *(sector ACKs stream)* →
`VERIFYING` → `COMMITTED` → *(link drops, device reboots)*. There is no notification for
a successful boot — you detect success by **reconnecting and reading ABD5** (§3.6), not
by waiting for another notification.

#### 3.3.2 Sector ACK frame

```
[0..1]  sector index being acknowledged (u16 LE)
[2..3]  status: 0x0000 OK, 0x0001 verify-err, 0x0002 sector/packet-out-of-sequence, 0x0003 length-err
[4..5]  next expected sector (u16 LE) - THIS is the resume anchor; also just "cur_sector"
[6..17] (unused, 0)
[18..19] CRC16
```

On `status != 0x0000`, resend **the whole sector named in `[4..5]`** starting from packet
0 — don't try to patch just the failed packet.

### 3.4 Reject / error / abort reason codes (`detail` in a status frame)

| Code | Name | Meaning | App action |
|---|---|---|---|
| 1 | NOAUTH | not authenticated over BLE yet | re-run the auth handshake, retry |
| 2 | BUSY | a session is already running, or resume params didn't match an in-flight resume | don't retry immediately |
| 3 | NOTPARKED | *(reserved, never sent — see §0.1)* | — |
| 4 | WEAKPOWER | battery in the weak band (8–12.2V) | tell the user to wait / check the battery |
| 5 | BADSIZE | outside 256KB–6MB, or bigger than the free OTA slot | bad manifest/build, don't retry |
| 6 | DOWNGRADE | `version` is older than what's installed | this shouldn't happen if you parse tags correctly |
| 7 | TRIALBOOT | the *running* firmware is still an unconfirmed trial boot | wait — it self-resolves within seconds of boot |
| 8 | LOWHEAP | heap too fragmented right now | retry shortly |
| 9 | BEGINFAIL | `esp_ota_begin` failed | retry; if persistent, a dongle-side issue |
| 10 | NOPARTITION | no inactive OTA slot found | dongle-side issue |
| 11 | WRITEFAIL | flash write failed mid-transfer | abort, retry from scratch |
| 12 | SIZEMISMATCH | bytes received at END ≠ declared size | some packets were lost silently — shouldn't happen given the ACK protocol; abort and retry |
| 13 | ENDFAIL | `esp_ota_end` rejected the image (bad header/format) | bad build artifact, don't retry the same file |
| 14 | HASHFAIL | staged image's hash ≠ declared sha256 | corrupted transfer or wrong manifest pairing; re-download and retry |
| 15 | COMMITFAIL | `esp_ota_set_boot_partition` failed | dongle-side issue |
| 16 | INACTIVITY | no data for 30s | you stalled too long between packets — send faster or check the link |
| 17 | SESSIONCAP | session exceeded 15 minutes total | transfer too slow; check MTU/connection interval |
| 18 | LINKLOST | BLE disconnected and didn't reconnect+resume within 90s | start over from scratch, no resume possible now |
| 19 | INTERNAL | shouldn't happen | report it |
| 20 | NOSIG | signature required but START didn't include one | you sent a short START payload — send the full 106 bytes |
| 21 | SIGFAIL | signature present but doesn't verify | wrong manifest/firmware pairing, or the app's key doesn't match what's on the dongle |
| 22 | NOKEY | dongle has no signing key installed | dongle-side — needs `tools/ota_keygen.py` + a USB re-flash |

### 3.5 Data writes on ABD6 (app → dongle, write-without-response)

Only accepted **after** you've received `ARMED` for the current session — anything
written before that (or after the session ends) is silently dropped. Per packet:

```
[0]    sector index LSB
[1]    sector index MSB   (0xFFFF here = "this is the final sector", an alternative to
                            using the real index - either convention works)
[2]    packet_seq         (0, 1, 2, ... within this sector; 0xFF = "last packet of this sector")
[3..]  firmware payload   (up to MTU - 3 ATT - 3 header bytes)
```

- Sector size is always **4096 bytes**. Pack as many packets as fit at your negotiated
  MTU; the dongle just accumulates until it sees `packet_seq == 0xFF`.
- **Strict stop-and-wait per SECTOR, not per packet**: send every packet of sector N
  back-to-back with write-without-response (that's fine — no per-packet ack needed), but
  **do not send any packet of sector N+1 until you've received sector N's ACK**. Packets
  sent early are silently ignored (not buffered, not an error) — so jumping ahead just
  wastes battery/airtime, it can't corrupt anything, but it also won't speed anything up.
- On an ACK with `status != 0`, resend sector `[4..5]` of that ACK from packet 0.
- Final sector: either mark it with the `0xFFFF` sentinel or just its real index — both
  work; either way its last packet still needs `packet_seq = 0xFF`.

### 3.6 Reading ABD5 directly (GATT read, not notify) — a THIRD, JSON representation

When idle (not mid-transfer), a plain GATT read of `ABD5` returns a small JSON blob
instead of a binary frame — use this to show "installed version" in the UI without
starting anything, and to detect success/rollback after reconnecting post-reboot:

```json
{"fw":2,"build":"ota-test-1","signed":1,"state":"idle","rolled_back":0,"sector":0,"sectors":0}
```

| Field | Meaning |
|---|---|
| `fw` | currently installed/confirmed firmware version |
| `build` | human-readable build tag (`FW_BUILD_TAG` in config.h + compile timestamp is in the serial banner, not here — this is just the short tag) |
| `signed` | 1 if this dongle build requires signed images (it does, by default) |
| `state` | `"idle"` \| `"receiving"` \| `"resume_wait"` |
| `rolled_back` | 1 if the *previous* update attempt was reverted by the bootloader this boot session |
| `sector` / `sectors` | current progress if mid-transfer, 0/0 if idle |

**Caveat: during an active transfer, a GATT read of `ABD5` returns the *last notified
binary ACK frame instead* of this JSON** (a BLE characteristic has one value; a notify
necessarily overwrites it). Only rely on the JSON read while idle. To detect
success/rollback after the post-commit reboot: reconnect, re-auth, then **read** `ABD5`
and check `fw` against the version you just pushed — if it matches, success; if it's
unchanged and `rolled_back` is 1, the update was reverted.

---

## 4. End-to-end sequence (updated for the real protocol)

1. Connect + auth (existing flow, unchanged).
2. Subscribe to `ABD5` notifications.
3. **Optional:** write `INFO` (`0x0004`), read the status frame to get the currently
   installed version without needing a GATT read.
4. Fetch `manifest.json` from the release (§2), compare `version` to what's installed.
5. Download `firmware.bin`, verify its length matches `manifest.size` (and optionally its
   sha256).
6. Write START (106 bytes, §3.2). Wait for a status frame: `ARMED` → proceed; anything
   else → show the mapped reason (§3.4) and stop.
7. Stream sectors on `ABD6`, stop-and-wait on `ABD5` sector ACKs (§3.3.2/§3.5). Drive
   progress off the ACK's sector index, not local bytes sent.
8. All sectors done → write END (`0x0002`). Wait for `VERIFYING` then `COMMITTED`.
9. Link drops, device reboots (~600ms after `COMMITTED`). Reconnect, re-auth (auth does
   **not** survive a BLE disconnect — always re-run it).
10. Read `ABD5` (GATT read, not notify) → compare `fw` to the pushed version → success or
    `rolled_back` (§3.6).

**On a mid-transfer disconnect:** the dongle holds the staged session for 90s
(`OTA_RESUME_GRACE_MS`). Reconnect within that window, re-auth, and send an **identical**
START (same size/version/sha256) — the dongle recognizes it as a resume and replies
`ARMED` with the sector to continue from, instead of re-erasing. A non-identical START
during the resume window is rejected `BUSY`. After 90s with no reconnect, the session is
gone (`LINKLOST`) and you must start over from sector 0.

**Timeouts to respect:** no more than 30s between data packets (`INACTIVITY`), and the
whole session (from START to END) capped at 15 minutes (`SESSIONCAP`) — plan your resend
logic to stay well inside both.

---

## 5. Everything else the app should know

- **`ABD1`–`ABD4` and the existing pairing/bonding flow are completely unaffected.** No
  new service, no new pairing step — OTA rides on the connection you already have.
- **Don't open a second BLE connection** for OTA; address `ABD5`/`ABD6` on the existing
  `deviceId`, same as the setup thunk already addresses `ABD2`.
- **BLE preserves write order** within one connection/characteristic — packet *loss* is
  the failure mode the protocol defends against, not reordering. That's why a mismatch is
  handled as "resend the sector," not a reorder buffer.
- **MTU**: request the largest MTU you can before starting (up to 517 on Android via
  `requestMTU`; iOS is OS-capped around ~185 and cannot be forced higher) — this is the
  main throughput lever. Expect **~2.5–5 minutes** for the current ~1.7MB image; plan the
  UI (keep-screen-awake, "don't close the app") around that.
- **Every update must currently be signed.** The dongle has a real ECDSA-P256 key
  compiled in and `OTA_REQUIRE_SIGNATURE` is on. `tools/ota_sign.py` is the only supported
  way to produce a `manifest.json`/`firmware.bin` pair that a dongle will accept — hand-
  built manifests without a valid signature will be rejected (`NOSIG`/`SIGFAIL`), by design.


---

# Appendix D — OTA app-side spec

> Original file: `OTA_UPDATE_APP_SPEC.md`

# Firmware OTA — App-side implementation spec (v1)

Companion to `ota-update-architecture-v4.md` (dongle side). This document covers **only the app
side**: how the update surfaces in the UI, the Redux/state shape, how it reuses (and must not
break) the existing BLE stack, and the considerations that fall out of *how this app actually
works today*. It is written to the repo's house rules (`AGENTS.md`/`CLAUDE.md`): RTK slices in
`src/state`, Biome + TS-strict, i18n for every string, unit tests from `src/state`/`src/util`,
additive-only changes that leave the telemetry core untouched.

---

## 0. Guiding principle — the app is a *dumb, untrusted courier + status screen*

The dongle owns the entire trust boundary (signature verify), the safety (dual-slot, rollback,
power gate), and the commit decision. The app must add **nothing** that the security of the
system depends on. Its job is only:

1. discover that a newer signed image exists,
2. get the bytes to the dongle (or tell the dongle to fetch them),
3. show progress/result and stay out of the way.

Everything below is designed so that if the app is buggy, killed, or malicious, the worst case
is a *failed/aborted* update — never a bricked dongle and never an unsigned image running. That
already falls out of the dongle design; the app spec just must not undermine it (e.g. by fighting
the transfer with the telemetry poller — see §6).

---

## 1. The architecture call to make *before* writing app code

**Who downloads the image — the app (over BLE) or the dongle (over its own cellular link)?**
This single decision determines how much app code exists.

| | **A. App downloads → BLE transfer** (v4 as written) | **B. Dongle self-fetches over cellular** (recommended to evaluate) |
|---|---|---|
| App work | Large: PAT/secure-store, 1.72 MB download, the full `ABD5`/`ABD6` sector protocol, resume, MTU tuning, backgrounding survival, poller back-off | Tiny: one "check/trigger" write + parse status notifications |
| Transfer time | ~2.5–5 min BLE, human-present, parked, app foregrounded the whole time | Dongle's cellular speed, phone can walk away |
| New dependencies | secure-storage lib (§4) unless PAT is dropped | none |
| Credentials on device | PAT lives in the **app** | fetch cred lives on the **dongle** (or public CDN) |
| Failure surface | iOS BLE backgrounding, MTU-on-iOS ceiling, drop/resume | dongle's existing cellular retry |

The dongle already authenticates and POSTs telemetry to `/1/tlm/bulk` over cellular — it has the
exact muscle needed to `GET` a signed blob. Model **B** deletes the hardest ~70% of the app work
(the entire §5 transfer layer) and the narrow "keep the phone glued to the dongle for 5 minutes"
window. v4 defers a "WiFi-HTTP transport" but the dongle's cellular is *already there*.

**Recommendation:** make the app's OTA layer **transport-agnostic** — a state machine + status UI
that is identical whether the bytes move over BLE or cellular — and treat BLE transfer as the
*fallback* for a dongle with no signal, not the default. The spec below writes the full BLE path
(so v1 can ship without dongle-cellular-fetch work) but isolates it behind one `transfer` module
so switching to B later changes ~one file, not the UI/state.

---

## 2. Where it lives (navigation + UI)

The advanced-OBD screens are registered in
`src/navigation/VehicleSettingsModalNavigator.tsx`
(`ObdPidEditorScreen`, `ObdPidEditorTestScreen`, …). Add the feature exactly like those:

- **New screen:** `src/screens/FirmwareUpdateScreen.tsx`, registered in
  `VehicleSettingsModalNavigator` with a route `FirmwareUpdate` (add to the param list in
  `src/navigation/types.tsx`). No new navigator, no change to existing routes.
- **Entry point:** a row under the same advanced-OBD area the PID editor is reached from
  (same gating as `ObdPidEditorScreen` — it already sits behind the advanced/developer surface).
  Reuse the existing List/ListItem components (as `ConnectLiveData`/PID editors do); no new UI kit.
- **Gating:** show the row only when the selected vehicle is OBD-paired (`tlm_type` is `obdble`
  or `abrpobd`) and BLE-connected — mirror the checks already used for the OBD screens. Consider
  keeping it developer-gated for v1 (`isDeveloperUserSelector`) since it's a risky power feature,
  then opening it up once proven.

House rule: **every visible string goes through `t()`** and into `translations/` — the Biome grit
plugin (`no-hardcoded-jsx-strings`) fails the build otherwise.

---

## 3. State — a dedicated `firmwareUpdate` RTK slice

Add `src/state/firmwareUpdate/firmwareUpdateSlice.ts` and a `SliceNames.FIRMWARE_UPDATE` entry
(the enum in `src/state/sliceNames.ts` — same place `BLE`/`OBD_BLE` live; the review-comment we
just handled shows the codebase wants slice-name constants, so use one here from day one).

Why a slice (not component state): the flow spans navigation, survives screen re-render, must be
observable by the poller (§6), and needs to be unit-tested — all reasons the app already uses
slices for BLE/vehicle state.

```ts
type FirmwareUpdatePhase =
  | 'idle'
  | 'checking'        // querying the release source
  | 'available'       // newer version found, awaiting user confirm
  | 'upToDate'
  | 'downloading'     // fetching the signed blob to the phone (BLE model only)
  | 'transferring'    // streaming to the dongle
  | 'verifying'       // dongle verifying + committing (from ACK/status notifications)
  | 'rebooting'       // dongle restarting into the new image
  | 'success'
  | 'failed'          // includes verify-fail, power-refusal, timeout
  | 'rolledBack';     // rebooted but version did not advance

interface FirmwareUpdateState {
  phase: FirmwareUpdatePhase;
  currentVersion: number | null;   // read from the dongle
  availableVersion: number | null; // from the release source
  progress: number;                // 0..1, driven by ACK cur_sector / total sectors
  error: string | null;            // i18n key or code, never a raw secret
}
```

Actions: `setPhase`, `setVersions`, `setProgress`, `setError`, `reset`. Async work lives in
thunks built with `createAppAsyncThunk` (from `src/state/service`), same as every BLE thunk.
Progress must be **throttled** before it hits Redux (e.g. update at most a few Hz, or per-sector)
so a 400-sector transfer doesn't dispatch hundreds of actions/sec — this is the same "keep
middleware traffic low" concern the reviewer raised about the telemetry lane.

---

## 4. No repository token

The channel is untrusted and the signature is the only thing that matters, so distribution is a
public signed artifact: the app `GET`s a public manifest + asset. No secret to store, no new
dependency, no native change. HTTP is done with the already-present `axios`; the blob is saved
with the already-present `expo-file-system`.

---

## 5. The BLE transfer layer (Model A) — reuse, don't rebuild

All of this is *additive* on top of the existing connection. **Do not open a second BLE
connection** — reuse `state.ble.deviceId` (the live session the poller/wizard already manage).

- **New characteristics** `ABD5` (control, WRITE+NOTIFY) and `ABD6` (data, WRITE_NO_RESPONSE) on
  the existing `ABD0` service. The app talks to them **directly via `bleManager`** using the known
  service/char UUIDs — the exact pattern `abrpObd/setupThunk.ts` already uses
  (`writeCharacteristicWithResponseForDevice`, `monitorCharacteristicForDevice`,
  `readCharacteristicForDevice`). This means **`bleConnectThunk` does not need to change** (it can
  keep early-returning for the `ABD0` service); the OTA thunks address `ABD5`/`ABD6` by UUID the
  same way `setupThunk` addresses `ABD2`. Additive, zero risk to the connect path.
- **Writes:** control frames (START/END/ABORT + metadata + signature) →
  `writeCharacteristicWithResponseForDevice(deviceId, ABD0, ABD5, base64)`. Data packets →
  `writeCharacteristicWithoutResponseForDevice(…, ABD6, …)` for throughput. The long START frame
  (~106 B) uses the same long-write path the config push already relies on.
- **ACK/status:** `monitorCharacteristicForDevice(deviceId, ABD0, ABD5, cb)` — parse the 20-byte
  frame (echoed sector, status, `cur_sector`, CRC16-CCITT). Pace the next sector on ACK
  (stop-and-wait), exactly as v4 §6.3 specifies. Remove the subscription on done/abort.
- **Packetization / framing / CRC16 are pure functions** — put them in
  `src/state/firmwareUpdate/utils/` (`buildSectorPackets`, `parseAckFrame`, `crc16Ccitt`,
  `compareVersions`) so they're unit-testable without a device (see §9). This mirrors how the
  abrpobd lane keeps `postProcessTlm`/`stripObdProvidedTelemetry` as testable utils.
- **MTU reality check:** the app does **not** currently call `requestMTU`. On **Android** the app
  can raise it (`connectToDevice({ requestMTU })` or `requestMTUForDevice`) — a real throughput
  lever. On **iOS**, MTU is OS-negotiated and **cannot be forced to 517**; you'll get whatever iOS
  gives (~185). So v4's "request 517" is Android-only; plan the iOS transfer time around the lower
  MTU. Raise MTU only for the OTA session if you add it, and don't disturb the telemetry path.

---

## 6. Coordination with the poller/telemetry — the one place we *must* touch core (safely)

This is the critical "don't break the app" section. During an update the dongle is in its
`UPDATING` state and stops telemetry; the app must not fight it. We already have the exact,
proven pattern for this: the `isObdBleSetupOngoing` flag added for the setup wizard.

**Mirror it — do not invent a new mechanism:**

- Add `isFirmwareUpdateOngoing: boolean` to `bleSlice` (next to `isObdBleSetupOngoing`), with a
  `setFirmwareUpdateOngoing` action. The `FirmwareUpdateScreen` sets it `true` on mount / `false`
  on unmount (dedicated effect, like the wizard), and the OTA thunks own it across the transfer.
- `ObdBlePoller` already reads `state.ble.isObdBleSetupOngoing` in two spots (the connected
  zombie-teardown guard and the `connect()` wizard guard). Extend **those same two conditions** to
  also honor `isFirmwareUpdateOngoing` — so the poller neither tears down the connection nor
  reconnects/streams while an update runs. One-line-each change, identical shape to what's there.
- The abrpobd listener / `abrpFrameWriter` will naturally see no frames (the dongle stopped
  sending), but the flag also prevents the poller from restarting them mid-update.
- **Whoever owns reconnect during OTA is the OTA screen, not the poller** — because the poller is
  suppressed. On a mid-transfer drop, the OTA thunk reconnects to `deviceId`, reads `cur_sector`
  from `ABD5`, and resumes. On the final reboot, the OTA screen reconnects and reads the version.

**Do not touch** anything else in the telemetry core: `getTelemetryUpdate`, the merge/attribution,
`updateVehicleTelemetry`, the generic `obdble` lane, or the existing `ABD1`–`ABD4` characteristics.

---

## 7. Flow (app perspective), mapped to code

1. **Enter** `FirmwareUpdateScreen` → dispatch `setFirmwareUpdateOngoing(true)`; read the dongle's
   current version (a small read on a control char, or from the existing startup/telemetry data).
2. **Check** → `checkFirmwareUpdateThunk`: `axios.get` the public manifest, `compareVersions`,
   set `available`/`upToDate`.
3. **Confirm** (user taps Update) → precondition gate message ("park the car, engine off, keep the
   app open, ~N min") — the dongle enforces the real gate and will refuse START if driving/weak
   rail; the app just pre-warns.
4. **Download** (Model A) → `axios` → `expo-file-system` temp file (`phase: downloading`).
5. **Transfer** → `transferFirmwareThunk`: START on `ABD5` (size+version+sha256+sig) → stream 4 KB
   sectors on `ABD6`, stop-and-wait on `ABD5` ACKs, update throttled `progress` (`phase:
   transferring`).
6. **Verify/commit** → dongle verifies + `esp_ota_set_boot_partition`; app reflects status frames
   (`phase: verifying` → `rebooting`).
7. **Reboot → reconnect → confirm** → connection drops; OTA screen reconnects, reads version:
   advanced → `success`; unchanged → `rolledBack` (self-test failed on the dongle).
8. **Exit / cleanup** → delete temp file, remove notify subscription, `setFirmwareUpdateOngoing
   (false)`, `reset`. The poller resumes normal telemetry.

---

## 8. UX & platform considerations (fallout from how the app runs)

- **iOS backgrounding:** a 2.5–5 min BLE transfer must survive the app being foregrounded; iOS
  `bluetooth-central` background mode keeps the link but **throttles/greatly slows** background
  transfers and can suspend a long-idle app. So: keep the screen active (disable idle timer during
  OTA), warn "keep ABRP open," and **rely on resume** (`cur_sector`) if the OS suspends — don't
  assume an uninterrupted foreground window. This is another argument for Model B.
- **Interruptions:** back-navigation, call, low battery → treat as abort (send ABORT on `ABD5`,
  clean up, release the flag). The dongle's inactivity timeout is the backstop if the app vanishes.
- **Progress honesty:** drive the bar from `cur_sector / total` in the ACK, not from bytes written
  locally (local writes are ahead of what the dongle has committed).
- **No silent failure:** every terminal state maps to a distinct i18n message (verify-failed,
  power-refused, timed-out, rolled-back) so a reviewer/user can tell what happened — the same
  "truthful status" bar philosophy as the connectivity UI.

---

## 9. Testing (house rule: coverage from `src/state`/`src/util`)

Everything security- or protocol-critical on the app side is a **pure function** and must be unit
tested (jest-expo), matching how the abrpobd lane is covered:

- `compareVersions` (newer/older/equal, malformed).
- `crc16Ccitt` against known vectors.
- `buildSectorPackets` (correct 4 KB sectoring, headers, last-packet/last-sector markers, MTU
  sizing).
- `parseAckFrame` (status decode, `cur_sector`, CRC validation, reject corrupt frames).
- `firmwareUpdateSlice` reducers (phase transitions, progress throttle, error set/reset).

The live BLE transfer itself is **not** unit-tested (same reasoning as the telemetry listener —
it needs the real `bleManager`); it's covered by on-device bench testing per the dongle phasing.
Standard gate loop before commit: `yarn format` → `yarn lint` → `yarn type:check` → `yarn circular`
→ `yarn test`.

---

## 10. What is additive vs. what changes existing files

**New (additive, isolated):**
- `src/screens/FirmwareUpdateScreen.tsx`
- `src/state/firmwareUpdate/` (slice, thunks, `utils/`)
- route entry in `src/navigation/VehicleSettingsModalNavigator.tsx` + `types.tsx`
- `SliceNames.FIRMWARE_UPDATE`, translations, store wiring for the new slice
- one advanced-OBD list row (entry point)

**Touched existing (minimal, mirror the proven flag pattern):**
- `bleSlice.ts`: add `isFirmwareUpdateOngoing` + action (parallels `isObdBleSetupOngoing`)
- `ObdBlePoller.ts`: extend the two existing setup-guard conditions to also honor the new flag

**Explicitly NOT touched:** telemetry merge/attribution, `updateVehicleTelemetry`, the generic
`obdble` lane, `bleConnectThunk` discovery, existing `ABD1`–`ABD4` characteristics, redux-persist
migrations (the `firmwareUpdate` slice should be **blacklisted from persistence** — it's transient).

---

## 11. Phasing (app side, mirrors the dongle's highest-risk-first order)

1. **Plumbing, no security:** the `firmwareUpdate` slice + `FirmwareUpdateScreen` + the poller
   back-off flag + reading the dongle version + the `ABD5`/`ABD6` write/notify wrappers. Prove the
   app can drive an *unsigned* bench transfer and that the poller correctly backs off (no telemetry
   contention, no zombie teardown, clean reconnect after the dongle reboots).
2. **Real update:** public-manifest check + `compareVersions` + download + full sector protocol
   with stop-and-wait + resume + progress + all terminal states + i18n. Signature is verified
   *on the dongle*; the app just carries bytes.
3. **Polish / optional:** Android MTU raise, throttled-progress tuning, resume-after-drop UX,
   and — if you go that way — swap the BLE `transfer` module for the cellular-trigger module
   (Model B) with the same slice/UI untouched.

---

## 12. Architecture feedback on v4 (dongle side)

v4 is genuinely strong and has clearly absorbed the earlier round: dropped encryption for the
right reason, USB recovery as a non-negotiable, native `esp_ota_ops` over `Update.h`, read-back-
from-flash verify (no huge RAM buffer), health check = hardware-init not connectivity, power/rail
gating, and a real app-coordination state (`UPDATING`). The read-back verify and "commit =
`esp_ota_set_boot_partition` only after both `esp_ota_end` and signature pass" is exactly right.

Points worth weighing:

1. **Cellular self-OTA (biggest one).** You already ship telemetry over cellular; the whole §6
   BLE transfer protocol, the 2.5–5 min window, MTU-on-iOS limits, and the backgrounding risk are
   avoidable if the dongle fetches its own signed image. v4 defers "WiFi-HTTP" but the transport
   you actually have is cellular. I'd at least prototype dongle-side `GET`+verify+`esp_ota_write`
   over cellular; it may make BLE transfer a fallback rather than v1's core, and it shrinks the app
   to a trigger+status screen.
2. **No credential on the phone.** The signature is the trust root, so distribute a public
   signed artifact.
3. **`ecdsa.h` present / `ed25519.h` absent → ECDSA-P256 is the pragmatic pick.** Fine. Just make
   the signed message unambiguous (hash over image *plus* version+size metadata, so a valid
   signature can't be replayed against a different declared version) and keep the metadata inside
   the signed blob, not alongside it.
4. **NVS soft anti-downgrade is fine for v1**, but note it resets on a USB reflash / NVS erase — so
   it deters a *remote* downgrade (good) but not a *physical* one (that's what the deferred eFuse
   secure-version is for). Document that boundary so it's a conscious v1 limitation.
5. **The stock arduino mark-valid timing** is correctly flagged as the one real unknown — nail it
   on the bench first, because if the platform auto-marks-valid before your self-test runs, the
   whole rollback guarantee is silently void.

Net: the dongle design is sound and close to implementation-ready. The highest-leverage change is
still *where the bytes come from* — solving that in favor of cellular makes the app side almost
disappear and removes the fragile part of the whole system.


---

# Appendix E — Backend expectations

> Original file: `backend_implements.md`

# Backend requirements — ABRP OBD dongle BLE access control

**Audience:** ABRP backend team.
**Status:** revised 2026-08-19. **The scope shrank dramatically** — an earlier draft asked for a signing endpoint,
a key pair, an allowlist and an audit log. That design was replaced by one where the dongle verifies a locally
provisioned secret, so **no new endpoints and no new keys are needed.**

**What remains is confirmation about an endpoint that already exists.** Companion doc: `BLE_SECURITY_PLAN.md`.

---

## 1. Context in four lines

- The ABRP OBD dongle is a standalone device in the car; the phone talks to it over Bluetooth.
- We are making the dongle refuse privileged actions (fetching its log, updating its firmware, changing its
  vehicle profile) unless the phone can prove it holds a secret provisioned into that specific dongle.
- That proof is computed and checked entirely on the phone and the dongle. **The backend is not involved.**
- One existing backend behaviour is load-bearing for a different protection, and we need it confirmed and kept
  stable. That is this whole document.

---

## 2. The one thing we need: keep serving the signed vehicle profile, unchanged

`POST /vehicles/obdble_get_pids` with `signed=true` already returns the vehicle profile as a **JWS compact
string (RS256)**. We need that to continue exactly as-is.

**What changes on our side, for context:** today the app verifies that JWS, discards the signature, and forwards
only the parsed profile to the dongle — so the dongle has no proof the profile is genuine. A profile contains raw
CAN commands, so a forged one can send arbitrary traffic onto a vehicle's bus. We are going to forward **the JWS
verbatim** and have the dongle verify it before use.

### C1 — The RS256 signing key must be stable and long-lived

We will embed its public half in dongle firmware. **Rotating it requires a firmware update to every dongle in the
field**, so please treat it as long-lived and **notify us before any change**. If a rotation is ever planned, we
need lead time to ship firmware first.

### C2 — Please confirm the payload's JSON types  *(most likely to cause a silent failure)*

The app currently coerces numeric fields after verifying (`Number(value.maxValue)`), which **masks any field that
is emitted as a quoted string**. The dongle will consume the signed payload directly and will **not** coerce.

So numeric fields — `minValue`, `maxValue`, and any similar — must be **genuine JSON numbers** in the signed
payload, e.g. `"maxValue": 90` and not `"maxValue": "90"`. If any are currently strings, everything looks fine in
the app today and would fail on the dongle for non-obvious reasons.

### C3 — Please confirm the signed payload is complete and self-contained

Everything the dongle needs to run the profile must be inside the signed payload, since the dongle uses those
bytes rather than anything the app assembles around them.

### C4 — Optional, not required: a version field

A monotonically increasing integer in the signed payload would let the dongle refuse older profiles. Without it,
someone in radio range could re-push an *older, legitimately signed* profile. The impact is limited to reverting
to a different ABRP-authored PID set — not arbitrary command injection — so we are **not** treating it as a
blocker. Only worth doing if it is cheap on your side.

---

## 3. Explicitly NOT needed

To be clear about what the earlier draft asked for and no longer does:

- **No challenge-signing endpoint.**
- **No new key pair**, and no key custody or rotation work.
- **No account allowlist**, and no per-dongle ownership or permission model.
- **No audit log of dongle access.**
- **No nonce storage or validation.**
- No changes to firmware image signing (unchanged, existing tooling and key).
- No changes to telemetry ingest.
- No Bluetooth knowledge or involvement of any kind.

---

## 4. What we need back from you

1. Confirmation on **C1** (signing key stability, and that you will notify us before rotating).
2. Confirmation on **C2** (numeric fields are real JSON numbers in the signed payload) — ideally verified against
   a real response rather than assumed, since this one fails silently.
3. Confirmation on **C3** (payload is self-contained).
4. A yes/no on **C4** (optional version field).


---

# Appendix F — Power management

> Original file: `POWER_MANAGEMENT.md`

# Power Management Reference — OBD_F3 (OnePlus R14) Hardware

This document describes the power architecture of the OBD_F3 board, the standby
current budget, and the firmware changes needed to gate individual power rails so
the device draws microamps in sleep instead of milliamps. Written as a working
reference for implementing low-power features.

> **Hardware revision note:** Several improvements below require *hardware* changes
> (adding/relocating MOSFET switches and regulator EN lines). Where a feature is
> firmware-only on the **current** PCB, it is marked **[FW-NOW]**. Where it needs a
> board change, it is marked **[HW-REV]**. Do not assume a rail can be cut in
> software unless it is marked [FW-NOW].

---

## 1. Power architecture (as built)

```
VCAR (12V, OBD pin 16, ALWAYS HOT)
  └─ D5 1N4001 (reverse protection) ──> VCC12V  (+ D6/D8 TVS transient clamp)
       ├─ U10 MP1582  buck  12V -> VCC5V     EN tied to VIN  -> ALWAYS ON
       │     ├─ U11 MP20045 buck 5V -> VCC3V3  EN tied high   -> ALWAYS ON
       │     │     └─ (most digital logic: ESP32, STM32, flash, MPU9250, LEDs)
       │     ├─ U12 XC6206  LDO  5V -> VCC1V8   fixed          -> ALWAYS ON
       │     │     └─ (M2M level-shifter bias, page 1)
       │     ├─ U9  TJA1050 CAN transceiver   VCC = VCC5V      -> ALWAYS ON  ***
       │     └─ T1  SI2301 P-FET, gated by Q3 <- IO12_MX_PWRKEY -> VCC5V_MX  [FW-NOW]
       │            └─ GSM modem + GPS (the "MX"/M2M section)
       └─ R29/R31 divider -> IO36_BatDec (12V sense, ADC)
```

Key facts that drive everything below:

- **The device cannot disconnect itself from VCAR.** There is no master cutoff
  between the OBD 12V pin and the board. As long as it is plugged in, it draws
  *something*. This is inherent to the hardware.
- **U10, U11, U12 enable pins are hard-wired on.** Their EN lines are not routed to
  any GPIO, so VCC5V / VCC3V3 / VCC1V8 cannot be shut off in firmware on the
  current board. **[HW-REV]** to gate them.
- **The only software-controlled rail today is `VCC5V_MX`** (modem + GPS), via
  `IO12_MX_PWRKEY` -> Q3 -> SI2301 high-side switch. **[FW-NOW]**
- ***** **The TJA1050 CAN transceiver (U9) has no low-power/standby mode.** Per the
  NXP datasheet its recessive supply current is ~5 mA typical, 13 mA max, and the
  only way to reduce it is to remove its VCC. On the current board its VCC is the
  always-on VCC5V, so **it burns ~5 mA continuously even in deep sleep.** This is
  the single largest fixable sleep load.

---

## 2. Standby current budget

All currents referred back to the 12V input (VCAR), since that is what drains the
car battery. Buck conversion assumed ~75% at light load; the 3.3V rail is a double
conversion (12->5->3.3). Component figures are datasheet typicals.

| Consumer | Rail | Current on rail | Equiv. @12V | Gateable? |
|---|---|---|---|---|
| MP1582 (U10) quiescent | 12V | 100 µA | 100 µA | no (always-on reg) |
| MP20045 (U11) quiescent | 5V | ~100 µA | ~56 µA | no |
| XC6206 (U12) quiescent | 5V | ~1 µA | <1 µA | no |
| **TJA1050 (U9) recessive** | **5V** | **~5 mA (13 mA max)** | **~2.8 mA** | **only by cutting VCC [HW-REV]** |
| ESP32 (U6) deep sleep | 3.3V | 0.15–0.8 mA | 0.06–0.3 mA | partial (sleep modes) |
| STM32 (U8) stop mode | 3.3V | ~50 µA | ~20 µA | partial |
| W25Qxx flash | 3.3V | 1 µA (deep PD) / 4 mA (idle) | up to 1.5 mA | yes, via SPI cmd |
| MPU9250 IMU | 3.3V | ~8 µA (sleep) | ~3 µA | yes, via reg |
| Pull-ups / LEDs / leakage | 3.3V | ~0.5 mA | ~0.2 mA | partial |

**Computed totals (sleep, referred to 12V):**

| Scenario | Sleep current @12V | Time to 50% of a healthy 60 Ah battery |
|---|---|---|
| A — best case as-designed (MCU deep sleep, modem/GPS off, flash in deep power-down) | **~3.6 mA** | ~347 days |
| B — realistic as-designed (flash + misc left active) | **~5.5 mA** | ~225 days |
| C — worst case (TJA1050 at 13 mA max) | **~10 mA** | ~125 days |
| D — proposed, with CAN transceiver powered off + flash deep-PD + ESP32 min sleep | **~0.28 mA** | >10 years (battery self-discharge dominates first) |

The dominant term in A/B/C is the **TJA1050 at ~2.8 mA-equiv**. Killing it is ~75%
of the available sleep-current savings and moves the board from "milliamps" to
"hundreds of microamps."

### 2.1 Important reconciliation with the field failure

A 3–10 mA sleep load takes **months** to flatten a healthy battery — it does **not**
explain a car going dead overnight (~15 h). For that you need on the order of
**hundreds of mA to >1 A average** over the night (e.g. 40 Ah aux battery losing
30–50% in 15 h implies ~0.7–1.3 A average draw).

This means one or both of:

1. **The firmware was not actually sleeping.** The telemetry showed the device
   cycling `POWERON` / `MOTION` / `HEARTBEAT` and repeatedly logging `http_err`
   (502/400) — i.e. the cellular radio was attached and retrying, not powered down.
   An ESP32 with the modem active is tens to >100 mA continuous, with TX bursts far
   higher. That is the most likely overnight killer, not the sleep floor.
2. **The aux battery (likely a small EV 12 V battery, 35–45 Ah) was already weak**,
   so even a modest continuous load finished it off.

**Therefore the firmware work has two equally important halves:**

- **(a) Reduce the sleep floor** with rail gating (the µA budget above), and
- **(b) Guarantee the device actually reaches and stays in that sleep state** —
  bounded ret/backoff on network errors, a hard "parked + idle -> deep sleep"
  transition, and a low-voltage cutoff. Rail gating alone is useless if the radio
  thrashes all night.

---

## 3. Rail/peripheral control map

What firmware can drive **on the current board**:

| Net / signal | GPIO (ESP32) | Controls | Active level | Status |
|---|---|---|---|---|
| `IO12_MX_PWRKEY` -> Q3 -> SI2301 | IO12 | `VCC5V_MX` = modem + GPS power | drive to enable FET | **[FW-NOW]** |
| `IO27_GSM_PWRKEY` | IO27 | GSM module PWRKEY (power on/off sequencing) | pulse per modem datasheet | [FW-NOW] |
| `PB15_MX_PWRKEY` | (STM32 side) | secondary modem power-key path | — | [FW-NOW] |
| W25Qxx `/HOLD`,`/CS` + SPI cmd | IO16/IO17 | flash deep power-down (cmd 0xB9) / release (0xAB) | SPI command | **[FW-NOW]** |
| MPU9250 `nCS`/I2C | IO21/IO22 | IMU sleep (PWR_MGMT_1 bit) | register write | **[FW-NOW]** |
| TX/OBD indicator LEDs (D3/D4) | IO4 / OBD_IND | status LEDs (R30/R32 470R, ~7 mA each when lit) | drive low to extinguish | **[FW-NOW]** |
| `IO36_BatDec` (ADC) | IO36 | 12V sense via R29/R31 divider | read-only | [FW-NOW] |

What requires a **board revision** to gate:

| Rail | Why it can't be gated now | Proposed HW change |
|---|---|---|
| VCC5V (U10) | EN hard-tied to VIN | Route U10 EN to an ESP32 GPIO with pull-up; or add a load switch on the TJA1050 VCC specifically (cheaper, lower risk) | **[HW-REV]** |
| VCC3V3 (U11) | EN hard-tied high | Route U11 EN to a GPIO — but the ESP32 itself lives on 3.3V, so this can only be cut by an external always-on keep-alive (e.g. STM32 as supervisor). Usually NOT worth it. | **[HW-REV]** |
| TJA1050 VCC | tied to VCC5V | **Highest-value change:** put a small P-FET high-side switch (like the existing SI2301 pattern) on the TJA1050 VCC, gated by a spare GPIO. Saves ~2.8 mA-equiv. | **[HW-REV]** |

---

## 4. Proposed firmware low-power state machine

Replace "just call `esp_deep_sleep()`" with explicit power states. The principle:
**power down every peripheral you can, in order, before sleeping the MCU — and
gate re-entry on real conditions, not a fixed timer.**

```
            ┌─────────────┐
            │   BOOT      │
            └──────┬──────┘
                   v
            ┌─────────────┐   parked && idle timeout && batt OK
   ┌───────>│   ACTIVE    │──────────────────────────────────┐
   │        │ (driving)   │                                   v
   │        └─────────────┘                          ┌─────────────┐
   │  motion / CAN wake          periodic wake        │  LIGHT_IDLE │
   │        ▲                    (e.g. every N min)    │ radio off,  │
   │        │                          │               │ CAN off*,   │
   │        │                          │               │ LEDs off    │
   │        │                          v               └──────┬──────┘
   │        │                   ┌─────────────┐                │ stays idle
   │        └───────────────────│  HEARTBEAT  │<───────────────┘
   │                            │ brief radio │
   │                            │ push, then  │      battery < threshold
   │                            │ back to deep│──────────────────────┐
   │                            └──────┬──────┘                       v
   │                                   │ done              ┌─────────────────┐
   │                                   v                   │ LOW_BATT_LOCKOUT│
   │                            ┌─────────────┐            │ everything off, │
   └────────────────────────────│ DEEP_SLEEP │            │ longest timer,  │
       wake source fires        │ all rails  │            │ no radio at all │
                                │ gated, MCU │            └─────────────────┘
                                │ deep sleep │
                                └─────────────┘
* CAN off = firmware-only "stop using it" today; true power-off needs [HW-REV].
```

### State definitions

- **ACTIVE** — car is running / recently moved. All peripherals on, normal logging.
- **LIGHT_IDLE** — parked, recently stopped. Radio off, flash in deep power-down,
  IMU in motion-wake mode, LEDs off. MCU light-sleep with wake on IMU INT or timer.
- **HEARTBEAT** — periodic wake from deep sleep to send a short status push, then
  immediately tear down the radio and return to deep sleep. **Bounded**: max N
  retries with exponential backoff, hard ceiling on radio-on time per wake.
- **DEEP_SLEEP** — everything gateable is off; MCU in deep sleep with wake sources =
  IMU motion INT, CAN activity (if transceiver powered), or RTC timer.
- **LOW_BATT_LOCKOUT** — entered when `IO36_BatDec` reads below threshold. Radio
  fully disabled, only a long-interval voltage check. Protects the car battery.
  This is the state that would have saved your car.

### Power-down sequence before sleeping (order matters)

```c
// Pseudocode — adapt pin macros to your HAL / board header.

void enter_deep_sleep(uint32_t wake_after_s) {
    // 1. Tear down network first (biggest load).
    modem_send_powerdown();             // graceful detach, AT+CPOWD or PWRKEY pulse
    gpio_set(IO27_GSM_PWRKEY, /*off*/); // sequence per modem datasheet
    gpio_set(IO12_MX_PWRKEY, RAIL_OFF); // [FW-NOW] cut VCC5V_MX -> modem + GPS dead

    // 2. Quiet the bus. On current HW this is firmware-only; true power-off = [HW-REV].
    can_stop();                         // stop TWAI/CAN driver
    // if [HW-REV] TJA1050 load switch present:  gpio_set(CAN_PWR_EN, RAIL_OFF);

    // 3. Flash into deep power-down (0xB9). Saves up to ~4 mA if it was idle-active.
    spi_flash_deep_power_down();

    // 4. IMU: configure motion-wake (low-power accel) or full sleep if not used as wake.
    mpu9250_set_low_power_motion_wake(threshold_mg, odr_hz);  // asserts INT on motion

    // 5. Kill indicator LEDs (~7 mA each when lit).
    gpio_set(IO4_TXD_LED, OFF);
    gpio_set(OBD_IND, OFF);

    // 6. Configure wake sources.
    esp_sleep_enable_ext0_wakeup(MPU_INT_GPIO, /*level*/);   // motion
    if (wake_after_s) esp_sleep_enable_timer_wakeup((uint64_t)wake_after_s * 1000000ULL);
    // optional: CAN/UART wake only if that transceiver is still powered.

    // 7. Float / pull GPIOs so no rail is back-fed through a driven pin.
    quiesce_gpios();                    // set unused pins to input, disable internal pulls
                                        // that would otherwise leak through level shifters

    // 8. Finally, sleep the MCU.
    esp_deep_sleep_start();
}
```

### Wake handler

```c
void on_wake(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // ALWAYS check battery before doing anything power-hungry.
    int mv = read_batt_decivolts() * 100;     // via IO36 ADC + R29/R31 scale
    if (mv < LOW_BATT_LOCKOUT_MV) {            // e.g. 11800 mV; tune to your aux battery
        enter_low_batt_lockout();              // long timer, radio stays OFF
        return;
    }

    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:            // motion
            transition(STATE_ACTIVE);
            break;
        case ESP_SLEEP_WAKEUP_TIMER:           // scheduled heartbeat
            spi_flash_release_deep_power_down(); // 0xAB
            do_bounded_heartbeat();            // see retry policy below
            enter_deep_sleep(HEARTBEAT_INTERVAL_S);
            break;
        default:
            enter_deep_sleep(HEARTBEAT_INTERVAL_S);
    }
}
```

### Bounded network policy (this is what stops the overnight drain)

The logged `http_err` storm shows the old firmware retrying the radio indefinitely.
Enforce hard limits:

```c
#define HB_MAX_RADIO_ON_MS    20000   // never keep radio up >20s per heartbeat
#define HB_MAX_RETRIES        3       // then give up THIS cycle
#define HB_BACKOFF_BASE_S     60      // exponential: 60, 120, 240 ... capped
#define HB_BACKOFF_MAX_S      3600

void do_bounded_heartbeat(void) {
    gpio_set(IO12_MX_PWRKEY, RAIL_ON);   // power modem+GPS
    modem_power_on_sequence();
    uint32_t t0 = millis();
    int tries = 0;
    bool ok = false;
    while (tries < HB_MAX_RETRIES && (millis() - t0) < HB_MAX_RADIO_ON_MS) {
        if (push_status() == 0) { ok = true; break; }
        tries++;
    }
    // ALWAYS power the radio back down, success or fail.
    modem_send_powerdown();
    gpio_set(IO12_MX_PWRKEY, RAIL_OFF);

    // On failure, lengthen the next interval (don't hammer the network/battery).
    next_hb_interval_s = ok ? HEARTBEAT_INTERVAL_S
                            : min(next_hb_interval_s * 2, HB_BACKOFF_MAX_S);
}
```

---

## 5. Concrete task list for the firmware change

**Firmware-only, do these first ([FW-NOW]):**

1. Implement the state machine in §4; replace any bare `deep_sleep()` calls.
2. Add the `enter_deep_sleep()` power-down sequence — especially:
   - cut `VCC5V_MX` via `IO12_MX_PWRKEY` (modem + GPS) on every sleep;
   - put W25Qxx flash into deep power-down (0xB9) and release (0xAB) on wake;
   - put MPU9250 into low-power motion-wake;
   - extinguish D3/D4 LEDs.
3. Add the **bounded network retry/backoff** policy (§4). This is the single most
   important change for preventing the overnight drain.
4. Add the **low-battery lockout** using the existing `IO36_BatDec` ADC sense.
   Pick a conservative threshold for the car's aux battery and stop all radio
   activity below it.
5. Stop/disable the CAN driver (`can_stop()`) before sleep so the MCU isn't kept
   awake servicing it, even though the transceiver stays powered for now.
6. `quiesce_gpios()` — set unused pins to input and disable internal pull-ups that
   could leak through the M2M level shifters (page 1 transistors).

**Needs a board revision to realize the full µA budget ([HW-REV]):**

7. Add a P-FET high-side load switch on the **TJA1050 VCC** (mirror the existing
   Q3/SI2301 `VCC5V_MX` circuit), gated by a spare ESP32 GPIO. This removes the
   ~2.8 mA-equiv always-on CAN load — the biggest remaining sleep term. When CAN
   wake is needed, leave it powered; when sleeping with timer/motion wake only,
   cut it.
8. (Optional, lower value) Route U10/U11 EN pins to GPIOs if a supervisor MCU
   architecture is ever adopted. Generally not worth it because the ESP32 lives on
   the rail it would be cutting.

**Validation:**

9. Measure actual sleep current at the OBD 12V pin (inline ammeter) in each state.
   Target after [FW-NOW] work: low single-digit mA (limited by the always-on
   TJA1050). After [HW-REV] CAN switch: a few hundred µA.
10. Soak test parked overnight and confirm the device reaches DEEP_SLEEP and stays
    there, with the radio off, and that `IO36_BatDec` trends flat.

---

## 6. Summary

- The hardware can **partially** power-manage: today, firmware can cut the
  modem+GPS rail (`VCC5V_MX`), deep-power-down the flash, sleep the IMU, and kill
  the LEDs. It **cannot** cut VCC5V/VCC3V3 or the CAN transceiver without a board
  change.
- The **TJA1050 CAN transceiver** is the dominant fixable sleep load (~2.8 mA
  @12V); eliminating it needs a small [HW-REV] load switch and takes the sleep
  floor from ~3.6 mA to ~0.28 mA.
- But the overnight battery death was almost certainly **not** the sleep floor — it
  was the device **failing to sleep** (radio thrashing, per the `http_err` log)
  and/or a weak aux battery. The firmware must both lower the floor **and**
  guarantee it actually reaches sleep, with bounded retries and a low-battery
  lockout.


---

# Appendix G — Power budget (bench measurements)

> Original file: `POWER_BUDGET.md`

# Power Budget — measured currents vs. the production state machine

Energy model for `baseLogger/logger.ino`, built from bench measurements taken 2026-08-17 on a
Freematics ONE+ at a supply of ~11.4 V, cross-checked against the state machine in
`Helpers/state_machine.h` and the timings in `config.h`.

---

## 1. Measured currents

| State | Current | Notes |
|---|---|---|
| ACTIVE, driving, cellular | **86 mA** resting, **130 mA** spikes | spikes = TLS POST; ACTIVE posts every 5 s |
| BLE data collection (phone connected) | **80 mA** | modem still powered — production powers it every wake |
| BLE log fetch (ABD7 dump) | **102 mA** | + flash reads and a high-duty BLE link |
| Deep sleep, first entry | **27 mA** | |
| Deep sleep, after a WOM wake cycle | **38 mA** | ⚠ see §5 |
| WOM wake (tap) | ~70 mA brief | |
| Parked timer wake (gate skip) | ~60 mA for ~5 s | negligible: ~0.1 mAh/day over 48 wakes |

**Cross-check.** An independent bench build with the same module set
(IMU+GPS+OBD+CELL+BLE, no WiFi) measured 80 mA resting / 130 mA spikes. Agreement with the 86 mA
production figure is within the rig's ±10 mA repeatability, so the awake numbers are trustworthy.

### ACTIVE average

ACTIVE polls *and* uploads every 5 s (`getTransmissionDelay(ACTIVE) == 5000`), so the current is a
duty cycle between 86 and 130 mA. A TLS POST occupies roughly 0.5–2 s of each 5 s window:

| Duty | ACTIVE average |
|---|---|
| 10 % | 90 mA |
| **25 % (used below)** | **97 mA** |
| 50 % | 108 mA |

---

## 2. What the state machine actually does

From `handleActivityState()` and `config.h`:

- `ACTIVE_TIMER` = 5 min, `CHARGING_TIMER` = 5 min, `MAX_AWAKE_MINUTES` = 60
- `WAKE_PARKED_MINUTES` = 30, `ENABLE_PARKED_HEARTBEAT` = 0, `ATRV_GATE_ENFORCE` = 1

**Idle-to-sleep is ~5 minutes, not ~10.** Both timers are compared against the *same*
`last_active` stamp, which is refreshed only by motion/speed (logger.ino:1081, 1102):

```
ACTIVE  -> CHARGING  when now - last_active > ACTIVE_TIMER    (5 min)
CHARGING-> STANDBY   when now - last_active > CHARGING_TIMER  (5 min)
```

By the time CHARGING is entered, its own exit condition is already true, so CHARGING and STANDBY
are traversed in one or two loop iterations and the device sleeps. The `config.h` comment
("Idle-to-sleep = ACTIVE + CHARGING = ~10 min") describes an intent the code does not implement.
The bench observation — sleep about 5 minutes after motion stops — confirms the 5-minute
behaviour. Either fix the comment or set `CHARGING_TIMER` to 10 min if 10 was intended.

**Parked cycle.** Deep sleep → 30-min timer wake → ATRV gate reads both voltages → parked rail
(not `rail_awake`, not motion, not cold, heartbeat disabled) → skip → straight back to sleep,
modem never powered. 48 such wakes per day contribute ~0.1 mAh — irrelevant to the budget.

---

## 3. 24 h with two separate 1 h drives

Assumptions: ACTIVE = 97 mA, sleep = 27 mA (and 38 mA as the pessimistic case), 5-minute tail
after each drive, remainder in deep sleep.

| Segment | Duration | Current | Energy | Paid by |
|---|---|---|---|---|
| Drive 1 + 2 (ACTIVE) | 120 min | 97 mA | 194 mAh | car DC-DC (engine/HV on) |
| Post-drive tails (2 × 5 min) | 10 min | 97 mA | 16 mAh | **12 V battery** |
| Deep sleep | 1310 min | 27 mA | 590 mAh | **12 V battery** |
| **Total / day** | 1440 min | | **800 mAh** (9.1 Wh) | |
| — of which from the battery | | | **606 mAh** | |

With sleep at 38 mA: sleep = 830 mAh, daily total **1040 mAh** (11.9 Wh), battery share
**846 mAh**.

**Deep sleep is 74 % of the daily energy** (80 % at 38 mA) even on a day with two hours of
driving. Everything else is rounding error.

---

## 4. Scenarios

Battery-side energy only (drive-time consumption is carried by the car's DC-DC while the vehicle
is awake, so it does not deplete the 12 V battery).

| Scenario | Sleep @ 27 mA | Sleep @ 38 mA |
|---|---|---|
| A. Parked all day, no drives | 648 mAh | 912 mAh |
| B. Two 1 h drives (baseline §3) | 606 mAh | 846 mAh |
| C. B + 30 min BLE data collection | 633 mAh | 866 mAh |
| D. B + 15 min BLE log fetch | 625 mAh | 861 mAh |
| E. B + 1 h BLE + 15 min log fetch | 677 mAh | 904 mAh |
| F. B, but sleep fixed to 10 mA | **235 mAh** | — |

Days to 50 % depth-of-discharge on a 60 Ah battery (30 Ah usable, dongle load only, ignoring the
car's own parasitic draw):

| Case | Days |
|---|---|
| B @ 38 mA sleep | **35** |
| B @ 27 mA sleep | **50** |
| B @ 10 mA sleep | **128** |

**BLE sessions barely register.** An hour of BLE plus a log fetch adds ~70 mAh — about 12 % of a
day. Optimising them is not worth the effort; the sleep current is the entire problem.

---

## 5. The two findings worth acting on

### 5a. The datasheet's "low-power mode" is never invoked

The ONE+ datasheet: *"Freematics ONE+ can enter and exit low-power mode programmatically. When GPS,
cellular, and Wi-Fi are powered off, power consumption is around 10mA."* That programmatic
low-power mode is `ATLP` — `COBD::enterLowPowerMode()`, which sends `ATLP\r` to the co-processor.

**`ATLP` appears nowhere in the firmware.** Verified across `logger.ino`, all of `Helpers/`, and
`OBDWrapper` — `enterLowPowerMode()` is never called. The co-processor therefore runs at full speed
through every deep sleep.

Note also what the 10 mA figure describes: radios off with the *main controller still running*
("allowing the main controller to run lightweight tasks such as motion detection"). Deep sleep,
where the ESP32 itself is down, should land at or **below** 10 mA — so 27 mA is worse than a
2.7× gap, not better.

**Test:** the bench sketch's `l` toggle exists exactly for this A/B —
`OBD_LOW_POWER_DEFAULT_ON 1` with `BENCH_AUTO_SLEEP_SECONDS 60`, then compare the sleep figure.
Per §4 case F, getting sleep to 10 mA would extend parked life from ~5 weeks to ~4 months.

**Fix** — before `esp_deep_sleep_start()`:

```c
obd.enterLowPowerMode();   // ATLP: the datasheet's low-power mode
```

and on wake, before the OBD profile is applied, `obd.leaveLowPowerMode()` (it retries `ATI` up to
30× until the co-processor answers).

### 5b. `powerOff()` can turn the modem back ON — explains the 27 → 38 mA step

`CellWrapper::powerOff()`:

```c
char b[64] = { 0 };
sys.xbPurge();
sys.xbWrite("AT+CPOWD=1\r");
sys.xbReceive(b, sizeof(b), 3000);        // <-- no expected[] array
if (!strstr(b, "POWER DOWN")) {
  sys.xbTogglePower(2510);                // <-- powers an ALREADY-OFF modem back ON
}
```

Two problems compound:

1. **No expected-match array**, so `xbReceive` never returns early — it spins the full 3000 ms
   accumulating everything the modem emits. `cellwrapper.cpp:337` states the rule this violates:
   *"every xbReceive() MUST pass expected-match strings."*
2. **`bufsize` is 64**, and `xbReceive` does
   `if (bytesRecv >= bufsize - 16) bytesRecv -= dumpLine(buffer, bytesRecv);` — past 48 bytes it
   **discards the oldest line**. `NORMAL POWER DOWN` is the *first* thing emitted, so any
   unsolicited output arriving in those 3 s pushes it out of the buffer.

When that happens `strstr` fails and the fallback asserts PWRKEY for 2.5 s on a modem that has
already powered down — switching it back on. The device then deep-sleeps with a booting, then
registering, SIM7070: ~10–20 mA, matching the observed +11 mA. Whether the ack survives depends on
network chatter in those 3 s, which is exactly why it is intermittent.

`sleep_power.h` already documents the hazard for the skip path: *"powerOff() ... would
PWRKEY-toggle an already-off modem back ON."*

**Fix** — match on the ack, and never toggle PWRKEY without confirming the modem still answers:

```c
char b[160] = { 0 };
const char* expect[] = { "POWER DOWN", "OK", "ERROR" };
sys.xbPurge();
sys.xbWrite("AT+CPOWD=1\r");
int r = sys.xbReceive(b, sizeof(b), 3000, expect, 3);
if (r != 1) {                                   // no POWER DOWN seen - is it actually still up?
  char probe[64] = { 0 };
  const char* ok[] = { "OK" };
  sys.xbPurge();
  sys.xbWrite("AT\r");
  if (sys.xbReceive(probe, sizeof(probe), 800, ok, 1) == 1) sys.xbTogglePower(2510);
}
m_ready = false;
```

### 5c. GNSS power is latched in the CO-PROCESSOR, not the ESP32

`gpsBegin()` sets `FLAG_GNSS_USE_LINK` and sends **`ATGPSON`** to the co-processor; `gpsEnd(true)`
sends **`ATGPSOFF`**. GNSS power therefore lives in a separate MCU that the ESP32's deep sleep and
reboot do not touch.

Production gates the shutdown on a RAM flag that a reboot clears:

```c
if (g_gps_powered) sys.gpsEnd(true);   // g_gps_powered is a plain global
```

So **any reset between `ATGPSON` and the next clean `enterDeepSleep()` leaves GNSS powered
indefinitely** — a WDT/panic reset (the boot-153 wedge class; WDT resets are present in the bench
journal), a brownout, or a reflash mid-wake. Every subsequent gate-skip wake has
`g_gps_powered == false`, so `ATGPSOFF` is never sent again. The WDT-streak escalation path is the
same shape: it calls `enterDeepSleep(true)` *before* the GPS block, so the flag is false there too.

An active GNSS receiver is ~20–40 mA, easily the largest single contributor.

**Fix** — make the pre-sleep shutdown unconditional. Do **not** simply drop the `g_gps_powered`
guard: if `gpsBegin()` was never called this boot, `FLAG_GNSS_USE_LINK` is unset and `gpsEnd()`
takes its *other* branch (`taskGPS.destroy()`, `uart_driver_delete()`) on resources that were never
created. Send the co-processor command directly instead — idempotent, no branch:

```c
{ char b[16]; if (sys.link) sys.link->sendCommand("ATGPSOFF\r", b, sizeof(b), 100); }
```

### 5d. Latent: the WiFi RF-flush path bypasses the whole power-down

`logger.ino:696` calls `esp_deep_sleep_start()` directly, skipping `enterDeepSleep()` — so no
modem power-off, no `ATGPSOFF`, no BLE deinit, no WOM arming. Harmless today (`ENABLE_WIFI 0`), but
it becomes a real leak the moment WiFi is re-enabled.

---

## 6. Caveats

1. **Supply voltage.** Measured at ~11.4 V; a car rests at ~12.6 V. The buck is roughly
   constant-power, so currents in the car will be ~10 % lower for the same energy. Quote the Wh
   figures when comparing.
2. **11.4 V is dangerously close to a firmware threshold.** `IO36_LOCKOUT_HIGH` is 11.0 V — only
   0.37 V below the bench rail. Sag under 11.0 V and the weak-rail lockout suppresses *all*
   polling, so every wake becomes a skip and the behaviour under measurement changes completely.
   Run the bench supply at ~12.6 V.
3. **No car attached.** OBD init failed in every bench run, so there is no real CAN traffic —
   `readPIDs` times out rather than exchanging frames. Expect a small increase with a live bus.
4. **GNSS never locked** (indoors, still searching). A searching receiver draws at least as much as
   a locked one, so this is conservative.
5. **Spikes are averaged.** A DMM samples a few times per second; real SIM7070 TX bursts reach
   1–2 A for milliseconds. 130 mA is a smeared average — correct for energy, useless for sizing a
   fuse or supply.
6. **Context:** a car's own parasitic drain is typically 20–50 mA, so at 27–38 mA this dongle
   roughly doubles it. At 10 mA it would be a minor addition.


---

# Appendix H — Deferred power work (read before touching sleep)

> Original file: `DEFERRED_POWER_WORK.md`

# Deferred work — sleep floor and radio hardening

Companion to `POWER_BUDGET.md` and `POWER_MANAGEMENT.md`. Everything here was **reviewed and deliberately
NOT implemented** because it needs bench measurement or carries a risk that only testing can retire. The
zero-risk subset was landed on 2026-08-18 (see §7).

Each item: **what**, **why deferred**, **what could go wrong if done blind**, **how to settle it**.

---

## 1. The sleep floor — the ~15-20 mA co-processor question

The STM32F103 runs from `VCC3V3` (U11/MP20045 off `VCC5V`), always on, with VBAT/VDDA/VDD1-3 hardwired.
**There is no firmware power gate for the co-processor.** The measured 27 mA sleep floor minus the TJA1050
(~5-10 mA, always powered, no standby) minus ESP32 deep sleep (~10 µA) leaves the STM32 at roughly
**15-20 mA** — the single largest item, and unreachable by any GPS/modem fix. Only two levers exist.

### 1a. `STM32_HOLD_RESET_ON_SLEEP` — hold the co-processor in reset (PREFERRED)

**What:** GPIO15 wires directly to the STM32's NRST (with R41 47 kΩ pull-up and C19 100 nF). Configure it as
an RTC GPIO, drive it LOW, and `rtc_gpio_hold_en()` so the level survives deep sleep. On wake, release the
hold and let it boot before `sys.begin()`.

**Why preferred over ATLP:** deterministic (a hardware reset cannot fail to take), immune to co-processor
firmware differences across units, and it should draw *less* than Stop mode since reset holds clocks and the
regulator down rather than retaining state. Standing cost is ~150 µA sunk through the two pull-ups
(STM32-internal ~40 kΩ + R41 47 kΩ).

**Risks / open questions:**
- STM32 pins go **Hi-Z** in reset. Audited from the schematic: K-line is safe (Q7 unbiased = off, line idles
  high via R38 68 kΩ), OBD_IND LED is safe (Hi-Z = dark). **CAN TXD is the one to verify** — see §1c.
- GPIO15 is the **MTDO strapping pin**. Waking with it still held LOW silences the ROM boot messages. Own
  `Serial.printf` output (including the `=== FW BUILD:` banner) is unaffected; flashing is unaffected because
  a hard reset clears holds.
- Adds STM32 boot time + `sys.begin()` + OBD init to the first poll of a drive (~1 s, measure it).

### 1b. `OBD_LOW_POWER_ON_SLEEP` — ATLP (fallback)

**What:** `obd.enterLowPowerMode()` (sends `ATLP`) as the last co-processor command before sleep.

**Why deferred — five independent reasons:**
1. **The savings have never been measured.** The bench harness cited in `POWER_BUDGET.md` §5a is untracked in
   a sibling checkout, and in its captured logs `ATLP` was **never once sent** (nine runs with it off; the
   tenth on but it never slept). The rig's run-to-run noise (~10 mA) equals the whole claimed saving.
2. **The 10 mA datasheet figure is a category error** — it describes ATLP with the *ESP32 awake* running
   motion detection (the vendor calls `waitMotion(-1)` right after `enterLowPowerMode()`). It cannot be added
   to a deep-sleep baseline.
3. **`ATRV` is a co-processor command.** `getVoltage()` makes one 500 ms attempt and returns **0** on failure.
   The gate's motion branch is `is_motion && (v >= ATRV_AWAKE_VOLTS)`, so a motion wake on a resting rail
   (~12.6 V, below `RAIL_AWAKE_VOLTS` 13.3) would fail `should_poll` and sleep — **drives would silently stop
   being logged** while charge sampling kept working. Mitigation is §2b.
4. **ATLP survives the ESP32 reboot** (nothing resets the co-processor; `resetLink()` is never called from
   baseLogger, no GPIO hold), and `sys.begin()` gives it only **3 ATI attempts** where the library's own
   `leaveLowPowerMode()` budgets 30. If those 3 fail: `link = NULL` → modem UART rebound to GPIO16/17 → OBD
   init panics → panic reset reports UNDEFINED wake → cold-boot path polls → panics again. **Boot loop,
   recoverable only by unplugging.** (The null guards landed in §7 now bound this to a bad wake instead.)
5. **It may do nothing.** Upstream changed `enterLowPowerMode()` to send `ATR` + 1 s delay *before* `ATLP`
   because it was unreliable; our copy lacks that.

**Do NOT use `obd.leaveLowPowerMode()` as the wake path** — it starts with `if (!link) return;`, so it
no-ops in exactly the state ATLP can create, and it cannot run before `sys.begin()` creates the link. Use the
GPIO15 hardware pulse instead (§1a).

**Ordering constraint if it is ever enabled:** `ATLP` must come **after** the unconditional `ATGPSOFF`
(landed in §7) and after `network.powerOff()`. Inserting it earlier leaves GNSS powered at 20-40 mA through
the whole sleep — larger than the saving, i.e. net-negative.

### 1c. Verify the CAN transceiver in both sleep variants

U9 (TJA1050) is powered from `VCC5V` permanently and its **S (silent) pin is hardwired to GND** — always
normal mode, no standby. TXD comes straight from the STM32's PA12, with **no external pull-up** on that net.

Good news, from the NXP datasheet: the TJA1050 has a **TXD dominant time-out** circuit. The timer is armed by
a falling edge on TXD, and if TXD stays low past `t_do` (~300-750 µs) the transmitter is disabled and the bus
released to recessive. That removes the bus-jamming scenario *and* prevents a stuck-dominant transceiver
drawing tens of mA — which would have exceeded the entire sleep budget.

**What still needs a scope:** the timer resets on every *rising* edge, so a TXD input floating near the logic
threshold could chatter and repeatedly re-arm it, producing sustained bus noise instead of one clean timeout.
Whether that happens depends on the TJA1050's internal TXD biasing versus a floating PA12. Two-minute check:
probe CANH/CANL during a held-reset (and an ATLP) sleep and confirm the line settles recessive.

Also worth knowing: the first ~0.5 ms *is* dominant before the timeout fires — one corrupted frame / error
frame if the bus happens to be active. Near-zero exposure since the dongle only sleeps when the gate says the
car is asleep, and CAN error handling absorbs a single error frame.

### 1d. Rail cut — GPIO12 / `VCC5V_MX` (the deterministic option, hardware-verified)

**What the schematic shows:** `IO12_MX_PWRKEY` → R24 100 Ω → R22 2.2 kΩ → Q3 (2N3904, emitter GND, R25
2.2 kΩ base bleeder) → collector pulls the gate of T1 (SI2301 P-MOSFET, source `VCC5V`, drain `VCC5V_MX`,
gate pulled up to `VCC5V` via R21 10 kΩ). So **IO12 HIGH = rail ON**, and the rail is **ESP32-owned, not
STM32-owned** — which matches the library's `PIN_GPS_POWER = 12`.

**Consequences worth recording:**
- **ATLP is not a GNSS failsafe.** The STM32 only owns GNSS *data* (PB10/PB11 to the XBEE socket), never its
  power. A GPS on J2 is powered by `VCC5V_MX` (ESP32-owned); a GNSS in the XBEE socket is powered by
  `VCC5V`, which is **always on**, and can only be silenced by `ATGPSOFF` over the co-processor link. Since
  the firmware uses the `FLAG_GNSS_USE_LINK`/`ATGPSON` path, the latter is likely what's fitted — so the
  `ATGPSOFF` fix in §7 is the real remedy, not a redundancy.
- Because R25 pulls Q3's base to ground, GPIO12 going Hi-Z in deep sleep switches `VCC5V_MX` **off by
  itself** — so that particular path cannot leak.
- **Strapping hazard:** GPIO12 is MTDI. Holding it **HIGH** across a reboot selects 1.8 V VDD_SDIO (flash
  brick territory). LOW/floating is the safe default — conveniently the direction wanted for sleep. **Never
  `gpio_hold_en` GPIO12 high.**

**Deferred because** it needs bench polarity verification and a decision about `gpio_hold` behaviour, and
because §1a likely makes it unnecessary.

---

## 2. Modem power-down hardening

### 2a. Toggle-then-verify-and-undo in `powerOff()`

**What:** on a missing CPOWD ack, wait ~1.5 s for any in-progress shutdown, PWRKEY, wait ~3 s for boot, probe
with the library's doubled `AT\rAT\r`, and if it answers, PWRKEY again to undo.

**Why deferred:** silence is ambiguous — "already off" and "powered but AT-wedged" look identical, and the two
failure modes cost about the same current, so any *guessing* rule is a swap rather than an improvement. The
act-then-verify scheme is correct in both cases but adds ~9 s to a sleep entry and depends on modem timings
we have not observed on this unit. The ack-detection half (expected array + bigger buffer + the `CPOWD r=`
log) was landed in §7 and is strictly better on its own; it should also make the fallback fire far less often,
which the new log line will show.

**Do NOT add `"OK"`/`"ERROR"` to the expected array.** `xbReceive` matches after each ~50 ms chunk in array
order, so on firmwares that emit `OK` before `NORMAL POWER DOWN` it would return 2, be read as failure, and
PWRKEY a modem that had obeyed — the library documents that as leaving it "in a half-on state."

### 2b. The other two blind PWRKEY toggles

`FreematicsNetwork.cpp` has two more copies of the unconditional toggle — `CellSIMCOM::end()` (reached from
`begin()`'s failure path) and `HTTPClientSIM::end()`. They detect the ack correctly but keep the same
fallback, and they run precisely when the modem is most likely already dead. Fix them together with §2a or
route them through one helper.

### 2c. Modem left powered after a reset (same shape as the GNSS leak)

`PIN_BEE_PWR` (GPIO27) is only driven LOW as an idle level in `sys.begin()`; the modem's on/off state lives
in the modem itself (which is why CPOWD exists). So after a reset that left it powered, the next **gate-skip**
wake calls `enterDeepSleep(false)` → `network.powerOff()` is never reached → it draws through the sleep,
sticky across many 30-minute cycles. The firmware's own comment at `logger.ino:630` shows the author knew the
shape (the WDT-escalation path deliberately passes `modem_on=true`).

**Why deferred:** the obvious fix (call `powerOff()` unconditionally) is only safe once §2a lands — otherwise
the missing-ack fallback PWRKEYs an off modem back ON, which is the very bug. A skip wake also has no modem
UART initialised, and `sys.begin(false, true)` is **not** a safe shortcut because `xbBegin()` pulses
`PIN_BEE_PWR`. If wanted later, gate it on an `RTC_DATA_ATTR` "modem was powered" flag.

### 2d. `gpio_hold` for GPIO27 (PWRKEY)

Nothing in the repo calls `gpio_hold_en`/`gpio_deep_sleep_hold_en`, so GPIO27 goes high-Z in deep sleep and
PWRKEY's state through sleep depends on board pulls rather than firmware. Worth an ammeter check.

---

## 3. Gate restructure (prerequisite for any co-processor sleep)

### 3a. Move `sys.begin()` to after the power gate

**Why it's required:** `sys.begin()` runs at `logger.ino:413`, the gate at ~`:576`. So as the code stands,
**every skip wake still fires 3× ATI at the co-processor** — which either wakes it (destroying the premise of
§1a/§1b) or nulls the link. To get "skip wakes are ESP32-only", `sys.begin()` + `obd.begin(sys.link)` + the
ATRV prime must move behind the gate, preceded by the GPIO15 reset pulse.

**Feasible, and the ordering already cooperates:** `datalogger.begin()` is at `:405`, *before* `sys.begin()`,
so skip-wake event logging survives; MEMS/WOM is native I2C (I2C_NUM_0, SDA 21 / SCL 22) and independent of
the co-processor.

**Gotcha:** `sys.begin()` internally calls `esp_task_wdt_init(600, 0)`, which is why the wedge guard is
re-armed right after it. That re-arm must travel with it, or the 300 s wedge timeout silently becomes 600 s
with panic disabled.

**Side benefits:** a skip wake becomes truly ESP32-only — no `xbBegin()`, so no `PIN_BEE_PWR` pulse and no
3× ATI — which should also cut the current ~5 s / ~60 mA skip cost.

### 3b. Harden the gate against `ATRV == 0`

Distinguish "co-processor didn't answer" (exactly 0.00) from "car asleep" (~5.3 V): treat 0 as unknown, retry
once, and fall back to rail-only logic so a motion wake still polls. Needed before §1b; also good hygiene now.

**When replacing the ATRV term with a rail comparison, keep the top-level `!weak_rail &&`.** A bare
"rail > 8.0 V" would admit the 8.0-11.0 V weak-battery band that `IO36_LOCKOUT_HIGH` deliberately suppresses
(the original dead-overnight scenario). Note 8.0 was calibrated for *ATRV* semantics (~5.3 V asleep, ~13.5 V
awake); on the true rail it degenerates to "any motion wake", which is the correct intent — so prefer plain
`is_motion` under the existing `!weak_rail` guard rather than inventing a rail threshold.

### 3c. Remove the `pinRx = 16; pinTx = 17;` override

`FreematicsPlus.cpp:912-919`: when the co-processor doesn't answer (`devType == 0`), the modem UART is bound
to different GPIOs and **cellular is dead for that boot**. Upstream has already commented these two lines
out; the in-use copy has not. Low risk, but it's a library edit that deserves its own verification.

---

## 4. Dormant / latent (no action needed today)

- **WiFi RF-flush bypass:** `logger.ino:696` calls `esp_deep_sleep_start()` directly, skipping
  `enterDeepSleep()` (no modem power-off, no `ATGPSOFF`, no BLE deinit, no WOM arming). It sits inside
  `#if ENABLE_WIFI` with `ENABLE_WIFI 0`, so it is **compiled out**. Fix when WiFi returns.
- **`FILE_TEST` GNSS leak:** the bare `sys.gpsBegin()` at `logger.ino:467` never sets `g_gps_powered`, so a
  `FILE_TEST` build would leak GNSS through sleep. `FILE_TEST 0`, so dormant.
- **Per-wake `new` leaks in `BLEWrapper::init()`:** 7 callback objects + 6 descriptors are never freed by
  `deinit()`. Harmless today because init/deinit are strictly 1:1 per boot (deinit is only reached inside
  `enterDeepSleep()`, which always ends in a reboot). Becomes a real leak the day a re-init-without-reboot
  path exists — worth a one-line warning comment in `deinit()`.

---

## 5. From the three-reviewer audit (2026-08-12), still open

- **OTA ACK-timeout resync (MEDIUM).** On a single lost ACK notification the app aborts a *healthy* session,
  and because ABORT reaches a live dongle it destroys the 90 s resume anchor, so the retry restarts from
  sector 0. The machinery to fix it already exists: the dongle `setValue`s every ACK before notifying, and
  `readLatestFrame()` can read it back — it's currently only used when the *subscription* was refused. Fix =
  on ACK timeout, try one read-back (or an INFO probe, whose ARMED reply carries `ota_cur_sector`) before
  aborting. Deferred because it touches field-proven OTA code for a rare, self-healing case.
- **Stale RESUMEWAIT blocks a different image for up to 90 s** with `OTA_ERR_BUSY`. On a parameter mismatch
  from an authenticated client, reset the stale session and accept the fresh START.
- **Buffer `logEvent` to RAM during a log dump** (reuse the `bleLogQueue` pattern) so the documented "no
  writes during a dump" invariant becomes real. Today a mid-dump event write can rotate a segment; the
  `readfail` guard landed earlier turns that into a clean failure rather than a livelock, but prevention is
  better.
- **Atomic command mailboxes.** Three hand-rolled `volatile` flag handoffs exist (config push, OTA command,
  log-dump command). `volatile` does not order the non-volatile buffer stores against the flag store, and the
  ESP32 is dual-core; the memcpy call boundary saves it in practice, but LTO or a toolchain bump could
  reorder. Replace with `std::atomic<bool>` release/acquire, once, used everywhere.
- **`DataLogger::begin()` formats on mount failure** (`LittleFS.begin(true)`), so a transient corruption
  silently destroys the exact log the BLE dump feature exists to retrieve. Try `begin(false)` first and log
  an event before formatting.
- **Protocol versioning.** Add `"proto":1` to the LOGDUMP header and a version byte to the OTA INFO/status
  frame. Today the only contract authority is comments, so a future mismatch surfaces as generic timeouts
  indistinguishable from a bad link.
- **Tail-fetch mode for the log dump.** The biggest remaining UX win: `{"LOGDUMP":{"tail":N}}` (or a START
  parameter) so the app pulls only what's new since the last fetch. A full dump measures **~9.6 KB/s → 2.7 MB
  in 278 s** (§8), not the ~22 KB/s / ~2 min this bullet claimed before — so the case is *stronger* than
  stated: a routine "what happened since yesterday" would be seconds, and a shorter session shrinks every
  race window in the protocol.
- **Log-dump notify log spam (§8).** 355 `[E]` lines per healthy dump, from the backpressure retry succeeding.
  Cosmetic, but it buries real errors; the fix is written out in §8 and gated on the multi-peer decision there.
- **App-side poller tests** for `isFirmwareUpdateOngoing` / `isDongleLogFetchOngoing`, mirroring the existing
  wizard/PID-test cases. The three guard sites are exactly the poller behaviour that branch changed.
- ~~**`ota_sign.py` should derive the version**~~ Done: the version is injected at build time and `ota_sign.py`
  refuses an image whose `FW_BUILD_TAG` doesn't match. Three stale-binary releases preceded it (v2-over-v1,
  v5-over-v4, 3-over-v2).

---

## 6. The measurement that unblocks most of this

The 27 mA baseline is **unexplained** and the 27 → 38 mA step has three equally supported causes (modem
toggle, GNSS latch, un-parked co-processor), so nothing above should be credited with a saving until this
runs. One session, four states, same rig:

1. **Baseline** — as shipped.
2. **+ the §7 fixes** (unconditional `ATGPSOFF`, CPOWD ack detection).
3. **+ `STM32_HOLD_RESET_ON_SLEEP`**.
4. **+ `OBD_LOW_POWER_ON_SLEEP`** (for comparison against 3).

Also, in the same session:
- **The discriminating test for `powerOff()`:** compare the sleep floor after a gate-**skip** wake (where
  `powerOff()` never runs) against a poll wake. If both step to 38 mA, `powerOff()` is exonerated.
- **Scope CANH/CANL** in states 3 and 4 (§1c).
- **Watch the blue OBD_IND LED during sleep** — it runs `VCC3V3` → R32 470 Ω → LED → the STM32's PC13, so if
  PC13 stays driven low that's ~5-7 mA of the saving visibly glowing.
- Read the new `CPOWD r=` and `sys.begin()=/devType=` lines to attribute what you measure.
- Run the bench supply at **~12.6 V, not 11.4 V** — 11.4 V is only 0.4 V above `IO36_LOCKOUT_HIGH` (11.0 V),
  and sagging under it flips the weak-rail lockout so every wake becomes a skip and the behaviour under
  measurement changes completely.

**Target, as a consistency check:** STM32 asleep + GPS off + modem off ≈ ESP32 (~0) + TJA1050 (5-10 mA) +
regulator quiescent = **~6-12 mA**, matching the datasheet's ~10 mA. The transceiver is the last item and
needs a board revision (`POWER_MANAGEMENT.md`).

---

## 7. Landed 2026-08-18 (zero-risk subset — do not redo)

1. **Unconditional `ATGPSOFF`** before sleep, kept *alongside* `if (g_gps_powered) sys.gpsEnd(true)` and
   positioned as the last co-processor command (`Helpers/sleep_power.h`). Closes the reset-orphaned GNSS leak;
   raw `sendCommand` rather than `gpsEnd()` because without `FLAG_GNSS_USE_LINK` that function tears down a
   task/UART driver never created this boot.
2. **CPOWD ack detection** (`NetworkWrapper/cellwrapper.cpp`): expected-match array with **only**
   `"POWER DOWN"`, buffer 64 → 192 bytes, plus a `CPOWD r=… fallback=… [buffer]` diagnostic. The
   unconditional PWRKEY fallback is **unchanged** — see §2a.
3. **`sys.begin()` diagnostic** (`logger.ino`): captures the return value, prints `devType` and link state,
   and logs an event when the co-processor fails to come up. Both were previously discarded.
4. **Null guards** on `COBD::readPID`, `COBD::init(vector)`, `OBDWrapper::readBuffer`,
   `OBDWrapper::pollStandardFallback` — converts the brick-class panic loop into a graceful failure, and also
   covers a genuinely dead/damaged co-processor. (`getVoltage()` and `init(protocol)` were already guarded.)
5. **Comment corrections:** the idle-to-sleep timer comment in `config.h` (it is ~5 min, not ~10 — with the
   reasoning for *not* raising `CHARGING_TIMER`), and the stale NITZ claim in `cellwrapper.cpp` (NITZ pushes
   *are* now arriving on this unit, and they are the ~50 bytes that could overrun the old CPOWD buffer).

Earlier in the same cycle (already in the tree): the ABD7 log-dump `readfail` guard and idempotent re-START,
`g_logdump_active` added to the sleep/backstop/motion-poll/config-apply paths, the OTA sticky-sector-error
restriction, the `att_mtu` per-connection reset, and the corrected GATT handle-budget comment (21/24).

---

## 8. Log-dump notify noise — and the multi-peer question behind it (OPEN, 2026-08-26)

**Symptom.** A completely healthy log dump emits **355 lines** of
`[E][BLECharacteristic.cpp:537] notify(): << esp_ble_gatts_send_ notify: rc=-1 Unknown ESP_ERR error`.
The transfer succeeds — `LOGDUMP end (app) reqs=336 retries=2 served=2727804/2727804 in 278s` — but the
serial output looks like a failing system, which is the actual harm: it buries genuine errors and reads as
alarming to anyone who did not write the retry loop.

**What is really happening.** `notifyLogDump` (`blewrapper.cpp:889`) already handles this: Bluedroid returns
`ESP_FAIL` when its TX queue is full (it never blocks), so we wait 10 ms and resend, up to 25 attempts. But
each attempt goes through the library's `notify()`, and `BLECharacteristic.cpp:537` prints `log_e`
**unconditionally** on a non-OK return *before* reporting it through the `onStatus` callback we act on. So
every successful recovery costs one red line. **The errors are the retry mechanism working.**

### Evidence that nothing is lost (recorded so this need not be re-derived)

| Measurement | Value | What it rules out |
| --- | --- | --- |
| Intra-burst gap | only 20/21/23/24/28 ms — **never 10 ms** | The retry delay is 10 ms, so a chunk needing 2+ attempts would show 10 ms spacing. Every failure was one attempt then success: **worst case 1 of a 25 budget.** |
| App-level `retries` | 2 of 336 windows | If the 355 drops were real losses the app would have re-requested ~23 times (one per affected window). The 2 are ordinary radio loss. |
| Burst shape | 23 bursts, 4–28 consecutive chunks, always < the 34-chunk window | Congestion builds within a window and clears at the next request — it is not a link fault. |

Do **not** read `served=` as proof of delivery on its own: `logdump_next_expected` is advanced from `off`
after the window is *sent*, so it is the dongle's own optimistic counter. The three rows above are the proof.

**Cost of all 355 retries: 3.5 s of 278 s — 1.3%.** The transfer is radio-bound at **24.4 ms/chunk**
(11,413 chunks), which is the ~1-notification-per-connection-event ceiling already documented at
`log_dump.h:33`, *not* the retries. Also note this run had 3.1% of chunks hit backpressure where the
2026-08-11 measurement at the same MTU/chunk saw zero — so the "zero drops at 4 ms" note in `log_dump.h`
describes one environment, not a guarantee. It does not change the pacing conclusion: raising the gap cannot
beat the connection-event ceiling.

### The fix (verified to compile against the in-tree library, not applied)

Bypass the library wrapper so we see the return code without it being printed first. All three accessors are
public and confirmed present: `BLEServer::getGattsIf()` (`BLEServer.h:108`),
`BLECharacteristic::getHandle()` (`BLECharacteristic.h:84`), and `param->connect.conn_id` is already
delivered to `ServerCallbacks::onConnect` (`blewrapper.h:375`).

```cpp
// blewrapper.h
uint16_t logdump_conn_id{ 0xFFFF };   // the peer we are serving; 0xFFFF = none
uint32_t logdump_backpressure{ 0 };   // retries this session, reported by the LOGDUMP end event

// ServerCallbacks::onConnect / onDisconnect — capture and clear it
wrapper->set_logdump_conn_id(param->connect.conn_id);   // onConnect
wrapper->set_logdump_conn_id(0xFFFF);                   // onDisconnect

// blewrapper.cpp — replaces the notify() + onStatus flag dance
bool BLEWrapper::notifyLogDump(const uint8_t* frame, size_t len) {
  if (!device_connected || !pLogDumpCharacteristic || !frame || !len) return false;
  if (!logDumpSubscribed() || logdump_conn_id == 0xFFFF) return false;
  pLogDumpCharacteristic->setValue((uint8_t*)frame, len);   // keep ABD7 readable, as today
  for (int attempt = 0; attempt < 25; ++attempt) {
    if (::esp_ble_gatts_send_indicate(pServer->getGattsIf(), logdump_conn_id,
          pLogDumpCharacteristic->getHandle(), len, (uint8_t*)frame, false) == ESP_OK) return true;
    logdump_backpressure++;
    delay(10);   // queue full - let the radio drain, then resend
  }
  return false;
}
```

Same semantics, same 25/10 ms budget. `setValue` stays so a plain GATT read still returns the last frame.
The `logDumpSubscribed()` check we already do covers the CCCD test `notify()` performed, and chunks are
already MTU-sized so its truncation warning is moot. `logdump_notify_err` / `LogDumpCallbacks::onStatus`
become dead and should be removed **with their comments**. Net effect: 355 red lines become one
`backpressure=355` field on the end event — better diagnostics, because a rising count is a number you can
trend instead of noise people learn to ignore.

### The multi-peer question — why NOT to mirror the library's loop

The natural objection to `getConnId()` is that it assumes a single peer, and that we should instead
replicate the library's `getPeerDevices()` loop, checking each peer's CCCD and sending only to subscribed
connection IDs. **That specific design is not implementable with this library, and the loop is the wrong
shape for a gated transfer.** Two independent reasons:

1. **The CCCD is not per-peer.** `BLE2902::getNotifications()` (`BLE2902.cpp:27`) returns
   `getValue()[0] & 1` — a **single shared descriptor byte per characteristic**. There is no
   per-connection subscription state to consult, so "only the peers who subscribed" cannot be expressed:
   if any peer subscribes, the flag reads true for all of them.
2. **Broadcast is a security regression, not hardening.** ABD7 is gated behind the auth handshake precisely
   so log contents go only to an authorized client. The library's loop sends every frame to *every*
   connected peer; combined with (1) that would stream the dump to a second, unauthenticated central. For
   this characteristic "send to all subscribed peers" is strictly worse than "send to one".

**So the correct answer is neither option: pin the dump to the conn_id that authenticated** — which is what
the snippet above does, and it is *more* robust than the library loop, not less. Capturing it in `onConnect`
is sufficient today (see below); capturing it at AUTH success instead would make the intent explicit and is
the better form if multi-peer ever becomes real.

**Why single-peer is currently structural, not luck:** `BLEDevice::startAdvertising()` is called only from
the two disconnect paths (`blewrapper.cpp:449`, `:745`). Advertising stops on connection and is never
restarted while connected, and the dongle never initiates — so a second central cannot attach. **The
invariant to protect: if anyone ever re-advertises while connected, this assumption and the `device_connected`
bool both break together.** A `static_assert` cannot express it; a comment at the advertising calls can.

### Why deferred

The noise is cosmetic and the data is provably intact, while the change touches the field-proven notify path
that OTA and the auth gate do not share but sit beside. It is also the wrong moment: the multi-peer decision
above should be settled deliberately rather than as a side effect of a log-tidying patch. Nothing here is
urgent — but it should not be lost, because "a handled backpressure event is being reported as an error by
the layer below us" is the exact inverse of the GATT handle-budget bug (where the layer below stayed silent
about a real fault), and both cost real debugging time.

**How to settle it:** apply the snippet, run one full dump on each platform, and confirm (a) zero `[E]` lines,
(b) `backpressure=` non-zero on Android, (c) `retries` still ~0–2, (d) `served` complete. If `backpressure`
ever approaches `25 × chunks`, the retry budget — not the logging — is what needs revisiting.


---

# Appendix I — OBD-alarm / ignition-sense architecture (final)

> Original file: `IGNITION_SENSE_ARCHITECTURE.md`

# OBD-Alarm Safety & Ignition Sense

> **STATUS 2026-08-28: enforcement BYPASSED** (`OBD_ALARM_ENFORCE 0`, config.h). Field testing
> found three enforcement-breaking bugs — see `ALARM_GATE_FIELD_FINDINGS.md` for root causes, the
> fix plan, and the re-enable checklist. The flag itself (toggle, ABD2 write, NVS, ABD5 read-back)
> keeps working as bookkeeping; the dongle otherwise behaves exactly as before this document.

**A delta against the current ABRP OBD Dongle firmware.** Third iteration. The first draft
(preserved as `IGNITION_SENSE_ARCHITECTURE.original-draft.md`) proposed sensing OBD pins 1/8/9/13
through on-board dividers; the second iteration declared the pin approach dead. Both were partly
wrong. This version is grounded in an exhaustive search of the firmware (source, compiled binary,
and vendor documentation — §4) and the manufacturer schematic, and it lands in the middle:

- **The pins cannot be read by any command, existing or hidden.** Proven three independent ways (§4).
- **The pins CAN be read through the Molex expansion socket** — a tap harness into GPIO34, a route
  the vendor's own firmware already contains reference code for (§6). No board revision.
- Everything works **without** the pin too: the GNSS/rail policy (§8) is complete on its own, and
  the vendor's own recommended ignition detection turns out to be the rail gate we already ship.

---

## 1. The requirement, stated correctly

"Know the ignition state" is a proxy. The real requirement is:

> **Never transmit on the vehicle bus while the car might be armed.**

Two consequences make a safe design possible:

- We do not need *ignition on*. We need **positive evidence of "not armed"**.
- Absence of evidence is not evidence. Unknown ⇒ stay silent (on an alarm-flagged car).

A measured ignition line (§6) is the *best* such evidence where one exists at the connector; GNSS
movement (§5) is the fallback that already works everywhere.

---

## 2. The field result this is built on

Confirmed on a BMW i3 **with a positive control**:

> The OBD anti-theft alarm is triggered by **active bus traffic**, not by the passive electrical
> presence of a device in the port.

A powered, connected, non-transmitting dongle did not trigger the alarm across a full lock cycle. A
deliberate poll while locked and armed did.

| Source | Claim | Assessment |
|---|---|---|
| Dauntless Devices | triggers if a device **sends an OBD request** while armed | Consistent with our result |
| OVMS (i3 docs) | the i3 alarms if **anything is left connected** | Almost certainly describing their own *polling* device; OVMS also warns of battery drain, which implies active work |

**Consequence:** silence while armed is a complete solution. No BimmerCode coding, no CAN
isolation, no pin-16 reroute.

---

## 3. Hardware reality

From `schematics_oneplus_r14_20190612.pdf` (manufacturer, r14 — see the revision caveat below).

**Five nets cross to the vehicle.** There is no OBD-II connector symbol in the drawing; the vehicle
interface exists only as nets:

| Net | OBD-II pin | Where it lands |
|---|---|---|
| `VCAR` → `VCC12V` | 16 (battery +12 V) | D5 1N4001 → DC/DC (U10), plus the two sense dividers below |
| `CANH` / `CANL` | 6 / 14 | TJA1050 (U9), R36 120 R termination |
| `Kline` | 7 | R34 510 R pull-up from VCC12V, R38/R40/Q7 → STM32 PA9/PA10 |
| `GND` | 4 / 5 | — |

**Pins 1, 8, 9, 13 have no destination net.** They are physically present (J1962 is a 16-position
molded standard) but electrically blind on this board. Independently corroborated twice:

- String-extraction of the schematic PDF itself finds only `VCAR, VCC12V, CANH, CANL, CANRX, CANTX,
  KLINE, GND, BATDEC, IO36_BATDEC, OBD_IND` as vehicle-side nets — no discretionary-pin net exists.
- Industry convention, not a quirk: a published ELM327-clone teardown found **only pins 4, 5, 6, 7,
  14, 15, 16 soldered**. The ELM327 datasheet itself leaves pins 1/3/8/9/11/12/13 "to the
  discretion of the vehicle manufacturer" — mainstream dongles do not connect them either.

**Two rail sense paths exist, both reading pin 16 only**, both 100 K/10 K off `VCC12V`:

- R29/R31 → **ESP32 IO36** (ADC1_CH0) — readable without waking the co-processor.
- R35/R37 → **STM32 PB0** — surfaced to us as `ATRV`.

**Revision caveat and the bench test that settles it.** r14 is from 2019; our unit is later (it
shows an MPU9250 and an XBEE-socket modem where ours has an ICM-42627 and integrated SIM7670). The
OBD-side topology is the part least likely to have changed, but the decisive check is empirical and
cheap: **bench-feed +12 V into pins 1/8/9/13 one at a time** (GND on 4/5, normal power on 16) and
watch `ATRV`, the IO36 reading, and the logs. Nothing moves ⇒ the pins are blind on our revision
too, and §6 is the only route. Something moves ⇒ revise this section happily.

---

## 4. The exhaustive search — no hidden command, and no way to add one

Ran 2026-08-26 (three parallel deep searches over both Freematics trees, our `libraries/`, the
vendor's compiled telelogger binary, and vendor web documentation). Recorded so nobody re-hunts.

### 4.1 No command reads a discretionary pin — proven three ways

1. **Source:** the complete AT surface sent to the co-processor (~35 commands across all trees)
   contains exactly one electrical read: `ATRV` (`FreematicsOBD.cpp:303`, "works without ECU"),
   wired to the STM32 PB0 divider — pin 16.
2. **Binary:** `strings` over the vendor's compiled telelogger binary yields **no command the source does not
   already call.** No `ATADC`, `ATGPIO`, `ATIGN`, `ATPIN`, `ATVOLT`, nothing pin-shaped.
3. **Vendor:** the web-documented co-processor set IS larger than what our code uses — extras are
   `ATACL ATGYRO ATMAG ATTEMP ATORI ATQU0/1 ATPTH ATPTA ATBR2 ATSGC` — but every one is MEMS
   fusion, K-line timing, or GPS-serial. The only "ignition" command in the entire Freematics
   ecosystem is `ATACC0/1` on the **OBD-II Emulator** (a different product; it *sets* a simulated
   ignition state, reads nothing).

A command can only report what the STM32 is wired to, and §3 enumerates its wiring; the three-way
search closes the "undocumented command" loophole on top of that.

### 4.2 The STM32 co-processor firmware is a closed blob — and unmodifiable in practice

- Its source was **never published** (the Freematics GitHub org has only ESP32-side code; multiple
  forum threads requesting it went unanswered). No `.hex`/`.dfu` exists anywhere in our repo — all
  8 `.bin` files verified by magic byte as ESP32 images.
- **No software flash path exists:** BOOT0 is never touched anywhere in any tree (so no serial
  bootloader entry), and the ESP32's only control over the STM32 is the GPIO15 NRST pulse.
- **Do not attach a programmer to the J1 SWD header.** If Freematics set readout protection (RDP),
  the only possible SWD operation is a mass erase — which destroys the co-processor firmware with
  **no image anywhere to restore it**. That is a brick, not a hack.

So the STM32's free ADC pins (PA0–PA7, unconnected on the schematic) are unreachable: not by
licence, by availability.

### 4.3 One genuine mechanism found — nothing behind it, but record it

Profile `init_commands` reach the co-processor **verbatim and unfiltered**
(`obdwrapper.cpp:47-64` → `COBD::init(vector)` → `link->sendCommand`). Any command the closed blob
implements is therefore invokable at runtime from a signed profile — the channel for a hidden
command existed all along; §4.1 shows there is no hidden command to reach. Two standing notes:

- This unfiltered channel is exactly what the JWS profile-signature gate stands in front of.
- On an alarm-flagged car, add the init-command **locality check** (§8): every init command must be
  local AT/ST configuration, because this channel can also carry bus-transmitting commands.

### 4.4 The vendor's own ignition detection is the rail gate we already ship

Freematics' recommended method (forum guidance): threshold `ATRV` — **over ~14 V ⇒ ignition on,
below ~13 V ⇒ off** (the alternator/DC-DC signature). We did not miss the standard method; it was
already implemented before this document existed.

**But the convergence is partial, and ours is the more capable of the two.** Freematics' guidance is
a *two-band* ignition proxy. Our gate has a protective band beneath it that theirs has no equivalent
for: `IO36_LOCKOUT_LOW 8.0` / `IO36_LOCKOUT_HIGH 11.0` suppress polling in the weak-battery region,
which is what stops the dongle finishing off a car whose 12 V is already dying — a failure mode a
bare ignition threshold cannot see.

Precision about what that buys, since it is easy to over-claim: **§8.2's table needs only the
energised/resting discrimination**, which is exactly where the two methods agree. The extra band is
battery protection, not alarm safety. So the honest statement is that the vendor's method and ours
agree on the term §8.2 depends on, and ours additionally protects a dying battery — not that the
extra bands are what make the alarm policy possible.

---

## 5. The passive signals we have today

All existing hardware, none transmit on the bus.

| Signal | Source | What it establishes |
|---|---|---|
| **GNSS speed** | `gpsMovingConfirmed()`, `gps_location.h:81` (≥5 km/h + streak, already debounced) | **Driving ⇒ in use ⇒ disarmed.** The strongest positive available without §6. |
| Rail voltage (ESP32) | IO36 divider | Car asleep vs. DC-DC energised; no co-processor needed |
| Rail voltage (co-proc) | `getVoltage()` → `ATRV` | Same, cross-check; local measurement, no bus traffic |
| Motion / WOM | ICM-42627 | Someone is at the car — but so is a tow truck. **Weak evidence, never a licence to transmit.** |
| CAN bus activity | `sniff()` → `ATM1` + `ATCF`/`ATCM` | Bus awake. Optional; gated on the §9 silence test |
| BMW terminal status | `ATM1`, filter `130` | Byte 0 `05` = ignition on (Part I §6b). Not lock state; the frame runs with the car off |

GNSS speed also fixes the original draft's re-verification circularity: it is independent of
polling, so it can *falsify* a broken detector ("GNSS says 60 km/h while the gate says asleep"),
which "speed>0 ⇒ pin high" never could on an alarm car (no poll ⇒ no speed ⇒ check never runs).

---

## 6. The measured route: one ADC input via the Molex socket

**The pins are reachable after all — through the expansion socket, not the OBD side of the board.**

### 6.1 Why GPIO34 works (all previously-claimed blockers checked and cleared)

`PIN_MOLEX_2 34`, `PIN_MOLEX_4 26`, `PIN_MOLEX_VCC 12` (`FreematicsPlus.h:71-73`); the official
guide confirms the socket is "2× GPIO (from ESP32), VCC and GND … GPIO26, GPIO34".

- **GPIO34 = ADC1_CH6** — ADC1, so usable with Bluetooth active (ADC2 is not).
- **RTC-capable (RTC_GPIO4)** — a valid `ext0` deep-sleep wake source. The device can wake the
  instant ignition comes on instead of on the next motion/timer event.
  **A wake is a scheduling mechanism, not a permit.** Waking on the pin only means "evaluate the
  gate sooner"; the woken firmware still runs the full §8.3 predicate, which requires
  `rail_energised` as well. Never treat "we woke on the ignition pin, therefore ignition is on,
  therefore poll" as a shortcut — that is precisely the scattered-gate failure §8.4 forbids, and it
  would bypass the one check that covers a stuck-high pin.
- **Free in our firmware.** Its only other assignment is the direct GNSS UART (`gpsBeginExt`), but
  we use `gpsBegin()` = `FLAG_GNSS_USE_LINK` = GNSS over the co-processor link
  (`logger.ino:483`) — GPIO34/26 are never bound.
- Input-only and **no internal pull resistors** — the external divider's lower leg defines the
  level when the source is dead, which is the level we want.
- GPIO26 is ADC2 — with BT active it is a **digital-only** second input. Usable for a second,
  pre-divided digital line; not for analog classification.

### 6.2 Vendor precedent — this is what the socket is for

Upstream telelogger contains a complete, compiled-out feature: `LOG_EXT_SENSORS`
(`telelogger.ino:181-196, 1392-1398`; `config.h:174-175` `PIN_SENSOR1 34`, `PIN_SENSOR2 26`) —
read the two socket pins digital or analog, ship them as telemetry under **already-allocated** IDs
`PID_EXT_SENSOR1/2 0x90/0x91` (`FreematicsBase.h:36-37`).

**Bug in the upstream analog mode — do not copy it:** it configures/reads `ADC1_CHANNEL_0/1` =
GPIO36/37, not 34/26, so it would sample our rail divider while labeling it "sensor 1". Write our
own three-line `adc1_get_raw(ADC1_CHANNEL_6)` read.

### 6.3 The physical tap

An OBD-II extension/Y-splitter that taps the candidate pin and feeds it, divided and clamped, into
the Molex socket. An **accessory harness, not a board revision** — and reversible.

Divider sizing (we control the external circuit, so do not copy the on-board 100 K/10 K):

- Target **~2.6–3.0 V at 12–14 V**, 3.3 V clamp (zener or ESD diode to 3V3).
- That clears GPIO34's V_IH (~0.75 × 3.3 ≈ 2.48 V), so `ext0` wake works.
- The ADC saturates around ~2.45 V at 11 dB attenuation — harmless: above the knee we only need
  "high", while the data-line discrimination (§7) happens in the low range where the ADC is linear.
- High-impedance tap (≥100 K total) — passive, adds no bus traffic, cannot worsen the alarm
  situation.

**One analog input ⇒ sequential calibration.** The original draft sensed all four candidates at
once; with one input the question "which of these behaved like ignition?" is answered in up to four
passes — move the tap, re-run. This makes fleet learning (§11) more valuable: one owner's passes
spare every later owner of that model.

---

## 7. App-guided calibration (restored from the original draft, adapted to one input)

Paired-state measurement with corroboration, run while the user is at the car. Per candidate pin:

**Step 1 — OFF baseline.** Ignition fully off. Sample ~50× over ~5 s; record mean and variance.
Corroborate: rail in the resting band.

**Step 2 — ON state.** Ignition on. Same sampling. **Corroborate before trusting** — the step most
likely to be faked by a user tapping *Next* without acting: require the rail to move into the
energised band (or, on a non-alarm car, an OBD response). Neither changed ⇒ discard, offer retry.

**Step 3 — Return to OFF.** Repeat step 1. A genuine ignition line returns to ~0 V; this proves the
pin *switches* rather than happening to read high once.

### Classification

| Signature | Verdict |
|---|---|
| OFF < ~1 V low variance; ON high (at/above the ADC knee) low variance; return-to-OFF confirms | **Ignition** |
| Either state mid-range with high variance / noise | **Data line** (K-line, LIN) — reject |
| Both states ~0 V | **Unpopulated** — reject |
| Both states high | **Always-live feed** — reject |

**Low variance in both states is the critical criterion.** A K-line idles high and drops during
transmission — a single sample cannot distinguish it from ignition; repeated sampling and spread
can. (With the divider sized per §6.3, "high" clips at the ADC knee — variance is measured usefully
in the OFF/mid range, which is where the impostors live.)

- **A pin qualifies:** store the mapping (§10), enable `ext0` wake on it, and the §8 policy gains
  the measured signal as its primary permit.
- **No pin qualifies:** *not an error* — the expected outcome on most post-2008 CAN vehicles. The
  §8 GNSS/rail policy stands on its own.

---

## 8. The policy

For a vehicle **not** flagged as alarm-equipped: **nothing changes.** Rail gate, 5-minute active
leash, charge cadence — all stand. This must remain a pure add-on.

### 8.1 The design axiom

The two failure modes are not comparable, and every choice below follows from that:

- **False permit** (transmit while armed) ⇒ an alarm sounds in a garage at 2 a.m. Unrecoverable:
  the user's trust is gone and the dongle comes out of the car.
- **False deny** (stay silent when polling was safe) ⇒ some lost telemetry. Invisible, recoverable.

So: **buy false denies cheaply to avoid false permits, and resolve every ambiguity to silence.**

### 8.2 Why no single signal is sufficient — the tow case

A car on a flatbed has ignition **off**, the alarm **armed**, and GNSS reporting 80 km/h. Any rule
of the form "moving ⇒ in use ⇒ disarmed" polls a locked, armed car at highway speed. Symmetrically,
"rail energised ⇒ awake ⇒ safe" polls a parked charging car — the exact i3 scenario in §2.

The **conjunction** is what holds, and both inputs already exist:

| Rail | Moving | Real state | Verdict |
|---|---|---|---|
| energised | yes | being driven | **permit** |
| resting | yes | **towed / transported** | deny |
| energised | no | parked & charging, or stationary with ignition on | deny (unless §6 pin) |
| resting | no | parked asleep | deny |

### 8.3 The permit — instantaneous, no latch, no leash

> **An earlier version of this section was wrong in a dangerous way. Recorded because the failure is
> instructive and the fix is counter-intuitive.** It said: acquire on rail+moving, then *hold while
> the rail stays energised*. That contradicts §8.2's own table (energised + not moving = deny) and
> reproduces the exact i3 scenario this document exists to prevent: drive home (permit acquired),
> park, plug in, lock the doors (alarm arms) — but the EV starts charging, so the DC-DC keeps the
> 12 V rail energised, the latch never releases, and the dongle polls a locked, armed car.

**The permit is a pure instantaneous predicate. There is no hold state.**

```
permit = setup_session_active                               // bounded window, see below
       | rail_energised AND ( (gnss_fix_valid AND speed >= GPS_MOVING_MIN_KMH)
                              OR calibrated_pin_ON )        // §6/§7, where a pin exists
```

Deny the moment every term fails — speed drops, fix lost, rail drops, pin off.

**The setup-session term (added at implementation, 2026-08-27).** The pairing/re-pairing wizard runs
with the user stationary in the car, and its Save button requires a live PID read — which the
driving term can never satisfy while parked. A completed **full profile push** (CLEAN → SETUP OK,
the config-apply block) is proof a human is actively driving the wizard at the car, so it grants a
bounded permit (`ALARM_SETUP_PERMIT_MINUTES`, 10 min). The background reconnect fast path (HASH OK)
deliberately does **not** qualify: the phone auto-reconnects from a pocket, so "any authenticated
BLE traffic ⇒ unlocked" would re-create the 2 a.m. garage alarm — a phone within range of an armed
car at home relays with nobody touching anything.

**Consequence found in the first field test (2026-08-27), fix on the app side:** a re-pair with an
unchanged profile *takes the fast path* — HASH matches the persisted profile — so the wizard got no
permit, its PID test saw only gate-denied frames (`is_parked`/`is_charging`, no `soc`), and Save
never enabled. Since the fast path must stay permit-free, the wizard now **forces the full push**
(`setupThunk` sends CLEAN instead of HASH when `isObdBleSetupOngoing`). The dongle already skips
the OBD re-init for an identical profile, so the cost is the transfer alone. Rule to keep: the
dongle never infers "wizard" from traffic shape — the app declares it by pushing fully, and only a
completed full push grants the permit. The WOM wake-and-wait behaviour this enables
needs no new machinery: a motion wake stays up on the existing `ACTIVE_TIMER` leash, polls only if a
permit arrives (movement or a wizard session), and sleeps otherwise.

**Why no grace period, despite tunnels.** The original latch existed to survive GNSS fix loss
mid-drive. But *any* grace period for "stopped at a traffic light" is the same grace period for
"parked, plugged in, and locking up" — shortening it only shrinks the exposure window, and a poll
cadence of a few seconds lands inside it. The question that settles it: what telemetry is worth
having while stationary? Speed is zero and SoC is unchanged — essentially nothing. The grace period
bought nothing and cost the safety property.

Consequence: park ⇒ speed hits zero ⇒ permit gone *before* the door opens. **Zero exposure.**
Stoplight ⇒ permit drops for 45 s ⇒ returns on movement. Lost: nothing.

**The accelerometer is inadmissible as a hold signal — do not re-add it.** It is the obvious
substitute (the IMU still sees a car moving in a tunnel), but getting out of the car, closing the
door and locking it all *produce* motion, so IMU-based holding would extend the permit through
precisely the arming sequence. It cannot distinguish "still driving" from "occupant leaving", and
that ambiguity falls on the catastrophic side of §8.1. It remains fine as a *wake* source, which is
all it is used for today.

**Asymmetric debounce.** `gpsMovingConfirmed()` already requires a streak to acquire — keep it — but
**release with no debounce at all.** Slow to permit, instant to deny, always.

**Decouple the OBD link lifecycle from the permit.** A strict instantaneous permit would otherwise
thrash `initialize()` in stop-go traffic. An initialised-but-idle OBD link is silent: init is local
AT/ST configuration on the co-processor (§8.5) and no bus traffic occurs until a request is made. So
gate the **requests**, not the link — hold the link up across brief permit gaps, tear it down only
on rail drop or a long gap. No re-init churn, no compromise on silence.

**Cost, stated honestly:** fix loss now denies, so tunnels and underground garages produce telemetry
gaps. Consistent with §12 (alarm-flagged cars get drive telemetry only), and it fails in the
recoverable direction.

**What the terms buy:**

- *Tow case* — on a flatbed the rail never energises, so no permit. Covered.
- *Parked charging* — rail energised but speed zero, so no permit. Covered, and this is the case the
  latch broke.
- *Stuck-high pin* — rail-energised is required alongside it. One residual hole (stuck-high pin
  *during* a parked charge session), bounded by the plausibility rule in §11.
- *Stationary with ignition on* (preconditioning, sitting in the car) — refused by the GNSS-only
  rule, **recovered safely by a calibrated pin**. This is the strongest argument for the §6 harness:
  the pin reads the actual physical state, so it releases the instant ignition goes off and permits
  the stationary case that GNSS alone must deny.

### 8.4 One gate function, default-deny

Make the decision a **single pure function** — `mayTransmit(rail, moving, pin, flags) → bool` —
returning false unless a permit is positively proven, and route **every** call site through it.

This is the direct lesson from the transport-handoff bug (`DEFERRED_POWER_WORK`): gating on
nearly-the-right condition in one place produced a field bug where data flowed nowhere. Scattered
conditions are how safety gates fail. A pure function is also the only way to test this
exhaustively — which matters because you cannot A/B a gate whose failure mode is a sounding alarm.

The alarm-off path is a single early return, which is what keeps this a true add-on:

```
if (!obd_alarm_flagged) return current_gate_verdict;   // today's behaviour, untouched
...conjunctive permit logic...
```

**Do not just validate the init commands — defer init entirely.** §8.5 keeps the locality check as
defence in depth, but the stronger rule is that the OBD link is not brought up at all until a permit
exists. Costs ~1 s at the start of a drive; removes the whole class of "did the profile contain
something that transmits".

**Every** call site goes through it — including the wake path (§6.1) and the setup/validation poll,
not just the cadence poll.

**Logging: one line per wake, not only on transitions.** Log the alarm flag, the gate inputs, and
the verdict *every wake*. Transition-only logging has a hole that matters: a wake where the gate
never permits produces no transition, so the log says nothing at all — and "the dongle went quiet"
with an empty log is exactly the shape that gets misattributed to the cellular stack for weeks.
This project has that history already (the transport-handoff bug starved cellular while looking like
a network fault; the persisted-profile bug had the hash claiming a profile that was not running).
A gate working correctly and a gate that is broken look identical from outside unless the log says
which inputs it saw.

### 8.5 Hard stop and init safety

**Ignition-off is a hard, immediate stop — not a leash expiry, not the next cadence tick.** The
dangerous window is the few seconds between switch-off and the lock button; a poll that lands after
the lock arms means an alarm in a parking garage. Treat loss of the permit as an interrupt into the
quiet state, and **check it between individual PID reads** — `readPIDs` blocks for seconds, so a
cadence-boundary check is not tight enough. (With the §6 input this stop becomes edge-accurate.)

**Init-path safety.** `OBDWrapper::initialize()` uses the `init(vector)` overload — only the
profile's `init_commands`, never the `readPID(PID_SPEED)` probe in the other overload
(`FreematicsOBD.cpp:405`). So init is silent *provided the profile contains only local AT/ST
configuration*. On an alarm-flagged vehicle, validate exactly that and skip anything else — cheap,
and it also closes the §4.3 channel for bus-transmitting commands.

**Fail-safe default throughout:** unknown mapping, failed calibration, failed re-verification ⇒
never poll while the car might be armed. A documented limitation is forgivable; a 2 a.m. alarm is
not.

---

## 9. If sniffing is used: one test first

**Optional for §8; a hard prerequisite for §12 — and therefore not something to defer.** The gate
works without sniffing, so this reads as skippable. It is not: §12's passive frame decoding is the
**only** path to recovering the parked-charge telemetry that alarm-flagged cars give up, and all of
it is blocked behind this one result. Ten minutes of bench work decides whether an entire capability
exists. Do it early (§13 build order) — it is the cheapest experiment in this document and the one
with the largest branch behind it.

Before trusting `ATM1` as a passive signal, establish that monitor mode is actually **silent**: a CAN
controller in normal mode transmits ACK bits, and the TJA1050's silent pin is hardwired to GND
(`DEFERRED_POWER_WORK.md §1c`), so ACK suppression must come from the controller — which is inside
the closed STM32 blob and **unverifiable by reading code** (§4.2). This is the one claim in the
document that no amount of source reading can settle.

**Test:** locked, armed, `ATM1` for ten minutes. Same positive-control shape as §2. Expected to
pass (alarms almost certainly watch for diagnostic requests, and a sleeping bus has no frames to
ACK), but it is one bench test against an assumption in someone else's firmware.

**Partial result (i3, 2026-10-01):** the listen-only build sat in `ATM1` through one short lock and
unlock, and the alarm did not sound. The bus was not asleep: `0x130` kept arriving at 10 Hz, so any
ACKs were going out while the car was locked. One brief cycle, not the ten-minute armed test above.

---

## 10. Configuration & persistence

### 10.1 The flag is device-local — no backend, by decision

**The dongle owns the flag, in NVS.** Not the account, not the app.

This is a deliberate consistency requirement, not an implementation shortcut. If the backend were
an acceptable dependency for device policy, Phases C and D (backend ECDSA auth) would have been the
access gate instead of the device-local HMAC secret. Routing a safety-critical setting through the
account while access control deliberately avoids it is exactly the split that rots. Same discipline
as the secret: stored on the device, authoritative locally, no round-trip.

Note the vehicle *binding* is server-side — `save()` stores it on the account and
`updateLocalVehicle` only caches it. That is why
the flag must **not** ride there: a second phone signed into the same account syncs `tlm_type` and
`obd_ble_device_ids` and can connect and relay **without ever opening the save screen**. An
account-carried flag would therefore be applied by phones that were never asked the question.

### 10.2 Where it is set: the pairing/save screen, once

The flag is a **property of the car**, in the same category as typecode — not a user preference that
drifts. Set-once at binding is the correct lifecycle.

- **Placement:** inside the existing `isAbrpObd` branch of `ObdBleSetupConnectionScreen`, above
  Save. `save()` already dispatches `updateVehicleObdBleThunk({ tlmType, vinHash, obdBleDeviceId })`
  — the toggle's value is written to the dongle in the same action.
- **Label:** *"OBD port Alarmed?"* — Yes / No. Precise, and for this audience clearer than a
  symptom description.
- **Component:** `MultiToggle` with `type="ToggleButton"`; its `OptionValue` already admits
  `boolean`, so `Option<boolean>[]` needs no string mapping, and its `tooltip` slot carries the
  one-line consequence. (`SwitchCard` is the switch-shaped alternative — its `title`/`description`
  are typed `TranslationKeys`, so i18n is compiler-enforced.) New strings need translation keys.
- **Scope: abrpobd only.** Technically justified, not just rule-compliance: obdble polls only while
  a phone drives the session, so the "parked and armed for hours" exposure barely arises. The
  autonomous dongle is the one at risk.

**Only one write path exists, and that is the point.** Because the save screen is the sole writer,
a second phone holding a shared QR secret cannot disturb the setting — it authenticates and relays
but never reaches the toggle. No tri-state wire contract, no reconciliation, no account field: there
is only ever one copy of the value, held by the device that enforces it.

### 10.3 Default: Alarmed when nothing is stored

Skipping or rushing the question must land on the protected side; someone who knows their car is
clean turns it off deliberately. This follows §8.1 consistently — and it is a change from an earlier
draft of this document, which had it backwards.

Two defaults that must stay **distinct** in the code, because collapsing them is where a subtle bug
would live:

| Situation | Resolution |
|---|---|
| Fresh save screen, dongle reports no stored value | Toggle shows **Alarmed** |
| Save screen, dongle reports a stored value | Toggle shows **the stored value** — never the default |
| Dongle NVS empty / never written (any firmware) | Resolve to **Alarmed** |

The middle row is not cosmetic: without it, anyone re-running setup on a car deliberately marked
"No" gets the protective default pre-selected, and the re-save silently flips it back. It fails in
the safe direction (lost telemetry, not an alarm), so it will never announce itself.

**Name the symptom, because it is the expensive part.** What the user sees is *parked-charge
telemetry quietly disappearing after an unrelated re-pair* — no error, no alarm, nothing pointing at
this toggle. That is the precise shape this project has already misdiagnosed twice (cellular starved
by the transport-handoff gate; a stored hash claiming a profile that was not running), and both times
it cost days aimed at the wrong subsystem. Two cheap defences: read the stored value so the flip
cannot happen (one line), and log the flag every wake per §8.4 so a log dump answers "why is this car
quiet" in one grep instead of a week in the cellular stack.

**Never auto-detect by deliberately triggering the alarm.** It is the only way to measure the
property directly and it is still the wrong product.

**Why unset ⇒ Alarmed is benign for a factory-fresh dongle (decided at implementation):** the
factory-fallback profile was removed at the same time. A dongle with no persisted vehicle profile
does not poll at all — it has no vehicle-correct PID list and no owner's push token, so factory
polling collected wrong data for nobody, and on an alarm car it tripped the alarm before the owner
ever reached the wizard. It now waits for the wizard's push (which persists to flash and drives
every later wake). So "unset ⇒ Alarmed" only ever bites a dongle that HAS a profile but predates
the flag — a firmware-upgraded in-field dongle — which loses parked-charge sampling until its next
re-pair writes the flag, and says why in the per-wake ALARMGATE log line.

### 10.4 Read-back: the dongle publishes its own state

The app stores nothing. To initialise the toggle (§10.3, middle row) and to display the state
anywhere it matters, the dongle **publishes the flag in the readable info JSON on ABD5**, beside the
installed firmware version already exposed by `setOtaInfo()`. That is the established
"dongle-publishes-its-own-state" channel, it is one more field, and it costs **zero GATT handles** —
the frozen table stays frozen. The dongle is authenticated and connected by the time the save screen
renders, so the read is available exactly when needed.

One copy, owned by the device that enforces it, readable by anyone authorised.

### 10.5 Changing it later: unlink and re-pair

Once the binding is saved, reconnecting goes through the poller, not the setup wizard — so the
toggle does not reappear. The edit path is **unlink the dongle from the vehicle and re-pair**: done
on the phone, at the car, in about a minute.

**Deliberately no serial command for this.** An earlier draft proposed `ALARM 1`/`ALARM 0` beside
`AUTHCLEAR`/`BONDCLEAR` as a "lighter" path; it is the opposite. Serial means a laptop, a cable, and
physical access to a dongle in a footwell, where the app path needs a phone already in your hand.
It would also add a second writer to a value whose single-write-path property is what makes §10.2
safe, and it cuts against keeping the serial set as small as possible.

### 10.6 Pin calibration result (only if §6 is pursued)

Separate NVS record — chosen pin, ON/OFF means and variances, timestamp,
schema version, and the **vehicle identity it was calibrated against** (moving the dongle to
another car invalidates the mapping rather than silently applying it). It cannot live inside the
signed profile: the profile is verified verbatim before parsing, and locally-derived data cannot be
injected into a verbatim-verified payload.

**Verify silence, not just the pin.** The invariant is *zero transmissions while parked* — directly
observable with tooling that already exists: flag the car, park and lock it, wait out two wake
cycles, fetch the log over BLE. Wakes, gate verdicts, and a transmission count that reads zero.
This answers "did it work?" — which a correctly-read pin alone never does.

---

## 11. Ongoing re-verification & fleet learning

Both checks use signals already collected:

- **Primary:** `gpsMovingConfirmed()` true while the policy is in the quiet state ⇒ the gate is
  broken. Independent of polling, so it can falsify (§5).
- **Inverse (the plausibility rule §8.3 depends on):** the calibrated pin reads ON while GNSS
  reports no movement for more than ~30 min ⇒ distrust the pin. **Deliberately not conditioned on
  the rail resting** — the dangerous case is pin-stuck-high *while the rail is energised*, i.e. a
  parked charge session, which is the one combination that passes both terms of §8.3's permit. A
  genuine ignition-on state that persists for half an hour while stationary is implausible anyway.
  Drop the mapping, fall back to §8.3's rail+GNSS permit, log it, flag the user.

**Fleet learning:** report calibration outcomes (model, pin, verdict) through the app. A per-model
pin map accumulates from the field; pre-fill the guess for known vehicles, but always verify per §7
rather than trusting the table — and the sequential one-input procedure (§6.3) makes the pre-fill
genuinely time-saving.

---

## 12. What this gives up, and the research track

Without a calibrated pin, an alarm-flagged car gets **drive telemetry only** — no parked-charge
data. The one possible exception: during a charge session the bus is typically active, and passive
sniffing issues no request, so (if §9 passes) the data may simply be there to read. Costed as a
research track, not a promise:

- Needs per-model passive **frame** decoding — the profile system carries PID *request* definitions
  (`data_commands`, `ATCRA` handling), not a frame map. New profile concept, new decoder.
- Scope the i3 first: it produced §2, and OVMS has published its CAN IDs.
- **i3 checked 2026-10-01: ruled out at the OBD port.** It carries only `0x130` (terminal status); the
  gateway forwards no other broadcast, so there is nothing to decode (Part I §6b).
- Prerequisite tooling: a serial `SNIFF` diagnostic dumping live frame IDs/rates (`STAT`/`RADIO`
  family, physical-access only).

---

## 13. Build order & scope summary

Sequenced by **cheap-experiment-first**: the two bench tests each take minutes and each can
invalidate or unlock a large branch, so neither should sit behind implementation work.

| # | Step | Why here |
|---|---|---|
| 0a | **`ATM1` silence test** (§9) — locked, armed, ten minutes | Ten minutes decides whether §12 exists at all. Largest branch, smallest cost. |
| 0b | **Bench test: +12 V into pins 1/8/9/13** (§3) — watch `ATRV`, IO36, logs | Settles whether our board revision differs from r14 before any harness work. |
| 1 | **The gate** (§8) — one pure default-deny function, conjunctive instantaneous permit, deferred init, per-wake logging | **IMPLEMENTED 2026-08-27** (`Helpers/alarm_gate.h`, wake-gate + poll-site wiring, per-PID guard in `OBDWrapper::readPIDs`, no-factory-fallback). |
| 2 | **Toggle + read-back** (§10) — save-screen toggle, NVS flag, ABD5 info field | **IMPLEMENTED 2026-08-27** (app: toggle on the save screen, `obd_alarm` ABD2 key, `alarm` field in the ABD5 info JSON, read-back before display). |
| 3 | **Silence verification** (§10.6) — park, lock, two wake cycles, fetch log, expect zero transmissions | Proves the property rather than assuming it. Do before trusting it on a real alarm car. |
| 4 | **GPIO34 tap harness + calibration** (§6, §7) — only if 0b says the pins carry anything | Optional accuracy layer; recovers the stationary-ignition-on case. |
| 5 | **Passive frame decoding** (§12) — only if 0a passed | Research track, i3 first. |

Steps 1–3 are the deliverable. 0a/0b are prerequisites for 4–5 and cost almost nothing, which is
why they run first rather than when 4–5 are reached.

| Item | Status |
|---|---|
| Rail gate, leashes, cadences on non-alarm cars | **Unchanged** — pure add-on discipline |
| Hidden co-processor command for pin reads | **Ruled out** — three-way proof (§4.1); do not re-hunt |
| STM32 firmware modification / SWD | **Ruled out** — closed blob, no image to restore; do not attach a programmer (§4.2) |
| On-board sensing of pins 1/8/9/13 | **Ruled out** pending the §3 bench test on our revision |
| Bench test: +12 V into 1/8/9/13, watch ATRV/IO36 | **Entry gate** — cheap, settles the revision question |
| GPIO34 Molex tap harness + divider | **The measured route** — new accessory hardware, no board revision (§6) |
| App-guided per-pin calibration, sequential | **Restored** (§7) |
| `ext0` wake from calibrated ignition edge | **New**, where a pin qualifies |
| Conjunctive rail+GNSS permit, **instantaneous — no latch, no grace period** | **The architecture** — works with or without the pin (§8.2–8.3) |
| Accelerometer ruled inadmissible as a hold signal | **Decision** — exiting the car produces motion (§8.3) |
| OBD link lifecycle decoupled from the permit | **New** — gate requests, not the link; avoids re-init churn (§8.3) |
| One pure default-deny gate function, every call site through it | **New rule**, the reliability keystone (§8.4) |
| Defer OBD init until a permit exists | **New**, stronger than validating init commands (§8.4) |
| "OBD port Alarmed?" toggle at pairing/save, abrpobd only | **New**, small — sole write path (§10.2) |
| Flag in dongle NVS, device-local, **no backend** | **Decision** — consistency with the HMAC access gate (§10.1) |
| Default Alarmed when unstored; toggle reads back stored value | **New**, both defaults kept distinct (§10.3) |
| Flag published in ABD5 info JSON for read-back | **New**, zero GATT handles (§10.4) |
| Edit path = unlink + re-pair; no serial command | **Decision** (§10.5) |
| Init-command locality check on alarm cars | **New**, small — also closes §4.3 (§8) |
| `ATM1` silence test | **Prerequisite** for anything using sniffing (§9). Partial pass on an i3 (§9) |
| Passive frame decoding for parked-charge telemetry | **Research track** (§12). i3: ruled out, only `0x130` reaches the port |
| Fleet-learned per-model pin map | **New**, app-side, verify-don't-trust (§11) |
| BimmerCode / CAN isolation / pin-16 reroute | **Not required** — silence suffices |


---

# Appendix J — Alarm-gate field findings & fix plan

> Original file: `ALARM_GATE_FIELD_FINDINGS.md`

# Alarm gate — field failure findings & fix plan

2026-08-28. Companion to `IGNITION_SENSE_ARCHITECTURE.md`. Three parallel deep traces (firmware
GNSS chain, firmware wake/sleep + wizard, app wizard/poller) against the field symptoms:

1. wizard fails at "Car connection status";
2. the dongle stops advertising and cannot be found again;
3. a long real drive never produced a single permit — no telemetry by any transport.

Every finding below carries file:line evidence; the wizard failure is additionally confirmed in a
field flash-log dump. **Current state: the gate is BYPASSED (`OBD_ALARM_ENFORCE 0` in config.h) — the
flag is still stored, published and shown in the app, but has no behavioral effect.** Re-enabling is
that one switch — but do not flip it before the Phase-1 fixes below are in.

---

## Root cause 1 — wizard failure: `clean()` does not reset `setup_unlocked`

`BLEWrapper::clean()` (blewrapper.cpp ~398-411) clears `config_keys`, `configJson`,
`tlm_push_token`, `hash_ok` — but not the `setup_unlocked` latch (reset only by
`clearAuthentication()` on disconnect and by `deinit()`). When the app writes `{"AUTH":…}` again on
the same connection (a retried setupThunk), the AUTH handler at blewrapper.cpp ~549 is skipped
(latch already set) and the write falls through to the generic package path:

- replies `ACK` (~619-620), then
- is refused as an unsigned profile key by the PROFILE_REQUIRE_SIGNATURE lockdown (~685-692) →
  `UNSIGNED`. Two contradictory replies to one write.

Field trail (flash log): AUTH → AUTH OK → clean → CLEAN OK → five refused re-AUTHs over ~22 s →
no `keys`/`HASH`/`JWS`/`SETUP OK (push complete)`. Consequences cascade: `completeSetup()` never
runs → the loop's config-apply never fires → `alarmGrantSetupPermit()` never called →
`setup_permit=0` on every ALARMGATE line → PID test gate-denied → wizard shows the generic
"OBD Connection Failed" checklist (the real reason, `no soc`, goes only to the eventlog).

**This also made the flag unrecoverable from the app**: Save requires the PID test, the PID test
requires the permit, the permit requires the push, the push was broken. NOTE: this bug is
independent of the alarm gate (it dates from PROFILE_REQUIRE_SIGNATURE) — the gate only made it
fatal. It is still latent with the gate bypassed, reachable whenever the app re-AUTHs on a live
connection.

**Fix (Phase 1):** in `StartupCallbacks::onWrite`, dispatch `message_key == "AUTH"` explicitly to
`authenticate()` (idempotent, replies AUTH OK) instead of dispatching on the latch state.

## Root cause 2 — the disappearing dongle

Two stacked mechanisms:

1. **5-minute unauthenticated window.** In the BLE state only `bleRelaying()` (connected AND
   authenticated) refreshes the leash (state_machine.h ~175-177). A wizard stalled before auth hits
   `ACTIVE_TIMER` → CHANGING→STANDBY (both timers 5 min against the same stamp) →
   `enterDeepSleep()` **mid-wizard**. The sleep calls `BLEDevice::deinit(true)` with no HCI
   disconnect — the phone believes it is connected until its supervision timeout.
2. **Suppression upstream of BLE bring-up.** The flagged-car stationary-poke suppression
   (logger.ino ~609) sits before `bleInitForWake()` (~682); the skip path deep-sleeps first. So
   every 30-minute timer wake on a stationary flagged car is **radio-silent** — a polling decision
   accidentally became a connectability decision. Only fresh motion (WOM ≥ 0.1582 g) or a cold boot
   (unplug/replug) makes the dongle visible again.

**Fixes (Phase 2):** (a) feed the leash from ABD2/ABD8 write activity — handshake writes require a
paired link, so a camper cannot exploit it; a stalled wizard no longer dies at 5 min. (b) Product
decision needed: advertise briefly on suppressed timer wakes (battery cost every 30 min) vs stay
radio-silent (unreachable until motion). Recommendation: keep radio silence — with fix 1 the wizard
completes well inside a motion-opened window.

## Root cause 3 — the blind drive: `gpsBegin()` one-shot + `g_gps_powered` lie

`gpsBegin()` (FreematicsPlus.cpp ~549-578) gives the GNSS ~1.4 s to emit a sentence; on failure it
**powers the GNSS back down** (`gpsEnd()` + clears `FLAG_GNSS_USE_LINK`) and returns false — and
logger.ino ~839 sets `g_gps_powered = true` **regardless**. `gpsGetData()` then takes the dead
direct-UART branch (no decode task) and returns false every cycle. No retry exists anywhere; every
sleep sends `ATGPSOFF`, forcing the next wake into a cold start (TTFF measured 329 s) and the same
one-shot. One quiet failure at wake = no fix = `gps_moving_streak` pinned 0 = **permit structurally
unreachable for the whole wake**, i.e. the whole drive.

Compounding: on a denied cycle while the phone is relaying, the BLE frame is stripped to
`{is_parked,is_charging}` (telemetry_format.h strips lat/lon/utc), the BLE send succeeds, so
logger.ino ~1360 **clears the upload buffer**, and `bleRelaying()` stands cellular down — data
flows nowhere; only the flash CSV survives. The app compounds it: after 5 min without telemetry the
UI reports "Not connected" on a perfectly healthy link (bleStateThunk/ConnectivityStatus).

**Fixes (Phase 1):** `g_gps_powered = <actual gpsBegin() result>`; when it failed on a
motion/cold wake, re-attempt `gpsBegin()` from the loop, one attempt per poll cycle until it takes
(state-driven recovery of a failed init, not a blind retry loop).

## Root cause 4 — the rail margin

Measured on this car: driving rail 13.33–13.47 V vs `RAIL_AWAKE_VOLTS 13.3` — 0.03 V headroom.
The bands (off 12.5–13.2, moving 13.25–15.2) leave no safe lower threshold, so the margin is
inherent per-vehicle. Any car whose driving rail sits below 13.3 gets a permanent rail-term deny.
**Not code-fixable** — this is the documented per-vehicle calibration requirement. The Phase-1
diagnostics fix (below) is what makes it visible in the field.

## Diagnostics hole — one DENY line per wake

`alarmGateAllowsPoll()` logs once per wake plus transitions only. A 60-minute all-deny drive
produces a single line and no record of WHICH term failed over time — the exact "quiet with an
empty log" shape the logging was meant to prevent.
**Fix (Phase 1):** re-log every ~2 min while denied, with per-term values
(rail, moving, fresh, hold, setup_permit, tx).

## Freshness bound is inert on the link path (garage exposure still open)

`gd->ts` is stamped with `millis()` **at call time** (FreematicsPlus.cpp:627, link branch) — the
co-processor re-echoing the last `$GNIFO` fix refreshes `ts` every cycle, so
`ALARM_GNSS_MAX_AGE_MS` never fails and a latched movement streak stays "fresh" forever. This fails
toward PERMIT: drive into a covered garage (fixes stop, streak latched), park, switch off (rail
deny ✓), plug in → rail back up → permit on a locked, armed car. Secondary: `parkAccumSample()`'s
ts-based dedupe contract is unimplementable on this path.
**Fix (Phase 4):** key freshness on the `$GNIFO` `date`/`time` fields CHANGING (parsed but unused
today), tracked against a local millis stamp of the last change.

Also found in the chain (record, low priority):
- `$GNIFO` parse sets `good = true` before `speed` is parsed — a truncated reply yields fresh
  coordinates with stale/zero speed.
- The `$GNIFO` raw-reply diagnostic is dead code: `#if print` (lowercase) never compiles in.
- SPI link OOB read: FreematicsPlus.cpp ~440 reads `buf[5..11]` without checking length.
- A timer wake mid-drive on a flagged car is dropped before GPS/BLE/modem power-up — the drive
  depends entirely on WOM, which the codebase itself documents as blind on smooth asphalt.
- The per-PID guard routes around `alarmGateAllowsPoll()`, so the sustained-collapse timer only
  advances at cycle boundaries, never during the multi-second frame it is meant to interrupt.
- `MAX_AWAKE_MINUTES` backstop is not conditioned on `bleRelaying()` — it can cut a live
  authenticated wizard/drive session at 60 min. `handleActivityState()` also runs before the
  config-apply block, so a sleep can wipe a pending (stashed) profile push.

---

## App-side findings

**A1 (real bug, abrpobd-only, Phase 3):** `setupThunk`'s prologue guards use `return reject(...)`
where `reject` returns void — the async thunk **fulfils with `undefined`**. "Not connected",
"not abrpobd", "awaiting pairing", "no PIDs", "no packages" all look like SUCCESSFUL setup to
`.unwrap()` callers, produce unhandled rejections, and make the `OBD_BLE_AWAITING_PAIRING` branch
in bleSlice unreachable from setup. (setupThunk.ts ~78-163.)

**A2 (abrpobd-only, Phase 3):** `listenerThunk` has no silence handling — a connected dongle that
never pushes `tlm` leaves the promise pending forever with a live monitor subscription. In the
wizard, the leaked subscription's late error/frame runs a STALE continuation during the next
attempt: its `stopCountdown()` kills the new stage's only timeout (shared countdown ref), or a late
good frame flips stage 3 to Success mid-retry (Save enabled on a mid-reconnect link). Fix: cancel
the listener subscription when stage 3's countdown fires.

**A3 (verified PRESENT and correct):** the hand-applied desync fix — `OBD_BLE_GATED_IN_FLIGHT`
sentinel, bleSlice early-return branch preserving deviceId/isAbrpObd, three `stopCountdown()`
calls. The gated-in-flight path is closed. Gaps: no test covers the new branch; `disconnect`'s
`cancelDeviceConnection` is fire-and-forget so a fast retry can race the native teardown.

**Shared code, NOT touched (owner decision):**
- `bleConnectThunk` rejects after a successful `connectToDevice` without cancelling
  (:66-69 discovery failure, :109-111 missing config) → same Redux-vs-radio desync shape, reachable
  from stage 2 for BOTH lanes → retry storm of `DeviceAlreadyConnected` until the OS drops the link.
- The shared single-slot countdown ref (useObdBleSetupIcons) lets any stale `stopCountdown()`
  disarm the current stage.
- `pauseCountdown()` on `bt_bond_state: 'bonding'` is never auto-resumed if the user ignores the OS
  dialog — stage 3 spins forever with a frozen number.
- Poller: no backoff/background-gating — a sleeping dongle costs a connect attempt every ~15 s or
  continuous `allowDuplicates` scanning, and floods the 300-entry eventlog ring (evicting the very
  evidence a bug report needs).
- While driving with the gate denying, the app is fail-silent: the poller never inspects `soc`,
  `awaitingPairing` is never set (not an ATT error), and after 5 min the UI shows "Not connected"
  for a healthy link.

---

## Fix plan (owner-approved order; none built yet)

| Phase | Fix | Where |
|---|---|---|
| 1 | AUTH dispatched by key, not latch (root cause 1) | blewrapper.cpp onWrite |
| 1 | `g_gps_powered` honesty + loop-side `gpsBegin()` recovery (root cause 3) | logger.ino |
| 1 | Periodic DENY re-log with per-term breakdown | alarm_gate.h |
| 2 | Handshake-activity leash feed (kills the 5-min mid-wizard death) | blewrapper + state_machine |
| 2 | DECISION: advertise on suppressed stationary wakes, or stay radio-silent | logger.ino |
| 3 | setupThunk fulfil-instead-of-reject bug | app, abrpobd only |
| 3 | Wizard-scoped listener-subscription cleanup on countdown expiry | app, abrpobd only |
| 4 | Real fix-time freshness key (closes the garage exposure) | gps_location.h |
| — | Shared-code items above | owner decision |

**Re-enable checklist:** Phase 1 complete → flash → wizard on the flagged car (expect full push,
`setup_permit=1`, PID test green) → one real drive with the periodic DENY log captured → only then
set `OBD_ALARM_ENFORCE 1`.
