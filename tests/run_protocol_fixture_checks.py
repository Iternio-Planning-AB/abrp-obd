"""Checks on the BLE protocol wire fixtures (protocol/fixtures, see its README.md).

SHA256SUMS lists every fixture file and nothing else, and every hash matches; every file is what generate.py writes
(no hand edit can drift from the generator); every file follows the schema in the README; every proof is the HMAC of
the file's own nonce under its own secret (computed here with hashlib, independently of the firmware); every nonce
frame's length is its compact text's; and a v0 session never has the app write to ABD9. The firmware's replay of the
files is tests/run_relay_checks.py fixtures; this runner needs no compiler."""
import base64
import hashlib
import hmac
import importlib.util
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
fixtures = root / "protocol/fixtures"


def fail(message):
    raise SystemExit("FAIL: " + message)


manifest = {}
for line in (fixtures / "SHA256SUMS").read_text().splitlines():
    digest, name = line.split("  ", 1)
    manifest[name] = digest
present = {str(path.relative_to(fixtures)) for path in fixtures.glob("v*/*.json")}
if present != set(manifest):
    fail(f"protocol/fixtures does not match SHA256SUMS: unlisted {sorted(present - set(manifest))}, "
         f"missing {sorted(set(manifest) - present)}")
for name, digest in manifest.items():
    if hashlib.sha256((fixtures / name).read_bytes()).hexdigest() != digest:
        fail(f"protocol/fixtures/{name} drifted from SHA256SUMS")

spec = importlib.util.spec_from_file_location("generate", fixtures / "generate.py")
generate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generate)
if set(generate.FIXTURES) != present:
    fail("generate.py and the fixture files name different files")
for name, content in generate.FIXTURES.items():
    if generate.render(content) != (fixtures / name).read_text():
        fail(f"protocol/fixtures/{name} is not what generate.py writes")


def is_version(value):
    return type(value) is int and 0 <= value <= 255


def is_range(value):
    return isinstance(value, list) and len(value) == 2 and all(map(is_version, value)) and value[0] <= value[1]


CHARS = {"ABD1", "ABD2", "ABD8", "ABD9"}
SESSIONS = {"none", "app_too_old", "dongle_too_old"}
for name in sorted(present):
    where = f"protocol/fixtures/{name}"
    doc = json.loads((fixtures / name).read_text())
    if doc.get("schema") != 1 or not isinstance(doc.get("title"), str):
        fail(f"{where}: schema must be 1 and title a string")
    for side in ("firmware", "app"):
        if not isinstance(doc.get(side), dict) or not (doc[side]["range"] is None or is_range(doc[side]["range"])):
            fail(f"{where}: {side}.range must be null or [min, max]")
    if not isinstance(doc["replay"], list) or not set(doc["replay"]) <= {"firmware", "app"}:
        fail(f"{where}: replay lists firmware, app, both or neither")
    inputs = doc["inputs"]
    secret, nonce = bytes.fromhex(inputs["secret_hex"]), bytes.fromhex(inputs["nonce_hex"])
    if len(secret) != 32 or len(nonce) != 16 or len(inputs["dongle_id"]) != 12 or inputs["reset_window"] is not False \
            or type(inputs["mtu"]) is not int or not 23 <= inputs["mtu"] <= 247:
        fail(f"{where}: inputs must hold a 32-byte secret, a 16-byte nonce, a 12-char id, an MTU and a closed window")
    expected_proof = base64.b64encode(hmac.new(secret, nonce, hashlib.sha256).digest()).decode()
    session = doc["expect"]["session"]
    if not (is_version(session) or session in SESSIONS) or type(doc["expect"]["app_writes_abd9"]) is not bool:
        fail(f"{where}: expect.session and expect.app_writes_abd9 are malformed")
    abd9_writes = proofs = 0
    polls, socs = [], []
    for index, step in enumerate(doc["steps"]):
        at = f"{where} step {index}"
        extra = set(step) - {"from", "char", "op", "json", "text", "length", "poll", "values", "note", "wait", "check"}
        if extra:
            fail(f"{at}: unknown fields {sorted(extra)}")
        if "wait" in step:
            if type(step["wait"]) is not int or step["wait"] <= 0:
                fail(f"{at}: wait must be a positive number of milliseconds")
            continue
        if "check" in step:
            if not set(step["check"]) <= {"transport", "queued", "uploaded"} or \
                    step["check"].get("transport", "phone") not in ("phone", "cellular"):
                fail(f"{at}: check holds transport, queued and uploaded only")
            continue
        if step.get("from") not in ("app", "dongle") or step.get("char") not in CHARS:
            fail(f"{at}: from must be app or dongle and char one of {sorted(CHARS)}")
        frame = step.get("json")
        if step["char"] == "ABD8" and step["from"] == "dongle" and "nonce" in (frame or {}):
            if frame["nonce"] != base64.b64encode(nonce).decode() or frame["id"] != inputs["dongle_id"]:
                fail(f"{at}: the nonce frame must carry the file's nonce and id")
            if step.get("length") != len(json.dumps(frame, separators=(",", ":")).encode()):
                fail(f"{at}: length must be the nonce frame's exact byte length")
            if not step["length"] < 93:
                fail(f"{at}: a nonce frame must fit the 93-byte notify of the MTU 96 floor")
        elif "length" in step:
            fail(f"{at}: only a nonce frame carries a length")
        if step["char"] == "ABD8" and step["from"] == "app":
            proofs += 1
            if frame.get("proof") != expected_proof:
                fail(f"{at}: the proof must be base64(HMAC-SHA256(secret, nonce))")
            if "text" in step and json.loads(step["text"]) != frame:
                fail(f"{at}: text must parse to json")
        if step["char"] == "ABD1" and step["from"] == "dongle":
            if step["op"] not in ("notify", "value", "none") or type(step.get("poll")) is not int or \
                    not isinstance(step.get("values"), dict) or ("json" in step) == (step["op"] == "none"):
                fail(f"{at}: an ABD1 poll has op notify/value/none, a poll number, values, and json unless none")
            polls.append(step["poll"])
            socs.append(step["values"].get("soc"))
        if step["char"] == "ABD9" and step["from"] == "app" and step["op"] == "write":
            abd9_writes += 1
        if step["char"] == "ABD2" and not isinstance(step.get("text"), str):
            fail(f"{at}: ABD2 steps carry text")
    if polls != list(range(1, len(polls) + 1)):
        fail(f"{where}: polls are numbered 1, 2, ... in order")
    if None in socs or len(set(socs)) != len(socs):
        fail(f"{where}: every poll reads a distinct soc (the firmware replay tells frames apart by it)")
    if doc["expect"]["app_writes_abd9"] != (abd9_writes > 0):
        fail(f"{where}: expect.app_writes_abd9 must say whether the steps have the app write ABD9")
    if session in ("app_too_old", "dongle_too_old") and proofs:
        fail(f"{where}: an app with no overlap writes no proof")
    if name.startswith("v0/") and (session != 0 or abd9_writes):
        fail(f"{where}: a v0/ file is a v0 session, and in one the app writes nothing to ABD9")
print(f"PASS protocol fixtures: {len(present)} files match SHA256SUMS and generate.py, schema, proofs and nonce lengths")
