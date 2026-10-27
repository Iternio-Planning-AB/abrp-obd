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


def nonce(range_):
    # The frame authSendNonce builds: nonce, id, then the range when this firmware negotiates. Its exact text decides
    # the length: keys in this order, no spaces.
    text = '{"nonce":"%s","id":"%s"%s}' % (
        NONCE_B64, DONGLE_ID, "" if range_ is None else ',"proto":[%d,%d]' % tuple(range_))
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


def state(relay, acked, sent, queued, why=None):
    frame = {"relay": relay}
    if why:
        frame["why"] = why
    frame.update({"acked": acked, "sent": sent, "queued": queued})
    return {"from": "app", "char": "ABD9", "op": "read", "json": frame}


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
        state("live", 0, 0, 0),
        poll(1, 1),
        poll(2, 2),
        abd9_write({"relay_ack": 2}, "the app posted the frames up to seq 2"),
        state("live", 2, 2, 0),
        check(transport="phone", queued=[], uploaded=[]),
        poll(3, 3),
        abd9_write({"relay": "stop"}, "the app went offline: it cannot post"),
        event("lapsed", "stop"),
        state("lapsed", 2, 3, 1, why="stop"),
        check(transport="cellular", queued=[3], uploaded=[]),
        poll(4, 4),
        check(queued=[], uploaded=[3, 4]),
        abd9_write({"relay": "resume"}, "the app is online again"),
        event("live"),
        state("live", 2, 4, 0),
        check(transport="phone"),
        poll(5, 5),
        abd9_write({"relay_ack": 5}),
        state("live", 5, 5, 0),
        poll(6, 6),
        {"wait": 120000, "note": "the app stopped posting without saying so (e.g. iOS suspended it)"},
        event("lapsed", "timeout"),
        check(transport="cellular", queued=[6], uploaded=[3, 4]),
        poll(7, 7),
        check(queued=[], uploaded=[3, 4, 6, 7]),
        abd9_write({"relay_ack": 7}, "the app caught up: a forward ack ends a timeout lapse"),
        event("live"),
        state("live", 7, 7, 0),
        check(transport="phone", queued=[], uploaded=[3, 4, 6, 7]),
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
