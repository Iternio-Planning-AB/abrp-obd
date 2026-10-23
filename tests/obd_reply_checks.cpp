// Host checks for genericProtocol() on protocol-6 UDS (22xxxx) replies. Fixtures are co-processor
// replies to 220105; the stale bytes in the stray-prefix and foreign-header fixtures are zeroed except
// for their structure, and the battery values are representative, not a real car's.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#define PRINT 0
#include "generic_protocol.inc"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// readBuffer() turns every '\r' into '\n' before the parser sees the reply.
static std::string asRead(std::string reply) {
  std::replace(reply.begin(), reply.end(), '\r', '\n');
  return reply;
}

struct Outcome {
  bool refused = false;  // UnexpectedPidResponse: not our answer
  bool failed = false;   // any other throw: readPIDs counts it as undecodable
  std::string reason, header;
  std::vector<int> bytes;
  int skipped = -1;
};

static Outcome decode(const std::string& reply, const char* requested = "220105", const char* expected = "") {
  Outcome o;
  try {
    o.bytes = genericProtocol(asRead(reply), "6", requested, o.skipped, expected);
  } catch (const UnexpectedPidResponse& e) {
    o.refused = true;
    o.reason = e.reason;
    o.header = e.header;
  } catch (const std::exception&) {
    o.failed = true;
  }
  return o;
}

static const std::string PROMPT = "\r\r>";

// A clean reply: seven 7EC frames, first-frame length 0x2E.
static const std::string GOOD =
  "7EC102E620105FFFB74\r7EC210F012C01012C0B\r7EC220B0B0B0B0B0B6C\r7EC23346C340000500C\r"
  "7EC240003A8B0336900\r7EC2554000000000000\r7EC26000B0B0B0B0000" + PROMPT;
static const std::vector<int> GOOD_BYTES = {
  255, 251, 116, 15, 1, 44, 1, 1, 44, 11, 11, 11, 11, 11, 11, 11, 108, 52, 108, 52, 0, 0, 80, 12,
  0, 3, 168, 176, 51, 105, 0, 84, 0, 0, 0, 0, 0, 0, 0, 11, 11, 11, 11, 0, 0 };

// The clean reply that followed a stray tail on the wire (one module byte differs from GOOD).
static const std::string B_REPLY =
  "7EC102E620105FFFB74\r7EC210F012C01012C0B\r7EC220B0B0B0B0C0B6C\r7EC23346C340000500C\r"
  "7EC240003A8B0336900\r7EC2554000000000000\r7EC26000B0B0B0B0000" + PROMPT;
static const std::vector<int> B_BYTES = {
  255, 251, 116, 15, 1, 44, 1, 1, 44, 11, 11, 11, 11, 11, 12, 11, 108, 52, 108, 52, 0, 0, 80, 12,
  0, 3, 168, 176, 51, 105, 0, 84, 0, 0, 0, 0, 0, 0, 0, 11, 11, 11, 11, 0, 0 };
// 16 hex chars with no header or PCI, arriving ahead of the reply.
static const std::string STRAY = "0000000000000BB8\r";

// The reply re-segmented by the co-processor behind 21 stale bytes, under a 5-digit "header".
static const std::string FOREIGN =
  "1785D1043000000000000\r1785D2100000000000000\r1785D2200000BB8000007\r1785D23EC620105FFFB74\r"
  "1785D240F012C01012C0B\r1785D250B0B0B0B0A0B6C\r1785D26346C3400006408\r1785D270003A8B0378900\r"
  "1785D285A000000000000\r1785D29000A0B0A0B0000" + PROMPT;

static const std::vector<int> MODE01_LEGACY_BYTES = { 65, 13, 50 };

// Issue #45: our answer spliced behind another message (zeroed), every line under one 5-char prefix. From CF 23 on
// the bytes are GOOD's 46-byte reply with its first-frame PCI replaced.
static const std::string SPLICE =
  "17BA51043000000000000\r17BA52100000000000000\r17BA52200000000000000\r17BA523EC620105FFFB74\r"
  "17BA5240F012C01012C0B\r17BA5250B0B0B0B0B0B6C\r17BA526346C340000500C\r17BA5270003A8B0336900\r"
  "17BA52854000000000000\r17BA529000B0B0B0B0000" + PROMPT;
static const std::string NRC = "7EC037F223100000000" + PROMPT;

static std::string withoutPrompt(const std::string& reply) { return reply.substr(0, reply.size() - PROMPT.size()); }
static std::string replaced(std::string reply, const std::string& from, const std::string& to) {
  for (size_t at = reply.find(from); at != std::string::npos; at = reply.find(from, at + to.size()))
    reply.replace(at, from.size(), to);
  return reply;
}

static void checkGood() {
  Outcome o = decode(GOOD);
  CHECK(!o.refused);
  CHECK(o.bytes == GOOD_BYTES);
  CHECK(o.skipped == 0);
  // Spaced output (ATS1) parses as before.
  Outcome spaced = decode("7EC 10 2E 62 01 05 FF FB 74 \r7EC 21 0F 01 2C 01 01 2C 0B \r"
                          "7EC 22 0B 0B 0B 0B 0B 0B 6C " + PROMPT);
  CHECK(!spaced.refused);
  CHECK(spaced.skipped == 0);
  CHECK(spaced.bytes == std::vector<int>(GOOD_BYTES.begin(), GOOD_BYTES.begin() + 17));
}

static void checkStrayPrefix() {
  Outcome o = decode(STRAY + B_REPLY);
  CHECK(!o.refused);
  CHECK(o.bytes == B_BYTES);
  CHECK(o.skipped == 1);
  // A foreign single frame ahead of the reply is skipped too.
  Outcome sf = decode("7EB037F22780000000000\r" + GOOD);
  CHECK(!sf.refused);
  CHECK(sf.bytes == GOOD_BYTES);
  CHECK(sf.skipped == 1);
  // A single-frame reply anchors too.
  const std::string single = "7EC0562010512340000" + PROMPT;
  Outcome one = decode(STRAY + single);
  CHECK(!one.refused && one.skipped == 1);
  CHECK(one.bytes == decode(single).bytes);
  // Spaced output (ATS1) anchors with the spaces ignored.
  Outcome spaced = decode("00 00 00 00 00 00 0B B8 \r" "7EC 10 2E 62 01 05 FF FB 74 \r7EC 21 0F 01 2C 01 01 2C 0B \r"
                          "7EC 22 0B 0B 0B 0B 0B 0B 6C " + PROMPT);
  CHECK(!spaced.refused && spaced.skipped == 1);
  CHECK(spaced.bytes == std::vector<int>(GOOD_BYTES.begin(), GOOD_BYTES.begin() + 17));
  // Lines too short to hold 62<DID> after their PCI are dropped, not read past their end: a 5-char
  // fragment, a 6-char spaced one and an echoed command.
  for (const char* shortLine : { "FFF10\r", "FF F1 0A\r", "220105\r" }) {
    Outcome cut = decode(shortLine + GOOD);
    CHECK(!cut.refused && !cut.failed);
    CHECK(cut.bytes == GOOD_BYTES);
    CHECK(cut.skipped == 1);
  }
  // Blank lines are not counted: a leading CR alone is a clean reply, and CRLF is one stray line.
  Outcome blank = decode("\r" + GOOD);
  CHECK(blank.bytes == GOOD_BYTES && blank.skipped == 0);
  Outcome crlf = decode("0000000000000BB8\r\n" + B_REPLY);
  CHECK(crlf.bytes == B_BYTES && crlf.skipped == 1);
}

static void checkForeignHeader() {
  Outcome o = decode(FOREIGN);
  CHECK(o.refused);
  CHECK(o.bytes.empty());
  CHECK(o.skipped == 0);
}

static void checkWrongDid() {
  std::string other = GOOD;
  other.replace(other.find("620105"), 6, "620101");
  CHECK(decode(other).refused);
  // The requested DID decides, not 220105 specifically.
  Outcome asked = decode(other, "220101");
  CHECK(!asked.refused);
  CHECK(asked.bytes == GOOD_BYTES);
  // A stray line ahead of a wrong-DID reply is still refused.
  CHECK(decode(STRAY + other).refused);
}

static void checkNoAnchor() {
  // Headers off (ATH0) and error replies carry no CAN frame; a bare prompt is undecodable.
  CHECK(decode("102E620105FFFB74\r210F012C01012C0B\r220B0B0B0B0B0B6C" + PROMPT).reason == "no_frame");
  Outcome error = decode("CAN ERROR" + PROMPT);
  CHECK(error.refused && error.reason == "no_frame" && error.header.empty());
  Outcome prompt = decode(">");
  CHECK(prompt.failed && !prompt.refused);
  // Only 22xxxx requests are anchored: a Mode 01 reply keeps the legacy result, stray line and all.
  Outcome mode01 = decode(STRAY + "7E804410D3200000000" + PROMPT, "010D");
  CHECK(!mode01.refused && !mode01.failed);
  CHECK(mode01.skipped == 0);
  CHECK(mode01.bytes == MODE01_LEGACY_BYTES);
}

static void checkTruncated() {
  const std::string cut = "7EC102E620105FFFB74\r7EC210F012C01012C0B\r7EC220B0B0B0B0C0B6C\r7EC23346C3400";
  const std::vector<int> partial(B_BYTES.begin(), B_BYTES.begin() + 21);
  Outcome clean = decode(cut);
  CHECK(!clean.refused);
  CHECK(clean.bytes == partial);
  Outcome stray = decode(STRAY + cut);
  CHECK(!stray.refused);
  CHECK(stray.bytes == partial);
  CHECK(stray.skipped == 1);
  // Known header: a cut reply still decodes, also when the cut falls inside the next line's header.
  CHECK(decode(cut, "220105", "7EC").bytes == partial);
  for (const char* tail : { "\r7E", "\r7EC", "\r7EC2" })
    CHECK(decode(cut + tail, "220105", "7EC").bytes == partial);
}

static void checkHeader() {
  // The field splice: refused under any expectation, and the seen prefix is reported whole.
  Outcome splice = decode(SPLICE, "220105", "7EC");
  CHECK(splice.refused && splice.reason == "wrong_hdr" && splice.header == "17BA5");
  Outcome unknown = decode(SPLICE);
  CHECK(unknown.refused && unknown.reason == "wrong_did" && unknown.header == "17BA5");
  // Under the expected header a clean reply decodes as before.
  Outcome good = decode(GOOD, "220105", "7EC");
  CHECK(!good.refused && good.bytes == GOOD_BYTES && good.skipped == 0);
  // Another ECU's answer to the same DID is not ours: skipped when 7EC is expected.
  const std::string other = "7EB102E620105000000\r7EB2100000000000000\r";
  Outcome skip = decode(other + GOOD, "220105", "7EC");
  CHECK(!skip.refused && skip.bytes == GOOD_BYTES && skip.skipped == 2);
  CHECK(decode(withoutPrompt(other) + PROMPT, "220105", "7EC").reason == "wrong_hdr");
  // Our first frame with someone else's consecutive frames is refused, never decoded.
  const std::string foreignCf = replaced(GOOD, "\r7EC2", "\r7EB2");
  for (const char* expected : { "7EC", "" }) {
    Outcome o = decode(foreignCf, "220105", expected);
    CHECK(o.refused && o.reason == "wrong_hdr" && o.header == "7EB");
  }
  // Frames from another ID inside or after our message are ignored, not decoded.
  const std::string inside = replaced(GOOD, "\r7EC23", "\r7EB037F223100000000\r7EC23");
  const std::string after = withoutPrompt(GOOD) + "\r7E8037F2231AAAAAAAA\r7EC102E620105FFFB74" + PROMPT;
  for (const char* expected : { "7EC", "" }) {
    CHECK(decode(inside, "220105", expected).bytes == GOOD_BYTES);
    CHECK(decode(after, "220105", expected).bytes == GOOD_BYTES);
  }
  // A lost consecutive frame would shift every later byte into the wrong field.
  Outcome gap = decode(replaced(GOOD, "\r7EC23346C340000500C", ""), "220105", "7EC");
  CHECK(gap.refused && gap.reason == "bad_seq" && gap.header == "7EC");
  // Sequence numbers wrap from F to 0 (ISO-TP): a 120-byte reply runs 21..2F, 20, 21.
  std::string longReply = "7EC1078620105000000";
  for (unsigned i = 1; i <= 17; ++i) {
    char line[32];
    snprintf(line, sizeof line, "\r7EC2%X%02X%02X%02X%02X%02X%02X%02X", i & 0xF, i, i, i, i, i, i, i);
    longReply += line;
  }
  Outcome wrap = decode(longReply + PROMPT, "220105", "7EC");
  CHECK(!wrap.refused && !wrap.failed && wrap.bytes.size() == 122);
  // A negative response is an answer, under any header and behind a stray line.
  Outcome nrc = decode(NRC, "220105", "7EC");
  CHECK(nrc.refused && nrc.reason == "nrc" && nrc.header == "7EC");
  CHECK(decode("7BB037F2231AAAAAAAA" + PROMPT).reason == "nrc");
  CHECK(decode(STRAY + NRC, "220105", "7EC").reason == "nrc");
  // Our header, another DID.
  std::string did = GOOD;
  did.replace(did.find("620105"), 6, "620101");
  Outcome wrong = decode(did, "220105", "7EC");
  CHECK(wrong.refused && wrong.reason == "wrong_did" && wrong.header == "7EC");
  CHECK(decode(STRAY + did, "220105", "7EC").header == "7EC");
  // With the ID unknown, only a hex ID anchors a reply.
  CHECK(decode("a,\x1b" "102E620105FFFB74\ra,\x1b" "30000000000000" + PROMPT).reason == "no_frame");
}

static void checkExpected() {
  using V = std::vector<std::string>;
  const V init = { "ATZ", "ATE0", "ATS0", "ATH1", "ATSP6", "ATSH7E4", "ATCM7FF", "ATCF7EC" };
  CHECK(expectedReplyHeaders(init, V{ "220101", "220105" }) == V({ "7EC", "7EC" }));
  CHECK(expectedReplyHeaders({ "at cm 7ff", "ATCF 7ec" }, V{ "220105" }) == V{ "7EC" });
  // No filter, a partial mask, or an argument that is not 3 hex digits: unknown.
  CHECK(expectedReplyHeaders({ "ATZ", "ATSP6", "ATSH7E4" }, V{ "220105" }) == V{ "" });
  for (const char* filter : { "ATCF7EX", "ATCF18DAF1EC", "ATCF" })
    CHECK(expectedReplyHeaders({ "ATSH7E4", "ATCM7FF", filter }, V{ "220105" }) == V{ "" });
  CHECK(expectedReplyHeaders({ "ATSH7E4", "ATCM7F0", "ATCF7E0" }, V{ "220105" }) == V{ "" });
  // A list that switches ECU and back: each command expects its own ECU, the leading one the init filter.
  V list = { "220105", "ATSH7C6", "ATCM7FF", "ATCF7CE", "22B002", "ATSH7E4", "ATCM7FF", "ATCF7EC", "220101" };
  V got = expectedReplyHeaders(init, list);
  CHECK(got.size() == list.size() && got[0] == "7EC" && got[4] == "7CE" && got[8] == "7EC");
  // A list that ends on another ECU: poll 2 starts there, so its leading command is unknown.
  got = expectedReplyHeaders(init, V{ "220105", "ATCM7FF", "ATCF7CE", "22B002" });
  CHECK(got[0] == "" && got[3] == "7CE");
  // ATAR leaves the filter unclear until it is set again; ATZ clears it.
  got = expectedReplyHeaders(init, V{ "ATAR", "220105", "ATCM7FF", "ATCF7EC", "220101" });
  CHECK(got[1] == "" && got[4] == "7EC");
  got = expectedReplyHeaders(init, V{ "220105", "ATZ", "220101", "ATCM7FF", "ATCF7EC" });
  CHECK(got[0] == "7EC" && got[2] == "");
}

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "all";
  struct { const char* name; void (*run)(); } groups[] = {
    { "good", checkGood }, { "stray-prefix", checkStrayPrefix }, { "foreign-header", checkForeignHeader },
    { "wrong-did", checkWrongDid }, { "no-anchor", checkNoAnchor }, { "truncated", checkTruncated },
    { "header", checkHeader }, { "expected", checkExpected },
  };
  for (const auto& g : groups) {
    if (group != "all" && group != g.name) continue;
    const int before = failures;
    g.run();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", g.name);
  }
  return failures ? 1 : 0;
}
