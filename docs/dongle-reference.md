# Detailed dongle reference and design history

Start with [DONGLE_CONTEXT.md](../DONGLE_CONTEXT.md) for the current overview and section index.
This consolidates operational detail, dated measurements and design decisions. Read the relevant
section, not the whole file. Protocol details live here once; implementation and release tooling
remain authoritative.

**Evidence is not a current backlog.** Dated captures and modeled power budgets retain their
original conditions. Research proposals are distinguished from implemented mitigations; closed-source
app status cannot be verified from this repository. Do not infer hardware validation from a source
inspection. The pre-consolidation text remains in git at `681c658:docs/dongle-reference.md`.

Section and appendix numbers are retained for existing source references. Paths in backticks are
relative to the repository root or `baseLogger/`, as in the original text. Named design documents
resolve to the appendix table in Part II; they are not missing files.

---

<a id="section-0"></a>

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
- App: the ABRP app (closed source) — `tlm_type: 'abrpobd'` identifies this dongle; the older generic
  dongle lane is `'obdble'`. **The two lanes must stay separate** — see §8.

---

<a id="section-1"></a>

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

<a id="section-2"></a>

## 2. The BLE protocol (GATT service ABD0)

**FROZEN TABLE.** The service is created with a fixed handle budget (32; 30 used). iOS caches the GATT
table, bonded phones harder, and nothing here tells a phone to rediscover it: the Arduino BLE library
has no Service Changed or Database Hash API, and Bluedroid's own automatic Service Changed fires when
the service starts, every wake, before any phone is connected. So a table that shifts handles silently
breaks already-paired phones. **Handles are not assigned in creation order**: `BLEService::start()`
registers characteristics in the order of a map keyed by object pointer, i.e. by heap address. Each
CCCD lands right after its own value. `init()` logs every value handle at boot (`GATT handles
ABD1-A=…`), and a table change is checked by comparing `esp_ble_gatts_show_local_database()` dumps.

ABD9/ABDA were created as reserved placeholders so a feature could reuse one instead of reshaping the
table. That one change was made in #90, while no dongle firmware was public: ABD9 became the protocol's
session channel and ABDA a notify-capable spare, each gaining notify and an encrypted CCCD (28 → 30
handles). ABD1–ABD8 are unchanged, because the released app (7.1.13) uses them. Whether a manual
Service Changed indication on the first encrypted connection after a table change makes bonded phones
rediscover is the Service Changed spike (`spike/service-changed`, bench only); until it is proven, a
phone bonded on the old table sees ABD9 without notify until its cache refreshes (the app polls ABD9's
state then).

| UUID | Purpose |
|---|---|
| `ABD0` | The service |
| `ABD1` | Telemetry (notify + read) — the live frame; in a v1 session `{"tlm":{…},"seq":n}` |
| `ABD2` | Setup/config: token handshake, profile push, `obd_alarm` flag; `PROTO` in a no-protocol session |
| `ABD3` | Developer |
| `ABD4` | Error |
| `ABD5` | OTA control + the readable info JSON (installed fw, state, `alarm`) |
| `ABD6` | OTA data (write-only firmware payload) |
| `ABD7` | Log dump |
| `ABD8` | Auth handshake (nonce + HMAC), and the protocol version negotiation |
| `ABD9` | Session channel (v1+): write, notify, readable state — relay acks, stop/resume, relay state |
| `ABDA` | Spare: read, write, notify + CCCD, inert (**do not repurpose casually**, see FROZEN TABLE) |

### Protocol versions (`Helpers/ble_protocol.h`)

Every nonce frame offers the versions this firmware speaks, `"proto":[BLE_PROTO_MIN,BLE_PROTO_MAX]`
(`[0,1]`). An app that negotiates answers with the version it chose in its proof
(`{"proof":…,"proto":1}`), and the `ok` names the session's version only when the proof carried one
(`{"auth":"ok","proto":1}`), so an app that does not negotiate reads exactly what it always has.
Absent means 0; malformed means absent (a version is a JSON unsigned integer token 0–255, so `"1"`,
`-1`, `1.5`, `1.0` and 2^40 are not); a version above the maximum runs at the maximum; below MTU 96 at
the proof the dongle picks v0. The version belongs to the session grant: a further proof on the same
grant keeps it (`AUTH2 ok - session kept proto=N (asked M)`), and with no grant the protocol is v0.

- **v0** is today's protocol: an authenticated phone is the transport while connected, its frames
  carry no `seq` and are not queued, ABD9 carries nothing (messages there are logged once by key and
  ignored).
- **v1** (#90) adds the relay session below.
- A proof **below `BLE_PROTO_MIN`** (`config.h`, 0, so unreachable today) gets a session with **no
  protocol**: OTA and the log dump work, nothing is relayed on ABD1 (frames stay queued for cellular),
  and every ABD2 write is answered `PROTO`, decided on the Bluedroid task from a `setup_allowed` bit the
  loop task sets before the grant. Raising the minimum strands apps that do not negotiate; judge it
  from the `app=M` field of `AUTH2 ok - session granted proto=N app=M range=MIN-MAX`, never guess.
- Callers never test a version number: they call the session's table (`bleProto()`). A new version
  builds its table from the one below; a message the other side may ignore goes on ABD9 freely, one it
  must act on needs a version.
- Contract tests: `protocol/fixtures/` holds one golden exchange per firmware × app pairing, replayed
  by the firmware (`tests/run_relay_checks.py fixtures`) and copied into the app with its `SHA256SUMS`.

### Hard-won BLE facts

- **MTU 247 is a ceiling, not a preference.** It's the largest value where a notification still fits
  one radio packet (251-byte DLE). At 517 the small frames arrive and every large chunk is
  *deterministically lost*. Measured, after being wrong about it.
- **A notify carries at most MTU−3 bytes and is never fragmented.** Chunk sizes are derived from the
  negotiated MTU for this reason.
- **Android stays at MTU 23** until the app requests a larger MTU. iOS negotiates ~185 itself.
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

### Relay acks: is the phone actually carrying the data? (#90, protocol v1)

A connected, authenticated phone used to count as the transport for as long as it stayed connected, and
a frame notified on ABD1 was never queued for cellular. An app that stopped posting (backgrounded, logged
out, offline) silenced the car: an ID.5 drove 65 minutes in the `BLE` state with nothing reaching ABRP.
ABD1 is notify-only, so the app has to say what it posted (`Helpers/ble_relay.h`). In a v1 session:

- An ABD1 frame sent **while the app is subscribed to ABD1** carries `"seq":n` beside `tlm`, counting
  from 1 every wake, and is also queued for cellular under that seq. Its bytes are reserved from the
  notify budget, so the trim drops telemetry only. A frame sent while the app is not subscribed (e.g.
  during the profile push) carries no seq and is queued as a plain cellular frame, so a long push cannot
  run the clock. ABD1's CCCD is cleared on disconnect: a reconnect starts unsubscribed.
- **Ack:** after each successful `APP_OBD` post the app writes `{"relay_ack":n}` to **ABD9**,
  cumulative over the frames it posted (it posts the latest frame per tick); acked frames come off the
  queue. **Stop/resume:** `{"relay":"stop"}` when it knows it cannot post (offline, logged out, a 401
  pause, a 403) hands the transport to cellular at once; `{"relay":"resume"}` is the only thing that
  ends a stop (an ack from a post already in flight does not). Not on backgrounding: the app relays with
  the phone locked.
- **Timeout, the safety net:** once tagged frames have waited `RELAY_ACK_TIMEOUT_MS` (120 s) with no ack
  moving forward, cellular takes over with every frame still queued; the next forward ack or a resume
  hands the transport back. Nothing outstanding is never late, and an update or a log dump (which pause
  polling) holds the clock without changing the state; the cellular gates hold off for both on their own.
- **Unsubscribed, the second bound:** an app that acked everything and then stopped listening on ABD1 (its
  subscription died, the link and grant did not) acks nothing more, so the ack clock never runs. Once frames have
  been queued untagged for `RELAY_UNSUBSCRIBED_MAX_MS` (300 s, above the longest profile push, which runs before the
  app subscribes) while it is not subscribed, the session lapses (`"why":"unsubscribed"`); subscribing again, a
  forward ack or a resume hands the transport back. An update or a log dump holds this clock too. Only a frame with
  data takes a seq, so an empty frame never starts the ack clock without being queued.
- **ABD9** answers with events (`{"relay":"lapsed","why":"timeout"|"stop"|"unsubscribed"}`, `{"relay":"live"}`, as
  notifies that never replace the value) and a readable state, `{"relay":"live"|"lapsed","why":…,
  "sent":s,"oldest":o}` (`{}` outside a v1 session, and to any link or grant it was not built for; at most 93 B,
  checked at compile time), served from `onRead` under the relay lock. `sent` is the last seq notified this wake;
  `oldest` the lowest seq still in the cellular queue (`obd_result_vector` and `g_upload_held`, tagged frames only),
  0 for none. It is rebuilt at the top of every loop pass and on each transition, so a frame shows from the pass
  after it was queued. Writes are dropped unless the link holds a grant, and one over 64 B is
  not parsed (logged as key `?`); the
  Bluedroid callback keeps one grant-stamped mailbox (highest ack, last verb), applied on the loop task
  by `sessionChannelService()` — ack first, then the verb — and an entry from an ended grant is
  discarded (`BLE session stale grant`).
- **The app's give-up rule:** after a failed post of seq N the app acks nothing until a state read shows `sent >= N`
  and `oldest` 0 or above N; a cumulative ack before that would drop N from the queue unposted. `sent >= N` proves the
  state was rebuilt after N was queued. A frame held for the next wake keeps `oldest` at or below it, so that app acks
  nothing more this wake: cellular carries the frames, duplicates at worst, never a lost point. The app also writes
  `{"relay":"stop"}` as soon as it gives up on N, so cellular sends N at once instead of after the 120 s timeout, and
  `{"relay":"resume"}` once the state shows N gone (and it can post). No firmware change: both verbs and the drain
  rule are v1 as released (`v1/fw-0-1_app-0-1-give-up-stop.json`).
- The session is one state machine: `NO_SESSION` → `GRANTED` (v0, no protocol) or `LIVE` (v1);
  `LIVE` → `LAPSED(stop|timeout|unsubscribed)`; back to `LIVE` on resume (or a forward ack after a timeout or
  unsubscribed lapse, or the app subscribing again after an unsubscribed one); any →
  `NO_SESSION` when the grant ends. An ack beyond the last seq sent is ignored, and a session's unacked
  frames lose their seq when it ends (sessions are counted by grant, so a reconnect between two loop
  passes still ends one), so the next session's acks cannot drop them. An ack still on its way when a
  session ends is lost with it: a duplicate point over cellular, never a lost one.
- **An app that does not negotiate gets v0 and is trusted as before**, frames byte-identical, so
  current apps keep #90 until they negotiate. Presence (the awake leash, the alarm gate) keys on
  `bleAuthenticated()`, which a lapse does not change.
- Log lines: `BLE relay acks on`, `BLE relay lapsed no ack … -> cellular`, `BLE relay lapsed unsubscribed for Ns
  queued=k -> cellular`, `BLE relay stop by app`, `BLE relay resume by app`, `BLE relay resumed ack=`,
  `BLE relay resubscribed`, `BLE relay ack ignored`,
  `BLE relay end acked= sent= unacked=`, `BLE session proto=N ignored key=K` (at most 8 keys a session, then
  `BLE session further ignored keys not logged`), `BLE session stale grant`.
  Valid acks themselves are not logged.

---

<a id="section-3"></a>

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
  destroys revocation.) It and the in-app key reset also forget the vehicle: profile file, stored hash,
  push token and upload queue (`PROFILE forgotten`), so polling and upload wait for the next owner's push.
  AUTHCLEAR also ends the grant of a session already open. Both erase the flash log too (newest segment first,
  before the key goes, so a power cut part-way leaves a reset that can be repeated) and end a log dump or update the
  revoked session had running (`err:ended`, OTA `ABORTED`/`NOAUTH`); serial AUTHCLEAR is refused during either, like
  CLEAR. The fresh log opens with `PROFILE forgotten (authreset|authclear) - log erased (N segments) …` after the
  header and `FW build=` line. If a segment, the profile file or the backlog file cannot be removed (or the log is not
  mounted or cannot be listed) the reset fails (`{"auth":"error"}`, or `AUTHCLEAR FAILED` on serial) with the key and
  token kept, so it can be repeated. An in-app reset on a dongle with no key, such as a repeat after a reply lost on a
  slow erase, still answers `reset`. The files are deleted,
  not scrubbed: a raw read of the flash chip can still recover them, and a reset does not touch the coredump
  partition, BLE bonds (`BONDCLEAR`), the OBD-alarm flag or the clock and OTA NVS. The in-app reset needs only the
  power-on window, not a grant, so anyone in range then can erase the log.
- **The handshake also negotiates the BLE protocol version** (§2): every nonce frame carries
  `"proto":[MIN,MAX]` (70 B, 80 B with `"reset":1`, 84 B at worst, held under the 93 B notify of the MTU
  96 floor at compile time), the proof may carry the version the app chose, and the `ok` names the
  session's version when it did. The version is chosen after the HMAC check, never before it.
- **Greet on the CCCD rising edge.** The dongle speaks first, when the app's subscription is armed,
  which removes the subscribe-vs-write race entirely — no retries, no timeouts.
- **Two flags, deliberately:** `setup_unlocked` (the ABD2 state-machine latch) vs `authenticated`
  (the session grant). Conflating them made enforcement impossible. Both are cleared on disconnect and again on
  connect, so a grant is always the current link's own (the protocol version and the relay session are keyed on it).
- Why it exists: the old handshake sent the vehicle's **cloud push token in plaintext on every
  connect**. Sniff it once and you could push fabricated telemetry for that vehicle forever.

### Layer 3 — Sign: is this payload legitimate?
A pushed JWS is applied only if its backend RS256 signature verifies. With `PROFILE_REQUIRE_SIGNATURE 1`
that is the only way in; the default in `config.h` is 0, so raw per-key packages are still accepted
(see above).

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

<a id="section-4"></a>

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

<a id="section-5"></a>

## 5. Log retrieval (ABD7)

- App-driven pull; the dongle never streams unprompted. While a dump runs the poller stands back, as
  for OTA.
- Over USB serial the same log comes from `DUMP`, `DUMP n` or `DUMP SINCE <utc>` (§9).
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

<a id="section-6"></a>

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
  A power-on in the band (`g_weak_rail_cold_boot`) stays up `WEAK_RAIL_COLD_BOOT_AWAKE_MS` for BLE only, so a person
  who just plugged it in can reach it: no OBD request, no GNSS, no modem. Every site checks the flag itself (#104,
  which found `loop()` polling the car for the whole window): the poll condition vetoes the cycle as a gate-denied
  one, the boot restore and a BLE profile push defer `obd.initialize()` to the next boot that is not a weak-rail cold
  boot, and the boot clock resync is skipped. The veto never lifts within the wake, so the wake ends instead: the
  window expires with no phone connected (parked cadence), or a cycle's rail reaches `RAIL_AWAKE_VOLTS` (the car was
  switched on) and it sleeps `WEAK_RAIL_RECOVERED_WAKE_MINUTES` so the normal wake follows. A connected phone holds
  that off only inside the key-reset window, which the sleep would close (the reset needs a power-on boot).
- `RAIL_AWAKE_VOLTS 13.3` is **calibrated to one car with almost no margin.** Measured over 1016 field
  samples: moving 13.25–15.2 V (mode 13.4), switched-off 12.5–13.2 V. The bands nearly touch, so
  there's no room for a lower release threshold and **no hysteresis is possible**. Measure any new
  car before trusting anything that keys on it.
- **Wake sources:** wake-on-motion (ICM-42627 interrupt, threshold ~0.158 g) and a timer
  (`WAKE_PARKED_MINUTES 30` / `WAKE_CHARGING_MINUTES 3`).
- **A confirmed DC fast charge stays awake** (issue #51). A poll that reads DC (the car's `is_dcfc`, else at least
  `DC_CHARGING_MIN_KW` inferred) holds the wake, and the car's `is_dcfc`, or two inferred DC polls in a row (one garbage
  frame must not do it), latch `dc_session` (RTC) for that charge. While the rail
  is at least `RAIL_AWAKE_VOLTS`, each poll that still shows charge evidence (the taper under 40 kW included), or
  comes within `CHARGE_SILENT_HOLD_POLLS` x `WAKE_DC_CHARGING_MINUTES` (4 min) of unbroken silence, sets
  `g_dc_confirmed`. Silence is counted by time, not by its 30 s polls: that is the tolerance a 1-minute DC wake had,
  and a car silent on ~60 % of polls would otherwise drop to `parked-held` several times an hour. The dongle then stays awake and polls
  and uploads every `DC_CHARGING_AWAKE_SECONDS` (30) over one reused session, and the ladder holds in `CHARGING`.
  - `dcAwakeHeld()` vetoes the hold live, not only at a poll: the BLE state (it keeps its own cadence), a rail under
    `RAIL_AWAKE_VOLTS`, an alarm-flagged car (§7), or motion within an `ACTIVE_TIMER` (`g_last_motion_ms`, stamped only
    by `detectMovement`). A drive starting is therefore polled at ACTIVE's 5 s from its first motion.
  - Motion on a held sample poke promotes the wake in place: BLE and GNSS come up, and from then on charging samples
    need the streaming confirm (`CHARGE_STREAM_CONFIRM_SECONDS`), so regen is never a charge. Sleeping instead would
    not bring a motion wake: `configureWOM()` clears motion already seen, and a person sitting still does not trip WOM.
  - Ends: the charge end (an answered poll with no charge evidence), the rail drop, silence past the hold
    (`parked-held`, as before), the cap, corroboration, the car's `is_dcfc` 0, a drive. The session itself ends at the
    charge end (an answered poll with no charge evidence, so a DC plug swapped for AC starts fresh), a fresh latch, a
    drive, the car's `is_dcfc` 0 or the stale reset. A sleep inside the session uses the 1-minute DC tier; the charge
    end and the rail drop sleep as before (3 min while latched). AC charging never latches.
  - Each held poll of a sample poke counts a whole `WAKE_DC_CHARGING_MINUTES` toward the cap, so it trips sooner (the
    safe way). A streaming wake (motion, a promoted poke) counts its elapsed time, at most a tier per poll, so somebody
    moving about the car for an hour costs an hour of budget, not a tier per 5 s poll. The
    max-awake backstop is not exempt: after `MAX_AWAKE_MINUTES` of awake time the wake sleeps 1 min and the session's
    next sample poke holds again, about a 1-minute gap per awake hour.
  - BLE keeps its rules, but its windows move: a held motion wake keeps advertising up to the backstop instead of about
    10 minutes, and a held sample poke has no BLE until something moves and promotes it.
  - With `ENABLE_IO36_RAIL 0` the rail reads 0 and it never holds.
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
- `bleRelaying()` requires authentication **and** the negotiated protocol's phone-transport state
  (v1 can hand back to cellular). **This, not `bleConnected()`, controls cellular stand-down.**
  Gating on mere presence meant a phone that never authorised silently
  starved cellular — data flowed *nowhere* live while frames piled up. Found in the field.
- Deep sleep tears BLE fully down (`BLEDevice::deinit`) — a left-initialised controller keeps
  deep-sleep current high and holds tens of KB of heap.
- Substantial deferred power work (STM32 reset-hold, modem power-down hardening, gate restructure)
  is scoped and costed in `DEFERRED_POWER_WORK.md` (Part II) — **read it before touching sleep**.

---

<a id="section-6a"></a>

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
Deflate is vendored zlib 1.3.2 (`libraries/zlib`, level 9, 4 KB window), driven from `gzip_body.h`, with
~40 KB of compressor and verifier state in PSRAM. Each body that is sent is inflated again and compared with
its input first. A body that fails is not sent, and its frames are kept. After `GZIP_FAILS_PER_WAKE`
failures (verify or server gzip errors), or after a reset inside the compressor (an `RTC_NOINIT`
breadcrumb), the rest of the wake goes plain where plain fits. A frame too big to send plain is held for
the next wake's backlog, and the frames behind it still go. It must not go back to the boot-ROM miniz: ROM `tdefl` needs ~6.5 KB of stack, which overflowed the
8 KB loop task on the pre-sleep drain, and ROM code has no PSRAM cache workaround (issue #21).

Bodies over 4 KB are in PSRAM, which this rev 1 chip writes safely only through the `esp32-psram` runtime
(§11 item 8, #64). Linked without it, the verifier refused up to every body over ~5 KB: header byte 9 came
back zeroed after zlib's `memcpy` wrote next to it (field: 6 of 7; bench: 300 of 300 at 12 KB, none with the
PSRAM runtime). The OTA self-test therefore compresses a 16 KB body, so the trial boot exercises PSRAM.
`GZTEST <bytes> [count]` on the serial console runs the compressor and verifier on synthetic bodies; run it at
9000 and 12000 B after a platform or toolchain bump. The per-wake `GZIP n= big= fail= raw_max=` line counts
bodies with a buffer in PSRAM and gzip failures.

Time sync has **three** sources, not four: GNSS (`CLK_GPS`, wins), the modem's `AT+CCLK` (`CLK_NITZ`),
`AT+CNTP` (`CLK_NTP`), plus the `Date` header on each telemetry POST (`CLK_HTTP`) as a coarse anchor.
All of them set the clock; only NTP and a confirmed GNSS pair (two records in one wake advancing as
`millis()` did) teach the deep-sleep drift rate. AT+CCLK was minutes off on one test SIM, so it never teaches,
and its steps are accounted to the millisecond (`clock_unref_step_ms`) so they cannot skew the next window; a
phone utc push restarts the window instead (`clockForgetWindow`). NTP costs data and a DNS lookup, so it never
drives the modem, attaches or extends a wake:
- `clockNtpAfterUpload` asks it right after an upload that sent frames while the car is in use (OBD answering,
  driving, charging, or the rail at `RAIL_AWAKE_VOLTS`), at most once per wake, and only when it is the only
  source (no AT+CCLK time this wake, or no valid clock) or a window of at least an hour of sleep is teachable
  and `CLOCK_NTP_MIN_INTERVAL_S` (6 h, doubling to 24 h after failures) has passed.
- `clockNtpNoClock` is the one exception: with no valid clock at all, setup() and the loop recovery ask it once
  per wake on a modem already up for this wake's upload, before the poll, even with the car asleep - otherwise
  every frame of the wake would go without a utc.
- NTP is unauthenticated, so with a valid clock a step is bounded: within `CLOCK_NTP_MAX_DISAGREE_S` of this
  wake's AT+CCLK reading, or without one, within what `CLOCK_DRIFT_MAX_PPM` over the sleep explains plus that
  margin. Further than that it is `disagree`: no set, no teach, and the attempt counts as failed.

A post-OTA boot zeroes the clock like a cold boot (#13).
There is no `GET /2/utc` call — `CellWrapper::getEpoch()` exists but nothing invokes it, and `UTC_KEY`
/ `UTC_PATH` are gone.

There is **no built-in token and no factory profile.** Both arrive together in the ABD2 push and
persist together in `/vehicle_profile.json`. Until then `g_no_profile` stops the poll and the empty
token stops the upload, so an unpaired dongle advertises on BLE and does nothing else. The push's HASH is
committed to NVS by the loop only once that file is written (zeroed first), so the HASH OK fast path never
claims a profile that is not on flash; a push the dongle loses gets HASH NOT OK and a re-push. A connect that
sends no HASH (the wizard's CLEAN) commits 0, so the next background connect re-pushes once. (`v1
/1/tlm/send` and `/1/tlm/bulk` are no longer used; `bulkString` and `CELL_BULK_PATH` survive only as
reference.)

**Telemetry keys** the frame can carry: `soc, soh, voltage, current, power, is_charging, is_parked,
speed, kwh_charged, soe, ext_temp, batt_temp, batt_temp_min, batt_temp_max, lat, lon, heading, utc` (exact
set is per-vehicle-profile). `is_parked` and `is_charging` are derived on-device; `lat/lon/heading`
come from GNSS. Note `bleTlmString()` **trims `lat`/`lon`/`utc`/`heading` from the BLE frame** — the
phone supplies its own position — so a BLE frame is smaller than the cellular body, and a frame that is
*only* position (no PID answered) serialises to `{"tlm":{"is_parked":…,"is_charging":…}}`.

**Park / drive / unconfirmed** (`Helpers/drive_state.h`, #6). The drive latch has three states. A speed key above 0
is DRIVING, as always. A fresh moving GNSS fix starts DRIVING only with the car on (rail ≥ `RAIL_AWAKE_VOLTS`, or an
OBD answer within `ACTIVE_TIMER`), an accelerometer hit within `DRIVE_MOTION_RECENT_MS` and no charge latch; a fix
without them (desk drift, a garage) is refused and logged once per streak (`LEASH gnss=… refused reason=motion|car-off|charging`).
Sustained motion (`DRIVE_SUSTAINED_ACT_PCT` of at least `DRIVE_SUSTAINED_MIN_SAMPLED_MS` sampled, over
`DRIVE_SUSTAINED_WINDOW_MS`) on a car that is on, with no charge latch, is UNCONFIRMED; with a discharge above
`DRIVE_DISCHARGE_KW` (the frame's `power`, else current × voltage) it is DRIVING. UNCONFIRMED keeps
`is_currently_parked` true (every parked-gated guard, the backstop included, treats it as parked) but the frame has no
`is_parked` key (so no `drivingState`, no `chargingState`, an empty CSV cell), the live location replaces the frozen
spot and no PARK marker is owed. It leaves for DRIVING on a speed key, a GNSS fix with the evidence above, or discharge;
for PARKED when the charge latch sets, `ACTIVE_TIMER` passes with no motion, or the ladder goes idle, never with a freeze.
The OBD-alarm gate takes none of this.

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
poll is buffered once while its fix is unsent, not once per poll, and not at all once accepted. Send-once leaves frames and the backlog untouched, so rolling back to
older firmware just resends as before. There is deliberately no periodic refresh: a parked car whose spot
the backend has aged out stays off the map until it relocates, drives or reboots. A 200 whose location the
backend silently dropped counts as sent until then too. `UPLOAD body` logs `loc=` (locations in the body) and
`ldup=` (suppressed repeats).

**A phone taking the transport keeps the cellular queue** (#58). The BLE notify goes out before the frame is
buffered, and its answer decides both: a frame relayed in a v0 session is not queued (the phone posts it as
`APP_OBD`; in a v1 session it is queued under its seq until the app acks it, §2), and
frames already queued (failed-POST retries, a slow drain, a restored backlog) are kept while cellular stands
down. They stay in PSRAM for the whole session (an earlier version spilled them to the backlog file, which freed no
internal heap and cost about 6 s of flash writes; #91 removed it). Cellular stands down while the phone relays;
when relaying ends the loop's upload drains them oldest first, at most `UPLOAD_MAX_BATCHES_PER_CYCLE` x
`MAX_FRAMES_PER_POST` per cycle, and a sleep in the session flushes them over cellular first and saves what is
left (appended to a backlog file the wake has not read, otherwise an overwrite). A backlog file over the cap restores only the newest frames. A frame the phone relayed in a v0 session, or that a v1 app acked, never goes over cellular, including the dongle's own fix (BLE trims
`lat`/`lon`/`heading`; the phone supplies its own). A profile push with a different (or, after an unreadable
profile, an unknown) `tlm_push_token` discards the queue and the backlog file (`UPLOAD discarded n=…
reason=token`): those frames may be another vehicle's history. The cap drops the oldest frame past
`MAX_UPLOAD_BUFFER`, logged once per capped stretch (`UPLOAD discarded n=… reason=cap`, at the next accepted
upload or the backlog save). `BLE relay queued=` logs the queued count and the heap at the start of each relay
stretch.

**Queued frames live in PSRAM** (#75). A frame is about 1 KB of pairs, under the 4 KB threshold that sends a malloc to
PSRAM by itself, so in internal heap the queue competed with Bluedroid and the modem: an ID.5 offline for 47 minutes
with BLE up had 120 frames queued, a 2292 B largest internal block, and 4 MB of PSRAM free, and the 120-frame cap
dropped 411 frames. The frame type (`OBDVector`, `libraries/OBDWrapper/obd_vector.h`) allocates its pairs and its
keys in PSRAM first (a key past the string's own 15 characters, such as `est_battery_range`, would otherwise take
internal heap in every frame), and the outer array is reserved for the whole cap, so a queued frame takes no internal
heap. The cap is 512 frames, an hour at the 7 s driving poll. `queueUploadFrame` still checks room before each copy:
free PSRAM must stay above `UPLOAD_MIN_FREE_PSRAM` (gzip's state and bodies; free, not the largest block, which a
freed frame seldom grows), and on a board without
PSRAM the internal heap must keep `UPLOAD_MIN_FREE_HEAP`/`UPLOAD_MIN_MAX_BLOCK`; otherwise the oldest frames go
first (`UPLOAD discarded n=… reason=heap free= maxblock= psram=`). Bench (a ONE+ on USB, BLE up, internal heap ballasted
below both reserves): 600 frames with long keys, 512 kept, no internal heap used, every frame read back exact; with
PSRAM ballasted to ~80 KB above the reserve, 300 frames settle at ~50 kept, the newest, and the reserve holds.

The backlog file shares LittleFS with the flash log, whose rotation keeps ~384 KB free. `saveUploadBacklog` writes at
most what is free above `BACKLOG_FS_RESERVE`, and a rewrite at most `BACKLOG_COMPACT_BYTES`; past that the oldest
frames are left out (`UPLOAD discarded n=… reason=fs`, or `reason=size`), so the trip-end marker, queued last, is
kept. A write that still fails ends the save (`BACKLOG write failed at`), and since `File` buffers, the file's size is
checked after close to catch a failed final flush; lost frames re-arm the marker debt if the marker was among them. A
full queue's file is ~260 KB: ~5.7 s to write at sleep and ~7.3 s to read, measured on the bench.

**The backlog is read only when it can be sent** (#92). `setup()` notes a file from an earlier wake
(`g_backlog_unread`, cleared once RAM holds it) and leaves it; `loadUploadBacklog()` runs once the modem has
registered, just before the loop's cellular upload or the pre-sleep flush, and before `connect()`, whose session-reuse
idle check a parse would outlast. Both upload gates count the unread file (`uploadQueued()`). A wake that never
registers appends its frames at sleep (`BACKLOG appended n bytes= ms=`; a torn last line is terminated first) and
moves them out of RAM, so a second save cannot append them twice. Past `BACKLOG_MAX_BYTES` or the flash budget it
compacts instead (`BACKLOG compacted`: a read and a rewrite of at most `BACKLOG_COMPACT_BYTES`, newest kept). A wake
whose RAM already holds the file overwrites it. While the file is unread, a poll carrying the frozen parked
fix is judged without its location, since the file may already hold it (it is the same every parked wake); a live fix,
and any poll with other data, is judged as before. So only an asleep car's intermediate frozen relocations go
unqueued, and the newest goes up after the load. Not fixed: a wake that registers but cannot open a session still
reads and rewrites the file. The file stays on flash through an offline stretch, so the flash log keeps about one
segment less history meanwhile.

**A frame older than 6 days is dropped from the queue** (#67). Parked history that old is worth little next to the
frames queued behind it, and the server answers 400 for the whole POST if any point in it is older than 30 days, so
one such frame would otherwise fail every retry of its batch. `uploadBuffered` judges each frame's `utc` against the
batch's trusted send clock before sizing, drops those past `BACKLOG_MAX_SEND_AGE_SECONDS` (inclusive; geo keeps its
own 29-day bound) and logs `UPLOAD dropped n=… reason=age sutc=first..last now=<send clock>`. A restored backlog
goes through the same check. An untrusted clock drops nothing. A trusted clock wrongly stepped 6 days or more
forward (bad NITZ/NTP, spoofed GNSS) drops the queue stamped before it; accepted, as unlikely, and the data is
useful only fresh. A 400 for a point past the server's 30 days logs `err=too_old`.

**Where the PID list / vehicle profile comes from:** the app fetches it from the ABRP backend by
`typecode` (`/vehicles/obdble_get_pids` with `signed=true`); that signed fetch returns the JWS, the app
takes the profile from its payload, and pushes it to the dongle over ABD2.
The profile is the per-model map of PIDs → CAN commands + equations that `OBDWrapper` evaluates; the
JWS is the same profile signed (required only when `PROFILE_REQUIRE_SIGNATURE` is on). So a new vehicle
is supported by adding its profile **on the backend**, not by reflashing — the dongle is generic and
carries no profile of its own at all. It used to: a dozen `init*()` PID blobs in `obdwrapper.cpp`, one
of which ran as a pre-pairing stand-in. Removed, because a profile for the wrong car does not merely
return nothing — `init_commands` is arbitrary CAN, so it puts wrong frames on a stranger's bus.
The one thing a profile cannot add is a new telemetry key: BLE forwards every key, but the cellular
upload drops one until `bulkStringV2` has a line mapping it to a v2 field.

**A profile is evaluated as the app evaluates it** (its reply parser, equation grammar and post-poll step), because
one profile serves both. `readPIDs()` follows the same rules (#84):
- A key's `command` may list several commands (`"22028C,222AB8"`). The key is evaluated on each reply, and a later
  value replaces an earlier one.
- A byte the reply lacks is NaN, as the app's `undefined` is. `ifgreaterthan(C+1, 0, …)` then takes its else branch, which
  is how MEB's `_soe_safe` falls back to -1000 when only the shorter reply came. Any other use stays NaN and is dropped.
- `isVirtual` keys (no reply of their own, an equation over `VAL{key}`; MEB's `soc`) are evaluated after the poll and
  the standard fallback, over its values, and clamped to `min`/`max`. One with a missing input is skipped, and a poll
  the car did not answer gets none.
- Function names in any case, and `==` (1 or 0, binding as `*` does). `!=` is refused, as the app has none. `&&` and
  `||` give the deciding operand and shifts wrap to 32 bits, as JavaScript's do. Before #84 every `==` profile (MEB,
  Mach-E, Bolt, BYD and others) built a null tree that `readPIDs()` dereferenced, a reboot once the ECU answered.
- A profile's own `is_parked` is dropped in `loop()` once `obd_count` has counted it: the drive latch publishes
  `is_parked`.
- With headers off, a multi-frame reply comes as `00B` / `0:…` / `1:…`: the length and each frame's index digit are
  dropped before the bytes are read. Before #84 the index digit stayed in, and every byte past the first frame was
  read a nibble out (MEB's `soe` and `kwh_charged`).
Keys starting `_` are the profile's intermediates; the cellular upload has no v2 field for them and drops them.

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
| Active MITM with any certificate under the bundled roots (name and dates are not checked) | uploads: positions, battery, the api key, keyid, signatures | read; replay an exact earlier request, at any later time (no `created`); swallow uploads by answering 200, so the frames are dropped; not forge a new request or learn the token |
| On-path network, DNS spoofer or NTP pool member (no certificate) | NTP replies | step the clock when NTP is asked and GPS is stale (only on a wake already uploading, at most once per wake): with a valid clock at most `CLOCK_NTP_MAX_DISAGREE_S` from this wake's AT+CCLK reading, or without one, beyond what `CLOCK_DRIFT_MAX_PPM` over the sleep explains; with no valid clock, freely (it is the fallback). It can nudge the drift rate within `CLOCK_DRIFT_MAX_PPM`, kept in NVS across a re-plug and halved toward the truth by each honest window after. The HTTP `Date` header is not read on cellular. It cannot roll a valid clock back days to defeat the send-time geo expiry |
| Anyone who can set the clock (GNSS in radio range, NTP on the path, a fake cell's NITZ); no certificate needed | — | step the clock 6 days or more forward so queued frames are dropped as too old (`UPLOAD dropped … now=`; the flash log keeps them) |
| Anyone with the token | — | post as the car. The token reaches the dongle over BLE at pairing (ABD2 profile push; LESC Just Works: encrypted, not MITM-authenticated) and is held by the phone app; cellular signing does not change either |

This assumes a high-entropy token (a UUIDv4, ~122 random bits): the keyid is an unsalted SHA-256 prefix
of it, which only an unguessable token keeps from being reversed. The bench WiFi lane (off in this build)
would sign the same way, over a TLS session that does not verify the server (`setInsecure()`); before
it is turned back on it needs `https.end()` (or its headers cleared) per POST, because `addHeader()`
accumulates.

---

<a id="section-6b"></a>

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
- **A reply is not always the ECU's frames verbatim.** Seen on an Ioniq 5 (issues #10, #45): a stray
  headerless tail ahead of a clean 7EC reply, or our whole answer spliced behind another ECU's message
  under one 5-digit "header" (`1785D`, `179E8`, `17BA5`). For protocol 6
  and `22xxxx`, `genericProtocol` takes the first single/first frame answering `62<DID>` under the
  expected ID (the profile's `ATCRA` filter; any 3-char ID when the profile sets none) and its
  consecutive frames under that ID, in sequence, up to the first frame's length. Lines ahead are dropped
  and logged `OBD<pid> reason=skipped_prefix lines=<n>`; frames under other IDs are never decoded.
  Anything else is refused as `wrong_hdr`, `wrong_did`, `bad_seq`, `nrc` or `no_frame`. The first three
  are sent once more in the same poll (not after a failed retry of that command, until it decodes again),
  logged `OBD<pid> retry reason=<r> hdr=<id> ok=<0|1>`, and `OBD220105 reason=` carries `hdr=` and
  `retry=`. Each line has its own 3-per-power-on cap, 15 min apart, no bytes except `OBDRAW`. The payload
  inside a splice is not recovered: its shape is one car's. Headers-off (`ATH0`) and 29-bit headers on
  protocol 6 are not parsed. Host checks: `tests/run_obd_reply_checks.py`.
- **A command that gets no usable answer is logged once, with its reply** (#85): `OBDFAIL <cmd> reason=<r> len=<n>`
  and up to three `OBDRAW fail` lines with its first 120 bytes (`\r` shows as `0A`: `readBuffer` turned it into `\n`). The reason is one of:
  - `no_data`, `no_connect`, `nrc`: the co-processor's own answer;
  - `at_reply`: an AT command the co-processor refused (`?`), such as a filter;
  - a refusal reason as above;
  - `eval`: no field could be computed from the reply;
  - `range`: the fields were out of range.

  It's logged once per command until a reset other than a deep-sleep wake, or a new profile, at most 4 commands a
  poll, and only on a poll the car answered: asleep, every command is `NO DATA`. Written for the MEB cars, where the second ECU on each bus (`FC0076`, `746`) never answers. One poll's
  lines show whether its filter was refused or the ECU stayed silent.

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

<a id="section-7"></a>

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

**Design constraints and field lessons:**
- The failure asymmetry: a false permit (transmit while armed) is unrecoverable; a false deny is
  invisible and recoverable. Buy false denies cheaply.
- **No single signal is sufficient.** A car on a flatbed has ignition off, alarm armed, and GNSS
  reporting 80 km/h. "Rail energised" alone polls a parked charging car. Only the conjunction holds.
- **Reject an unbounded rail hold.** An early proposal kept permission while the rail stayed
  energized, which survives parked charging and locking. Current code instead has explicit,
  bounded setup and OBD-speed continuation rules; see Appendix I §8.3, not the old “no grace” claim.
- **The accelerometer is inadmissible** as a hold signal: getting out, closing the door and locking
  all produce motion, so it would extend the permit through exactly the arming sequence.
- **Asymmetric debounce:** slow to permit, instant to deny.
- **Gate the requests, not the link** — an initialised-but-idle OBD link transmits nothing.
- **The old GNSS-age defect is fixed:** `gd->ts` is parser-call time and cannot establish freshness.
  `gpsFixAgeMs()` now ages changes in receiver date/time, so a re-echoed fix expires.

**A passive ignition signal on BMWs (2026-10-01):** `0x130` data byte 0 reads `05` with the ignition
on, and reading it transmits nothing (§6b). That is positive evidence of "in use", which the gate
lacks. It says nothing about locked/armed, and the frame keeps running with the car off, so only the
value counts, never the frame's presence.

---

<a id="section-8"></a>

## 8. App side — the rules that matter

The app is the ABRP app, which is **closed source**; this document describes only what it must do on
the wire, never how it is built. Note the protocol level of the app you test against matters: the
firmware requires the ABD8 handshake unconditionally and bonding is on (§1), so an app build without
the handshake cannot get a session.

**The poller** is the app's background loop that reconnects to a bound dongle and runs the connect order
(Appendix A §7b); the setup wizard and the OTA and log-dump flows make it stand back.

- **`tlm_type: 'abrpobd'`** = this dongle. **`'obdble'`** = the older generic dongle lane.
  **Everything added for this dongle must be an add-on that leaves the obdble lane byte-identical.**
  This was verified repeatedly and is the single most important app-side rule.
- **The vehicle↔dongle binding is server-side** (stored on the ABRP account). Consequence: a second phone signed into the same account syncs
  the binding and can connect **without ever opening the setup wizard**. Any per-vehicle setting that
  rides the account is therefore applied by phones that were never asked.
- **The dongle secret is device-local by design** (no backend). That was a deliberate decision — the
  backend-auth phases were struck. Device policy should stay consistent with it.
- **No timeout or retry where the protocol can be made deterministic** (e.g. the greet on the CCCD
  rising edge): both add failure modes.
- **"Pairing declined" is a *confirmed decline*, detected by the sustained telemetry subscription.**
  Never conclude it from a one-shot operation: the first gated access is what **triggers** pairing, so
  it races the procedure it started. Mapping that to "user declined" made every first-ever connect
  fail and succeed only on retry. A confirmed decline puts the app in the pairing-declined state
  (Appendix A §7b).
- **App state and the radio drifting apart is a recurring bug class.** A path that forgets the dongle
  without cancelling the BLE connection makes the next connect target a device the radio still holds,
  which fails as "already connected" until the OS drops the link. Any path that drops the device from
  app state must also cancel the radio connection.
- **Relay acks are what keeps the phone the transport** (§2, #90). Negotiate in the ABD8 handshake (the
  version is per BLE connection and shared by every caller of the handshake); in a v1 session read `seq`
  beside `tlm`, ack on ABD9 only what a post actually stored, write `{"relay":"stop"}` when it cannot
  post and `{"relay":"resume"}` when it can again, and send no acks while stopped. After a post of seq N
  fails, ack nothing until ABD9's state shows `sent >= N` and `oldest` 0 or above N, since a later ack
  would take N off the dongle's queue too. In a v0 session write nothing to ABD9. An app that stops
  acking hands the car back to cellular after 120 s, which is safe; an app that acks what it did not
  post loses those frames.

Per-feature obligations: relay acks → §2 and the bullet above; profile push → Appendix A §3; connect
order and decline detection → Appendix A §7b; OTA → Appendix B §4, Appendix C §5; log dump → §5.

---

<a id="section-9"></a>

## 9. Diagnostics: how to actually find things

- **`DUMP` over serial** is the primary tool, and the BLE log dump exists so you can get it without
  unplugging the dongle from the car. A full dump takes about 4 minutes; `DUMP n` streams only the
  newest `n` segments (about 25 s each), and `DUMP 3` usually covers the last drive. It is genuinely "the tool that made every other bug in this
  project findable".
- **`DUMP SINCE <utc>`** streams from the first line the host capture script's `--since` rule keeps, so the
  part after a drive no longer costs the segments already analysed (`DataLogger::dumpSince`). The rule: a line's
  time is its utc when that is >= 1e9; a clockless line takes its boot's first real epoch, a never-clocked boot the
  epoch before it; a boot starts at `WAKE reason=` or a millis drop; the boot's `WAKE`/`FW build=` lines and the
  segment's header come ahead of its first kept line. The device finds only that first line and then streams
  verbatim, as `DUMP` does; the host filter trims the rest. It reads the last 512 bytes of each segment, newest
  first, and starts at the newest one whose last epoch is before `<utc>`, printing its header straight away (the
  capture script waits 3 s for a header before falling back to `DUMP n`). Older segments are never opened, so a
  clock that steps backwards across segments, a boot's `WAKE` in a skipped segment, and clockless boots or
  pattern-less lines at the very head of the log are the known differences from filtering a full `DUMP`.
- **Clock lines.** The board has no 32 kHz crystal, so deep sleep keeps time on the internal RC, which runs
  ~0.6 % fast. Each wake takes that back at a learned rate; after each deep-sleep `WAKE`, `CLOCK wake slept <s>,
  adjusted <ms> at <ppm>` says what that wake corrected. `CLOCK drift <raw> raw, <residual> residual over <s>
  asleep -> rate <ppm> ... src=<ntp|gps>` is a teaching sync (NTP, or a confirmed GNSS pair); `residual` is what
  compensation left over, net of any CCLK or single-GNSS steps since the last teaching sync. `CLOCK <src> step
  <s>[ ref]` is the first step of 2 s or more per source per wake, and every teaching one (`ref`); `CLOCK <src>
  set, was unset` the first time after a cold boot or OTA.
  `CLOCK ntp=<ok|same|disagree|code|to>/<ms>ms on=<upload+obd|drive|charge|rail|noclock> for=<time|teach>`
  `[ d=<s>][ retry=<h>h]` is one NTP attempt and what triggered it (`same`: the readback could not be told apart
  from CCLK, so it was applied as CCLK; `disagree`: refused as further than the bound, by `d`; `62` is DNS).
  `CLOCK ntp=gated why=<asleep|nodata> for=time` (once per wake) means the clock had no source this wake and NTP
  could not run: the car is asleep, or the upload sent nothing. `CLOCK resync FAILED` (once per wake) means the
  wake runs on the compensated RTC, or with no clock at all. Still to check on a dongle: what `AT+CNTP` mode 1
  really does (the time on the result line, and whether it also sets the modem RTC), and NTP right after a POST
  (the SH session is still open, `syncNtp` purges the UART, and a failure can block ~20 s).
- **`MOTION act=<%> n=<windows> s=<span> peak=<g> mean=<g> axmax=<g> obd parked chg`** is one line per awake
  minute (and the partial minute before `SLEEP enter`; one in progress when an OTA reboots is lost). `act` is the
  share of observed time whose `detectMovement` window saw motion: each window stands for the time
  since the previous one, capped at 2 s, because a moving window ends on its first sample; `axmax` is the largest single-axis value, which is what
  that 0.1 g threshold compares; `peak` is the largest bias-removed |acc| and `mean` the mean of per-window peaks.
  The bias is a short running average, so these show vibration, not sustained acceleration. `s` is the span the
  windows cover: a blocking upload between windows stretches a "minute". Rail and time are on the EVT prefix.
  Diagnostic only, gathered to judge accelerometer drive detection for cars with no OBD speed PID; nothing decides
  on it, and OTA/log-dump windows (which bypass `detectMovement`) are not counted.
- **`FLUSHSTAT open=<ms> drain=<ms> posts=<ok>/<tried> frames=<ok>/<queued>`** (#110) follows a connected pre-sleep
  `FLUSH` line, which is unchanged: `open` is the flush's `connect()` alone (the backlog load is before it), `drain`
  is `uploadBuffered(true)`, `posts` counts batches accepted and tried (one per `UPLOAD try`), and `frames` the frames
  accepted batches erased, against those queued at the drain's start. Aged, oversized and held frames log their own
  lines and are not counted. An open failure adds nothing (its `FLUSH skipped: open failed` lines say it).
- **`WAKESTAT prep=<ms> loopmax=<ms> at=<millis> loops=<n>`** (#110), once per wake that reaches `enterDeepSleep`,
  beside the last `MOTION` line: `prep` runs from just past the OTA/dump guard through the park marker, the flush and
  the backlog save to just before the BLE teardown (not "time to sleep": the power-down follows it). `loopmax` is the
  longest gap between two visits to the top of `loop()` this wake and `at` the millis it ended, to match against the
  `UPLOAD`/`OBD` lines; its floor is the 500 ms `detectMovement` window, and a serial console command re-seeds it, so
  a `DUMP` never reads as a stall. When `loop()` ran, the sleep's entry is one more visit: a sleep from inside the loop
  body (`net-recover` after a modem stall) still closes the gap before it, and `loops` is the loop visits plus that
  one. `loops=0` (an ATRV skip, which sleeps from `setup()`) leaves `loopmax`/`at` 0. The
  crash-streak timer sleep, the OTA reboot and `ESP.restart()` skip it. A scheduler-gap proxy, not BLE latency.
- **`SLEEP enter ... reason=<r>`** names why the dongle slept. `dc-charge`, `charging` and `parked` are the
  cadence tiers, read back from the armed timer (1, 3, 30 min). `net-recover` is the modem-recovery
  reboot (1 min), `wdt-streak` the WDT-streak escalation (below), `backstop` the parked MAX_AWAKE ceiling. Charge
  cadence decisions the timer can't tell apart name themselves: `parked-held`, `parked-cap`,
  `parked-uncorroborated`, `pdw` and `followup` (below). An explicit sleep reason wins over a cadence
  label, which wins over the timer.
- **`DC_AWAKE on rail=<v> fast=<min>`** and **`DC_AWAKE off reason=<r> rail=<v>`** bracket a DC charge held
  awake (§6). In between, `UPLOAD ok` comes about every 30 s plus the poll, on a reused session; `NET diag ... idle=<ms>`
  is the time since the last upload, to check against `SESSION_IDLE_MAX_MS` and the ~60 s drop (#8). `r` is the live
  veto (`ble`, `rail`, `alarm`, `motion` with the window's `peak=`/`axmax=`), else the cadence label (`parked-cap`,
  `parked-uncorroborated`, `parked-held`), else `not-dc`; a sleep that ends a hold logs its own reason (`backstop`,
  `net-recover`). `DC_AWAKE refused reason=<veto>`, once per wake, is a DC poll that could not hold: a car whose DC-DC
  sits under `RAIL_AWAKE_VOLTS` shows `rail`, charger vibration over the 0.1 g `detectMovement` threshold shows
  `motion` (read the `MOTION` lines). `DC_AWAKE poke promoted` is motion on a held sample poke. During a held charge
  `AWAKE_BACKSTOP` is expected once an awake hour (counted from boot, so sooner after a long drive).
- **`<reset> reset recovered (streak=<n> retained=<0|1> owed=<0|1>)`** follows the `WAKE` of a panic, WDT
  or brownout reset (`<reset>` is the `WAKE` line's `reset=`). `streak` counts consecutive ones, brownouts
  included (#50: a battery that sags out on every modem start loops like a crash); a deep-sleep wake or any
  other reset ends it. `retained=0` means the count did not survive the reset and restarted at 1; on a
  `BROWNOUT` line it answers whether RTC memory survives a brownout on this board. `owed=1`: this boot owes
  the trip-end `is_parked:1` marker, restored from the same store (#49). At `streak=2` the existing late
  setup escalation logs `WDT streak=2 -> skip init, sleep <m>min` and sleeps with `reason=wdt-streak`: 30 min,
  or 3 min when the rail says the car is awake. If normal recovery does not complete and the crash streak reaches
  3—including a crash inside the threshold-2 `enterDeepSleep()`—an early setup exception starts a timer-only
  parked sleep before clock acquisition, filesystem/NVS storage, co-processor or gate/MEMS initialisation. It
  prints one serial line, not a flash log or `SLEEP enter`; zeros RTC system time locally without acquiring time,
  disables all wake sources and arms the parked timer regardless of rail, without WOM or peripheral shutdown.
  Already-powered peripherals and the modem/marker flags are unchanged for this sleep; marker debt survives the
  clean timer wake, and its deep-sleep reset clears the streak so initialization retries. This exception does not
  guarantee that every battery-drain mode is solved. In the normal recovery path a crash boot zeroes the clock,
  so its ordinary log lines and sleep usually carry no UTC; read them in order against the next `WAKE`. Nothing
  is reachable over BLE during that sleep; re-plugging the dongle is a power-on, which never escalates. The same
  store remembers whether
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
- **`NET diag ... open=<trace>`** names the session the POST used: `reused idle=<s>s` kept the previous one,
  idle that long; `idle=<s>s <trace>` closed one idle past `SESSION_IDLE_MAX_MS` and opened fresh; a bare open
  trace is a fresh open. On the cellular path an idle flow dies at about 60 s (most likely the carrier's NAT;
  the server keeps one 200 s), and a POST on it gets the prompt and body ACK and then nothing
  (`UPLOAD id=… http=0 err=empty p/a/r=1/1/255`). `AT+SHSTATE?` still says 1 on such a session, so nothing short
  of sending detects it. The 50 s driving cadence keeps `reused idle=` at about 50-57 s, and `SESSION_IDLE_MAX_MS`
  (57 s, checked at connect; the POST follows within ~2 s) reopens anything idle longer (a slow poll, every
  CHARGING upload, whose poll is 60 s), so `reused idle=58s` or more is a bug.
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

<a id="section-10"></a>

## 10. Where to look for what

Everything below except `README.md` and `docs/` is embedded in **Part II** of this file (with app
internals removed; see the note at its top).

| Question | File |
|---|---|
| Setup, supported cars | `README.md`, `docs/getting-started.md` (in repo) |
| Flags, file map, state machine | `docs/technical-overview.md` (in repo) |
| BLE security design + phase history | Appendix A |
| OTA wire protocol (authoritative) | Appendices C, B |
| OTA transport decision + design review | Appendix D |
| Signed firmware binaries + manifest | Public GitHub Releases of this repo, no token (§4; once a private release repository) |
| Power measurements + deferred work | Appendices H, G, F |
| OBD-alarm architecture + decisions | Appendix I |
| Why the alarm gate is off + fix plan | Appendix J |
| Backend expectations | Appendix E |
| What the co-processor can't do, cars it rules out (BMW i3 …) | §6b |

---

<a id="section-11"></a>

## 11. If you change one thing, know this

1. **Release through the workflow** (`RELEASING.md`); `OTA_FW_VERSION` comes from the tag, not an edit.
2. **Don't reorder the GATT table** — nor change it at all: ABD9/ABDA used up the one change (#90), and handle order
   follows heap addresses, not creation order (FROZEN TABLE). Compare the boot `GATT handles` line and a table dump.
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
   park-marker debt that is cleared only once the marker is sent, since the backlog is deleted when it loads (once
   the modem registers, #92).
10. **UNCONFIRMED is parked to every reader except the frame's `is_parked`, `addGpsData` and the marker.** `drive_unconfirmed` (written only by `drive_state.h`, like
   the latch) withholds the frame's `is_parked`, switches `addGpsData` to the live fix and owes no marker; everything
   else keeps reading `is_currently_parked`. A new parked-gated guard needs no change; one that must tell the states
   apart reads `drive_unconfirmed`. `tests/run_sm_checks.py rules` pins the transitions.
11. **Never branch on a BLE protocol version number outside `ble_protocol.h`** — call the session's table
   (`bleProto()`), and add a field to it when a version needs to differ. A new message the app must act on, a changed
   format or a removed one needs a new version; one the other side may ignore goes on ABD9 freely; ABD2 and the
   inside of `tlm` gain nothing (released apps fold unknown ABD2 keys into the profile and forward `tlm` as
   telemetry). Released versions' wire fixtures (`protocol/fixtures/`) are frozen.


---
---

# PART II — DESIGN DECISIONS AND FIELD EVIDENCE

The former standalone documents are consolidated below, not reproduced as parallel specifications.
Appendix letters and useful section numbers remain for source references. Rejected proposals are
decision history, not APIs or implementation tasks. App obligations are stated on the wire;
closed-source app internals are outside this repository.

| Appendix | Document | Original filename |
|---|---|---|
| A | BLE security plan (design + phase history) | `BLE_SECURITY_PLAN.md` |
| B | OTA update architecture (v1) | `ota-update-architecture-v1.md` |
| C | OTA wire-protocol reference (implementation wins on conflict) | `ota-protocol-reference.md` |
| D | OTA app-side spec (transport decision and design review only) | `OTA_UPDATE_APP_SPEC.md` |
| E | Backend expectations | `backend_implements.md` |
| F | Power management | `POWER_MANAGEMENT.md` |
| G | Power budget (bench measurements) | `POWER_BUDGET.md` |
| H | Deferred power work (read before touching sleep) | `DEFERRED_POWER_WORK.md` |
| I | OBD-alarm / ignition-sense architecture (final) | `IGNITION_SENSE_ARCHITECTURE.md` |
| J | Alarm-gate field findings & fix plan | `ALARM_GATE_FIELD_FINDINGS.md` |



---

<a id="appendix-a"></a>

# Appendix A — BLE security plan (design + phase history)

> Original file: `BLE_SECURITY_PLAN.md`

# ABRP OBD dongle — BLE security & access plan

Implemented security layers and their rollout history, originally drafted 2026-08-18. Current
defaults are in §1; dated captures below establish what was observed then, not the current
validation status of every phone/app build.

Companion docs: `ota-protocol-reference.md` (ABD5/ABD6 contract), `baseLogger/DEFERRED_POWER_WORK.md`
(power work).

---

## 0. What we are protecting, and from whom

| Channel | Current default controls | Protected capability / remaining boundary |
|---|---|---|
| ABD1 telemetry (notify) | Encrypted link + local HMAC grant | Live SoC/voltage/speed; `bleTlmString` omits lat/lon/utc |
| ABD2 setup / profile push | Encrypted link + local HMAC grant; profile signatures optional by default | An authenticated raw-profile writer can inject arbitrary CAN commands |
| ABD5/ABD6 OTA | Encrypted link + local HMAC grant + mandatory image signature | Authorized staging/update requests; possession of the session secret does not allow unsigned firmware |
| ABD7 log dump | Encrypted link + local HMAC grant | CSV GPS traces, timestamps and vehicle history |

Three independent boundaries:

1. **Payload legitimacy:** image signature; profile signature when strict enforcement is enabled.
2. **Session authority:** local challenge/HMAC proves secret possession, not backend user identity.
3. **Link privacy:** encryption; Just Works does not authenticate initial pairing against active MITM.

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

## 2. Phase A — developer gating (app only, no firmware) — DONE

Purpose: the app's firmware-update and dongle-log tools were visible to every user. They are internal
tools, so the app was changed to show them only to developer accounts (a server-side role a normal user
cannot set) that have also switched them on; the QR give/receive tool later shipped behind the same
switch. This is **product gating, not security** — the characteristics still exist and still accept
writes, which is why the later phases exist. No firmware change.

---

## 3. Phase B — signed vehicle profiles (firmware + app, NO backend work)

**Implemented; accept-both by default.** `PROFILE_REQUIRE_SIGNATURE 0` permits raw packages as
well as verified JWS. Setting it to 1 refuses raw profile content with `UNSIGNED`, but not
`keys`, `HASH`, `tlm_push_token` or `utc`. Do not confuse bench verification below with a decision
to enable strict enforcement fleet-wide.

| Piece | Where |
| --- | --- |
| RSA-2048 RS256 verification, base64url and pinned key | `Helpers/jws_verify.h` |
| Chunk assembly and loop-task verify/apply | `Helpers/profile_sig.h` |
| ABD2 chunk receiver and acknowledgements | `BLEWrapper` |

After `CLEAN OK`, send ordered, individually acknowledged
`{"JWS":{"i":<idx>,"n":<total>,"d":"<180-char slice>"}}` writes. The loop task verifies the
verbatim `<header>.<payload>` using RSASSA-PKCS1-v1_5/SHA-256 and replies `JWS OK` or `JWS BAD`.
Only after OK send the token and UTC packages that complete setup. Never fall back to an unsigned
push after BAD. Verification translates/re-pads base64url and returns a payload location only on
success; it does not trust an attacker-selected algorithm or parse unverified profile values.

**Compatibility constraints:** keep JWS separate from the shared parsed profile so generic obdble
behaviour and HASH input do not change. HASH is the app-computed CRC32 of the sanitized profile
serialization with the token appended; firmware stores/compares it, not recomputes it. On HASH OK,
no JWS chunks are transferred. Keep loop-task apply, identical-profile ATZ suppression and
persistence semantics unchanged. A cleaned but interrupted push invalidates the hash and forces
a later full push. Strict enforcement refuses an unsigned full push from an old **app-side cache**;
the app must refetch the JWS. It does not re-verify an already persisted device profile.

**Bench evidence, 2026-08-25:**
- 10 ordered chunks yielded a 1660 B JWS, 9 keys, **54 ms** RSA verification, a 941 B applied config
  and a persisted profile. Later boots took `HASH OK` / `SETUP OK` without a repush.
- `JWSTEST` altered one valid base64url character (not a separator), then restored it:
  `intact=ACCEPT tampered=reject restored=ACCEPT`. This distinguishes signature rejection from
  malformed input or a verifier stuck false.
- An app push with the last 6 signature characters overwritten logged
  `PROFILE JWS REJECTED (bad signature, 1660 B)` and no JWS OK, setup completion or config apply.
  The previous running profile continued polling; clearing staging did not erase the active OBD profile.

`JWSTEST` remains a read-only serial diagnostic for key/verification checks. Profile signatures
do not gate authenticated OTA, so a bad profile key/format can be repaired without an unsigned
profile override. Auth itself does gate OTA, with `AUTHCLEAR` as the state-recovery mechanism.

**Rejected draft mechanisms:** `JWSBEGIN`/`JWSEND`, a separate `pid_pubkey.h`, and a serial
`UNSIGNED ON` escape hatch were proposals, not the implemented interface. The implemented chunk
format and verifier are listed above. The signed bytes must remain verbatim: app-side number
coercion or JSON reserialization changes the signature input. Strict profile enforcement would
also refuse unsigned custom PID profiles; account for that before changing the default.

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
> **App side shipped too.** The handshake first ran after a *successful* setup only (the order was
> later inverted, below): after a declined pairing a write to encryption-gated ABD8 would re-prompt, so
> the app never writes ABD8 once a decline is known. HMAC-SHA256 is validated against the RFC 4231
> vectors, and TOFU generates the 32-byte secret from a cryptographic RNG. The keyring entry is
> persisted the moment the dongle confirms `provisioned` — deliberately NOT after the proof round, so
> a BLE drop between the two cannot strand a provisioned dongle with a secret the app never saved.
> **Android greet loss — fixed (2026-08-21).** The greet fires on the ABD8 subscribe, which on Android
> happens BEFORE MTU negotiation. A notify carries at most MTU-3 bytes and is never fragmented, so at
> Android's unnegotiated default of 23 the 20-byte ceiling silently dropped both greet payloads
> (`{"auth":"unprovisioned"}` is 24 B, a nonce frame ~36 B) and the app never answered — while iOS,
> which negotiates ~247 on its own before the greet, provisioned first time. Same trap the log dump
> already guarded with `{"err":"mtu"}`; the auth gate had no such guard. Two-sided fix: the firmware
> folds `mtu() >= 64` into the greet's edge condition, so a too-narrow pipe DEFERS the greet and it
> fires the moment MTU rises (no extra state, no timer), and the app requests MTU 517 (the dongle caps
> it at 247) **before** subscribing — subscribing is what triggers the greet, so widening afterwards
> would be too late.
>
> **Keyring is keyed by the DONGLE's own reported id.** The dongle now sends a stable identity
> (`authDongleId()`, eFuse-MAC derived, 12 hex) in every ABD8 greet, and the app keys its keyring by
> that. Neither obvious alternative works: the **BLE device id** differs per phone (iOS hands out a
> per-central UUID, Android an address — which is why a vehicle stores an *array* of BLE device
> ids), so it could never be shared phone-to-phone and a QR code carrying one would
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
> holds only its own secret, and the dongle computed and compared an HMAC to reject the other.
> Same-session success was then witnessed on 2026-08-24, below.
>
> **The enforce path is now real.** `authenticated` used to be one flag doing two jobs — the ABD2
> state-machine latch (while false, the next write is parsed as an AUTH token) *and* the session grant
> (telemetry / OTA / log dump / cellular stand-down). That conflation made enforcement impossible:
> withholding the grant would have re-parsed every profile packet as an AUTH attempt and broken the
> push. They are now split — `setup_unlocked` (latch) and `authenticated` (grant).
> The former audit mode let the legacy token write grant access; that behaviour and the
> `AUTH_ENFORCE` switch were removed. Before enforcement, successful proof/reconnect and QR
> sharing had to work on both phone platforms because account sync does not carry the local secret.
>
> **Handshake verified end to end (2026-08-24):** `greet: unprovisioned` → `provisioned` →
> `AUTH2 ok (ENFORCED - session granted)`, all in ONE session. Two fixes got it there. First, a
> dongle could commit a secret to NVS that the app never saved (unrecoverable except by `AUTHCLEAR`,
> with no symptom but an unanswered nonce); the rule is that the app persists the secret when the
> dongle confirms `provisioned`, through one write path. Second, provisioning alone proved
> nothing, so under enforcement no session was ever granted and telemetry stayed dark; the app now
> sends `{"challenge":1}` straight after provisioning and proves in the same session.
>
> **ABD2 (the vehicle profile) is now authorization-gated, and the connect order was inverted for it.**
> ABD2 carries the PID list and the tlm push token, so a peer that can write there can inject arbitrary
> PIDs at the CAN bus — the worst case in §0's table, and worse than reading telemetry. Pairing alone
> does not earn that: it proves the link is private, not that the peer is allowed. Under
> the enforced gate the dongle refuses ABD2 with `NOAUTH` until the ABD8 proof lands, and the app
> runs the auth handshake **before** the profile push (it used to push the profile first and
> prove afterwards, which would fail on every first connect under enforcement). Correct layering:
> pair → prove → data. The handshake stays non-fatal so old firmware and declined pairings still
> complete setup; under enforcement the *dongle* is what says no, not the app.
>
> **QR give/receive shipped** in the app, behind the Phase A developer switch. Each held secret is
> listed with the dongle id it belongs to, and the id always stays visible — it is what matches the
> dongle's own `AUTH2 greet … id=` line. The QR payload is validated hard — version, 12-hex id,
> secret decoding to exactly 32 bytes — because a camera hands the app any QR in view and a malformed
> keyring entry produces a dongle that silently refuses this phone.
>
> Enforcement was field-confirmed (`AUTH2 ok (ENFORCED - session granted)`); it is now unconditional.
>
> **The vestibule/bouncer is STRUCK, and replaced by something better.** Dropping a connection that
> never authorizes cannot work here: the dongle cannot distinguish a camping stranger from a user
> mid-pairing-decision, and dropping the link makes the poller reconnect and re-raise the OS
> pairing prompt every cycle — precisely the churn the app's pairing-declined state was built to prevent.
>
> **Replacement:** give a new connection `ACTIVE_TIMER` grace by stamping `last_active`, then
> refresh its awake leash only on authenticated presence. That is not the same as v1 relay health:
> a phone can stop carrying telemetry while its authenticated user still needs the dongle awake.
> Without the initial grace, a connection near expiry could sleep during its handshake.
> The two idle timers share a timestamp; their five-minute values do not add to ten minutes.
> The parked max-awake backstop remains separate, with OTA/dump/maintenance exemptions.

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

(As built, the nonce frame also carries the dongle `"id"`, `"reset":1` while the key-reset window is open, and,
since #90, the protocol range `"proto":[MIN,MAX]`; the proof may carry `"proto":v` and the reply is then
`{"auth":"ok","proto":v}`. See §2 and §3.)

No backend call. HMAC-SHA256 is microseconds on the ESP32 and mbedTLS is already linked.

**Provisioning decision:** one ECDH-derived secret per dongle, shared explicitly by QR, with no
backend or per-person slots. The old raw-secret `provision` write and audit-mode grants were removed.
Just Works encryption does not authenticate first provisioning against an active MITM.
`AUTHCLEAR` now revokes a live grant too; reset details are in [§3](#section-3).

**Honest limit that remains:** B2 removes the token from *every connect*, but the token is still part of a
**full profile push** (it is one of the pushed keys and is inside `HASH`'s input), so it still crosses the
air — now encrypted by Phase E — whenever the profile is re-pushed.

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

The rejected alternative would have added an account/ownership endpoint signing a dongle-bound
nonce with a separate ECDSA key. That would provide per-user authorization, unlike shared-secret
possession, but added backend dependency and key custody. `AUTHSIG`, `auth_pubkey.h` and
`ble.privileged()` from that proposal are not firmware APIs.

---

## 6. Phase D — same authorization for OTA

> **SUPERSEDED — struck by decision (2026-08-25).** OTA START already requires the session grant
> (`ota_update.h`, the `OTA_ERR_NOAUTH` check), granted only by a valid HMAC proof.
> The prerequisite this phase demanded — a serial-only `AUTHOFF` override before enforcing — was also
> dropped, on the reasoning that `AUTHCLEAR` already restores access for every *state* failure, and
> the only case it cannot cover (the auth code itself being broken) needs new firmware regardless,
> which needs physical access either way. `AUTHOFF` would have saved the build toolchain on a visit,
> not the visit.

Resume after a disconnect requires a new local authenticated session within the 90 s window;
there is no backend authorization call or cached ticket.

---

## 7. Phase E — link-layer encryption (bonding / SMP)

> **First encrypted-dump validation — ABD7-only stage, Android and iOS** (2026-08-20, real dongle,
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
> | 2. Handle budget | Raised to 32 before bonding; current use is 30 after the reserved-characteristic change. |
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
> **Decline-path failure:** mere connection previously suppressed upload, modem re-init and NTP
> retry, so an unpaired phone starved cellular while carrying nothing. Those paths now use
> `bleRelaying()`; v1 also requires relay health (§2). Awake-leash rules are separate (§4 above).
> Cellular may overlap CONNECT→proof. Each new unpaired connection can prompt again; repeatedly
> disconnecting it is not a pairing-decline fix.
>
> The rollout went ABD7 → ABD1/ABD2 → everything, each step field-validated before the next. The original
> plan kept ABD5/6 (OTA) open until last, fearing a permission mistake would brick updates — that caution
> was retired on evidence: the identical pattern moved full 2.8 MB log dumps on both platforms with
> `retries=0`, comfortably above OTA's 1.7 MB stop-and-wait transfer. Gating OTA buys something real: an
> unpaired stranger could otherwise write `START` to ABD5 and spam the staging slot. ABD6 is WRITE_NR —
> ATT silently drops write commands to an encryption-required attribute on a plaintext link (no error
> response), which is fine since ABD6 is only written after START is ACKed on ABD5, which forced
> encryption. ABD8 gated = pairing-before-authorization, the intended layering for the phase-2 HMAC
> handshake. App impact of gating ABD3–ABDA: none — ABD3 is declared but unused, ABD4 is only read on an
> error path that tolerates a refusal, and ABD5–7 are developer tools reached only by already-paired
> sessions.
>
> **The former app-side blocker is resolved.** The app now treats ATT security refusals (insufficient
> authentication/authorization/encryption/key-size) as the pairing-declined state (shown as "Pairing
> required – data via cellular") instead of a lost connection: it **keeps the connection** (a teardown
> would reconnect and re-prompt every poll cycle). That state clears on disconnect/reconnect or when a
> frame arrives. The refusal must be recognised from the ATT error code itself, before any generic error
> handling discards it. **Ship dongle and app together**: an old app against ABD1/2-gated firmware loops
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

### GATT migration decision

The initial plan called for Service Changed/Database Hash and a larger handle budget before bonding.
Instead, the table was frozen and the budget raised to 32; #90's reserved-characteristic change
uses 30. The old 21/24 and 28/32 counts describe earlier layouts, not spare capacity today.
Stale iOS caches produced `notify change failed` and missing ABD6; bonded phones may require
“Forget This Device”, not just a Bluetooth toggle. Do not infer safe migration from a successful
unbonded test. `BONDCLEAR` remains the device-side recovery.

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
| Steady state | the poller | connect → **ABD8 proof** → ABD2 profile push → ABD1 telemetry subscribe |
| First-time pairing | the setup wizard | connect → **ABD8 proof** → ABD2 profile push → ABD1 telemetry subscribe |

The wizard was missed when the poller was reordered, and it does not inherit the fix: the poller
deliberately stands down while the wizard runs, so nothing else can authorize on the wizard's
behalf. The symptom was that pairing a NEW dongle to a vehicle always failed at "error writing
init commands" (ABD2 answering `NOAUTH`), while already-paired dongles kept working because their
secret predated enforcement — so the regression was invisible until someone claimed a fresh dongle.

The wizard is also the natural home for TOFU provisioning: the phone claiming a dongle is the phone
pairing it to a vehicle, so first-connect is exactly when the secret should be minted.

**A gated-channel refusal is NOT proof the user declined.** Whichever operation is FIRST to touch a
gated attribute is the one that TRIGGERS pairing, so it races the procedure it started — "pairing still
in flight" and "user declined" look identical at that instant. Latching "declined" off such a
one-shot made every first-ever connect fail: the state was set, the profile push refused on it, the
wizard reported "OBD connection failed", and a retry then worked because the bond existed by then.
Field-reported and fixed.

So the rule is: **only the SUSTAINED subscription latches.** The telemetry subscription (ABD1 notify)
is long-lived, so a refusal there really does mean access is being denied. The ABD8 handshake and the
ABD2 profile push are one-shots — they log the refusal and fail plainly. The push still *honours* a
decline the subscription already detected (it should short-circuit the next cycle); it just never
concludes one itself.

The former non-fatal-handshake fallback served firmware predating ABD8. Current firmware requires
the proof before ABD2; do not treat that historical compatibility path as an authorization bypass.

---

## 8. Ordering summary and the one invariant

The rollout staged developer UI gating, signed profiles, local HMAC and link encryption; log-dump
encryption provided evidence before OTA was gated. Separate backend authorization and `AUTHOFF`
were rejected, not pending phases. Preserve USB recovery and validate both refusal and recovery
before tightening a gate. The current enabled/default matrix is in [§1](#section-1).


---

<a id="appendix-b"></a>

# Appendix B — OTA update architecture (v1)

> Original file: `ota-update-architecture-v1.md`

Architecture decisions and implementation constraints. The single byte-level contract is in
[Appendix C](#appendix-c); release/signing operations are in [RELEASING.md](../RELEASING.md).

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

The app must not be trusted to validate an image or commit it safely. Inactive-slot staging
contains interrupted transfers; it is not a blanket guarantee against hardware failure.

---

## 1. Settled decisions (v1)

1. **Distribution:** signed `.bin` files + a manifest as GitHub Releases of this repo. The
   app fetches them, then transfers over BLE.
2. **Repo access:** none. The signature is the trust boundary, so authenticated
   distribution buys nothing.
3. **Signing trust root:** the vendor private key remains in KMS; the public key is compiled
   into firmware. Release authorization and key-management operations are in `RELEASING.md`.
4. **Transport:** BLE only. The app downloads the image and streams it to the dongle.
   Byte-transfer is isolated behind one module on each side so the OTA backend does not
   hard-code the byte source (clean seam; no alternative transport is built in v1).
5. **Encryption:** dropped. Authenticity + integrity come from the signature, which is
   the trust boundary; app-level encryption with a plain-flash key adds complexity for no
   real confidentiality.
6. **Crypto:** **ECDSA-P256 + SHA-256**, using the bundled mbedTLS (`mbedtls/ecdsa.h` +
   `mbedtls/sha256.h` ship in-framework; Ed25519 would need an extra library, so it is
   not used).
7. **Version scheme:** release `X.Y.Z` maps to `major*1000000 + minor*1000 + patch`.
   Nonzero versions below the effective compiled/NVS floor are refused; equal versions are
   allowed. RC and final names can share a numeric version. No eFuse secure-version is enabled.

---

## 2. Shared BLE contract (the interface both sides implement)

Use Appendix C's distinct ACK/status frames and error table, not the original draft's combined
frame. ABD5 is READ + WRITE + NOTIFY; ABD6 is WRITE_NO_RESPONSE. Sectors are 4096 B,
stop-and-wait; a NACK retries the whole sector. `cur_sector` is the **next expected sector**.
Resume means re-authenticate and send matching START, then use ARMED's returned anchor.

The dongle caps MTU at 247; data payload is MTU−6. Request MTU before the subscription that
triggers the greeting, not only after entering OTA. iOS negotiation is OS-controlled (historically
around 185). At 517, large notifications were lost; see [§2](#section-2).

**Historical throughput estimate, not a current-firmware measurement:** the optimized NimBLE
reference transferred 1,038,544 B in 85 s (about 12 KB/s). That implied 2.4 minutes for a then-1.72 MB
image, or an estimated 2.5–5 minutes on Bluedroid with stop-and-wait.

---

## 3. DONGLE SIDE

The implementation is `Helpers/ota_update.h`; these are its design constraints, not pseudocode
to copy into a second implementation.

- **Partitions:** `default_16MB.csv` has two 6.5 MB app slots, 3.5 MB LittleFS and 64 KB coredump.
  App OTA changes neither bootloader nor partition table. Keep USB recovery and do not shrink slots.
- **Stage/commit:** native `esp_ota_ops` writes unverified sectors only to the inactive slot.
  `esp_ota_end`, read-back hashing and signature verification must succeed before the boot pointer
  changes. A whole-image RAM buffer is neither needed nor safe.
- **Signature:** sign SHA-256 of `"ABRPOTA1" || uint32le(version) || uint32le(size) || SHA256(image)`.
  ECDSA-P256 yields raw 32-byte `r` plus 32-byte `s`. The binary itself remains a raw app image;
  metadata/signature travel separately. Official releases use KMS/OIDC, not a local build hook.
- **Rollback:** `verifyRollbackLater()` in `logger.ino` defers Arduino's confirmation. The pending
  image's `otaHandlePendingVerify()` self-test runs before the sleep gate and marks valid or rolls
  back. A coverage outage, GNSS outage or sleeping car is not a failed firmware self-test.
- **Tasks:** callbacks assemble/stash data; loop-task code writes flash and ACKs after writing.
  Stop-and-wait permits one sector buffer. Do not perform flash/crypto work in Bluedroid callbacks.
- **Power:** `otaPowerOk()` reads the rail afresh while polling is paused. Below 8 V is bench/USB,
  at least 12.2 V is allowed, and the intermediate band is refused. Contrary to the initial plan,
  the firmware does not enforce a parked-state requirement; the app's parking warning is not a gate.
- **Activity:** `g_ota_active`/maintenance holds suppress normal polling, uploads and sleep.
  The configured limits are 30 s inactivity, 90 s disconnected resume and 15 minutes per session;
  image bounds and contiguous-heap admission are checked before staging.
- **Downgrade:** refuse a nonzero version below `max(compiled version, NVS floor)`, not equal
  versions. A normal PlatformIO upload preserves NVS; factory-image flashing/erase does not.
  Physical reflashing remains outside this remote anti-downgrade boundary.

---

## 4. APP SIDE

What the dongle relies on the app doing:

- **Discovery:** select the newest image by `manifest.version` (see Appendix C §2.2), never by tag
  order. The signed `.bin` + manifest are public GitHub Releases; no token. Show the update only for
  an OBD-paired, BLE-connected vehicle; keep it developer-gated for v1, open up once proven.
- **Transport — reuse, don't rebuild.** Use the **existing** BLE connection; never open a second one.
  Address `ABD5`/`ABD6` by UUID on the `ABD0` service. Control frames (START/END/ABORT + metadata +
  signature) are writes with response on `ABD5`, the ~106 B START as a long write; data packets are
  writes without response on `ABD6`. Subscribe to `ABD5` notifications **before** the first write
  (Appendix C §3.1), parse the 20-byte frame, and pace the next sector on its ACK (stop-and-wait).
  Unsubscribe on done/abort.
- **Stand back during the transfer.** The dongle is `UPDATING` and stops telemetry; while an update
  runs the poller must neither tear the connection down nor reconnect or stream.
  The update flow owns reconnect: on a mid-transfer drop it reconnects, authenticates and sends
  matching START, then resumes at the returned ARMED anchor; after reboot it reads the version. Nothing else in the
  telemetry path, the generic `obdble` lane or `ABD1`–`ABD4` changes.
- **UX obligations:** drive progress from `cur_sector / total` in the ACK, not local bytes written
  (local writes lead what the dongle has committed). Treat interruptions (back-navigation, a call,
  low battery) as an abort: send ABORT on `ABD5`; the dongle's inactivity timeout is the backstop if
  the app vanishes. On iOS a backgrounded app has its BLE transfer throttled and may be suspended, so
  a 2.5–5 min transfer must survive backgrounding: keep the screen awake, warn "keep the app open",
  and **rely on the `cur_sector` resume anchor** if the OS suspends. Every terminal state
  (verify-failed, power-refused, timed-out, rolled-back) is reported distinctly.

---

## 5. End-to-end flow (both sides)

See Appendix C §4 for the single maintained sequence, including subscription-before-START,
resume and post-reboot confirmation. A matching numeric version alone cannot distinguish an
equal-version reinstallation or an RC from its final release; use reported status/build identity.

---

## 6. Non-negotiables (binding on both sides)
- **Keep physical USB/serial recovery supported.** App OTA does not change the bootloader or
  partition table; hardware faults can still prevent recovery.
- **Partition table sized generously and frozen before v1.**
- **Signing key custody:** official signing is KMS/OIDC; follow `RELEASING.md`, not local-key recipes.
- **App is not the verifier:** device-side checks must reject unsigned images, and inactive-slot
  staging contains interrupted transfers. Neither is an unconditional no-brick guarantee.

---

## 7. Phasing (highest-risk-first, both sides in lockstep)

The original rollout separated unsigned bench transfer mechanics from signed updates. Both are
implemented; signatures are required by default. The retained lesson is to test recovery,
rollback and refusal before deployment, not to reintroduce an unsigned production phase.


---

<a id="appendix-c"></a>

# Appendix C — OTA wire-protocol reference

> Original file: `ota-protocol-reference.md`

Wire reference for `Helpers/ota_update.h`, `Helpers/ota_pubkey.h` and `tools/ota_sign.py`.
Implementation wins on disagreement; Appendix B summarizes architecture rather than duplicating
these byte-level tables.

---

## 0. Safety and status boundaries

The dongle does not enforce parked state; `OTA_ERR_NOTPARKED` (3) is reserved, not currently sent.
The power gate allows rail below 8 V for bench/USB and at least 12.2 V for a healthy battery;
the intermediate band is refused. Do not present an app-side parking recommendation as a
firmware-enforced guarantee.

ABD5 carries distinct status and sector-ACK frames (§3.3). Parse status for START refusal,
verification/commit progress and INFO, rather than interpreting every notification as an ACK.

---

## 1. `manifest.json` — exact schema

Produced by `tools/ota_sign.py`. One manifest per firmware version, always paired with
that version's `firmware.bin`.

The emitter, not a stale example signature, defines the manifest. Current releases include
`version_name`; older integer releases did not.

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

### 2.2 Release names and numeric versions

Tags are `vX.Y.Z` or `vX.Y.Z-rc.N`; the numeric code is `major*1000000 + minor*1000 + patch`.
Select by manifest version within the chosen channel, not list order or an integer parsed from
the tag. RC and final share a code, so release identity/`version_name` must break ties, preferring
final over its RC. Publication, signing and version injection are defined in
[RELEASING.md](../RELEASING.md).

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

Unauthenticated requests share GitHub's per-IP rate limit (60/hour). Fetching many manifests can
exhaust it; account for caching and pagination rather than assuming a check-on-open is cheap.

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
and no error. Always subscribe to notifications on `ABD5` first, then write.

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
on, so any START without all 106 bytes gets rejected with `NOSIG` or `NOKEY` (§3.4). Always
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
| IDLE | `0x00` | ABORT ack while nothing was running; INFO reply when idle | 0 | effective anti-downgrade version floor |
| ARMED | `0x01` | START accepted or resumed; also INFO reply mid-transfer | next sector expected (0 on fresh start) | START/resume: bytes staged; INFO: effective anti-downgrade version floor |
| VERIFYING | `0x03` | after END, while hashing/verifying the staged image | 0 | total bytes written |
| COMMITTED | `0x04` | signature verified, boot pointer moved — **device reboots ~600ms after this** | 0 | the version being committed to |
| ERROR | `0x05` | a step failed after acceptance (write/verify/commit failure) | `OTA_ERR_*` code (§3.4) | bytes written so far |
| ABORTED | `0x06` | ABORT received, or a guard fired (inactivity/session-cap/link-lost/weak-power) | `OTA_ERR_*` code (§3.4) | bytes written so far |
| REJECTED | `0x07` | START refused before anything was staged (no erase happened) | `OTA_ERR_*` code (§3.4) | 0 |
| TRIAL | `0x08` | *(reserved, not currently emitted)* | — | — |
| ROLLEDBACK | `0x09` | INFO reply, when this boot detected the previous update was reverted by the bootloader | current sector (irrelevant here) | effective anti-downgrade version floor |

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
{"fw":1000,"floor":1000,"build":"v0.1.0","signed":1,"state":"idle","rolled_back":0,"sector":0,"sectors":0,"alarm":-1}
```

| Field | Meaning |
|---|---|
| `fw` | running image's compiled firmware version; not the NVS floor or a separate confirmation flag |
| `floor` | effective anti-downgrade floor: maximum of compiled and NVS versions |
| `build` | human-readable, build-injected `FW_BUILD_TAG`; compile timestamp is in the serial banner, not this field |
| `signed` | 1 if this dongle build requires signed images (it does, by default) |
| `state` | `"idle"` \| `"receiving"` \| `"resume_wait"` |
| `rolled_back` | 1 if the *previous* update attempt was reverted by the bootloader this boot session |
| `sector` / `sectors` | current progress if mid-transfer, 0/0 if idle |
| `alarm` | raw stored alarm flag: -1 unset, 0 off, 1 enforced |

**Caveat: during an active transfer, a GATT read of `ABD5` returns the *last notified
binary ACK frame instead* of this JSON** (a BLE characteristic has one value; a notify
necessarily overwrites it). Only rely on the JSON read while idle. To detect
success/rollback after reboot: reconnect, re-auth, then **read** ABD5 and check `rolled_back`,
running `fw` and expected `build` identity. Numeric equality alone cannot distinguish RC/final
or an equal-version reinstall. Reinstalling the identical build cannot be proven from identity
alone; retain the transfer/commit outcome instead of claiming the version read proves it.

---

## 4. End-to-end sequence (updated for the real protocol)

1. Connect + auth (existing flow, unchanged).
2. Subscribe to `ABD5` notifications.
3. Read idle ABD5 JSON for the running `fw`/`build`. Optional `INFO` (`0x0004`) reports status and
   the effective anti-downgrade floor, not necessarily the running version.
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
10. Read idle ABD5 JSON and assess rollback, running version and build identity together (§3.6).

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
  connection, the same way the profile push addresses `ABD2`.
- **BLE preserves write order** within one connection/characteristic — packet *loss* is
  the failure mode the protocol defends against, not reordering. That's why a mismatch is
  handled as "resend the sector," not a reorder buffer.
- **MTU:** request before subscribing; the verified dongle cap is 247. Historical app captures
  requested 517 on Android and observed around 185 on OS-controlled iOS; current app behaviour is
  outside this repository. Appendix B §2 records the historical 1.72 MB / 2.5–5 minute estimate,
  not a measured promise for current images. Handle backgrounding and resume regardless of duration.
- **Every update must currently be signed.** The dongle has a real ECDSA-P256 key
  compiled in and `OTA_REQUIRE_SIGNATURE` is on. `tools/ota_sign.py` is the only supported
  way to produce a `manifest.json`/`firmware.bin` pair that a dongle will accept — hand-
  built manifests without a valid signature will be rejected (`NOSIG`/`SIGFAIL`), by design.


---

<a id="appendix-d"></a>

# Appendix D — OTA app-side spec

> Original file: `OTA_UPDATE_APP_SPEC.md`. That document was mostly an implementation spec for the
> closed-source app (navigation, state, code layout, testing, file lists); those sections are omitted.
> The protocol obligations it set out are in Appendix B §4 and Appendix C §4–5. What remains is the
> guiding principle, the transport decision and its review of the dongle design.

## 0. Guiding principle — the app is a *dumb, untrusted courier + status screen*

The app discovers and transports an image and reports progress. Signature verification, power
gates, dual-slot staging and the commit decision stay on the dongle. This is a design boundary,
not proof against every hardware failure. The implemented transfer contract is in [Appendix C](#appendix-c).

---

## 1. The architecture call to make *before* writing app code

**Chosen:** the app downloads a public signed image and transfers it over BLE. No PAT/secure-store
dependency is required. The original estimate for a 1.72 MB image was 2.5–5 minutes, with phone
backgrounding and disconnect/resume as the transport risks.

**Alternative, not implemented:** dongle-side cellular fetch could remove the phone's sector
sender and human-present transfer window, but needs its own download/staging/retry integration.
Existing telemetry POST support is not evidence that this OTA path works. Keep transport separate
from verification so a future fetch path can share the same trust boundary; BLE remains useful
when the dongle has no cellular service.

---

## 2. Architecture feedback on v4 (dongle side)

Retained review decisions:

- Public artifacts need authenticity, not a download credential or an application-level encryption key.
- Bundled mbedTLS supports ECDSA-P256; adding Ed25519 would add a dependency. Bind version and size
  with the image digest so metadata cannot be swapped under a valid signature.
- Verify from staged flash before `esp_ota_set_boot_partition`; avoid a whole-image RAM buffer.
- Trial self-tests assess hardware initialization, not coverage, GNSS lock or a responding car.
- NVS anti-downgrade is a remote policy, not protection from physical flash/NVS replacement.
- Keep USB recovery and explicitly verify the framework's mark-valid timing when changing platforms.


---

<a id="appendix-e"></a>

# Appendix E — Backend expectations

> Original file: `backend_implements.md`

## 1. Context in four lines

The 2026-08-19 design removed backend-issued BLE authorization tickets in favour of a local
shared-secret proof. No challenge endpoint, nonce store, account allowlist or access-signing key
is required for that handshake. This does not remove the backend's separate telemetry/profile roles.

---

## 2. The one thing we need: keep serving the signed vehicle profile, unchanged

`POST /vehicles/obdble_get_pids` with `signed=true` returns a compact RS256 JWS. The signed-profile
path forwards those bytes unchanged; the dongle verifies before parsing. It must not verify a
reserialized/coerced profile instead. Raw profiles remain permitted by the current build default.

### C1 — The RS256 signing key must be stable and long-lived

The public key is embedded in firmware. Ship firmware trusting a replacement before rotating the
backend signing key; devices that missed the update can still be repaired through authenticated OTA.

### C2 — Payload types

The original request assumed quoted numbers would fail on the dongle. The current OBD parser
accepts both numeric and quoted numeric fields (`jsonToDouble` in `obdwrapper.cpp`), for compatibility
with older profiles. Keep signed bytes untouched and test actual backend payloads; do not infer
parser equivalence from app-side coercion.

### C3 — Signed payload completeness

Everything the dongle needs to run the profile must be inside the signed payload, since the dongle uses those
bytes rather than anything the app assembles around them.

### C4 — Optional, not required: a version field

A signed monotonic version could prevent an authenticated writer from replaying an older genuine
profile. Under strict signature enforcement, that replay changes which ABRP-authored PID set runs;
it does not forge new commands. Mere radio proximity does not authorize ABD2, and the current
accept-both default additionally allows authenticated raw-profile writes. Do not conflate those
boundaries with the narrower signed-replay question.

---

## 3. Scope boundary

The rejected ticket service and its new keys were unrelated to OTA signing or telemetry request
signatures. Do not read the original “no backend changes” decision as a description of those newer
interfaces. Their current contracts are in [§6a](#section-6a) and [Appendix C](#appendix-c).


---

<a id="appendix-f"></a>

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
  the largest term in this theoretical component budget, not a measured attribution of the board's floor.

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

**Historical proposal, not the production state machine.** LIGHT_IDLE, explicit flash/rail
gating, MCU-owned CAN/TWAI shutdown, an 11.8 V cutoff and a 20-second/three-attempt heartbeat
budget were proposed, not implemented as that design. The STM32, not the ESP32, owns CAN.

Production uses the activity ladder and wake gates in `state_machine.h`/`logger.ino`, with
normal teardown in `sleep_power.h`: BLE shutdown, conditional modem shutdown, unconditional
raw `ATGPSOFF`, then WOM/timer wake. The early crash-streak timer-only escape is an explicit
exception that skips peripheral teardown. Co-processor reset/ATLP and additional rail gating
remain separate experiments in [Appendix H](#appendix-h).

## 5. Hardware options and validation

The original board-change proposal put a P-FET high-side switch on TJA1050 VCC, mirroring
Q3/SI2301, to remove its estimated 2.8 mA-equivalent load. Routing U10/U11 enable pins to GPIO
would need a supervisor: the ESP32 cannot remain running on a rail it cuts. These are design
options, not measured savings or instructions to change the production wake path.

Measure supply current at the OBD pin for each state and repeat on the same rig; then soak parked
overnight to verify the device actually sleeps and the rail remains healthy. The later 27–38 mA
bench floor (Appendix G) is much larger than the schematic estimate, so do not declare the CAN
transceiver the dominant real load from a component budget alone.

---

## 6. Summary

The lasting lesson is to bound awake/network work as well as lower sleep current. The
`http_err` storm supported an awake-drain hypothesis, not a measured attribution of every
milliamp. The ~3.6 mA baseline and ~0.28 mA revised-board target above are schematic estimates,
not demonstrated production sleep currents.


---

<a id="appendix-g"></a>

# Appendix G — Power budget (bench measurements)

> Original file: `POWER_BUDGET.md`

# Power budget — dated measurements and historical model

Energy model for `baseLogger/logger.ino`, built from bench measurements taken 2026-08-17 on a
Freematics ONE+ at a supply of ~11.4 V, cross-checked against the state machine in
`Helpers/state_machine.h` and the timings in `config.h` **at that time**. The daily scenarios
below are historical calculations, not predictions for today's upload/charge cadence. Preserve
the measurements and rig conditions; remeasure before claiming a current saving.

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
| Parked timer wake (gate skip) | ~60 mA for ~5 s | 4 mAh/day over 48 wakes; ~2.2 mAh incremental over a 27 mA sleep baseline |

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

## 2. State-machine assumptions used in the historical model

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
are traversed in one or two loop iterations and the device sleeps. The bench observation was
about five minutes after motion stopped. The old ten-minute comment has since been corrected;
the five-minute tail was deliberately retained, not left as an unresolved timer choice.

**Parked cycle.** Deep sleep → 30-min timer wake → ATRV gate reads both voltages → parked rail
(not `rail_awake`, not motion, not cold, heartbeat disabled) → skip → straight back to sleep,
modem never powered. 48 such wakes consume 4 mAh/day, about 2.2 mAh above the displaced 27 mA sleep load.

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

This is not a safe one-line production fix. Shutdown ordering, wake initialization, CAN silence
and the unmeasured saving must be resolved first; see Appendix H §1b and §3.

### 5b. Missing shutdown acknowledgement and the 27 → 38 mA step

The old 64-byte CPOWD receive had no expected-match strings, waited all 3000 ms and could discard
the first `NORMAL POWER DOWN` line once the buffer passed 48 bytes. Its fallback then pulsed
PWRKEY for 2.5 s, potentially turning an already-off modem back on. A registering SIM7070 could
add 10–20 mA, consistent with the observed +11 mA, but that was not a measured causal attribution.

**Landed mitigation:** `CellWrapper::powerOff()` now uses 192 bytes and matches only `POWER DOWN`.
The old proposed match list including `OK`/`ERROR` was unsafe: it could return before shutdown
finished. The missing-ack PWRKEY fallback remains because silence can also mean a powered but
hung modem. Do not replace it with “no AT reply means off”; further hardening is in Appendix H §2.

### 5c. GNSS power is latched in the CO-PROCESSOR, not the ESP32

`gpsBegin()` sets `FLAG_GNSS_USE_LINK` and sends **`ATGPSON`** to the co-processor; `gpsEnd(true)`
sends **`ATGPSOFF`**. GNSS power therefore lives in a separate MCU that the ESP32's deep sleep and
reboot do not touch.

The old RAM-flag-guarded shutdown missed a GNSS receiver left on by an ESP32 reset; repeated skip
wakes never corrected it. An active receiver could add ~20–40 mA. **Landed:** normal sleep sends
raw `ATGPSOFF` unconditionally when the link exists. Merely removing the guard around `gpsEnd()`
would be wrong: without this boot's link flag it takes the UART-task branch on uncreated resources.
The early crash-streak timer-only exception still skips peripheral teardown.

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

<a id="appendix-h"></a>

# Appendix H — Deferred power work (read before touching sleep)

> Original file: `DEFERRED_POWER_WORK.md`

# Deferred work — sleep floor and radio hardening

Historical power review, not a current backlog. Co-processor sleep, rail gating and missing-ack
modem hardening still require measurement; several mitigations below have landed, including
those recorded on 2026-08-18 (§7). No sleep-current saving is established without same-rig measurements.

Each item: **what**, **why deferred**, **what could go wrong if done blind**, **how to settle it**.

---

## 1. The sleep floor — the ~15-20 mA co-processor question

The STM32F103 runs from `VCC3V3` (U11/MP20045 off `VCC5V`), always on, with VBAT/VDDA/VDD1-3 hardwired.
**There is no firmware power gate for the co-processor.** The unexplained part of the 27 mA
board-input floor motivated a hypothesis of substantial STM32 draw. The historical **15–20 mA**
estimate was not a component measurement and did not exclude modem/GNSS leakage. Convert all
currents to the same supply reference before attributing a remainder. Two experiments follow.

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

**Mitigation implemented:** a validated `RTC_NOINIT_ATTR` record tracks whether the modem may
remain on across a crash. Normal skip sleeps probe AT when that flag is set and power down a
responding modem; only acknowledged CPOWD proves shutdown. Silence remains ambiguous. The early
crash-streak timer-only sleep intentionally bypasses this teardown.

Do not replace this record with `RTC_DATA_ATTR`, which reloads after panic/WDT/software reset.
Nor initialize via `xbBegin()` merely to probe: it pulses PWRKEY. Unconditional `powerOff()` can
toggle an already-off modem on through its missing-ack fallback.

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
- **`FILE_TEST` GNSS shutdown:** the test still calls bare `gpsBegin()`, but normal sleep's
  unconditional raw `ATGPSOFF` now covers link GNSS even when the RAM flag is false.
- **Per-wake `new` leaks in `BLEWrapper::init()`:** 7 callback objects + 6 descriptors are never freed by
  `deinit()`. Harmless today because init/deinit are strictly 1:1 per boot (deinit is only reached inside
  `enterDeepSleep()`, which always ends in a reboot). Becomes a real leak the day a re-init-without-reboot
  path exists — worth a one-line warning comment in `deinit()`.

---

## 5. Findings from the three-reviewer audit (2026-08-12)

These are candidate follow-ups, not approved implementation instructions. App-side ACK-timeout
behaviour is unverified here. The log-dump mailbox, format-on-mount-failure, parameter-dependent
resume matching and notify-retry paths remain in the firmware; recheck the exact path before
changing it. New protocol fields must respect the negotiated-version contract in §2.

- **OTA ACK-timeout resync (MEDIUM).** On a single lost ACK notification the app aborts a *healthy* session,
  and because ABORT reaches a live dongle it destroys the 90 s resume anchor, so the retry restarts from
  sector 0. The machinery to fix it already exists: the dongle `setValue`s every ACK before notifying, and
  the app can read it back with a GATT read of ABD5 — it currently does so only when the *subscription* was
  refused. Fix = on ACK timeout, try one read-back (or an INFO probe, whose ARMED reply carries
  `ota_cur_sector`) before aborting. Deferred because it touches field-proven OTA code for a rare, self-healing case.
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
- **Protocol versioning — superseded proposal.** The earlier suggestion of independent LOGDUMP/OTA
  version fields was not adopted. ABD8 now negotiates the session protocol; released wire fixtures
  are frozen. Do not insert a version byte into existing OTA frames.
- **Tail-fetch mode for the log dump.** The biggest remaining UX win: `{"LOGDUMP":{"tail":N}}` (or a START
  parameter) so the app pulls only what's new since the last fetch. A full dump measures **~9.6 KB/s → 2.7 MB
  in 278 s** (§8), not the ~22 KB/s / ~2 min this bullet claimed before — so the case is *stronger* than
  stated: a routine "what happened since yesterday" would be seconds, and a shorter session shrinks every
  race window in the protocol.
- **Log-dump notify log spam (§8).** 355 `[E]` lines per healthy dump, from the backpressure retry succeeding.
  Cosmetic, but it buries real errors; the fix is written out in §8 and gated on the multi-peer decision there.
- ~~**`ota_sign.py` should derive the version**~~ Done: the version is injected at build time and `ota_sign.py`
  refuses an image whose `FW_BUILD_TAG` doesn't match. Three stale-binary releases preceded it (v2-over-v1,
  v5-over-v4, 3-over-v2).

---

## 6. The measurement that unblocks most of this

The 27 mA baseline is **unexplained** and the 27 → 38 mA step has three equally supported causes (modem
toggle, GNSS latch, un-parked co-processor), so nothing above should be credited with a saving until this
runs. The original experiment proposed four states on the same rig, with a **pre-fix** baseline:

1. **Baseline** — the historical build before the §7 mitigations.
2. **+ the §7 fixes** (unconditional `ATGPSOFF`, CPOWD ack detection).
3. **+ `STM32_HOLD_RESET_ON_SLEEP`**.
4. **+ `OBD_LOW_POWER_ON_SLEEP`** (for comparison against 3).

Also, in the same session:
- **Current-firmware comparison:** record whether the retained-modem probe and CPOWD actually
  ran on each wake. A skip wake can now power down a retained modem; it no longer proves
  `powerOff()` was bypassed. Compare instrumented shutdown paths, not wake labels alone.
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

<a id="appendix-i"></a>

# Appendix I — OBD-alarm / ignition-sense architecture (final)

> Original file: `IGNITION_SENSE_ARCHITECTURE.md`

# OBD-Alarm Safety & Ignition Sense

> **Current implementation:** the compile-time bypass was removed. Only explicit `obd_alarm=1`
> enables enforcement; unset and zero do not. See §8.3 for current continuation/setup rules and
> Appendix J for the field failures and their resolution. GPIO34 harness/calibration sections
> describe research, not shipped ignition sensing.

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
- A proposed init-command locality filter would restrict this channel to local AT/ST commands.
  It is not implemented; current firmware instead defers profile initialization until a permit exists (§8).

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
  gate sooner". A future pin-enabled driving predicate must retain the rail conjunction;
  current §8.3 has no pin input. Never treat "we woke on the ignition pin, therefore ignition is on,
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

- **Proposed, if a pin qualifies:** store the mapping (§10), enable `ext0` wake and add the
  measured signal to a future rail-conjoined permit. None of these calibration additions is shipped.
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

Why neither signal alone establishes an initial driving permit (**excluding setup and speed
continuation**, specified in §8.3):

| Rail | Moving | Real state | Verdict |
|---|---|---|---|
| energised | yes | being driven | **permit** |
| resting | yes | **towed / transported** | deny |
| energised | no | parked & charging, or stationary with ignition on | deny initial GNSS-based permit |
| resting | no | parked asleep | deny |

### 8.3 The permit — current rule and rejected holds

`alarmGatePermit()` allows a bounded setup session, or energized rail **and** either fresh
confirmed GNSS movement or a bounded OBD-speed continuation. A calibrated ignition-pin input
is research, not a term in the current implementation.

- **Setup:** a completed full profile push grants at most 10 minutes, including an identical-profile
  push. HASH-only reconnects and authentication alone do not. With BLE enabled, authenticated
  presence is also required, allowing a 30-second disconnect grace.
- **Driving:** OBD speed at least 25 km/h extends continuation for 30 seconds. It cannot obtain the
  first speed reading without another permit. GNSS movement expires after 10 seconds without a
  new receiver date/time. A live rail drop denies immediately; a sustained 3-second drop also
  clears stored speed continuation.
- **Rejected rail latch:** “acquire on motion, hold while rail high” survives drive → park →
  plug in → lock because charging keeps the DC-DC rail energized.
- **Rejected IMU hold:** exiting, closing doors and locking all produce motion, so IMU holding
  extends permission through the arming sequence. It is a wake source, not proof of disarm.

The former “no hold/grace” and “zero exposure” claims preceded the explicit continuation and
setup rules above. Do not use them as guarantees. A flatbed's movement alone and parked charging's
rail alone are insufficient, but bounded holds are deliberate tradeoffs requiring vehicle tests.

**Wizard evidence, 2026-08-27:** an unchanged profile took HASH OK, so the stationary wizard got
no setup permit and only gate-denied frames, without `soc`; Save never enabled. The recorded fix
was to force a full push in the wizard. Firmware still suppresses redundant OBD reinitialization,
so the cost is transfer, not ATZ. Do not grant a wizard permit to background reconnects.

Gate requests at the per-PID boundary without unnecessarily destroying an initialized, idle link
through short permit gaps. Deferred init still needs a permit: user profile commands are not
assumed local or safe merely because they occur during initialization.

### 8.4 One centralized gate function

Centralize the enforced decision in `alarmGatePermit()` and route transmit guards through it.
Its setup/speed helpers use session state and time; it is not a pure function of rail and movement.
Return false on a flagged vehicle unless one of the explicit permits is satisfied.

This is the direct lesson from the transport-handoff bug (`DEFERRED_POWER_WORK`): gating on
nearly-the-right condition in one place produced a field bug where data flowed nowhere. Scattered
conditions are how safety gates fail. Centralizing the decision also permits controlled-state/time
host tests; a real-car A/B experiment that deliberately sounds an alarm is not an acceptable oracle.

The alarm-off path is a single early return, which is what keeps this a true add-on:

```
if (!obd_alarm_flagged) return current_gate_verdict;   // today's behaviour, untouched
...conjunctive permit logic...
```

**Defer init until a permit exists.** Init-command locality filtering was proposed, not implemented.
`OBDWrapper::initialize()` forwards profile commands; even a signed profile can transmit on the bus.
Do not infer a command-filtering safety boundary from the historical proposal.

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

Check the evaluated permit **between individual PID reads**; a cadence-only guard leaves seconds
of in-flight requests. For the driving permit, a live rail drop denies at the next per-PID guard.
The bounded setup permit is a separate override: ignition-off alone does not revoke it.

The `init(vector)` overload avoids the other overload's standard speed probe, but still executes
the profile's commands without a locality filter. Deferred initialization, not a hypothetical
filter, is the implemented protection. Pin-edge stopping and calibration fail-closed rules remain
requirements for a future harness, not guarantees supplied by the current rail/GNSS gate.

---

## 9. If sniffing is used: one test first

**Optional for §8; a hard prerequisite for §12 — and therefore not something to defer.** The gate
works without sniffing, so this reads as skippable. It is not: §12's passive frame decoding is the
**only** path to recovering the parked-charge telemetry that alarm-flagged cars give up, and all of
it is blocked behind this one result. Ten minutes of bench work decides whether an entire capability
exists. Do it early (§13 research prerequisites) — it is the cheapest experiment in this document and the one
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

Note the vehicle *binding* is server-side — saving stores it on the account and the phone only
caches it. That is why the flag must **not** ride there: a second phone signed into the same account
syncs `tlm_type` and the dongle's BLE device ids and can connect and relay **without ever reaching the
setup wizard's save step**. An account-carried flag would therefore be applied by phones that were
never asked the question.

### 10.2 Where it is set: the setup wizard's save step, once

The flag is a **property of the car**, in the same category as typecode — not a user preference that
drifts. Set-once at binding is the correct lifecycle.

- **Placement:** the setup wizard's save step for an abrpobd dongle, above Save. The toggle's value is
  written to the dongle (ABD2 `obd_alarm`) as part of the same save that binds the vehicle.
- **Label:** *"OBD port Alarmed?"* — Yes / No. Precise, and for this audience clearer than a
  symptom description, with a one-line note on the consequence.
- **Scope: abrpobd only.** Technically justified, not just rule-compliance: obdble polls only while
  a phone drives the session, so the "parked and armed for hours" exposure barely arises. The
  autonomous dongle is the one at risk.

**Only one write path exists, and that is the point.** Because the setup wizard's save step is the
sole writer, a second phone holding a shared QR secret cannot disturb the setting — it authenticates
and relays but never reaches the toggle. No tri-state wire contract, no reconciliation, no account
field: there is only ever one copy of the value, held by the device that enforces it.

### 10.3 Default: explicit opt-in after field use

The original proposal treated unset as alarmed. Production resolves unset and zero to **not
alarmed**; only explicit `obd_alarm=1` enables enforcement. This preserves parked telemetry for
the majority of vehicles but does not protect an affected vehicle before opt-in.

Read back the stored choice rather than replacing it with an onboarding default. Otherwise an
unrelated re-pair can silently change safety policy or remove parked telemetry, easily mistaken
for a cellular fault. Log the effective flag and explain the tradeoff during onboarding.
The app's current UI default is not verified in this repository.

No-profile dongles still wait for a vehicle-correct profile/token instead of using a factory
fallback. **Never discover alarm sensitivity by deliberately triggering an alarm.**

### 10.4 Read-back: the dongle publishes its own state

The device is the authority. To initialise the toggle (the stored-choice rule in §10.3) and display the state
anywhere it matters, the dongle **publishes the flag in the readable info JSON on ABD5**, beside the
installed firmware version already exposed by `setOtaInfo()`. That is the established
"dongle-publishes-its-own-state" channel, it is one more field, and it costs **zero GATT handles** —
the frozen table stays frozen. The dongle is authenticated and connected by the time the setup wizard's
save step renders, so the read is available exactly when needed.

One copy, owned by the device that enforces it, readable by anyone authorised.

### 10.5 Changing it later: unlink and re-pair

Once the binding is saved, reconnecting goes through the poller, not the setup
wizard — so the toggle does not reappear. The edit path is **unlink the dongle from the vehicle and
re-pair**: done on the phone, at the car, in about a minute.

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

These were **proposed pin-calibration diagnostics**, not implemented checks:

- **Primary:** movement while quiet warrants inspection of rail, freshness and the evaluated permit;
  it does not by itself prove a broken gate (a flatbed must remain denied).
- **Inverse (proposed pin plausibility rule):** the calibrated pin reads ON while GNSS
  reports no movement for more than ~30 min ⇒ distrust the pin. **Deliberately not conditioned on
  the rail resting** — the dangerous case is pin-stuck-high *while the rail is energised*, i.e. a
  parked charge session, which could pass a future rail-plus-pin predicate. Current §8.3 has no pin term. A
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

## 13. Implementation versus research

| Scope | Disposition |
| --- | --- |
| Device-local flag, ABD2 write and ABD5 read-back | Implemented; explicit opt-in |
| Deferred init, per-PID guard, setup permit, fresh movement and speed continuation | Implemented |
| Unset-default-alarmed, rail-only latch, IMU hold | Superseded/rejected |
| Hidden pin-read command / STM32 reflash | Ruled out by §4; no recovery image, do not attach a programmer |
| GPIO34 harness, pin calibration, ignition wake, fleet-learned maps | Research; board-revision pin tests and calibration required |
| Passive decoding | Requires §9's silence test; i3 parked battery telemetry ruled out, only `0x130` exposed |

Do not infer protected silence from feature presence: park/lock and inspect two wake cycles,
using the transmission counter with its local-AT limitation. New hardware/pin work first needs
the board-revision check in §3 and the input constraints in §§6–7. Non-alarm vehicles must remain
unaffected. BimmerCode, CAN isolation and pin-16 rerouting are not required when silence is achieved.


---

<a id="appendix-j"></a>

# Appendix J — Alarm-gate field findings & fix plan

> Original file: `ALARM_GATE_FIELD_FINDINGS.md`

# Alarm gate — field failure findings & fix plan

2026-08-28. Companion to `IGNITION_SENSE_ARCHITECTURE.md`. Three parallel deep traces (firmware
GNSS chain, firmware wake/sleep + wizard, app wizard/poller) against the field symptoms:

1. wizard fails at "Car connection status";
2. the dongle stops advertising and cannot be found again;
3. a long real drive never produced a single permit — no telemetry by any transport.

The narratives below record the original failures; their source line numbers are historical.
Several fixes have since landed and the bypass switch was removed. Use the resolution table at
the end for current disposition, not the original proposed fixes. App-side status is unverified here.

---

## Root cause 1 — wizard failure: `clean()` does not reset `setup_unlocked`

`BLEWrapper::clean()` (blewrapper.cpp ~398-411) clears `config_keys`, `configJson`,
`tlm_push_token`, `hash_ok` — but not the `setup_unlocked` latch (reset only by
`clearAuthentication()` on disconnect and by `deinit()`). When the app writes `{"AUTH":…}` again on
the same connection (a retried setup), the AUTH handler at blewrapper.cpp ~549 is skipped
(latch already set) and the write falls through to the generic package path:

- replies `ACK` (~619-620), then
- is refused as an unsigned profile key by the PROFILE_REQUIRE_SIGNATURE lockdown (~685-692) →
  `UNSIGNED`. Two contradictory replies to one write.

Field trail (flash log): AUTH → AUTH OK → clean → CLEAN OK → five refused re-AUTHs over ~22 s →
no `keys`/`HASH`/`JWS`/`SETUP OK (push complete)`. Consequences cascade: `completeSetup()` never
runs → the loop's config-apply never fires → `alarmGrantSetupPermit()` never called →
`setup_permit=0` on every ALARMGATE line → PID test gate-denied → wizard shows the generic
"OBD Connection Failed" checklist (the real reason, `no soc`, goes only to the app's log).

**This also made the flag unrecoverable from the app**: Save requires the PID test, the PID test
requires the permit, the permit requires the push, the push was broken. NOTE: this bug is
independent of the alarm gate (it dates from PROFILE_REQUIRE_SIGNATURE) — the gate only made it
fatal. At the time it remained reachable even with the gate bypassed, whenever the app re-AUTHed
on a live connection. AUTH-key dispatch has since fixed this path.

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
direct-UART branch (no decode task) and returned false every cycle. No retry existed in that build; every
sleep sends `ATGPSOFF`, forcing the next wake into a cold start (TTFF measured 329 s) and the same
one-shot. One quiet failure at wake = no fix = `gps_moving_streak` pinned 0 = **permit structurally
unreachable for the whole wake**, i.e. the whole drive.

Compounding: on a denied cycle while the phone is relaying, the BLE frame is stripped to
`{is_parked,is_charging}` (telemetry_format.h strips lat/lon/utc), the BLE send succeeds, so
logger.ino ~1360 **clears the upload buffer** (fixed in #58, see §6a), and `bleRelaying()` stands cellular down — data
flows nowhere; only the flash CSV survives. The app compounds it: after 5 min without telemetry the
UI reports "Not connected" on a perfectly healthy link.

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

The original `alarmGateAllowsPoll()` logged once per wake plus transitions only. An all-deny drive
produced a single line and no record of WHICH term failed over time — the exact "quiet with an
empty log" shape the logging was meant to prevent.
**Fix (Phase 1):** re-log every ~2 min while denied, with per-term values
(rail, moving, fresh, hold, setup_permit, tx).

## Historical freshness defect (fixed by receiver-time tracking)

`gd->ts` is stamped with `millis()` **at call time** (historically FreematicsPlus.cpp:627, link branch).
The old age check therefore never expired when the co-processor re-echoed its last `$GNIFO` fix;
a latched movement streak stayed "fresh". This failed toward PERMIT: drive into a covered garage
(fixes stop, streak latched), park, switch off (rail
deny ✓), plug in → rail back up → permit on a locked, armed car. Secondary: `parkAccumSample()`'s
ts-based dedupe contract was unimplementable on this path.
**Implemented fix:** freshness and dedupe now track changes in `$GNIFO` date/time, using a local
millis stamp and sequence. Those fields were previously parsed but unused for freshness.

Also found in the chain (record, low priority):
- The old `$GNIFO` parse set `good = true` before parsing speed, accepting fresh coordinates with
  stale/zero speed. Candidate-record validation now prevents that partial update.
- The `$GNIFO` raw-reply diagnostic is dead code: `#if print` (lowercase) never compiles in.
- SPI link OOB read: FreematicsPlus.cpp ~440 reads `buf[5..11]` without checking length.
- A timer wake mid-drive on a flagged car is dropped before GPS/BLE/modem power-up — the drive
  depends entirely on WOM, which the codebase itself documents as blind on smooth asphalt.
- The per-PID guard routes around `alarmGateAllowsPoll()`, so the sustained-collapse timer only
  advances at cycle boundaries, never during the multi-second frame it is meant to interrupt.
- The current `MAX_AWAKE_MINUTES` backstop is parked-only and exempts OTA/dump/maintenance,
  but not every authenticated parked session. Activity handling precedes config apply, so a
  pending stashed push is not itself a guarantee against sleep.

---

## App-side findings

What the app's half of the trace means on the wire, and what the firmware relies on the app to get
right:

- **A1 (real bug, abrpobd-only, Phase 3):** a profile push that stopped early (not connected, not an
  abrpobd dongle, the pairing-declined state, no PIDs) was reported to the wizard as a successful
  setup. A push that did not end in `SETUP OK` must be reported as a failure.
- **A2 (abrpobd-only, Phase 3):** a telemetry subscription on a connected dongle that never sends a
  frame stayed open forever, and its late error or frame then acted on the wizard's *next* attempt
  (cancelling that attempt's timeout, or marking a mid-reconnect link good). Fix: cancel the ABD1
  subscription when the wizard's wait for telemetry expires.
- **A3 (verified present and correct):** the fix for a gated access in flight during pairing keeps
  the connection rather than dropping it (Appendix A §7, §7b). Gap: a fast retry can race the OS's
  teardown of the previous connection.

Other app-side issues found by the trace are tracked with the app.

---

## Resolution status

| Historical finding | Current disposition |
| --- | --- |
| Re-AUTH misparsed after CLEAN | AUTH-key dispatch implemented in `BLEWrapper` |
| Failed GNSS init reported powered, with no retry | Actual result stored; poll-side recovery in `logger.ino` |
| Only one DENY line in an all-denied drive | 120-second denied heartbeat in `alarm_gate.h` |
| Replayed parser timestamp looked fresh | Receiver date/time age and sequence dedupe in `gps_location.h` |
| Truncated `$GNIFO` accepted before speed was parsed | Candidate record committed only after required fields pass in `FreematicsPlus.cpp` |
| Suppressed timer wake prevents BLE visibility | Still present before full bring-up; not fixed |
| Proposed handshake/write-activity leash | Not built in that form; connect-time grace and authenticated-presence refresh are used |
| App push-result/subscription bugs | App status unverified in this repository |

The enforcement bypass switch was removed; there is no `OBD_ALARM_ENFORCE` re-enable step.
Rail calibration remains vehicle-specific. A host pass does not establish that a flagged car's
wizard, real drive, GNSS outage and parked/armed silence are safe; retain those hardware checks.
