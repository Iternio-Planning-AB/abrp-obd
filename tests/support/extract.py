"""Source extraction shared by the host runners: brace-matched blocks and the setup() wake-gate slice."""


def find(text, anchor, start=0, what="source"):
    """Index of anchor in text; a missing anchor fails with its text rather than a bare ValueError."""
    index = text.find(anchor, start)
    if index < 0:
        raise SystemExit(f"FAIL: anchor not found in {what}: {anchor!r} - the code it extracts has moved")
    return index


def block(text, opener, what="source"):
    """From opener through the brace that closes the first { after it."""
    start = find(text, opener, what=what)
    depth, i = 0, text.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
        if depth == 0:
            return text[start:i] + "\n"


def wake_gate(logger, what="logger.ino"):
    """The setup() poll decision, from the weak-rail test through should_poll."""
    start = find(logger, "    bool weak_rail = (rail >= IO36_LOCKOUT_LOW", what=what)
    end = find(logger, "heartbeat_due);\n", start, what) + len("heartbeat_due);\n")
    return logger[start:end]
