# Dongle context

Current architecture and constraints, with links to detailed evidence. Section numbers are stable
because code comments refer to them. **Code is authoritative.** The
[detailed reference](docs/dongle-reference.md) preserves measurements and older designs, not a current
implementation backlog. Build/workflow rules live in [AGENTS.md](AGENTS.md); host coverage lives in
[tests/README.md](tests/README.md).

## 0. Thirty-second orientation

The dongle polls the car and uploads to ABRP over its own LTE-M modem. A phone is an occasional
privileged client and can take over telemetry delivery; it is not required for parked reporting.
Access control protects the vehicle bus, and power management protects an unattended 12 V battery.

Hardware: Freematics ONE+, with our ESP32 firmware and a closed STM32 co-processor owning CAN,
K-line and OBD pin-16 voltage. Its ELM327-style interface is incomplete and cannot be fixed by
reflashing the ESP32 (§6b).

The closed-source ABRP app is a separate repository. This dongle uses `tlm_type: 'abrpobd'`;
the generic `'obdble'` lane must remain unaffected by its protocol changes.

## 1. Current state of every staged feature

| Feature | Default / control | Implementation |
| --- | --- | --- |
| Cellular uploads | Enabled | `Helpers/network_upload.h`, `NetworkWrapper` |
| BLE telemetry | Enabled | ABD1, `Helpers/ble.h` |
| Flash logging / BLE retrieval | Enabled | `DataLogger`, ABD7 |
| OTA updates | Signature required | ABD5/ABD6, `Helpers/ota_update.h` |
| Sleep/wake management | Enabled | `Helpers/sleep_power.h`, `state_machine.h` |
| BLE bonding | `BLE_ENABLE_BONDING 1` | `BLEWrapper` |
| HMAC authentication | Required, no bypass flag | ABD8, `Helpers/auth_gate.h` |
| Signed vehicle profiles | Verification built; `PROFILE_REQUIRE_SIGNATURE 0` | `profile_sig.h`, `jws_verify.h` |
| OBD-alarm gate | Enforced when the per-vehicle flag is set | `Helpers/alarm_gate.h` |
| BLE relay negotiation | v0/v1, selected per authenticated grant | `ble_protocol.h`, `ble_relay.h` |

Paths under `Helpers/` are relative to `baseLogger/`; wrappers are under `libraries/`.
The profile-signature requirement remains off pending field validation, not for old-app compatibility.
The old auth audit mode and alarm-enforcement bypass are gone.
[Feature history](docs/dongle-reference.md#section-1).

## 2. The BLE protocol (GATT service ABD0)

| Characteristic | Purpose |
| --- | --- |
| ABD1 | Telemetry notify + read; v1 adds `seq` beside `tlm` |
| ABD2 | Setup/profile push and `obd_alarm`; not the authentication grant |
| ABD3 / ABD4 | Developer / error |
| ABD5 / ABD6 | OTA control and readable info / firmware data |
| ABD7 | Log dump |
| ABD8 | Provisioning, nonce/HMAC authentication and version negotiation |
| ABD9 | Relay ack/stop/resume writes, event notifications and readable session state |
| ABDA | Reserved read/write/notify characteristic |

**The GATT table is frozen.** It has a 32-handle budget, 30 used. Handles follow heap-address order,
not creation order; bonded phones cache them. Compare boot `GATT handles` lines and database dumps
before considering a table change. Automatic cache invalidation is not proven.

MTU 247 is the tested ceiling. Notifications carry at most MTU−3 bytes; larger ones are not
fragmented. ABD1's notification is trimmed to fit while its readable value retains the full frame.
Android can remain at MTU 23 until the app requests more.

The ABD8 nonce advertises `[MIN,MAX]`; a proof may choose a version. Absent/malformed means v0,
above maximum clamps to maximum, and MTU below 96 falls back to v0. A repeat proof keeps the existing
grant's version. Below the configured minimum gives no telemetry/setup protocol, but OTA/dump remain
available after auth. Callers use `bleProto()` capabilities, not numeric version checks.

- **v0:** the authenticated phone is the transport while connected; relayed frames are not queued.
- **v1:** subscribed ABD1 frames receive a wake-local sequence and stay queued until cumulatively
  acked on ABD9. An ack advances only through data successfully posted by the app.
- No advancing ack for `RELAY_ACK_TIMEOUT_MS` (120 s), or untagged data waiting while unsubscribed for
  `RELAY_UNSUBSCRIBED_MAX_MS` (300 s), hands delivery to cellular. Empty queues are not late.
  OTA/dump pause these clocks and independently suppress cellular.
- `{"relay":"stop"}` hands off immediately; only `resume` ends an explicit stop. Timeout/unsubscribed
  lapses can also recover through a forward ack; resubscribing ends an unsubscribed lapse.
- After failing to post seq N, the app must not ack later frames until ABD9 shows `sent >= N` and
  `oldest == 0` or `oldest > N`. Otherwise a cumulative ack would discard N unposted.
- A grant ending strips its queued frames' sequence tags, so a new session cannot ack old data.
  Sequence metadata never enters the cloud body or persistent backlog.

[Detailed BLE contract and field failures](docs/dongle-reference.md#section-2);
[versioned wire fixtures](protocol/fixtures/README.md).

## 3. The security stack (pairing and handshake on; signed profiles not yet required)

Three separate boundaries:

1. **Link privacy:** encrypted values and CCCDs, LE Secure Connections bonding with Just Works.
   Just Works provides no MITM authentication. Distribute the IRK to avoid accumulating bonds on
   address rotation. Pairing is central-driven through gated attributes; peripheral security requests
   caused duplicate Android dialogs. Clearing bonds requires forgetting the device on both sides.
2. **Session authority:** ABD8 HMAC-SHA256 over a fresh nonce grants access. The 32-byte secret is
   provisioned through ECDH TOFU, never sent as a raw shared secret. This blocks passive capture,
   not an active MITM during first provisioning. The eFuse-MAC dongle id, not a phone's BLE id,
   identifies the secret. `setup_unlocked` and `authenticated` are separate, link-scoped flags.
3. **Payload authority:** signed OTA is mandatory. Profile JWS verification uses the original bytes
   and RS256 before parsing; raw profiles are still accepted while `PROFILE_REQUIRE_SIGNATURE` is 0.
   Profile init commands can inject vehicle-bus traffic, so this is a security boundary, not formatting.

`AUTHCLEAR` and in-app key reset revoke the secret and forget profile, hash, token, queued uploads
and flash logs. Failed removal retains the key/token so reset can be retried. Deletion is not secure
erasure; coredumps, BLE bonds, alarm flag, clock and OTA NVS are not cleared. In-app reset requires
the power-on window, not an existing grant. Dump logs first if they are needed.

BLE callbacks hand off to the loop task; they must not perform flash, crypto or OBD work.
`JWSTEST` tests intact → tampered → intact: a successful push alone cannot prove enforcement.
[Detailed security/reset behaviour](docs/dongle-reference.md#section-3);
[design history (A)](docs/dongle-reference.md#appendix-a).

## 4. OTA firmware update (ABD5/ABD6)

- 4096-byte sectors, stop-and-wait ACKs and CRC16-CCITT frames. ABD5 carries both ACKs and status.
- Subscribe to ABD5 before START. START carries image size/version, raw SHA-256 (32 B) and
  ECDSA-P256 signature (64 B), not their hexadecimal strings.
- Transfers can resume within 90 s after disconnect. Commit leads to a trial boot with rollback.
- The image's signature is the trust boundary, not the download location. Key rotation must reach
  devices before images signed only with the new key.
- The release workflow generates/signs `firmware.bin` and `manifest.json`; version fields come from
  the build tag. Do not hand-bump them. `OTARESET` is for clearing a bench anti-downgrade floor.

[Release procedure](RELEASING.md), [OTA wire reference (C)](docs/dongle-reference.md#appendix-c),
[architecture history (B)](docs/dongle-reference.md#appendix-b).

## 5. Log retrieval (ABD7)

The app pulls windows; the dongle does not stream unsolicited logs. Polling pauses during a dump.
`LOGDUMP START` returns size/chunk/window information; offset requests return chunks and end-of-window
status; `LOGDUMP END` closes it. Binary chunks are `[0xD7][u32 offset LE][payload]`, allowing idempotent
re-requests from the last contiguous offset.

One filesystem read per roughly 8 KB window avoids the old 1–2 KB/s per-chunk-read bottleneck.
Measured throughput is about 9.6 KB/s; shortening inter-chunk delays did not improve it.

Every new log segment identifies its writer with `FW build=… v=… elf=… part=…`, and non-deep-sleep
boots write it after `WAKE`. A non-deep-sleep `WAKE` without that following line indicates older
firmware; a preceding segment's build line is not proof of the running image.
The ELF hash prefix identifies the matching coredump symbols.

[Detailed dump contract and build attribution](docs/dongle-reference.md#section-5).
Serial equivalents and diagnostic interpretation are in §9.

## 6. Power management (the top constraint)

**Car wakeups dominate drain.** Polling can wake ECUs that remain awake on the car's own timer.
Check the rail before powering peripherals or issuing bus traffic.

- ATRV is a local co-processor measurement, not CAN traffic, but can differ from the true rail by
  several volts. IO36's divider is the trusted independent rail measurement.
- The 8–11 V weak-rail band suppresses polling. Below it is treated as bench/no-car.
  A weak-rail cold boot opens a bounded BLE window but never starts modem/GNSS/OBD work.
  Recovery ends the wake rather than lifting that veto in place; preserve the key-reset window.
- `RAIL_AWAKE_VOLTS` needs per-car evidence. Measured driving/off bands nearly touched on the
  calibration car; do not invent hysteresis or assume every car has the same margin.
- Normal wake sources are motion and timer. Parked/charging/DC sleep tiers are 30/3/1 minutes.
  A confirmed DC charge can stay awake at a 30 s cadence, bounded by rail, alarm, motion, silence,
  charge budget/corroboration and the max-awake backstop. AC does not acquire the DC session latch.
- The charge-end follow-up and post-drive window shorten cadence only after otherwise-permitted
  rail-high polls. They do not override the alarm gate. A silent charge must not reset its fast budget.
- Bring BLE up before slow modem initialization. Suppress cellular only while `bleRelaying()` says
  the phone is the transport—not merely connected or, in v1, authenticated.
- Normal sleep flushes/saves uploads, deinitializes BLE, powers down peripherals and arms wake sources.
  At crash streak 3, the early timer-only escape intentionally skips peripheral initialization and
  shutdown and does not arm WOM.

Read [charge/wake rules and measurements](docs/dongle-reference.md#section-6) and
[deferred power work (H)](docs/dongle-reference.md#appendix-h) before modifying sleep.
RTC retention requirements are in §11.

## 6a. The cloud path, the data model, and where profiles come from

Cellular sends signed, gzipped v2 `InputPointList` bodies to `/2/app/tlm` as `ABRP_OTA`;
the phone posts relayed frames as `APP_OBD`. HTTP 200 is acceptance; there is no v2 body `status`.
The provider label is not proof of origin. `X-API-KEY` attributes the integration; the vehicle
token signs the request and is never sent as a bearer token.

**Size and memory:** SIM7070 `BODYLEN` tops out at 1024 B; `CELL_MAX_POST_BYTES` is 1000.
Every gzip body is inflated and compared before sending. Failures retain frames and eventually
fall back to plain bodies that fit. Frames too large while gzip is disabled are held for a later
wake; frames still over the cap after compression are logged and dropped to unblock the queue.
Use vendored zlib, not ROM miniz: the latter overflowed the loop stack and lacks the PSRAM workaround.
Preserve the PSRAM-safe runtime; test bodies over 4 KB after toolchain changes (`GZTEST`).

**Profiles:** there is no factory profile/token. Both arrive over ABD2 and persist together in
`/vehicle_profile.json`; the hash is committed only after persistence succeeds. Without a profile
there is no vehicle poll. New cars normally require backend profiles, not firmware, but new cellular
telemetry keys need an explicit v2 mapping. Parser behaviour must match the app, including multi-command
keys, missing-byte NaNs, virtual `VAL{}` keys, clamping and supported operators.

**Queue/backlog:** retain old cellular frames while BLE relays. v0-relayed frames are not also queued;
v1 frames stay until acked. Load an unread backlog only after modem registration, before `connect()`
and its session-idle check. Offline wakes append without loading; bounded compaction keeps newest data.
The PSRAM-first queue, heap/flash reserves and cap can discard oldest frames; losing a trip-end marker
must re-arm its debt. Changing to a different or unknown token discards the old vehicle's backlog.

**Drive and geo:** `drive_state.h` owns PARKED/DRIVING/UNCONFIRMED. GNSS needs car/motion evidence and
no charge latch to start driving; sustained motion without sufficient driving evidence can be
UNCONFIRMED. That state omits the frame's `is_parked`, uses live location and owes no PARK marker,
but remains parked to other readers. The alarm gate is independent.
Location and heading retain their observation times and are sent once per accepted observation.
Only HTTP 200 advances accepted marks; failed sends do not. Frozen positions are not periodically
re-uploaded merely to keep a backend map entry alive.

**Time:** GNSS and modem time/NTP supply cellular clock updates; HTTP Date is a coarse WiFi source,
not a cellular one. Only NTP or a confirmed GNSS pair teaches sleep drift. NTP never powers/attaches
the modem or extends a wake just to obtain time; its no-clock fallback uses an already-active modem.
Send-time clock trust gates geo, not eligible battery/state data. Geo expires at 29 days; queued frames
past 6 days are dropped only against a trusted send clock. A wrongly advanced trusted clock can
therefore discard history. The target's signed32 `time_t` remains bounded by 2038.

**TLS and signatures:** cellular verifies chains against its installed root bundle, but the modem
does not verify certificate names and is configured to ignore certificate dates. WiFi remains
bench-only with `setInsecure()`. Request signatures prevent new-payload forgery and disclosure of
the token, not reading/replaying uploads or an on-path peer falsely acknowledging them.
Do not describe this as full hostname-authenticated TLS.

When changing CA vendor, ship trusting firmware first, then switch the server. Run `TLSCHECK` on
a dongle and `tests/run_server_roots_checks.py --require-live` around the switch. An untrusted chain
keeps frames queued and must not trigger the stale-bearer restart ladder. Root selection, the modem's
5120-byte conversion limit, installation/retry behaviour and signing profile are in the
[detailed cloud/TLS reference](docs/dongle-reference.md#section-6a).
[Host coverage and remaining hardware gates](tests/README.md).

## 6b. What the co-processor can do on the bus, and the cars it rules out

The STM32 advertises ELM327 v1.5 but does not implement the full command set. It inserts its own
length byte and lacks raw-frame mode, extended addressing and programmable flow control.
`ATCRA` is rewritten to `ATCF`/`ATCM`; other unsupported commands cannot be repaired by a profile.
`NO DATA` is also returned with no bus connected, so it proves nothing about bus participation.

BMW i3/iX3 and raw-frame profiles such as `eGolf`/`MEB_new` hit these limits; alternative MEB
profiles do not necessarily require those commands. Check the [supported-car table](README.md)
and the [measured command/profile matrix](docs/dongle-reference.md#section-6b), not just a profile's existence.
The i3 listen-only experiments found ignition broadcast `0x130`, not passive battery telemetry;
those branch experiments are not a claim of battery support in main.

For protocol-6 `22xxxx` replies, require the expected ECU/DID and consecutive-frame order.
Do not salvage arbitrary payloads from the co-processor's observed cross-ECU splices.
`tests/run_obd_reply_checks.py` covers parser/retry behaviour; real bus compatibility still needs a car.

## 7. The OBD-alarm gate (enforced per vehicle — read before changing)

Some locked, armed vehicles sound their alarm on OBD transmission. Passive presence is not the
trigger. The app sets a per-vehicle NVS flag through ABD2, read back in ABD5 info.
Enforcement is enabled for flagged cars; historical “gate off” descriptions are superseded.

Outside the bounded setup permit, polling requires an energized rail **and** either fresh confirmed
GNSS movement or the bounded OBD-speed hold that sustains a permit through GNSS loss.
A rail-only gate would permit a parked charging car; a movement-only gate would permit an armed
car on a flatbed. Neither signal is sufficient.
Accelerometer activity cannot extend a permit through closing doors and locking.
Do not add a generic latch/grace that survives parking; preserve the explicit setup/speed rules.

The old timestamp-age defect is fixed in the current helper: `gpsMovingFresh()` uses
`gpsFixAgeMs()`, which ages changes in receiver date/time, not repeated parsing of `gd->ts`.
This is evidence of fresh receiver time, not proof of physical position age.
The old re-auth parsing and GNSS one-shot defects are also historical, not tasks to redo.

Remaining constraints include rail calibration and timer wakes suppressed before BLE bring-up:
a stationary flagged car can be radio-silent until motion. Gate bus requests, not connectability.
DENY diagnostics must expose inputs and `tx_count`; that counter includes local AT commands,
so a nonzero delta is not itself proof of CAN traffic.

[Gate detail](docs/dongle-reference.md#section-7),
[architecture (I)](docs/dongle-reference.md#appendix-i),
[original field failures (J)](docs/dongle-reference.md#appendix-j).

## 8. App side — the rules that matter

- Keep `'abrpobd'` separate from generic `'obdble'`.
- Account-synced vehicle binding is not a dongle secret. A second phone can inherit the binding
  without visiting setup; secrets remain device-local and are shared explicitly.
- Gated access triggers pairing. A single failed operation does not establish that pairing was
  declined; it can be racing the pairing procedure it started.
- Dropping an app-side device binding must also cancel its radio connection.
- Subscribe before expecting the dongle's greeting/status. Follow the negotiated version for all
  flows, especially relay acknowledgements (§2); acknowledging unposted data loses it.
- Coordinate wire changes across repositories. Describe obligations here, not closed-source app internals.

[Detailed app obligations](docs/dongle-reference.md#section-8);
profile/connect history in [Appendix A](docs/dongle-reference.md#appendix-a).

## 9. Diagnostics: how to actually find things

Use flash logs, not serial output that disappears after unplugging. `DUMP 3` usually covers a recent
drive; `DUMP` reads all segments; `DUMP SINCE <utc>` selects a starting point with clockless-boot
handling. It is not identical to filtering a full dump when clocks step backwards across segments.
ABD7 retrieves logs without unplugging the dongle. Logs contain private location history.

| Evidence | Interpretation |
| --- | --- |
| `WAKE` / `FW build` | Reset reason and image provenance; see §5 |
| `CLOCK wake`, `drift`, `ntp` | Compensation, teaching source and acquisition verdict |
| `MOTION` | Sampled vibration/activity, not sustained acceleration or proof of driving |
| `ALARMGATE` / `ATRVGATE` | Permit inputs and wake/poll decision |
| `SAMPLE_POLL`, `DC_AWAKE`, `FOLLOWUP`, `POSTDRIVE` | Charge evidence and cadence/hold reasons |
| `UPLOAD not sent` | Local token/signature/header failure, not a failed bearer |
| `NET diag ... open=` | Fresh/reused session and idle duration; modem state alone misses stale sessions |
| `FLUSHSTAT` | Open/drain timings and accepted/tried batches and frames |
| `WAKESTAT` | Pre-sleep preparation and maximum loop gap, not BLE latency or total sleep-entry time |
| `SLEEP enter reason=` | Actual cadence tier or explicit recovery/backstop reason |

`logEvent` messages have 119 usable characters. Log inputs and reasons, not only transitions:
an all-deny drive otherwise looks like a dead modem. Prove gates by observing refusals, not only
happy paths. Crash-streak 3 takes early timer-only sleep without a flash log or `SLEEP enter`.

Exact field meanings, clockless/reset handling, `SAMPLE_POLL`/`ATRVGATE` keys, session-idle bounds and
known limitations are in the [diagnostic reference](docs/dongle-reference.md#section-9).

## 10. Where to look for what

Use [technical overview](docs/technical-overview.md) for layout/settings,
[getting started](docs/getting-started.md) for setup, and [RELEASING.md](RELEASING.md) for releases.
The original design documents remain in the detailed reference. Names used in old comments map here:

| Appendix | Original document | Role |
| --- | --- | --- |
| [A](docs/dongle-reference.md#appendix-a) | `BLE_SECURITY_PLAN.md` | Security design and phase history |
| [B](docs/dongle-reference.md#appendix-b) | `ota-update-architecture-v1.md` | Original OTA architecture |
| [C](docs/dongle-reference.md#appendix-c) | `ota-protocol-reference.md` | Detailed OTA wire contract; code wins on conflict |
| [D](docs/dongle-reference.md#appendix-d) | `OTA_UPDATE_APP_SPEC.md` | Transport decision and design review |
| [E](docs/dongle-reference.md#appendix-e) | `backend_implements.md` | Backend expectations |
| [F](docs/dongle-reference.md#appendix-f) | `POWER_MANAGEMENT.md` | Hardware/power design |
| [G](docs/dongle-reference.md#appendix-g) | `POWER_BUDGET.md` | Bench measurements |
| [H](docs/dongle-reference.md#appendix-h) | `DEFERRED_POWER_WORK.md` | Power tradeoffs, rejected approaches and proposed work |
| [I](docs/dongle-reference.md#appendix-i) | `IGNITION_SENSE_ARCHITECTURE.md` | Alarm/ignition architecture |
| [J](docs/dongle-reference.md#appendix-j) | `ALARM_GATE_FIELD_FINDINGS.md` | Historical failures and original fix plan |

These are evidence, not current task lists. Check implementation and newer findings before acting on
an old “open” item. Keep new operational guidance here; retain dated field evidence in the reference.

## 11. If you change one thing, know this

1. Release through the workflow; version fields are generated, not edited.
2. Preserve GATT handles and negotiated protocol contracts; never switch on version numbers outside
   `ble_protocol.h`. Released wire fixtures are frozen.
3. Keep flash/crypto/OBD off BLE callbacks, and pairing central-driven.
4. Measure a vehicle's rail before trusting it as a permit. Never substitute motion for alarm safety.
5. Read [deferred power work](docs/dongle-reference.md#appendix-h) before “improving” sleep.
6. Keep the PSRAM-safe runtime/linker check. Buffers under 4 KB do not exercise its failure mode.
7. `RTC_DATA_ATTR` survives deep sleep, not panic/WDT/software reset. Crash-surviving state uses
   validated `RTC_NOINIT_ATTR`; power-on garbage and another image's rollback layout must be rejected.
   Preserve the crash streak, modem-may-be-on flag and park-marker debt until its marker is sent.
8. UNCONFIRMED remains parked except for frame state, live geo and marker semantics (§6a).
9. Keep the generic app lane untouched and land paired wire changes together.
10. Host tests and a linked firmware image are not hardware evidence. Report what still needs a
    dongle, drive or charge; see [test guidance](tests/AGENTS.md).
