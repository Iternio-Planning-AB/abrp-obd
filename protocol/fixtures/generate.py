"""Write the BLE protocol wire fixtures (see README.md) and their SHA256SUMS.

Every expected value is written out here by hand, from the protocol as specified, never captured from the firmware:
the firmware replay (tests/run_relay_checks.py fixtures) then holds the real code to these files, and the app replays
the same files. Run from anywhere: python3 protocol/fixtures/generate.py. A released version's files are frozen
(README.md, Drift control): regenerating must leave them byte-identical.
"""
import base64
import hashlib
import hmac
import json
from pathlib import Path

here = Path(__file__).resolve().parent

SECRET = bytes(range(0x00, 0x20))
NONCE = bytes(range(0xA0, 0xB0))
DONGLE_ID = "A1B2C3D4E5F6"
MTU = 185
T0 = 1791500000  # utc of poll 0; poll n is read at T0 + n

INPUTS = {
    "secret_hex": SECRET.hex(),
    "nonce_hex": NONCE.hex(),
    "dongle_id": DONGLE_ID,
    "mtu": MTU,
    "reset_window": False,
}
NONCE_B64 = base64.b64encode(NONCE).decode()
PROOF = base64.b64encode(hmac.new(SECRET, NONCE, hashlib.sha256).digest()).decode()


def compact(value):
    return json.dumps(value, separators=(",", ":"))


def nonce(range_, raw=None):
    # The frame authSendNonce builds: nonce, id, then the range when this firmware negotiates. Its exact text decides
    # the length: keys in this order, no spaces. raw: a malformed "proto" as written (the app-side rows).
    proto = "" if range_ is None and raw is None else ',"proto":%s' % (raw if raw is not None else "[%d,%d]" % tuple(range_))
    text = '{"nonce":"%s","id":"%s"%s}' % (NONCE_B64, DONGLE_ID, proto)
    frame = json.loads(text)
    return {"from": "dongle", "char": "ABD8", "op": "notify", "json": frame, "length": len(text.encode())}


def proof(proto=None, raw=None):
    frame = {"proof": PROOF}
    if proto is not None:
        frame["proto"] = proto
    step = {"from": "app", "char": "ABD8", "op": "write", "json": frame}
    if raw is not None:
        # A malformed proto the app sends as is; `json` holds the parsed value, `text` the exact write.
        step["json"] = json.loads('{"proof":"%s","proto":%s}' % (PROOF, raw))
        step["text"] = '{"proof":"%s","proto":%s}' % (PROOF, raw)
    return step


def ok(proto=None):
    frame = {"auth": "ok"}
    if proto is not None:
        frame["proto"] = proto
    return {"from": "dongle", "char": "ABD8", "op": "notify", "json": frame}


def values(n):
    return {"utc": T0 + n, "soc": 10 + n, "voltage": 350, "current": -20}


def tlm(n):
    return {"soc": 10 + n, "voltage": 350, "current": -20}


def poll(n, seq=None, op="notify"):
    step = {"from": "dongle", "char": "ABD1", "op": op, "poll": n, "values": values(n)}
    if op != "none":
        step["json"] = {"tlm": tlm(n)}
        if seq is not None:
            step["json"]["seq"] = seq
    return step


def subscribe(char):
    return {"from": "app", "char": char, "op": "subscribe"}


def abd9_write(frame, note=None):
    step = {"from": "app", "char": "ABD9", "op": "write", "json": frame}
    if note:
        step["note"] = note
    return step


def state(relay, sent, oldest, why=None, note=None):
    # sent: the last seq notified this wake; oldest: the lowest seq still in the dongle's cellular queue, 0 for none.
    frame = {"relay": relay}
    if why:
        frame["why"] = why
    frame.update({"sent": sent, "oldest": oldest})
    step = {"from": "app", "char": "ABD9", "op": "read", "json": frame}
    if note:
        step["note"] = note
    return step


def event(relay, why=None):
    frame = {"relay": relay}
    if why:
        frame["why"] = why
    return {"from": "dongle", "char": "ABD9", "op": "notify", "json": frame}


def check(**fields):
    return {"check": fields}


def fixture(title, firmware, app, replay, steps, session, app_writes_abd9, app_extra=None, firmware_extra=None):
    out = {
        "schema": 1,
        "title": title,
        "firmware": dict({"range": firmware}, **(firmware_extra or {})),
        "app": dict({"range": app}, **(app_extra or {})),
        "replay": replay,
        "inputs": INPUTS,
        "steps": steps,
        "expect": {"session": session, "app_writes_abd9": app_writes_abd9},
    }
    return out


FIXTURES = {}

# A v1 session from the grant to a resume after a timeout lapse: seqs beside tlm, acks, stop and resume on ABD9.
FIXTURES["v1/fw-0-1_app-0-1.json"] = fixture(
    "v1 firmware [0,1] x negotiating app [0,1]: a v1 session", [0, 1], [0, 1], ["firmware", "app"], [
        nonce([0, 1]),
        proof(1),
        ok(1),
        subscribe("ABD9"),
        subscribe("ABD1"),
        state("live", 0, 0),
        poll(1, 1),
        poll(2, 2),
        abd9_write({"relay_ack": 2}, "the app posted the frames up to seq 2"),
        state("live", 2, 0),
        check(transport="phone", queued=[], uploaded=[]),
        poll(3, 3),
        abd9_write({"relay": "stop"}, "the app went offline: it cannot post"),
        event("lapsed", "stop"),
        state("lapsed", 3, 3, why="stop"),
        check(transport="cellular", queued=[3], uploaded=[]),
        poll(4, 4),
        check(queued=[], uploaded=[3, 4]),
        abd9_write({"relay": "resume"}, "the app is online again"),
        event("live"),
        state("live", 4, 0),
        check(transport="phone"),
        poll(5, 5),
        abd9_write({"relay_ack": 5}),
        state("live", 5, 0),
        poll(6, 6),
        {"wait": 120000, "note": "the app stopped posting without saying so (e.g. iOS suspended it)"},
        event("lapsed", "timeout"),
        check(transport="cellular", queued=[6], uploaded=[3, 4]),
        poll(7, 7),
        check(queued=[], uploaded=[3, 4, 6, 7]),
        abd9_write({"relay_ack": 7}, "the app caught up: a forward ack ends a timeout lapse"),
        event("live"),
        state("live", 7, 0),
        check(transport="phone", queued=[], uploaded=[3, 4, 6, 7]),
    ], 1, True)

# The app's give-up rule: its post of seq 2 failed, so it acks nothing (an ack of 3 would take 2 off the dongle's queue
# unposted) until a state read shows sent >= 2 and oldest 0 or above 2. Firmware only: no step says a post failed.
FIXTURES["v1/fw-0-1_app-0-1-give-up.json"] = fixture(
    "v1 firmware [0,1] x negotiating app [0,1]: after a failed post the app waits for the dongle to send that frame",
    [0, 1], [0, 1], ["firmware"], [
        nonce([0, 1]),
        proof(1),
        ok(1),
        subscribe("ABD9"),
        subscribe("ABD1"),
        poll(1, 1),
        abd9_write({"relay_ack": 1}),
        dict(poll(2, 2), note="the app's post of seq 2 fails: it gives up on 2"),
        state("live", 1, 0, note="rebuilt before seq 2 was queued: oldest 0, but sent 1 < 2, so no ack"),
        dict(poll(3, 3), note="the app posts 3 but does not ack it"),
        state("live", 2, 2, note="sent >= 2, but oldest 2: seq 2 is still queued, so no ack"),
        {"wait": 120000, "note": "no ack has moved forward since seq 1"},
        event("lapsed", "timeout"),
        state("lapsed", 3, 2, why="timeout"),
        poll(4, 4),
        check(transport="cellular", queued=[], uploaded=[2, 3, 4]),
        {"wait": 1000, "note": "the dongle's next loop pass rebuilds the state"},
        state("lapsed", 4, 0, why="timeout", note="sent >= 2 and oldest 0: seq 2 went over cellular, the app may ack again"),
        abd9_write({"relay_ack": 4}),
        event("live"),
        state("live", 4, 0),
        check(transport="phone", queued=[], uploaded=[2, 3, 4]),
    ], 1, True)

# The app's give-up stop: it writes a stop as soon as it gives up on seq 2, so cellular sends 2 at once rather than
# after RELAY_ACK_TIMEOUT_MS, and a resume once the state shows 2 gone. The same v1 wire messages as the file above,
# which stays as it was (v1 is released). Firmware only: no step says a post failed.
FIXTURES["v1/fw-0-1_app-0-1-give-up-stop.json"] = fixture(
    "v1 firmware [0,1] x negotiating app [0,1]: after a failed post the app stops the relay until that frame is sent",
    [0, 1], [0, 1], ["firmware"], [
        nonce([0, 1]),
        proof(1),
        ok(1),
        subscribe("ABD9"),
        subscribe("ABD1"),
        poll(1, 1),
        abd9_write({"relay_ack": 1}),
        dict(poll(2, 2), note="the app's post of seq 2 fails: it gives up on 2"),
        abd9_write({"relay": "stop"}, "a give-up hands over at once"),
        event("lapsed", "stop"),
        state("lapsed", 2, 2, why="stop", note="seq 2 is still queued: no resume, no ack"),
        check(transport="cellular", queued=[2], uploaded=[]),
        dict(poll(3, 3), note="the app posts 3 but does not ack it"),
        check(queued=[], uploaded=[2, 3]),
        {"wait": 1000, "note": "the dongle's next loop pass rebuilds the state"},
        state("lapsed", 3, 0, why="stop", note="sent >= 2 and oldest 0: seq 2 went over cellular, the app resumes"),
        abd9_write({"relay": "resume"}),
        event("live"),
        state("live", 3, 0),
        poll(4, 4),
        abd9_write({"relay_ack": 4}),
        state("live", 4, 0),
        check(transport="phone", queued=[], uploaded=[2, 3]),
    ], 1, True)

# The second bound: the app's ABD1 subscription died with the link and the grant alive. Firmware only: the app does not
# unsubscribe on purpose.
FIXTURES["v1/fw-0-1_app-0-1-unsubscribed.json"] = fixture(
    "v1 firmware [0,1] x negotiating app [0,1]: an app that stops listening on ABD1 lapses after 300 s",
    [0, 1], [0, 1], ["firmware"], [
        nonce([0, 1]),
        proof(1),
        ok(1),
        subscribe("ABD9"),
        subscribe("ABD1"),
        poll(1, 1),
        abd9_write({"relay_ack": 1}),
        state("live", 1, 0),
        {"from": "app", "char": "ABD1", "op": "unsubscribe", "note": "the subscription died; the link and grant did not"},
        poll(2, op="value"),
        {"wait": 300000, "note": "RELAY_UNSUBSCRIBED_MAX_MS since the first frame queued untagged"},
        event("lapsed", "unsubscribed"),
        state("lapsed", 1, 0, why="unsubscribed"),
        check(transport="cellular", queued=[2]),
        poll(3, op="value"),
        check(queued=[], uploaded=[2, 3]),
        subscribe("ABD1"),
        event("live"),
        state("live", 1, 0),
        poll(4, 2),
        check(transport="phone", queued=[4], uploaded=[2, 3]),
    ], 1, True)


def render(content):
    # One top-level key per line and one step per line: diffs then show exactly which step changed.
    lines = []
    for key, value in content.items():
        if key == "steps":
            body = ",\n".join("    " + json.dumps(step, separators=(", ", ": ")) for step in value)
            lines.append('  "steps": [\n' + body + "\n  ]")
        else:
            lines.append("  " + json.dumps(key) + ": " + json.dumps(value, separators=(", ", ": ")))
    return "{\n" + ",\n".join(lines) + "\n}\n"


def abd2(text, reply):
    return [{"from": "app", "char": "ABD2", "op": "write", "text": text},
            {"from": "dongle", "char": "ABD2", "op": "notify", "text": reply}]


def v0_session(nonce_step, proof_step, note=None):
    # A v0 session: frames byte-identical to today's (no seq), relayed and not queued, nothing written to ABD9.
    steps = [nonce_step, proof_step, ok(), subscribe("ABD1"), poll(1), poll(2), poll(3),
             check(transport="phone", queued=[], uploaded=[])]
    if note:
        steps[1] = dict(steps[1], note=note)
    return steps


FIXTURES["v0/fw-none_app-none.json"] = fixture(
    "main firmware (no negotiation) x app without negotiation: today's exchange", None, None, [],
    v0_session(nonce(None), proof()), 0, False)
FIXTURES["v0/fw-none_app-0-1.json"] = fixture(
    "main firmware (no negotiation) x negotiating app [0,1]: no range offered, so no proto in the proof",
    None, [0, 1], ["app"], v0_session(nonce(None), proof()), 0, False)
FIXTURES["v0/fw-0-1_app-none.json"] = fixture(
    "v1 firmware [0,1] x app without negotiation: a v0 session, frames byte-identical to today's",
    [0, 1], None, ["firmware"], v0_session(nonce([0, 1]), proof()), 0, False)
for name, raw in (("string", '"1"'), ("negative", "-1"), ("fraction", "1.5"), ("float", "1.0"),
                  ("huge", "1099511627776")):
    FIXTURES[f"v0/fw-0-1_app-malformed-{name}.json"] = fixture(
        f"v1 firmware [0,1] x a proof whose proto is {raw}: malformed means absent, a v0 session, no echo",
        [0, 1], None, ["firmware"], v0_session(nonce([0, 1]), proof(raw=raw)), 0, False,
        app_extra={"proof_proto": raw})
for name, raw in (("number", "1"), ("one-element", "[1]"), ("reversed", "[1,0]"), ("string-element", '["0",1]'),
                  ("out-of-range", "[0,256]")):
    FIXTURES[f"v0/fw-malformed-{name}_app-0-1.json"] = fixture(
        f"a nonce frame whose proto is {raw} x negotiating app [0,1]: malformed means absent, so no proto in the proof",
        None, [0, 1], ["app"], v0_session(nonce(None, raw=raw), proof()), 0, False,
        firmware_extra={"nonce_proto": raw})

FIXTURES["v1/fw-0-2_app-0-1.json"] = fixture(
    "firmware [0,2] x negotiating app [0,1]: the app chooses 1", [0, 2], [0, 1], ["app"], [
        nonce([0, 2]), proof(1), ok(1), subscribe("ABD9"), subscribe("ABD1"), state("live", 0, 0),
        poll(1, 1), abd9_write({"relay_ack": 1}), state("live", 1, 0),
    ], 1, True)
FIXTURES["v1/fw-0-1_app-0-2.json"] = fixture(
    "v1 firmware [0,1] x negotiating app [0,2]: the app chooses 1", [0, 1], [0, 2], ["firmware", "app"], [
        nonce([0, 1]), proof(1), ok(1), subscribe("ABD9"), subscribe("ABD1"), state("live", 0, 0),
        poll(1, 1), abd9_write({"relay_ack": 1}), state("live", 1, 0), check(transport="phone", queued=[]),
    ], 1, True)
FIXTURES["v1/fw-0-1_app-proof-7.json"] = fixture(
    "v1 firmware [0,1] x a proof asking for 7: above the maximum runs at it, and the echo says 1",
    [0, 1], None, ["firmware"], [
        nonce([0, 1]), proof(7), ok(1), subscribe("ABD1"), poll(1, 1), check(transport="phone", queued=[1]),
    ], 1, False, app_extra={"proof_proto": "7"})
FIXTURES["v1/fw-1-1_app-none.json"] = fixture(
    "firmware [1,1] (BLE_PROTO_MIN 1) x app without negotiation: a session with no protocol", [1, 1], None,
    ["firmware"], [
        nonce([1, 1]), proof(), ok(), subscribe("ABD1"),
        *abd2('{"obd_alarm":1}', "PROTO"),
        poll(1, op="none"), check(transport="cellular"),
        poll(2, op="none"), check(transport="cellular", queued=[], uploaded=[1, 2]),
    ], "none", False, firmware_extra={"build": "-DBLE_PROTO_MIN=1"})
FIXTURES["v1/fw-1-2_app-0-0.json"] = fixture(
    "firmware [1,2] x an early negotiating app [0,0]: no overlap, the app is too old and writes no proof",
    [1, 2], [0, 0], ["app"], [nonce([1, 2])], "app_too_old", False)
FIXTURES["v1/fw-none_app-1-2.json"] = fixture(
    "main firmware (no negotiation) x app [1,2]: no overlap, the dongle is too old and the app writes no proof",
    None, [1, 2], ["app"], [nonce(None)], "dongle_too_old", False)


def main():
    sums = []
    for name, content in sorted(FIXTURES.items()):
        path = here / name
        path.parent.mkdir(parents=True, exist_ok=True)
        text = render(content)
        if json.loads(text) != content:
            raise SystemExit(f"{name}: the rendering does not parse back to the fixture")
        path.write_text(text)
        sums.append(f"{hashlib.sha256(text.encode()).hexdigest()}  {name}")
    (here / "SHA256SUMS").write_text("\n".join(sums) + "\n")


if __name__ == "__main__":
    main()
