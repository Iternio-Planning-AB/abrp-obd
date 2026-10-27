#pragma once

#include <string>
#include "../nlohmann/json.hpp"

/*
  What an app write is, without what it says: length plus top-level keys. ABD2 carries the AUTH secret and
  tlm_push_token - the vehicle's cloud upload credential - and the flash log can be read over USB with no
  auth, so a write's values never reach the log. This runs before the auth gate, so keys are client data
  too: only short identifier-shaped ones (every protocol key and PID name) are named, the rest only counted,
  which also keeps a token-as-key or a newline out of the CSV.
*/
inline bool bleLoggableKey(const std::string& key) {
  if (key.empty() || key.size() > 24) return false;
  for (const char c : key)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

inline const char* bleKeyForLog(const std::string& key) { return bleLoggableKey(key) ? key.c_str() : "?"; }

// The app's relay ack, {"relay_ack":n} (#90): one arrives per app post, so the rx log leaves it out and the loop
// task logs its transitions instead (Helpers/ble_relay.h).
inline bool bleRelayAckWrite(const std::string& input) {
  const auto j = nlohmann::json::parse(input, nullptr, false);
  return !j.is_discarded() && j.is_object() && j.size() == 1 && j.contains("relay_ack");
}

inline std::string bleWriteSummary(const std::string& input) {
  const auto j = nlohmann::json::parse(input, nullptr, false);
  std::string out = "len=" + std::to_string(input.size());
  if (j.is_discarded() || !j.is_object()) return out + " non-object";
  std::string keys;
  int other = 0;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!bleLoggableKey(it.key())) { other++; continue; }
    keys += (keys.empty() ? "" : ",") + it.key();
  }
  out += " keys=" + keys;
  if (other) out += " +" + std::to_string(other) + " other";
  return out;
}
