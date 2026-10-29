#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"

#define ENABLE_BLE 1
#define ENABLE_LOGGING 1
#define ENABLE_NETWORK 1
#define ENABLE_OTA 1
#define ENABLE_MEMS 1
#include "backlog_fs.h"

using json = nlohmann::json;
#include "../libraries/OBDWrapper/obd_vector.h"
// Single-threaded host: the lock is a no-op, so these checks prove what is taken and when, not mutual exclusion.
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct SerialStub {
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
// Everything the forget does to the log, in order: event lines, and markers for the calls around them.
std::vector<std::string> events;
void logEvent(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  events.push_back(line);
}
struct FakeLogger {
  int result = 3; // segments removed, -1 if any stayed
  int clearAll() { events.push_back("<erase>"); return result; }
} datalogger;
struct FakeQueue {} fake_queue;
FakeQueue* bleLogQueue = &fake_queue;
void xQueueReset(FakeQueue*) { events.push_back("<drop ble events>"); }
void flushMotionStats() { events.push_back("<motion flush>"); }

struct FakeObd {
  json commands;
  int inits = 0;
  std::function<void()> during_init;
  void setVehicleCommands(json value) { commands = value; }
  void initialize() {
    ++inits;
    if (during_init) during_init();
  }
} obd;
// The stored hash as NVS holds it. The wrapper's own push handling (clean, HASH) is not modelled.
struct FakeBle {
  unsigned long nvs_hash = 0;
  bool forgotten = false;
  void invalidateHash() { nvs_hash = 0; }
  void commitHash(unsigned long hash, bool persist) { if (persist) nvs_hash = hash; }
  void forgetStoredPush() { nvs_hash = 0; forgotten = true; events.push_back("<forget push>"); }
} ble;

// RAM the extracted code shares with the sketch. reboot() resets it, except the block's function-static
// last_applied_profile; LittleFS and the stored hash survive.
std::string tlm_push_token;
bool g_ota_active = false, g_logdump_active = false, g_obd_needs_init = false, g_no_profile = true,
     config_initialized = false;
bool g_weak_rail_cold_boot = false; // set by setup()'s ATRV gate before the restore runs (#104)
std::vector<OBDVector> obd_result_vector, g_upload_held;
void alarmGrantSetupPermit() {}
bool alarmEnforced() { return false; }
bool frozen_valid = false;
double frozen_lat = 0, frozen_lon = 0;
unsigned long upload_capped = 0;
struct GeoMarks { double lat = 0; } g_geo_queued;
uint64_t obd_fail_logged = 0;
char last_good_220105_reply[513] = {};
void resetGeoAccepted(bool deep_sleep) { if (!deep_sleep) events.push_back("<geo marks reset>"); }
void resetFrozenGpsProvenance(bool deep_sleep) { if (!deep_sleep) events.push_back("<frozen fix reset>"); }
#include "ota_codes.inc"
void logDumpEnd(const char* why) { g_logdump_active = false; events.push_back(std::string("<dump end ") + why + ">"); }
void otaReset(uint8_t phase, uint16_t reason) {
  check(phase == OTA_PH_ABORTED && reason == OTA_ERR_NOAUTH, "a forget aborts the update as not authorized");
  g_ota_active = false;
  events.push_back("<ota abort>");
}
size_t indexOf(const std::string& prefix) {
  for (size_t i = 0; i < events.size(); ++i) if (events[i].rfind(prefix, 0) == 0) return i;
  check(false, "missing " + prefix);
  return 0;
}

#include "backlog_path.inc"
bool uploadBacklogPending() { return LittleFS.exists(UPLOAD_BACKLOG_PATH); }
bool g_backlog_unread = false;
#include "config_callback.inc"
#include "persisted_profile.inc"
#include "forget_profile.inc"

void applyPending() {
#include "config_apply.inc"
}

// What BLEWrapper::completeSetup hands over at SETUP OK.
bool push(const json& profile, const std::string& token, unsigned long hash) {
  return configCallback(json{{"configJson", profile}, {"tlm_push_token", token}, {"hash", hash}});
}

std::string savedToken() {
  return json::parse(LittleFS.files.at("/vehicle_profile.json")->c_str()).at("tlm_push_token").get<std::string>();
}

void reboot() {
  tlm_push_token.clear();
  g_pending_config_str.clear();
  g_config_pending = false;
  obd = {};
  loadPersistedProfile();
}

int main() {
  const json profile = {{"soc", {{"cmd", "2201"}}}, {"utc", 1}};
  check(!configCallback(json{{"configJson", profile}, {"tlm_push_token", "token-a"}}) && !g_config_pending,
        "a push without its hash is refused");

  ble.nvs_hash = 99;
  check(push(profile, "token-a", 11), "a full push is stashed");
  check(ble.nvs_hash == 99, "a stashed push leaves the stored hash alone");
  applyPending();
  check(obd.inits == 1 && tlm_push_token == "token-a", "the first push applies");
  check(ble.nvs_hash == 11 && savedToken() == "token-a", "its hash is committed once its file is written");
  applyPending();
  check(ble.nvs_hash == 11 && savedToken() == "token-a" && !g_config_pending, "an applied push is not applied again");

  push(profile, "token-b", 12);
  applyPending();
  check(obd.inits == 1, "a push with the running profile skips the re-init");
  check(tlm_push_token == "token-b" && ble.nvs_hash == 12, "it still takes the new token and hash");
  reboot();
  check(tlm_push_token == "token-b" && ble.nvs_hash == 12, "the token from a push with an unchanged profile survives a reboot");
  check(obd.commands == json{{"soc", {{"cmd", "2201"}}}}, "the restored profile is the pushed one, without utc");

  // A second push completes on the BLE task while the first is still initializing.
  const json other = {{"soc", {{"cmd", "2202"}}}};
  obd.during_init = [&] { push(profile, "token-x", 14); obd.during_init = nullptr; };
  push(other, "token-c", 13);
  applyPending();
  check(savedToken() == "token-c" && ble.nvs_hash == 13, "the file and hash are the push that was applied");
  check(g_config_pending, "a push that lands during the apply stays pending");
  applyPending();
  check(savedToken() == "token-x" && ble.nvs_hash == 14, "and is applied on the next pass");

  push(profile, "token-d", 15);
  LittleFS.fail_open = true;
  applyPending();
  LittleFS.fail_open = false;
  check(ble.nvs_hash == 0, "a push that cannot be written leaves no hash");
  reboot();
  check(tlm_push_token.empty(), "and the next boot restores no older token");

  push(profile, "token-e", 16);
  applyPending();
  check(LittleFS.exists("/vehicle_profile.json") && ble.nvs_hash == 16, "a later push saves again");
  g_pending_config_str = R"({"configJson":{},"hash":17})";
  g_config_pending = true;
  applyPending();
  check(ble.nvs_hash == 0, "a push that does not apply leaves no hash");
  reboot();
  check(tlm_push_token.empty(), "and the next boot restores neither half of it");

  push(profile, "token-f", 18);
  applyPending();
  obd_result_vector = {{{"utc", 1}, {"soc", 50}}};
  g_upload_held = {{{"utc", 2}, {"soc", 51}}};
  LittleFS.files["/upload_backlog.jsonl"] = std::make_shared<std::string>("{}\n");
  push(profile, "token-g", 19);
  events.clear();
  check(forgetProfile("authclear"), "a forget that erases the log succeeds");
  check(std::find(events.begin(), events.end(), "<ota abort>") == events.end(), "with no update running it aborts nothing");
  check(!LittleFS.exists("/vehicle_profile.json") && ble.forgotten && ble.nvs_hash == 0, "a key reset forgets the stored push");
  check(tlm_push_token.empty() && g_no_profile && obd_result_vector.empty() && g_upload_held.empty(),
        "and stops uploading and polling, held frames included");
  check(!LittleFS.exists("/upload_backlog.jsonl"), "and removes the backlog file");
  check(!g_config_pending && g_pending_config_str.empty(), "and drops a push still waiting for the loop");
  reboot();
  check(tlm_push_token.empty(), "and the next boot restores nothing");

  // A key reset while the revoked session is downloading the log and updating the firmware.
  push(profile, "token-h", 20);
  applyPending();
  obd_result_vector = {{{"utc", 1}, {"soc", 50}}};
  g_upload_held = obd_result_vector;
  g_logdump_active = g_ota_active = frozen_valid = true;
  frozen_lat = 1;
  g_geo_queued.lat = 1;
  upload_capped = 2;
  obd_fail_logged = 1;
  last_good_220105_reply[0] = '6';
  events.clear();
  check(forgetProfile("authreset"), "the reset succeeds");
  const size_t erase = indexOf("<erase>");
  check(indexOf("<dump end") < erase && indexOf("<ota abort>") < erase, "the transfers end before the log is erased");
  check(indexOf("<drop ble events>") < erase && indexOf("<motion flush>") < erase, "and nothing queued from before lands after it");
  check(erase < indexOf("<forget push>"), "the log goes first, so a power cut part-way leaves a reset that can be repeated");
  size_t first = erase + 1;
  while (first < events.size() && events[first][0] == '<') ++first; // call markers, not log lines
  check(first < events.size() && events[first] ==
        "PROFILE forgotten (authreset) - log erased (3 segments), profile and push token cleared, ended dump, ended ota",
        "the reset reason is the first line of the fresh log");
  check(indexOf("UPLOAD discarded n=2 reason=forget") > erase && upload_capped == 0,
        "every queued frame is dropped and counted: in RAM and held for gzip");
  check(obd_fail_logged == 0 && !last_good_220105_reply[0], "the fresh log names OBD failures again, without the old reply");
  check(!frozen_valid && frozen_lat == 0, "the frozen parked fix is dropped too");
  indexOf("<frozen fix reset>");
  indexOf("<geo marks reset>");
  check(g_geo_queued.lat == 0, "and the send-once geo marks start again");

  // An erase that had no segment to remove still succeeds.
  push(profile, "token-i", 21);
  applyPending();
  datalogger.result = 0;
  check(forgetProfile("authclear"), "an empty log erases");

  // A log that cannot all be erased, or a file that stays: the caller keeps the key and can repeat the reset.
  for (const char* stuck : {"", "/vehicle_profile.json", "/upload_backlog.jsonl"}) {
    push(profile, "token-j", 22);
    applyPending();
    obd_result_vector = {{{"utc", 1}, {"soc", 50}}};
    LittleFS.files["/upload_backlog.jsonl"] = std::make_shared<std::string>("{}\n");
    frozen_valid = true;
    push(profile, "token-k", 23); // waiting for the loop
    datalogger.result = *stuck ? 3 : -1;
    LittleFS.fail_remove = stuck;
    events.clear();
    check(!forgetProfile("authreset"), "a forget that leaves the log, the profile or the backlog on flash fails");
    LittleFS.fail_remove.clear();
    datalogger.result = 3;
    check(std::find(events.begin(), events.end(), *stuck ? "PROFILE forget FAILED (authreset) - profile or backlog file not removed"
                                                         : "PROFILE forget FAILED (authreset) - log not erased, nothing cleared") != events.end(),
          "and says why");
    check(tlm_push_token == "token-j" && !obd_result_vector.empty() && frozen_valid,
          "and keeps the token, the queue and the frozen fix");
    if (!*stuck)
      check(LittleFS.exists("/vehicle_profile.json") && LittleFS.exists("/upload_backlog.jsonl") && ble.nvs_hash == 22 &&
            g_config_pending && std::find(events.begin(), events.end(), "<forget push>") == events.end(),
            "and a log that would not erase touches nothing else");
    applyPending();
  }

  // A weak-rail cold boot sends nothing to the bus (#104): the boot restore and a push on that wake both take the
  // profile and its token but leave the init deferred, for loop()'s lazy init, which that wake's poll veto never runs.
  auto logged = [](const std::string& prefix, const std::string& suffix) {
    for (const auto& e : events)
      if (e.rfind(prefix, 0) == 0 && e.size() >= suffix.size() && e.compare(e.size() - suffix.size(), suffix.size(), suffix) == 0)
        return true;
    return false;
  };
  push(profile, "token-l", 24);
  applyPending();
  g_weak_rail_cold_boot = true;
  g_obd_needs_init = false;
  events.clear();
  reboot();
  check(obd.inits == 0 && g_obd_needs_init, "a weak cold boot's restore defers the OBD init");
  check(tlm_push_token == "token-l" && obd.commands == json{{"soc", {{"cmd", "2201"}}}}, "but still takes the profile and token");
  check(logged("PROFILE restored from flash (", " bytes) - OBD init deferred (weak-rail cold boot)"), "and says why");
  const json weak = {{"soc", {{"cmd", "2203"}}}};
  push(weak, "token-m", 25);
  applyPending();
  check(obd.inits == 0 && g_obd_needs_init && obd.commands == weak, "a push on that wake applies with its init deferred");
  check(logged("CONFIG applied from BLE push (", " bytes) - OBD init deferred (weak-rail cold boot)"), "and says so");
  check(savedToken() == "token-m" && ble.nvs_hash == 25, "it is persisted and its hash committed as any push");
  g_weak_rail_cold_boot = false;
  g_obd_needs_init = false;
  reboot();
  check(obd.inits == 1 && !g_obd_needs_init && obd.commands == weak && tlm_push_token == "token-m",
        "the next wake restores that push and initializes it");
  // Off that wake, a changed profile initializes at once and logs the line it always has.
  push(json{{"soc", {{"cmd", "2204"}}}}, "token-m", 26);
  applyPending();
  check(obd.inits == 2 && !g_obd_needs_init, "a push off a weak cold boot initializes the profile");
  check(logged("CONFIG applied from BLE push (", " bytes, on loop task)"), "and logs it as before");
  for (const auto& e : events) check(e.size() <= 119, "a logEvent line is longer than the 119 characters it keeps: " + e);
  std::puts("profile checks passed");
}
