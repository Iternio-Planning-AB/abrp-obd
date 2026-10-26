// The #85 OBDFAIL logging from logger.ino's loop(), extracted by run_obd_reply_checks.py into obd_fail_log.inc, run
// over scripted readPIDs() failure lists: each command logged once with its reply, at most 4 a poll, again after a
// new profile.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

struct CommandFailure { size_t index; std::string pid, reason, reply; };
struct { std::vector<CommandFailure> failures; uint32_t pids_hash = 0; } obd;
uint64_t obd_fail_logged = 0;
uint32_t obd_fail_profile = 0;
std::vector<std::string> lines;

void logEvent(const char* fmt, ...) {
  char text[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  lines.emplace_back(text);
}

void poll() {
  auto logRaw = [&](const char* kind, const char* data, size_t size) {
    logEvent("OBDRAW %s %u", kind, (unsigned)size);
  };
#include "obd_fail_log.inc"
}

int main() {
  for (size_t i = 0; i < 6; ++i) obd.failures.push_back({ i * 2, "22" + std::to_string(1000 + i), "no_data", "NO DATA\n\n>" });
  obd.pids_hash = 7;
  poll();
  CHECK(lines.size() == 8);   // 4 commands, each a line and its reply
  CHECK(lines[0] == "OBDFAIL 221000 reason=no_data len=10" && lines[1] == "OBDRAW fail 10");
  for (const auto& line : lines) CHECK(line.size() <= 119);
  lines.clear();
  poll();
  CHECK(lines.size() == 4 && lines[0] == "OBDFAIL 221004 reason=no_data len=10");   // the two left over
  lines.clear();
  poll();
  CHECK(lines.empty());   // each once
  obd.failures.push_back({ 70, "ATCF17FE0076", "at_reply", "?\n\n>" });
  poll();
  CHECK(lines.empty());   // past the 64 bits: never logged rather than misattributed
  obd.pids_hash = 8;      // a new profile: its indices mean other commands
  poll();
  CHECK(lines.size() == 8 && lines[0] == "OBDFAIL 221000 reason=no_data len=10");
  std::printf("%s fail-log\n", failures ? "FAIL" : "ok  ");
  return failures ? 1 : 0;
}
