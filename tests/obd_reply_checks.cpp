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
  bool refused = false;  // UnexpectedPidResponse: the wrong_did path
  bool failed = false;   // any other throw: readPIDs counts it as undecodable
  std::vector<int> bytes;
  int skipped = -1;
};

static Outcome decode(const std::string& reply, const char* requested = "220105") {
  Outcome o;
  try {
    o.bytes = genericProtocol(asRead(reply), "6", requested, o.skipped);
  } catch (const UnexpectedPidResponse&) {
    o.refused = true;
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
  // Headers off (ATH0): never parsed on protocol 6, still refused the same way.
  CHECK(decode("102E620105FFFB74\r210F012C01012C0B\r220B0B0B0B0B0B6C" + PROMPT).refused);
  // Error and prompt-only replies fail as before, in the same way (legacy reads "CAN ERROR" as a
  // wrong DID, a bare prompt as undecodable).
  Outcome error = decode("CAN ERROR" + PROMPT);
  CHECK(error.refused);
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
  const std::vector<int> partial(B_BYTES.begin(), B_BYTES.begin() + 20);
  Outcome clean = decode(cut);
  CHECK(!clean.refused);
  CHECK(clean.bytes == partial);
  Outcome stray = decode(STRAY + cut);
  CHECK(!stray.refused);
  CHECK(stray.bytes == partial);
  CHECK(stray.skipped == 1);
}

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "all";
  struct { const char* name; void (*run)(); } groups[] = {
    { "good", checkGood }, { "stray-prefix", checkStrayPrefix }, { "foreign-header", checkForeignHeader },
    { "wrong-did", checkWrongDid }, { "no-anchor", checkNoAnchor }, { "truncated", checkTruncated },
  };
  for (const auto& g : groups) {
    if (group != "all" && group != g.name) continue;
    const int before = failures;
    g.run();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", g.name);
  }
  return failures ? 1 : 0;
}
