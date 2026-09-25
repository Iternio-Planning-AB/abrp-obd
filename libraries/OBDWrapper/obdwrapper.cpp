#include "obdwrapper.h"
#include <fstream>
#include <sstream>
#include <cmath>

#ifndef PRINT
#define PRINT 0
#endif

bool OBDWrapper::initialize() {
  // Approximate: COBD::init() lives in the Freematics library and sends one command per entry (plus
  // its own housekeeping), so an exact figure would mean editing the vendor lib.
  // NOTE ON SEMANTICS: most init entries are LOCAL ELM configuration (ATZ/ATE0/ATSP7/ATSH/ATCRA...)
  // and never put a frame on the CAN bus, so counting them over-counts. They are counted anyway
  // because a pushed profile's init list CAN legitimately contain a real request (misc.h's join
  // probe appends "2291C8" to exactly such a list), and because a nonzero jump here while the gate
  // denies is still a red flag worth seeing. Read tx_count as "co-processor commands issued", a
  // superset of bus frames - see the note at the ALARMGATE log line before chasing a phantom.
  tx_count += (uint32_t)init_commands.size();
  return init(init_commands);
}

void OBDWrapper::initVehicle(std::string vehicleJson) {
  try {
    setVehicleCommands(readString(vehicleJson));
  }
  catch (json::parse_error err) {
    Serial.printf("Not a valid json: %s\n", vehicleJson.c_str());
  }
}

json OBDWrapper::readString(const std::string& unparsed) const {
  return json::parse(unparsed);
}

void OBDWrapper::setVehicleCommands(const json& newData) {
  vehicle_commands = newData;
  pids.clear();
  try {
    auto newPids = vehicle_commands["data_commands"]["command"];
    for (int i{}; i < newPids.size(); ++i) {
      auto s = std::string(newPids[i]);
      size_t pos = s.find("ATCRA");
      if (pos != std::string::npos) {
        s.replace(pos, 5, "ATCF");
        pids.emplace_back("ATCM7FF");
      }
      pids.emplace_back(s);
    }
  }
  catch (json::out_of_range e) {
    Serial.printf("%s at data_commands\n", e.what());
  }

  init_commands.clear();
  try {
    auto newInits = vehicle_commands["init_commands"]["command"];
    for (int i{}; i < newInits.size(); ++i) {
      auto s = std::string(newInits[i]);
      size_t pos = s.find("ATCRA");
      if (pos != std::string::npos) {
        s.replace(pos, 5, "ATCF");
        init_commands.emplace_back("ATCM7FF");
      }
      init_commands.emplace_back(s);
    }
  }
  catch (json::out_of_range e) {
    Serial.printf("%s at init_commands\n", e.what());
  }
}

json OBDWrapper::getVehicleCommands() const {
  return vehicle_commands;
}

std::vector<std::string> OBDWrapper::get_init() const {
  return init_commands;
}

std::vector<std::string> OBDWrapper::get_pids() const {
  return pids;
}

std::vector<int> genericProtocol(std::string data, std::string protocol) {
#if PRINT
  Serial.printf("Buffer in: %s\n", data.c_str());
  Serial.printf("Buffer len: %d\n", data.length());
#endif

  std::vector<int> obdVariables{};
  data.erase(remove_if(data.begin(), data.end(), isspace), data.end());
  data.erase(std::remove(data.begin(), data.end(), ':'), data.end());
  auto npos = std::string::npos;
  auto updatedCommandLen = 6;

  if (protocol != "6") {

    auto commandIndex = -1;
    auto commandIndex62 = data.find("62");
    auto commandIndex41 = data.find("41");

    if (commandIndex62 == npos || ( commandIndex41 != npos && commandIndex41 < commandIndex62 )) {
      commandIndex = commandIndex41;
      updatedCommandLen = 4;
    } else {
      commandIndex = commandIndex62;
    }

    auto payload = data.substr(commandIndex + updatedCommandLen);
    for (int i{ 0 }; i < payload.length() - 2; i += 2) {
      auto ps = payload.substr(i, 2);
      obdVariables.push_back(std::stoi(ps, 0, 16));
    }
    return obdVariables;
  }

  else {
    auto identifier = data.substr(0, 3);
    auto len = stoi(data.substr(3, 2), 0, 16);
    std::string payload{};
    if (len == 0x10) {
      len = stoi(data.substr(5, 2), 0, 16);
      payload = data.substr(7);
    } else {
      payload = data.substr(5);
    }
    for (int i{}; i < payload.length() - 2; ) {
      if (
        payload[i] == identifier[0] &&
        payload[i + 1] == identifier[1] &&
        payload[i + 2] == identifier[2]) {
        i += 3; //skip identifier
        i += 2; //skip frame number
        continue;
      }
      auto ps = payload.substr(i, 2);
      obdVariables.push_back(std::stoi(ps, 0, 16));
      i += 2;
    }
    if (obdVariables.size() < (size_t)(updatedCommandLen / 2)) throw std::runtime_error("short frame");
    return std::vector<int>(obdVariables.begin() + updatedCommandLen / 2, obdVariables.end());
  }
}

bool OBDWrapper::readBuffer(std::string pid_string, char* buffer, int bufsize = 64, bool print = PRINT) {
  if (!link) return false; // null co-processor link: fail the read instead of panicking (see COBD::readPID)
  tx_count++;              // one PID request onto the bus (see tx_count - the alarm gate's audit trail)
  link->send(buffer);
  idleTasks();
  if (print) {
    Serial.printf("\nSENDING::\nPID: %s\nBuffer: %s\n\n", pid_string.c_str(), buffer);
  }

  int ret = link->receive(buffer, bufsize, OBD_TIMEOUT_SHORT); //OBD_TIMEOUT_SHORT);
  // Truncated reply: still decoded (fields past the cut fail the parser's bounds check), but counted.
  if (ret >= bufsize - 1 && !strchr(buffer, '>')) drop_trunc++;
  for (int f = 0; f < strlen(buffer); f++) {
    if (buffer[f] == '\r') {
      buffer[f] = '\n';
    }
  }
  if (print) {
    Serial.printf("RECEIVING:\n%s\n", buffer);
  }
  if (strstr(buffer, "NO DATA")) {
    if (print) {
      Serial.printf("NO DATA\n");
    }
  } else if (strstr(buffer, "UNABLE TO CONNECT")) {
    if (print) {
      Serial.printf("UNABLE TO CONNECT\n");
    }
  } else if (strstr(buffer, "7EC 03 7F 22 12")) {
    if (print) {
      Serial.printf("Invalid PID\n");
    }
  } else {
    return true;
  }
  return false;
}


void OBDWrapper::testPIDs() {
  std::vector<std::string> responses{};
  for (auto pid_string : pids) {
    // auto pid = std::stol(pid_string, 0, 16);
    char buffer[512]{};   // match readPIDs so a capture shows the whole reply
    if (pid_string.size() > sizeof(buffer) - 2) continue;
    snprintf(buffer, sizeof(buffer), "%04s\r", pid_string.c_str());   // same format as readPIDs
    readBuffer(pid_string, buffer, sizeof(buffer), true);
  }
#if PRINT
  Serial.printf("Complete\n");
#endif
}

template< typename T >
std::string int_to_hex(T i, std::string prefix) {
  std::stringstream stream;
  stream << prefix
    << std::setfill('0') << std::setw(2)
    << std::hex << i;
  return stream.str();
}

void OBDWrapper::testPIDLoop() {
  std::vector<std::string> exists{};
  std::vector<std::string> prefixes{ "2220" };
  for (auto prefix : prefixes) {
    for (int i{ 0 }; i < 256; ++i) {
      auto pid_string = int_to_hex(i, prefix);

      Serial.printf("i: %d, HEX: %s\n", i, pid_string.c_str());
      char buffer[128]{};
      sprintf(buffer, "%04s\r", pid_string.c_str());
      if (readBuffer(pid_string, buffer, sizeof(buffer))) {
        exists.push_back(pid_string);
      }
    }
  }
  Serial.printf("Existing PIDS: \n");
  for (auto x : exists) {
    Serial.printf("%s\n", x.c_str());
  }
  delay(5000);
}

// Read a JSON field that may be a number (Scenic profile) or a quoted string (older profiles) as a double.
static double jsonToDouble(const json& j) {
  if (j.is_number()) return j.get<double>();
  if (j.is_string()) return atof(j.get<std::string>().c_str());
  return 0.0;
}

OBDVector OBDWrapper::readPIDs(bool print) {
  std::vector<std::pair<std::string, double>> results{};
  // A profile without a string obd_protocol can't be polled; throwing here would reboot the dongle.
  if (!vehicle_commands.contains("obd_protocol") || !vehicle_commands["obd_protocol"].is_string()) return results;
  std::string protocol = vehicle_commands["obd_protocol"].get<std::string>();

  // Multi-frame UDS replies (e.g. 220101) exceed 256 chars as text. static: off the loop task's
  // stack; safe because readPIDs only runs on the loop task.
  static char buffer[512];

  for (auto pid_string : pids) {
    // Hard stop between reads (see setPollGuard): a revoked guard means "stop transmitting NOW" -
    // return what was read so far rather than finishing the frame. The switch-off-to-lock window
    // on an alarm car is seconds; a full frame blocks for seconds.
    if (poll_guard && !poll_guard()) break;

    // Every field decoded from this command - one reply often carries several (soc, current, voltage...).
    // A command with no fields (e.g. an ATSH in data_commands) is still sent.
    std::vector<std::pair<std::string, std::string>> fields{};
    for (const auto& j : vehicle_commands.items()) {
      const auto& value = j.value();
      if (!value.is_object() || !value.contains("command") || !value.contains("equation")) continue;
      if (value["command"] != pid_string) continue;
      // A profile error, not a reading. get<std::string>() on a non-string would throw, and an
      // exception out of readPIDs reboots the dongle on every wake with the profile persisted.
      // Length-capped too: the parser recurses per term, so a huge equation overflows the loop stack.
      if (!value["equation"].is_string() || value["equation"].get<std::string>().empty() ||
          value["equation"].get<std::string>().size() > 512) {
        drop_eval++;
        continue;
      }
      fields.emplace_back(j.key(), value["equation"].get<std::string>());
    }
#if PRINT
    if (fields.empty()) Serial.printf("No PID found: %s\n", pid_string.c_str());
    for (const auto& f : fields) Serial.printf("Pid: %s, Equation: %s\n", pid_string.c_str(), f.second.c_str());
#endif

    // An over-long profile command would overrun the static buffer into its neighbours.
    if (pid_string.size() > sizeof(buffer) - 2) {
      drop_eval += fields.size();
      continue;
    }
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%04s\r", pid_string.c_str());
    if (!readBuffer(pid_string, buffer, sizeof(buffer), print) || fields.empty()) continue;

    // Decode once for all fields. genericProtocol throws on replies readBuffer lets through ("?",
    // "STOPPED", "CAN ERROR", short frames); uncaught, that is a reboot.
    std::vector<int> variables{};
    try {
      variables = genericProtocol(buffer, protocol);
    }
    catch (...) {
      Serial.printf("failed to decode reply to %s\n", pid_string.c_str());
      drop_eval += fields.size();
      continue;
    }
#if PRINT
    Serial.printf("Variables, length: %d\n", variables.size());
    for (auto var : variables) {
      Serial.printf("%d ", var);
    }
    Serial.printf("\n");
#endif

    for (const auto& f : fields) {
      const std::string& key = f.first;
      const std::string& equation = f.second;
      double res{};
      try {
        res = parser.parse(equation, variables)->evaluate();
      }
      catch (...) {
        // Dropped, never reported as 0: a 0 soc/current would reach the upload and the charge logic.
        Serial.printf("failed to parse evaluation: %s\n", key.c_str());
        drop_eval++;
        continue;
      }
      // NaN passes the range check below, and "nan" is invalid JSON in the BLE frame.
      if (!std::isfinite(res)) {
        Serial.printf("Dropped non-finite %s\n", key.c_str());
        drop_eval++;
        continue;
      }
#if PRINT
      Serial.printf("value: %s, Equation: %s, res: %f\n", key.c_str(), equation.c_str(), res);
#endif
      // Clamp-and-drop: reject a value outside the profile's [minValue, maxValue]. Intermittent bad buffer
      // reads produce garbage (e.g. kwh_charged = -2.9e8) on otherwise-fine frames; rather than parser
      // surgery, just drop the out-of-range value so it never pollutes the log or the ABRP upload.
      bool in_range = true;
      const auto& vc = vehicle_commands.at(key);
      if (vc.contains("minValue") && res < jsonToDouble(vc["minValue"])) in_range = false;
      if (vc.contains("maxValue") && res > jsonToDouble(vc["maxValue"])) in_range = false;
      if (in_range) {
        results.push_back(std::make_pair(key, res));
      } else {
        Serial.printf("Dropped out-of-range %s = %f\n", key.c_str(), res);
        drop_range++;
      }
    }
  }

  pollStandardFallback(results, protocol);

  return results;
}

// ======================================================================
// STANDARD OBD-II FALLBACK (universal speed & ambient air temp)
// ----------------------------------------------------------------------
// Proprietary EV profiles point the ELM327 at a BMS/gateway ECU that
// usually ignores the globally-mandated Mode 01 PIDs (010D speed, 0146
// ambient temp). Whatever the active profile already produced for these
// is kept; we only fill the gaps by briefly talking to the functional
// broadcast address, then restore the profile's ELM state so the next
// poll cycle's proprietary requests still work.
//
// Uses the inherited COBD::readPID(), which natively scales the result
// (010D -> km/h, 0146 -> degC with the -40 offset applied).
// ======================================================================
void OBDWrapper::pollStandardFallback(OBDVector& results, const std::string& protocol) {
  if (!link) return; // null co-processor link: nothing to poll (see COBD::readPID)
  // Only standard 11-bit (ATSP6) and 29-bit (ATSP7) CAN buses have a
  // functional broadcast address; skip bench/"freematics" profiles.
  if (protocol != "6" && protocol != "7") {
    return;
  }

  bool has_speed = false;
  bool has_ext_temp = false;
  

  // Notice the '&' (reference) - this allows us to directly change the contents of the vector
  for (auto& r : results) {
    if (r.first == "speed") {
      has_speed = true;
    } else if (r.first == "vehicle_reported_speed") {
      r.first = "speed"; // Rename it instantly so the API accepts it!
      has_speed = true;
    }

    if (r.first == "ext_temp") {
      has_ext_temp = true;
    } else if (r.first == "ambient_temp") {
      r.first = "ext_temp"; // Rename it instantly so the API accepts it!
      has_ext_temp = true;
    }
  }

  if (has_speed && has_ext_temp) {
    return;
  }

  // TRANSMIT GUARD. readPIDs' per-PID guard uses `break`, which exits the PID loop and lands HERE -
  // so without this check a revoked guard ("stop transmitting NOW") was followed by the entire
  // fallback exchange anyway: ~7 AT commands plus two Mode 01 reads. That defeated the hard stop
  // on exactly the alarm-equipped cars it exists for.
  //
  // Checked ONLY at the top, deliberately. Steps 1-4 below are one atomic sequence: step 1 points
  // the ELM at the functional broadcast address and step 4 restores the profile's header/filter.
  // Bailing out between them would leave the ELM aimed at the broadcast address, breaking every
  // proprietary read on the next cycle. The sequence is short and bounded, so completing it once
  // admitted is the safe trade; the long, unbounded part of a poll is the PID loop, and that is
  // guarded per read.
  if (poll_guard && !poll_guard()) return;

  char buf[128]{};

  // 1. Point at the functional broadcast address.
  if (protocol == "6") {            // ISO 15765-4 CAN 11-bit
    link->sendCommand("ATSH7DF\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  } else {                          // protocol == "7": ISO 15765-4 CAN 29-bit
    link->sendCommand("ATSH18DB33F1\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  }

  // 2. Open the receive path. ATAR restores the automatic receive address,
  //    and the match-anything mask/filter undo the ATCF/ATCM7FF filter that
  //    setVehicleCommands installs in place of ATCRA (otherwise the engine/
  //    gateway reply from 7E8/18DAF1xx is masked out -> NO DATA).
  link->sendCommand("ATAR\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  link->sendCommand("ATCM000\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  link->sendCommand("ATCF000\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  // readPID() parses the "41 " token, so spaces must be on regardless of any
  // ATS0 the profile set; the profile's setting is restored in step 4.
  link->sendCommand("ATS1\r", buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  tx_count += 5;   // the ATSH + ATAR + ATCM + ATCF + ATS1 commands just sent

  // 3. Poll the standard PIDs (Mode 01).
  dataMode = 1;
  int value = 0;
  if (!has_speed) tx_count++;
  if (!has_speed && readPID(PID_SPEED, value)) {            // 010D, km/h
    results.emplace_back("speed", (double)value);
#if PRINT
    Serial.printf("Standard fallback - speed: %d km/h\n", value);
#endif
  }
  if (!has_ext_temp) tx_count++;
  if (!has_ext_temp && readPID(PID_AMBIENT_TEMP, value)) {  // 0146, degC (-40 applied)
    results.emplace_back("ext_temp", (double)value);
#if PRINT
    Serial.printf("Standard fallback - ext_temp: %d C\n", value);
#endif
  }

  // 4. Restore the profile's ELM state. Re-issuing the init commands re-applies
  //    the custom header and ATCF/ATCM filter; ATZ (reset) and ATSP (protocol)
  //    are skipped because they are slow and the bus is already initialised.
  for (const auto& c : init_commands) {
    if (c.find("ATZ") != std::string::npos || c.find("SP") != std::string::npos) {
      continue;
    }
    std::string cmd = c + "\r";
    tx_count++;
    link->sendCommand(cmd.c_str(), buf, sizeof(buf), OBD_TIMEOUT_SHORT);
  }
}

OBDVector OBDWrapper::getFakeReading() const {
  std::vector<std::pair<std::string, double>> results{};
  int i = 10;
  for (auto pid_string : pids) {
    std::pair<std::string, double> result_pair = std::make_pair(pid_string, i);
    results.push_back(result_pair);
    i += 10;
  }
  return results;
}
 /*
  The compiled-in vehicle profiles used to live here: initZoe, initZoe2, initZoeTest, initScenic,
  initEmulator, initFreematic, initIoniq, initPSA, initMEB, initPolestar, initPolestarTest, initTest
  and inithkmc2019 - about a thousand lines of PID JSON for a dozen specific cars, each a raw
  R"delim(...)" blob handed straight to setVehicleCommands().

  All removed. Profiles are the BACKEND's to define and the app's to push (ABD2, signature-verified -
  see profile_sig.h): they are per-typecode, they get corrected without a firmware release, and a
  profile for the wrong car does not merely return nothing - init_commands is arbitrary CAN, so it
  puts wrong frames on a stranger's bus. Keeping a dozen of them in the image bought nothing that the
  push does not, and the one that was actually wired up shipped a developer's telemetry token beside
  it (see config.h).

  setVehicleCommands() and initialize() are untouched; they are how a pushed profile is applied.
*/
