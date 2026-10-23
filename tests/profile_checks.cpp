#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"

#define ENABLE_BLE 1
#define ENABLE_LOGGING 1
#include "backlog_fs.h"

using json = nlohmann::json;
using OBDVector = std::vector<std::pair<std::string, double>>;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct SerialStub {
  template<typename... Args> void printf(const char*, Args...) {}
} Serial;
template<typename... Args> void logEvent(const char*, Args...) {}

struct FakeObd {
  json commands;
  int inits = 0;
  void setVehicleCommands(json value) { commands = value; }
  void initialize() { ++inits; }
} obd;
struct FakeBle {
  bool hash_valid = true;
  void invalidateHash() { hash_valid = false; }
} ble;

// RAM the extracted code shares with the sketch. reboot() resets it, except the block's function-static
// last_applied_profile; LittleFS and the hash survive.
std::string tlm_push_token, g_pending_config_str;
volatile bool g_config_pending = false;
bool g_ota_active = false, g_logdump_active = false, g_obd_needs_init = false, g_no_profile = true,
     config_initialized = false;
std::vector<OBDVector> obd_result_vector; // PR #63's token line in the extracted block discards this queue
void alarmGrantSetupPermit() {}
bool alarmEnforced() { return false; }

#include "persisted_profile.inc"

void applyPending() {
#include "config_apply.inc"
}

// The Bluedroid side of a completed full push (BLEWrapper::completeSetup): commit the hash, stash and flag.
void push(const json& profile, const std::string& token) {
  ble.hash_valid = true;
  g_pending_config_str = json{{"configJson", profile}, {"tlm_push_token", token}}.dump();
  g_config_pending = true;
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
  push(profile, "token-a");
  applyPending();
  check(obd.inits == 1 && tlm_push_token == "token-a", "the first push applies");
  LittleFS.files.clear();
  applyPending();
  check(!LittleFS.exists("/vehicle_profile.json"), "an applied push is not applied again");

  push(profile, "token-b");
  applyPending();
  check(obd.inits == 1, "a push with the running profile skips the re-init");
  check(tlm_push_token == "token-b", "it still takes the new token");
  reboot();
  check(tlm_push_token == "token-b", "the token from a push with an unchanged profile survives a reboot");
  check(obd.commands == json{{"soc", {{"cmd", "2201"}}}}, "the restored profile is the pushed one, without utc");
  check(ble.hash_valid, "a persisted push keeps the hash");

  push(profile, "token-c");
  LittleFS.fail_open = true;
  applyPending();
  LittleFS.fail_open = false;
  check(!ble.hash_valid, "a push that cannot be written invalidates the hash");
  reboot();
  check(tlm_push_token.empty(), "and the next boot restores no older token");

  push(profile, "token-d");
  applyPending();
  check(LittleFS.exists("/vehicle_profile.json"), "a later push saves again");
  g_pending_config_str = R"({"configJson":{}})";
  g_config_pending = true;
  applyPending();
  check(!ble.hash_valid, "a push that does not apply invalidates the hash");
  reboot();
  check(tlm_push_token.empty(), "and the next boot restores neither half of it");
  std::puts("profile checks passed");
}
