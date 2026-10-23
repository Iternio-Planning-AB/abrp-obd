// Host checks for the reply retry in the real OBDWrapper::readPIDs(), compiled from obdwrapper.cpp against
// tests/obd/FreematicsOBD.h. The co-processor is a scripted CLink; replies are the scrubbed 220105 fixtures of
// obd_reply_checks.cpp.
#include <cstdio>
#include <string>
#include <vector>

#include "obdwrapper.h"

HostSerial Serial;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const std::string PROMPT = "\r\r>";
static const std::string GOOD =
  "7EC102E620105FFFB74\r7EC210F012C01012C0B\r7EC220B0B0B0B0B0B6C\r7EC23346C340000500C\r"
  "7EC240003A8B0336900\r7EC2554000000000000\r7EC26000B0B0B0B0000" + PROMPT;
static const std::string SPLICE =
  "17BA51043000000000000\r17BA52100000000000000\r17BA52200000000000000\r17BA523EC620105FFFB74\r"
  "17BA5240F012C01012C0B\r17BA5250B0B0B0B0B0B6C\r17BA526346C340000500C\r17BA5270003A8B0336900\r"
  "17BA52854000000000000\r17BA529000B0B0B0B0000" + PROMPT;
static const std::string NRC = "7EC037F223100000000" + PROMPT;

// Answers each send() with the next scripted reply, NO DATA once they run out; records what was sent.
struct ScriptedLink : CLink {
  std::vector<std::string> replies, sent, commands;
  bool send(const char* str) override { sent.emplace_back(str); return true; }
  int receive(char* buffer, int bufsize, unsigned int) override {
    std::string reply = "NO DATA" + PROMPT;
    if (!replies.empty()) { reply = replies.front(); replies.erase(replies.begin()); }
    const int n = std::min((int)reply.size(), bufsize - 1);
    memcpy(buffer, reply.data(), n);
    buffer[n] = '\0';
    return n;
  }
  int sendCommand(const char* cmd, char* buf, int bufsize, unsigned int) override {
    commands.emplace_back(cmd);
    snprintf(buf, bufsize, "OK\r\r>");
    return 2;
  }
};

// speed and ext_temp come from the reply too, so the standard fallback has nothing to add and sends nothing.
static const char* PROFILE = R"({
  "obd_protocol": "6",
  "init_commands": { "command": ["ATZ", "ATSP6", "ATSH7E4", "ATCRA7EC"] },
  "data_commands": { "command": ["220105"] },
  "soc": { "command": "220105", "equation": "a/2" },
  "soh": { "command": "220105", "equation": "b" },
  "speed": { "command": "220105", "equation": "c" },
  "ext_temp": { "command": "220105", "equation": "d" }
})";

static int guard_calls = 0, guard_closes_at = 0;
// Once closed the gate stays closed, as the alarm gate's does.
static bool guard() { ++guard_calls; return !guard_closes_at || guard_calls < guard_closes_at; }

struct Poll {
  OBDVector results;
  std::vector<std::string> sent;
  uint32_t tx = 0;
};

static Poll poll(OBDWrapper& obd, ScriptedLink& link, std::vector<std::string> replies) {
  link.replies = std::move(replies);
  link.sent.clear();
  const uint32_t before = obd.tx_count;
  Poll p;
  p.results = obd.readPIDs();
  p.sent = link.sent;
  p.tx = obd.tx_count - before;
  return p;
}

static bool hasSoc(const Poll& p) {
  for (const auto& r : p.results) if (r.first == "soc") return r.second == 255 / 2.0;
  return false;
}

static void setUp(OBDWrapper& obd, ScriptedLink& link, const char* profile = PROFILE) {
  obd.begin(&link);
  obd.initVehicle(profile);
  obd.setPollGuard(guard);
  guard_calls = guard_closes_at = 0;
}

static void checkRetry() {
  {
    // The field case: a splice, then a clean reply to the one resend of the same command.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    const uint32_t drops = obd.drop_eval;
    Poll p = poll(obd, link, { SPLICE, GOOD });
    CHECK(hasSoc(p));
    CHECK(p.sent == std::vector<std::string>({ "220105\r", "220105\r" }));
    CHECK(p.tx == 2);
    CHECK(obd.drop_eval == drops);
    CHECK(obd.anomalous_220105_reason == "wrong_hdr" && obd.anomalous_220105_header == "17BA5");
    CHECK(obd.anomalous_220105_reply == SPLICE);   // the capture is the bad reply, not the retry's
    CHECK(obd.anomalous_220105_retry == "ok");
    CHECK(obd.good_220105_reply == GOOD);
    CHECK(obd.first_retry.pid == "220105" && obd.first_retry.reason == "wrong_hdr" &&
          obd.first_retry.header == "17BA5" && obd.first_retry.ok);
    // Per-poll state is cleared.
    Poll next = poll(obd, link, { GOOD });
    CHECK(hasSoc(next) && next.sent.size() == 1);
    CHECK(obd.first_retry.pid.empty() && obd.anomalous_220105_reason.empty() && obd.anomalous_220105_retry == "no");
  }
  {
    // Our ID with another DID, and a lost consecutive frame, are refused and retried too.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    std::string did = GOOD;
    did.replace(did.find("620105"), 6, "620101");
    std::string gap = GOOD;
    gap.erase(gap.find("\r7EC23"), 20);
    for (const std::string& bad : { did, gap }) {
      Poll p = poll(obd, link, { bad, GOOD });
      CHECK(p.sent.size() == 2 && hasSoc(p));
    }
  }
  {
    // A negative response is the car's answer: one send, no retry.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    Poll p = poll(obd, link, { NRC, GOOD });
    CHECK(p.sent.size() == 1 && !hasSoc(p));
    CHECK(obd.anomalous_220105_reason == "nrc" && obd.anomalous_220105_retry == "no");
    CHECK(obd.first_retry.pid.empty());
    Poll error = poll(obd, link, { "CAN ERROR" + PROMPT, GOOD });
    CHECK(error.sent.size() == 1 && obd.anomalous_220105_reason == "no_frame");
  }
  {
    // Without the prompt the first reply may still be arriving: no resend into it.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    Poll p = poll(obd, link, { SPLICE.substr(0, SPLICE.size() - 1), GOOD });
    CHECK(p.sent.size() == 1 && !hasSoc(p));
  }
  {
    // The alarm gate closing between the first send and the retry stops the retry and everything after it.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    guard_closes_at = 2;
    Poll p = poll(obd, link, { SPLICE, GOOD });
    CHECK(p.sent.size() == 1 && p.tx == 1 && !hasSoc(p));
    CHECK(link.commands.empty());
  }
  {
    // One retry per command; a command whose retry failed is not retried until it decodes again.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link);
    const uint32_t drops = obd.drop_eval;
    std::string other = GOOD;
    other.replace(other.find("620105"), 6, "620101");
    Poll failed = poll(obd, link, { SPLICE, other, GOOD });
    CHECK(obd.anomalous_220105_reason == "wrong_hdr" && obd.anomalous_220105_reply == SPLICE);
    CHECK(failed.sent.size() == 2 && !hasSoc(failed));
    CHECK(obd.drop_eval > drops);
    CHECK(obd.anomalous_220105_retry == "fail" && obd.first_retry.pid == "220105" && !obd.first_retry.ok);
    Poll held = poll(obd, link, { SPLICE, GOOD });
    CHECK(held.sent.size() == 1 && !hasSoc(held));
    CHECK(obd.anomalous_220105_retry == "no" && obd.first_retry.pid.empty());
    CHECK(hasSoc(poll(obd, link, { GOOD })));
    Poll rearmed = poll(obd, link, { SPLICE, GOOD });
    CHECK(rearmed.sent.size() == 2 && hasSoc(rearmed));
  }
}

static const char* TWO = R"({
  "obd_protocol": "6",
  "init_commands": { "command": ["ATZ", "ATSP6", "ATSH7E4", "ATCRA7EC"] },
  "data_commands": { "command": ["220101", "220105"] },
  "x": { "command": "220101", "equation": "a" },
  "soc": { "command": "220105", "equation": "a/2" },
  "soh": { "command": "220105", "equation": "b" },
  "speed": { "command": "220105", "equation": "c" },
  "ext_temp": { "command": "220105", "equation": "d" }
})";

static void checkTwo() {
  OBDWrapper obd;
  ScriptedLink link;
  setUp(obd, link, TWO);
  std::string bad101 = GOOD;
  bad101.replace(bad101.find("620105"), 6, "620101");
  for (size_t at; (at = bad101.find("7EC")) != std::string::npos; ) bad101.replace(at, 3, "7EB");
  Poll first = poll(obd, link, { bad101, bad101, SPLICE, GOOD });
  CHECK(first.sent.size() == 4 && hasSoc(first) && obd.first_retry.pid == "220101");
  Poll second = poll(obd, link, { bad101, SPLICE, GOOD });
  CHECK(second.sent.size() == 3 && hasSoc(second));
}

// A profile that throws part-way through being applied (callers catch json::exception and keep polling) leaves
// pids longer than before; polling it must not read past what was derived for the old list.
static void checkPartialProfile() {
  OBDWrapper obd;
  ScriptedLink link;
  setUp(obd, link);
  try {
    obd.setVehicleCommands(json::parse(R"({ "obd_protocol": "6", "data_commands": { "command": ["220101", "220105", 5] },
      "soc": { "command": "220105", "equation": "a/2" } })"));
  } catch (const json::exception&) {
  }
  Poll p = poll(obd, link, { GOOD, GOOD });
  CHECK(hasSoc(p));
}

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "all";
  if (group == "all" || group == "retry") {
    const int before = failures;
    checkRetry();
    checkTwo();
    checkPartialProfile();
    std::printf("%s retry\n", failures == before ? "ok  " : "FAIL");
  }
  return failures ? 1 : 0;
}
