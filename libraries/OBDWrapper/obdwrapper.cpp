#include "obdwrapper.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <stdexcept>

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
  reply_headers = expectedReplyHeaders(init_commands, pids);
  retry_held.assign(pids.size(), false);
  pids_hash = 2166136261u;
  for (const auto& p : pids) {
    for (char c : p + '\n') { pids_hash ^= (uint8_t)c; pids_hash *= 16777619u; }
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

// A reply that is not our answer. reason: wrong_did (another DID), wrong_hdr (another ID), bad_seq (a frame missing
// from our message), nrc (the ECU's negative response), no_frame (no CAN frame at all). header: the ID seen, hex.
struct UnexpectedPidResponse : std::runtime_error {
  std::string reason, header;
  explicit UnexpectedPidResponse(std::string why, std::string seen = "")
    : std::runtime_error("unexpected PID response"), reason(std::move(why)), header(std::move(seen)) {}
};

static bool isHex(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return isxdigit((unsigned char)c); });
}

// A UDS ReadDataByIdentifier, 22 + four hex digits.
static bool isUdsRead(const std::string& requested) {
  return requested.size() == 6 && requested.compare(0, 2, "22") == 0 && isHex(requested.substr(2));
}

static std::string withoutSpaces(const std::string& line) {
  std::string s;
  for (char c : line) if (c != ' ') s += c;
  return s;
}

// The ID a reply line carries, "" when it is not a CAN frame: the first token with spaces on (ATS1); without, the
// chars ahead of 8 padded data bytes, or 3 ahead of a PCI that fits the line.
static std::string frameHeader(const std::string& line) {
  const size_t space = line.find(' ');
  if (space != std::string::npos) {
    const std::string token = line.substr(0, space);
    const bool more = line.find_first_not_of(' ', space) != std::string::npos;
    return more && token.size() >= 3 && token.size() <= 8 && isHex(token) ? token : "";
  }
  if (!isHex(line)) return "";
  const size_t n = line.size();
  if (n >= 19 && n <= 24) return line.substr(0, n - 16);
  if (n >= 5 && line[3] == '0') {
    const size_t len = std::stoul(line.substr(4, 1), nullptr, 16);
    if (len >= 1 && len <= 7 && 5 + 2 * len <= n) return line.substr(0, 3);
  }
  if ((n >= 7 && line[3] == '1') || (n >= 5 && line[3] == '2')) return line.substr(0, 3);
  return "";
}

static void appendBytes(std::vector<int>& bytes, const std::string& s, size_t from) {
  for (size_t i = from; i + 1 < s.size(); i += 2) {
    const std::string pair = s.substr(i, 2);
    if (!isHex(pair)) throw std::runtime_error("bad byte");
    bytes.push_back(std::stoi(pair, nullptr, 16));
  }
}

// Protocol 6, 22xxxx. The co-processor sometimes puts a stray tail ahead of a clean reply (#10), or splices our
// answer behind another ECU's message under a foreign ID (#45). The reply is the first single or first frame
// (HHH 0N / HHH 1N NN, spaces ignored) under `expected` answering 62<DID>, any 3-char ID when expected is unknown;
// lines ahead of it are dropped and counted in skipped_lines. Its consecutive frames are taken in sequence under
// the same ID up to the first frame's length; other IDs are never decoded, and only refuse the reply when ours is
// incomplete. A reply cut short decodes its whole bytes. Headers off (ATH0) and 29-bit IDs are not handled.
static std::vector<int> udsReply(const std::string& data, const std::string& requested, const std::string& expected,
                                 int& skipped_lines) {
  const std::string did = "62" + requested.substr(2);
  std::vector<std::string> lines;
  for (size_t at = 0; at <= data.size(); ) {
    size_t eol = data.find_first_of("\r\n", at);
    if (eol == std::string::npos) eol = data.size();
    const std::string line = data.substr(at, eol - at);
    if (line.find_first_not_of(' ') != std::string::npos) lines.push_back(line);
    at = eol + 1;
  }

  size_t anchor = 0;
  std::string id;
  for (; anchor < lines.size(); ++anchor) {
    const std::string s = withoutSpaces(lines[anchor]);
    const std::string h = expected.empty() ? s.substr(0, 3) : expected;
    if (s.size() < h.size() + 2 || s.compare(0, h.size(), h) != 0 || !isHex(h)) continue;
    const char pci = s[h.size()];
    const size_t payload = h.size() + (pci == '0' ? 2 : 4);
    if ((pci == '0' || pci == '1') && s.size() >= payload + did.size() && s.compare(payload, did.size(), did) == 0) {
      id = h;
      break;
    }
  }
  if (anchor == lines.size()) {
    std::string first;
    bool any = false;
    for (const auto& line : lines) {
      if (line == ">") continue;
      any = true;
      const std::string h = frameHeader(line);
      if (h.empty()) continue;
      const std::string s = withoutSpaces(line);
      if (s.size() >= h.size() + 6 && s[h.size()] == '0' && s.compare(h.size() + 2, 4, "7F22") == 0)
        throw UnexpectedPidResponse("nrc", h);
      if (first.empty()) first = h;
    }
    if (!any) throw std::runtime_error("empty reply");
    if (first.empty()) throw UnexpectedPidResponse("no_frame");
    throw UnexpectedPidResponse(!expected.empty() && first != expected ? "wrong_hdr" : "wrong_did", first);
  }
  skipped_lines = (int)anchor;

  std::vector<int> bytes;
  const std::string s = withoutSpaces(lines[anchor]);
  const size_t h = id.size();
  if (s[h] == '0') {
    appendBytes(bytes, s, h + 2);
  } else {
    const size_t length = std::stoul(s.substr(h + 1, 3), nullptr, 16);
    appendBytes(bytes, s, h + 4);
    unsigned seq = 1;
    std::string foreign;
    for (size_t i = anchor + 1; i < lines.size() && bytes.size() < length; ++i) {
      const std::string l = withoutSpaces(lines[i]);
      if (l.size() < h + 2 && (id + "2").compare(0, l.size(), l) == 0) continue;   // cut inside our ID
      if (l.compare(0, h, id) != 0) {
        const std::string other = frameHeader(lines[i]);
        if (foreign.empty()) foreign = other;
        continue;
      }
      if (l[h] != '2' || !isHex(l.substr(h + 1, 1)) || std::stoul(l.substr(h + 1, 1), nullptr, 16) != seq)
        throw UnexpectedPidResponse("bad_seq", id);
      appendBytes(bytes, l, h + 2);
      seq = (seq + 1) & 0xF;
    }
    if (bytes.size() < length && !foreign.empty()) throw UnexpectedPidResponse("wrong_hdr", foreign);
  }
  return std::vector<int>(bytes.begin() + did.size() / 2, bytes.end());
}

// The reply ID each data command's profile filter admits, "" when unknown: ATCF<id> under ATCM7FF, the pair
// setVehicleCommands puts in place of ATCRA<id>. Taken from the profile, not the co-processor: the join probe and
// the standard fallback open the filter, and the intended ID still passes. From poll 2 on, a command ahead of the
// list's first filter change runs under whatever the list ended with, so a part that differs from init is unknown.
// ATZ clears the filter; ATAR, or an argument other than 3 hex digits, leaves that part unknown.
std::vector<std::string> expectedReplyHeaders(const std::vector<std::string>& init,
                                              const std::vector<std::string>& pids) {
  struct Filter { std::string id, mask; };
  auto apply = [](Filter& f, const std::string& command) {
    std::string c;
    for (char ch : command) if (ch != ' ') c += (char)toupper((unsigned char)ch);
    const std::string arg = c.size() == 7 && isHex(c.substr(4)) ? c.substr(4) : "?";
    if (c == "ATZ") f = Filter{};
    else if (c == "ATAR") f.id = f.mask = "?";
    else if (c.compare(0, 4, "ATCF") == 0) f.id = arg;
    else if (c.compare(0, 4, "ATCM") == 0) f.mask = arg;
  };
  Filter start;
  for (const auto& c : init) apply(start, c);
  Filter end = start;
  for (const auto& c : pids) apply(end, c);
  Filter f{ end.id == start.id ? start.id : "?", end.mask == start.mask ? start.mask : "?" };
  std::vector<std::string> headers;
  for (const auto& c : pids) {
    apply(f, c);
    headers.push_back(f.mask == "7FF" && f.id.size() == 3 ? f.id : "");
  }
  return headers;
}

std::vector<int> genericProtocol(std::string data, std::string protocol, const std::string& requested,
                                 int& skipped_lines, const std::string& expected = "") {
#if PRINT
  Serial.printf("Buffer in: %s\n", data.c_str());
  Serial.printf("Buffer len: %d\n", data.length());
#endif
  skipped_lines = 0;
  if (protocol == "6" && isUdsRead(requested)) return udsReply(data, requested, expected, skipped_lines);

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
  if (pid_string == "220105") raw_220105_reply.assign(buffer, strnlen(buffer, bufsize));
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
  anomalous_220105_reply.clear();
  anomalous_220105_reason.clear();
  anomalous_220105_header.clear();
  anomalous_220105_retry = "no";
  first_retry = ReplyRetry();
  good_220105_reply.clear();
  raw_220105_reply.clear();
  skipped_prefix_pid.clear();
  skipped_prefix_lines = 0;
  failures.clear();
  // A profile without a string obd_protocol can't be polled; throwing here would reboot the dongle.
  if (!vehicle_commands.contains("obd_protocol") || !vehicle_commands["obd_protocol"].is_string()) return results;
  std::string protocol = vehicle_commands["obd_protocol"].get<std::string>();

  // Multi-frame UDS replies (e.g. 220101) exceed 256 chars as text. static: off the loop task's
  // stack; safe because readPIDs only runs on the loop task.
  static char buffer[512];
  // A profile that threw part-way through setVehicleCommands (its callers keep polling) leaves these behind pids.
  if (reply_headers.size() != pids.size()) {
    reply_headers.assign(pids.size(), "");
    retry_held.assign(pids.size(), false);
  }

  for (size_t n = 0; n < pids.size(); ++n) {
    const std::string& pid_string = pids[n];
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
    // The command, never the last reply, is what goes back out on a retry.
    auto send = [&]() {
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%04s\r", pid_string.c_str());
      return readBuffer(pid_string, buffer, sizeof(buffer), print);
    };
    // A command that got no usable answer is kept for the log (#85), with the reply as it came.
    auto failed = [&](const char* reason) {
      if (failures.size() < 8) failures.push_back({ n, pid_string, reason, std::string(buffer, strnlen(buffer, 120)) });
    };
    if (!send()) {
      failed(strstr(buffer, "NO DATA") ? "no_data" : strstr(buffer, "UNABLE TO CONNECT") ? "no_connect"
             : strstr(buffer, "7EC 03 7F 22 12") ? "nrc" : "no_link");
      continue;
    }
    if (fields.empty()) {
      // A filter or header command the co-processor did not take ("?") leaves every command after it unanswered.
      if (pid_string.compare(0, 2, "AT") == 0 && !strstr(buffer, "OK")) failed("at_reply");
      continue;
    }

    // Decode once for all fields. genericProtocol throws on replies readBuffer lets through ("?",
    // "STOPPED", "CAN ERROR", short frames); uncaught, that is a reboot.
    std::vector<int> variables{};
    int skipped = 0;
    std::string refused;   // why the reply that ended this command was not decoded; empty = decoded
    bool retried = false, logged = false, stop = false;
    for (;;) {
      std::string header;
      try {
        variables = genericProtocol(buffer, protocol, pid_string, skipped, reply_headers[n]);
        refused.clear();
      }
      catch (const UnexpectedPidResponse& e) {
        refused = e.reason;
        header = e.header;
        if (pid_string == "220105" && !retried) {
          anomalous_220105_reason = e.reason;
          anomalous_220105_header = e.header;
          anomalous_220105_reply = raw_220105_reply;
        }
      }
      catch (...) {
        refused = "undecodable";
      }
      // Not our answer (#45): sent once more, unless the reply was the car's own (nrc, no_frame), may still be
      // arriving (no prompt), or this command's last retry failed too. At most one extra request per command.
      if (retried || retry_held[n] || !strchr(buffer, '>') ||
          (refused != "wrong_hdr" && refused != "wrong_did" && refused != "bad_seq"))
        break;
      if (poll_guard && !poll_guard()) { stop = true; break; }
      if (first_retry.pid.empty()) {
        first_retry.pid = pid_string;
        first_retry.reason = refused;
        first_retry.header = header;
        logged = true;
      }
      retried = true;
      if (!send()) { refused = "no_reply"; break; }
    }
    if (retried) {
      retry_held[n] = !refused.empty();
      if (logged) first_retry.ok = refused.empty();
      if (pid_string == "220105") anomalous_220105_retry = refused.empty() ? "ok" : "fail";
    } else if (refused.empty()) {
      retry_held[n] = false;
    }
    if (!refused.empty()) {
      failed(refused.c_str());
      Serial.printf("failed to decode reply to %s\n", pid_string.c_str());
      drop_eval += fields.size();
      if (stop) break;
      continue;
    }
    if (skipped > 0 && skipped_prefix_pid.empty()) {
      skipped_prefix_pid = pid_string;
      skipped_prefix_lines = (unsigned)skipped;
    }
#if PRINT
    Serial.printf("Variables, length: %d\n", variables.size());
    for (auto var : variables) {
      Serial.printf("%d ", var);
    }
    Serial.printf("\n");
#endif

    bool low_soc = false, soh_bad = false, have_soc = false, have_soh = false;
    size_t decoded = 0, out_of_range = 0;   // this command's fields, for the OBDFAIL reason
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
        if (key == "soh") soh_bad = true;
        drop_eval++;
        continue;
      }
      // NaN passes the range check below, and "nan" is invalid JSON in the BLE frame.
      if (!std::isfinite(res)) {
        Serial.printf("Dropped non-finite %s\n", key.c_str());
        if (key == "soh") soh_bad = true;
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
        decoded++;
        results.push_back(std::make_pair(key, res));
        if (key == "soc") { have_soc = true; low_soc = res == 0.5; }
        else if (key == "soh") have_soh = true;
      } else {
        Serial.printf("Dropped out-of-range %s = %f\n", key.c_str(), res);
        if (key == "soh") soh_bad = true;
        drop_range++;
        out_of_range++;
      }
    }
    if (!decoded) failed(out_of_range ? "range" : "eval");
    if (pid_string == "220105") {
      if (low_soc && soh_bad) {
        anomalous_220105_reason = "soc0.5_soh_drop";
        anomalous_220105_header.clear();
        anomalous_220105_reply = raw_220105_reply;
      } else if (have_soc && have_soh && !skipped) {  // the baseline must not carry the stray tail
        good_220105_reply = raw_220105_reply;
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
