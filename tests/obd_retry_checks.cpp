// Host checks for the reply retry in the real OBDWrapper::readPIDs(), compiled from obdwrapper.cpp against
// tests/obd/FreematicsOBD.h. The co-processor is a scripted CLink; replies are the scrubbed 220105 fixtures of
// obd_reply_checks.cpp.
#include <cmath>
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

// ---- commands with no usable answer (#85): what readPIDs keeps for the log ----

static void checkFailures() {
  // MEB's second ECU, the shape #85 needs: a filter the co-processor refuses, then silence, a value out of range and
  // a reply too short for its equation.
  static const char* ECU = R"P({
    "obd_protocol": "7",
    "init_commands": { "command": ["ATZ"] },
    "data_commands": { "command": ["ATCRA17FE0076", "22295A", "222609", "22210E"] },
    "odometer": { "command": "22295A", "equation": "INT24(A:B:C)" },
    "ext_temp": { "command": "222609", "equation": "A/2-50", "minValue": -40, "maxValue": 80 },
    "gear": { "command": "22210E", "equation": "D" }
  })P";
  OBDWrapper obd;
  ScriptedLink link;
  setUp(obd, link, ECU);
  // ATCRA becomes ATCM7FF + ATCF<id>: five commands.
  Poll p = poll(obd, link, { "OK" + PROMPT, "?" + PROMPT, "NO DATA" + PROMPT, "62260900" + PROMPT, "62210E01" + PROMPT });
  CHECK(p.results.empty());
  const std::vector<std::pair<std::string, std::string>> want = {
    { "ATCF17FE0076", "at_reply" }, { "22295A", "no_data" }, { "222609", "range" }, { "22210E", "eval" } };
  CHECK(obd.failures.size() == want.size());
  for (size_t i = 0; i < want.size() && i < obd.failures.size(); ++i) {
    const auto& f = obd.failures[i];
    CHECK(f.pid == want[i].first && f.reason == want[i].second);
    CHECK(f.index < obd.get_pids().size() && obd.get_pids()[f.index] == f.pid);
  }
  if (obd.failures.size() == want.size()) {
    CHECK(obd.failures[1].reply.find("NO DATA") != std::string::npos);
    CHECK(obd.failures[3].reply.find("62210E01") != std::string::npos);
  }
  // Answered next time: nothing kept, and the list is per poll.
  Poll ok = poll(obd, link, { "OK" + PROMPT, "OK" + PROMPT, "62295A001234" + PROMPT, "62260964" + PROMPT, "62210E01020304" + PROMPT });
  CHECK(ok.results.size() == 3 && obd.failures.empty());
  // The hash names the command list: the same list hashes the same, another one differently.
  const uint32_t hash = obd.pids_hash;
  OBDWrapper same;
  ScriptedLink other_link;
  setUp(same, other_link, ECU);
  CHECK(same.pids_hash == hash && hash != 0);
  setUp(same, other_link, PROFILE);
  CHECK(same.pids_hash != hash);
  // Commands already logged (fail_known) are not kept, so the 8 slots go to the ones not seen yet: 10 silent commands,
  // the first 8 known, leave the last two.
  {
    std::string many = R"P({ "obd_protocol": "7", "init_commands": { "command": ["ATZ"] }, "data_commands": { "command": [)P";
    std::string keys;
    for (int i = 0; i < 10; ++i) {
      const std::string cmd = "2201" + std::to_string(10 + i);
      many += (i ? ", \"" : "\"") + cmd + "\"";
      keys += ", \"k" + std::to_string(i) + "\": { \"command\": \"" + cmd + "\", \"equation\": \"A\" }";
    }
    many += "] }" + keys + " }";
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, many.c_str());
    poll(obd, link, {});
    CHECK(obd.failures.size() == 8 && obd.failures[0].index == 0);
    obd.fail_known = 0xFF;
    poll(obd, link, {});
    CHECK(obd.failures.size() == 2 && obd.failures[0].index == 8 && obd.failures[1].index == 9);
  }
  // An AT command that answers with text, not OK, worked: only the co-processor's ? is a refusal.
  {
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, R"P({ "obd_protocol": "7", "init_commands": { "command": ["ATZ"] },
      "data_commands": { "command": ["ATI", "ATRV", "ATSH7E4"] } })P");
    poll(obd, link, { "ELM327 v1.5" + PROMPT, "12.4V" + PROMPT, "OK" + PROMPT });
    CHECK(obd.failures.empty());
  }
  // A refused reply keeps its refusal reason; the reply kept is the last one, the retry's.
  OBDWrapper uds;
  ScriptedLink uds_link;
  setUp(uds, uds_link);
  std::string other = GOOD;
  other.replace(other.find("620105"), 6, "620101");
  poll(uds, uds_link, { SPLICE, other });
  CHECK(uds.failures.size() == 1 && uds.failures[0].pid == "220105" && uds.failures[0].reason == "wrong_did");
  CHECK(uds.failures.size() == 1 && uds.failures[0].reply.size() == 120);
}

// ---- profile evaluation as the app does it (#84): virtual keys, multi-command keys, the MEB soc ----

// The backend's MEB profile: its soc and the inputs soc reads, on one bus for the scripted link.
static const char* MEB = R"P({
  "obd_protocol": "7",
  "init_commands": { "command": ["ATZ"] },
  "data_commands": { "command": ["22028C", "222AB2", "222AB8"] },
  "soc": { "isVirtual": true, "min": -5, "max": 105,
    "equation": "ifgreaterthan(VAL{_cap_safe}, 0, ifgreaterthan(VAL{_soc_raw}, VAL{_soe_safe}/VAL{_cap_safe}+7, VAL{_soc_raw}*0.4425-6.1947, VAL{_soc_raw}), VAL{_soc_raw}*0.4425-6.1947)" },
  "_soc_raw": { "command": "22028C", "equation": "A", "minValue": 0, "maxValue": 255 },
  "soe": { "command": "222AB8", "equation": "INT24(C:D:E)/10", "minValue": -10000, "maxValue": 100000 },
  "capacity": { "command": "222AB2", "equation": "INT16(A:B)*50/1000", "minValue": 1, "maxValue": 200 },
  "_soe_safe": { "command": "22028C,222AB8", "equation": "ifgreaterthan(C+1, 0, INT24(C:D:E)/10*0.163, -1000.0)",
    "minValue": -1001, "maxValue": 20000 },
  "_cap_safe": { "command": "22028C,222AB2", "equation": "ifgreaterthan(INT16(A:B), 19, INT16(A:B)*50/1000, -1)",
    "minValue": -2, "maxValue": 200 }
})P";

// A co-processor reply with headers off: a reference reply below, with the line breaks taken out of it put back.
static std::string reply(const std::string& app) {
  if (app.empty()) return "NO DATA" + PROMPT;
  if (app.find(':') == std::string::npos) return app + PROMPT;
  std::string lines = app.substr(0, 3);
  for (size_t i = 3; i < app.size(); ++i) {
    if (i + 1 < app.size() && app[i + 1] == ':') lines += '\r';
    lines += app[i];
  }
  return lines + PROMPT;
}

static bool value(const OBDVector& results, const std::string& key, double& out) {
  int seen = 0;
  for (const auto& r : results) if (r.first == key) { out = r.second; ++seen; }
  return seen == 1;   // never twice: a later reply replaces the earlier value
}

static bool near(double a, double b) { return a > b - 5e-4 && a < b + 5e-4; }

static void checkProfile() {
  // Reference MEB SoC cases, shared with the app (software 6.0 and older, plus missing-reply fallbacks; the
  // expected values are the app's), each reply in command order: 22028C, 222AB2, 222AB8; "" is no answer.
  struct Case { const char* name; std::string soc, cap, soe; double expected; };
  const std::vector<Case> cases = {
    { "Enyaq SW 6.0", "62028C3F", "622AB206160400", "00B0:622AB80000071:5200000000AAAA", 63 },
    { "ID. Buzz SW 6.0", "62028C2E", "622AB2063F0400", "00B0:622AB80000051:6C00000000AAAA", 46 },
    { "Born facelift SW 6.0", "62028C42", "622AB204A70400", "00B0:622AB80000051:E400000000AAAA", 66 },
    { "older MEB at 90 %", "62028CDA", "622AB205290400", "00B0:622AB80000081:9200000000AAAA", 90.2703 },
    { "older MEB at 91 %", "62028CDB", "622AB206940400", "00B0:622AB800000B1:B200000000AAAA", 90.7128 },
    { "older MEB at low SoC", "62028C28", "622AB206160400", "00B0:622AB80000011:6D28000000AAAA", 11.5053 },
    { "soe missing", "62028CDA", "622AB205290400", "", 90.2703 },
    { "capacity missing", "62028CDA", "", "00B0:622AB80000081:9200000000AAAA", 90.2703 },
    { "soe and capacity missing", "62028C3F", "", "", 21.6828 },
    { "capacity reported as 0", "62028CDA", "622AB200000400", "00B0:622AB80000081:9200000000AAAA", 90.2703 },
    { "capacity below 1 kWh", "62028CDA", "622AB2000A0400", "00B0:622AB80000081:9200000000AAAA", 90.2703 },
  };
  for (const auto& c : cases) {
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, MEB);
    Poll p = poll(obd, link, { reply(c.soc), reply(c.cap), reply(c.soe) });
    double soc = -1;
    const bool ok = value(p.results, "soc", soc) && near(soc, c.expected);
    if (!ok) std::printf("  %s: soc %f, expected %f\n", c.name, soc, c.expected);
    CHECK(ok);
  }
  {
    // A multi-frame reply's bytes run on across frames: soe = INT24(07 52 00) / 10, not the index digit spliced in.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, MEB);
    Poll p = poll(obd, link, { reply("62028C3F"), reply("622AB206160400"), reply("00B0:622AB80000071:5200000000AAAA") });
    double soe = 0, raw = 0, safe = 0, cap = 0;
    CHECK(value(p.results, "soe", soe) && near(soe, 47974.4));
    CHECK(value(p.results, "_soc_raw", raw) && raw == 0x3F);
    CHECK(value(p.results, "_soe_safe", safe) && near(safe, 47974.4 * 0.163));
    CHECK(value(p.results, "_cap_safe", cap) && near(cap, 77.9));
  }
  {
    // Only the shorter reply came: the missing bytes take ifgreaterthan's else branch, as the app's undefined does.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, MEB);
    Poll p = poll(obd, link, { reply("62028C3F"), reply(""), reply("") });
    double safe = 0, cap = 0;
    CHECK(value(p.results, "_soe_safe", safe) && safe == -1000);
    CHECK(value(p.results, "_cap_safe", cap) && cap == -1);
  }
  {
    // A silent car gets no virtual keys, even one with no VAL{} input (bolt's hvac_power): no drop is counted and
    // nothing makes the poll look answered.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, R"P({
      "obd_protocol": "7", "init_commands": { "command": ["ATZ"] }, "data_commands": { "command": ["22028C"] },
      "raw": { "command": "22028C", "equation": "A" },
      "constant": { "isVirtual": true, "equation": "2+3" }
    })P");
    const uint32_t drops = obd.drop_eval;
    Poll silent = poll(obd, link, { reply("") });
    CHECK(silent.results.empty() && obd.drop_eval == drops);
    double v = 0;
    Poll answered = poll(obd, link, { reply("62028C3F") });
    CHECK(value(answered.results, "constant", v) && v == 5);
  }
  {
    // No input at all: the virtual key is skipped, not reported as 0, and is not a drop.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, MEB);
    const uint32_t drops = obd.drop_eval;
    Poll p = poll(obd, link, { reply(""), reply(""), reply("") });
    double soc = 0;
    CHECK(!value(p.results, "soc", soc));
    CHECK(obd.drop_eval == drops);
  }
  {
    // Virtual keys read each other in any profile order, are clamped to min/max rather than dropped, and one with
    // a missing input or a non-finite result is left out.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, R"P({
      "obd_protocol": "7", "init_commands": { "command": ["ATZ"] }, "data_commands": { "command": ["22028C"] },
      "raw": { "command": "22028C", "equation": "A" },
      "a_twice": { "isVirtual": true, "equation": "VAL{z_half}*4" },
      "z_half": { "isVirtual": true, "equation": "VAL{raw}/2" },
      "capped": { "isVirtual": true, "equation": "VAL{raw}*10", "min": 0, "max": 100 },
      "negated": { "isVirtual": true, "equation": "-VAL{raw}" },
      "lost": { "isVirtual": true, "equation": "VAL{nowhere}+1" },
      "byte": { "isVirtual": true, "equation": "A+1" }
    })P");
    Poll p = poll(obd, link, { reply("62028C3F") });
    double v = 0;
    CHECK(value(p.results, "z_half", v) && v == 31.5);
    CHECK(value(p.results, "a_twice", v) && v == 126);
    CHECK(value(p.results, "capped", v) && v == 100);
    CHECK(value(p.results, "negated", v) && v == -63);
    CHECK(!value(p.results, "lost", v) && !value(p.results, "byte", v));
  }
  {
    // MEB's is_parked: == is 1 or 0. It parsed to a null tree before, which readPIDs dereferenced.
    OBDWrapper obd;
    ScriptedLink link;
    setUp(obd, link, R"P({
      "obd_protocol": "7", "init_commands": { "command": ["ATZ"] }, "data_commands": { "command": ["22210E"] },
      "is_parked": { "command": "22210E", "equation": "B==8", "minValue": 0, "maxValue": 1 },
      "scaled": { "command": "22210E", "equation": "2*B==16" },
      "differs": { "command": "22210E", "equation": "B!=8" }
    })P");
    double v = -1;
    Poll parked = poll(obd, link, { reply("62210E0008") });
    CHECK(value(parked.results, "is_parked", v) && v == 1);
    CHECK(value(parked.results, "scaled", v) && v == 1);   // == binds as * does: (2*B)==16
    CHECK(!value(parked.results, "differs", v));            // the app has no !=
    Poll driving = poll(obd, link, { reply("62210E0001") });
    CHECK(value(driving.results, "is_parked", v) && v == 0);
  }
  {
    // The parser on its own: function names in any case, VAL{} lookups, NaN through the integer operations.
    Parser parser;
    const ParserValues vals = { { "_x", 2 }, { "y", -3 }, { "_x", 5 } };
    auto eval = [&](const std::string& e, const std::vector<int>& bytes = {}) {
      return parser.parse(e, bytes, &vals)->evaluate();
    };
    CHECK(eval("ifgreaterthan(1, 0, 7, 8)") == 7 && eval("IFGREATERTHAN(0, 1, 7, 8)") == 8);
    CHECK(eval("Int16(A:B)", { 1, 2 }) == 258);
    CHECK(eval("VAL{_x}") == 5);                       // the newest value of a name
    CHECK(eval("val{y}*2") == -6 && eval("-VAL{y}") == 3);
    CHECK(eval("ifgreaterthan(C+1, 0, 1, 2)", { 9 }) == 2);
    CHECK(std::isnan(eval("{C:1}", { 9 })) && std::isnan(eval("C<<1", { 9 })) && std::isnan(eval("signed(C)", { 9 })));
    CHECK(eval("A&&C", { 0 }) == 0 && std::isnan(eval("C&&A", { 1 })) && eval("A||C", { 1 }) == 1);
    // The deciding operand, as JavaScript's: eny1's is_dcfc (ER==0)&&(FJ<252) is then above its maxValue 1, as in the app.
    CHECK(eval("A&&B", { 5, 3 }) == 3 && eval("A||B", { 5, 3 }) == 5 && eval("A||B", { 0, 3 }) == 3);
    CHECK(eval("1<<252") == (1 << 28) && eval("A<<24", { 255 }) == -16777216 && eval("-8>>1") == -4);
    bool threw = false;
    try { eval("VAL{absent}"); } catch (...) { threw = true; }
    CHECK(threw);
    // A function the tree has no node for (max) left a null tree that readPIDs dereferenced: it throws instead.
    threw = false;
    try { parser.parse("max(1,2,3,4)", {}); } catch (...) { threw = true; }
    CHECK(threw);
  }
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
  if (group == "all" || group == "failures") {
    const int before = failures;
    checkFailures();
    std::printf("%s failures\n", failures == before ? "ok  " : "FAIL");
  }
  if (group == "all" || group == "profile") {
    const int before = failures;
    checkProfile();
    std::printf("%s profile\n", failures == before ? "ok  " : "FAIL");
  }
  return failures ? 1 : 0;
}
