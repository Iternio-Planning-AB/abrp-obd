# BLE protocol wire fixtures

Golden exchanges between the dongle and the ABRP app, one file per firmware × app pairing. They are the contract
between the two repositories: the firmware replays the files whose firmware column is this firmware
(`tests/run_relay_checks.py fixtures`), and the app copies this directory and replays the files whose app column it
can play in its own tests (its handshake, its ABD1 listener and its acker).

## Layout

```
protocol/fixtures/
  README.md         this file
  SHA256SUMS        sha256 of every fixture file below, `<hex>  <path relative to this directory>`
  generate.py       writes every fixture file and SHA256SUMS (the expected values are written out by hand there)
  v0/*.json         pairings whose session runs protocol v0 (today's protocol), including both nonce shapes
  v1/*.json         pairings that need negotiation to decide (a v1 session, no protocol, no overlap)
```

File names are `fw-<firmware range>_app-<app range>[-<variant>].json`, where a range is `min-max` and `none` means a
side that does not negotiate (firmware: no `proto` in its nonce frame; app: no `proto` in its proof).

| File | Firmware | App | Session | Replayed by |
|---|---|---|---|---|
| `v0/fw-none_app-none.json` | no negotiation (main) | no negotiation | v0 | neither (documents today's exchange) |
| `v0/fw-none_app-0-1.json` | no negotiation (main) | `[0,1]` | v0 | app |
| `v0/fw-0-1_app-none.json` | `[0,1]` | no negotiation | v0 | firmware |
| `v0/fw-0-1_app-malformed-*.json` | `[0,1]` | proof with a malformed `proto` | v0 | firmware |
| `v0/fw-malformed-*_app-0-1.json` | nonce with a malformed range | `[0,1]` | v0 | app |
| `v1/fw-0-1_app-0-1.json` | `[0,1]` | `[0,1]` | v1 | firmware, app |
| `v1/fw-0-2_app-0-1.json` | `[0,2]` | `[0,1]` | v1 | app |
| `v1/fw-0-1_app-0-2.json` | `[0,1]` | `[0,2]` | v1 (clamped) | firmware |
| `v1/fw-1-1_app-none.json` | `[1,1]` (`-DBLE_PROTO_MIN=1`) | no negotiation | no protocol | firmware |
| `v1/fw-1-2_app-0-0.json` | `[1,2]` | `[0,0]` | app too old | app |
| `v1/fw-none_app-1-2.json` | no negotiation | `[1,2]` | dongle too old | app |

The design's `[1,2]` × no-negotiation row is `v1/fw-1-1_app-none.json`: this firmware's maximum is 1, so the
`BLE_PROTO_MIN` override build speaks `[1,1]`, and the dongle side of that row (a no-protocol session) is the same.

## File format

Every file is one JSON object:

```json
{
  "schema": 1,
  "title": "v1 firmware [0,1] x negotiating app [0,1]",
  "firmware": { "range": [0, 1] },
  "app": { "range": [0, 1] },
  "replay": ["firmware", "app"],
  "inputs": {
    "secret_hex": "<32 bytes, hex>",
    "nonce_hex": "<16 bytes, hex>",
    "dongle_id": "A1B2C3D4E5F6",
    "mtu": 185,
    "reset_window": false
  },
  "steps": [ ... ],
  "expect": { "session": 1, "app_writes_abd9": true }
}
```

- `firmware.range`: the `proto` range the dongle sends in its nonce frame, or `null` for firmware that does not
  negotiate. `firmware.build` (optional) names a test build, e.g. `"-DBLE_PROTO_MIN=1"`.
- `app.range`: the range the app supports, `[min,max]`, or `null` for an app that does not negotiate.
  `app.proof_proto` (optional) is the raw JSON the proof carries as `proto` when it is not what the range implies
  (the malformed rows).
- `replay`: which sides replay the file. A side not listed must not fail on it; it may still parse it.
- `inputs`: everything that makes the exchange deterministic. The secret is the dongle's provisioned key; the nonce is
  what the firmware's `esp_random()` yields (the firmware replay fakes it, four little-endian 32-bit words in byte
  order); `dongle_id` is the `id` of every greet; `mtu` is the negotiated ATT MTU for the whole connection;
  `reset_window` says whether the power-on key-reset window is open (it is `false` everywhere, so no `"reset":1`).
- `expect.session`: `0`, `1`, … (the session's protocol version), `"none"` (a no-protocol session: the proof is
  accepted, setup is refused), `"app_too_old"` (the app refuses the dongle and writes no proof) or
  `"dongle_too_old"` (the app treats the dongle's firmware as too old and writes no proof).
- `expect.app_writes_abd9`: whether the app writes anything to ABD9 in this exchange. `false` in every v0 session: a
  negotiating app writes nothing to ABD9 unless the session is v1.

### Steps

`steps` is the exchange in order. A step is one of:

| Step | Meaning |
|---|---|
| `{"from":"dongle","char":"ABD8","op":"notify","json":{…},"length":70}` | A notify the dongle sends. `length`, when present (only on nonce frames), is the frame's exact byte length. |
| `{"from":"app","char":"ABD8","op":"write","json":{…},"text":"…"}` | A write the app makes (the proof). `text`, when present (the malformed rows), is the exact bytes written, since a token such as `1.0` and its parsed value differ. |
| `{"from":"app","char":"ABD1","op":"subscribe"}` | The app arms a CCCD (`ABD1` or `ABD9`); `"op":"unsubscribe"` disarms it. |
| `{"from":"dongle","char":"ABD1","op":"notify","poll":3,"values":{…},"json":{…}}` | One poll. `values` is what the dongle read from the car (numbers, `utc` included); `json` is the frame it notifies. |
| `{"from":"dongle","char":"ABD1","op":"value","poll":3,"values":{…},"json":{…}}` | One poll while the app is not subscribed to ABD1: `json` becomes ABD1's readable value, no notify goes out. |
| `{"from":"dongle","char":"ABD1","op":"none","poll":3,"values":{…}}` | One poll in a no-protocol session: nothing is written to ABD1. |
| `{"from":"app","char":"ABD9","op":"write","json":{…}}` | A session-channel write (`relay_ack`, `relay`). |
| `{"from":"app","char":"ABD9","op":"read","json":{…}}` | The app reads ABD9; `json` is the value the dongle returns. |
| `{"from":"dongle","char":"ABD9","op":"notify","json":{…}}` | A session-channel event. |
| `{"from":"app","char":"ABD2","op":"write","text":"…"}` | A setup-channel write (raw text, as written). |
| `{"from":"dongle","char":"ABD2","op":"notify","text":"…"}` | A setup-channel reply (plain text such as `PROTO`, compared exactly). |
| `{"wait":120000}` | Nothing happens for this many milliseconds. |
| `{"check":{"transport":"phone","queued":[3],"uploaded":[1,2]}}` | Dongle-internal state at this point (below). |

Any step may carry a `"note"` string for people; it has no meaning.

Timing: every poll step takes one second (the clock advances 1000 ms after it); every other step except `wait` takes
no time.

`check` fields, each optional: `transport` is `"phone"` while the phone counts as the transport (cellular stands down)
and `"cellular"` otherwise; `queued` lists, in order, the polls (by `poll` number) waiting in the dongle's cellular
queue; `uploaded` lists, in order, every poll the dongle has uploaded over cellular so far. The app may use
`transport` (the relay status it shows) and ignores the rest.

### Comparing

- JSON frames (`json`) are compared after parsing: object key order does not matter, numbers compare by value, and an
  integer must be an integer token (`1`, never `1.0`). Nothing may be missing and nothing extra may be present.
- The nonce frame's byte length (`length`) is checked exactly by the firmware. It is the one frame sent before
  either side knows the other's version, and its headroom under the 93-byte notify (MTU 96) is the constraint.
- ABD2 `text` is compared byte for byte.
- `proof` values are `base64(HMAC-SHA256(secret, nonce))` over the raw bytes from `inputs`; the fixture checker
  recomputes every one.
- Dongle notifies on ABD8 and ABD9 are complete: the firmware replay fails if it sent one the file does not list, in
  that place. ABD1 frames appear exactly where the polls are.

### Who replays what

- **Firmware** (`tests/run_relay_checks.py fixtures`): every file whose `replay` lists `firmware`. The nonce frame
  comes from the real `authSendNonce` (with `esp_random` faked to `nonce_hex`), the proof is accepted through the real
  `bleProtoAccept` (the HMAC itself is checked by the runner in Python, since `auth_gate.h` does not compile on a host),
  polls run through the real ABD1 send, queue and cellular gate, and ABD9 writes and reads go through the real
  session hooks. After each `app` step and each `wait` the replay runs one loop pass without a poll. ABD2 replies are
  checked only for the gate replies the sketch decides (`NOAUTH`, `PROTO`); the ABD2 handler itself runs on the
  Bluedroid task and is pinned structurally. The `fw-none_*` nonce frames are checked byte for byte against
  `authSendNonce` from the runner's pinned main commit, so the main-firmware shape is the real one, not a copy.
- **App**: every file whose `replay` lists `app`. Dongle steps are what the app receives; app steps are what it must
  write (and nothing else may be written: in particular, no ABD9 write where the file has none). `check` steps are
  optional for the app.

## Drift control

`SHA256SUMS` is checked by `tests/run_protocol_fixture_checks.py` (every file listed, nothing unlisted, every hash
equal), which also validates every file against this format and recomputes every proof. The app copies this directory
together with `SHA256SUMS` and the firmware commit it copied from, verifies its copy against that file, and re-copies
at the firmware release tag before it merges.

A version's fixtures are frozen once that version is released: `v0/` already is (v0 is today's protocol, so its
exchanges never change; frozen by policy), and `v1/` freezes when the first firmware speaking v1 ships. CI does not
yet refuse edits to a released version's files; that check comes with the v1 release. Until then a change here is a
protocol change: it lands together with the matching change in the app.
