#ifndef OBDWRAPPER_H
#define OBDWRAPPER_H

/*  
  Wrapper for Freematics class COBD
  As little as possible has been adjusted in COBD, but some small changes have been made
  COBD could be cleaned up massively, mosly used for sending commands via CLink* 
*/

#include <string>
#include <vector>
#include <json.hpp>
#include <parser.h>
#include <FreematicsOBD.h>
// NOT included: ../ev-obd-pids-main/headers/CarModels.h. Nothing in this class ever referenced it, but
// the include alone pulled twenty compiled-in car PID definitions into every build. Profiles come from
// the backend over ABD2 now, so the vendored library is gone from the tree with it.

using json = nlohmann::json;
using OBDVector = std::vector<std::pair<std::string, double>>;

/* The reply ID each data command expects, "" when unknown (see obdwrapper.cpp) */
std::vector<std::string> expectedReplyHeaders(const std::vector<std::string>& init,
                                              const std::vector<std::string>& pids);

class OBDWrapper : public COBD {
public:

  /* Mathparser */
  Parser parser;

  /* Initialize contact with freematics ESP32 */
  bool initialize();

  /* Apply string as JSON */
  void initVehicle(std::string vehicleJson);

  /* Transform string to json */
  json readString(const std::string& unparsed) const;

  /* Set variables for init commands and data commands from json */
  void setVehicleCommands(const json& newData);

  /* Return vehicle_commands */
  json getVehicleCommands() const;

  /* Return init_commands */
  std::vector<std::string> get_init() const;

  /* Return PIDs */
  std::vector<std::string> get_pids() const;

  /* Send PIDs over CAN and read buffer */
  void testPIDs();
  void testPIDLoop();

  /* For each PID, send over CAN and evaluate value of buffer, return all values as vector */
  OBDVector readPIDs(bool print = false);

  /* Optional per-PID transmit guard, checked BETWEEN individual reads inside readPIDs. A full frame
     blocks for seconds, so a caller that must stop transmitting immediately (the OBD-alarm gate:
     the ignition-off-to-lock window is seconds) can't wait for the cycle boundary - the guard lets
     it interrupt the frame mid-read instead. Must be fast and side-effect-free; called per PID.
     Unset (default) = no guard, behaviour unchanged. */
  void setPollGuard(bool (*guard)()) { poll_guard = guard; }

  /* Count of transmissions this wrapper has put on the OBD bus (PID reads, init/AT commands, the
     standard-fallback exchange). Exists to make the OBD-alarm gate VERIFIABLE: the gate's whole
     promise is "no bus traffic while the car may be armed", and nothing else measures that
     directly. Log it beside the gate verdict - any increment recorded while the gate says DENY is
     a leak, and the delta size identifies which path leaked. Free when unused. */
  volatile uint32_t tx_count{ 0 };

  /* Cumulative counts of fields readPIDs dropped, for the sketch to put in the flash log.
       drop_eval  - bad/empty equation, undecodable reply, evaluation threw, or non-finite result
       drop_range - outside the profile's [minValue, maxValue]
       drop_trunc - replies that filled the read buffer before the prompt */
  uint32_t drop_eval{ 0 };
  uint32_t drop_range{ 0 };
  uint32_t drop_trunc{ 0 };
  std::string anomalous_220105_reply;
  std::string anomalous_220105_reason;
  std::string anomalous_220105_header;        // the ID the refused reply carried, hex
  std::string anomalous_220105_retry;         // no, ok or fail
  std::string good_220105_reply;
  /* First command this poll that was sent again after its reply was refused (see readPIDs). Hex only, safe to log. */
  struct ReplyRetry { std::string pid, reason, header; bool ok = false; } first_retry;
  /* First 22xxxx command this poll whose reply decoded only after dropping lines ahead of it (see
     genericProtocol), and how many lines. Hex-only by construction, safe to log. */
  std::string skipped_prefix_pid;
  unsigned skipped_prefix_lines{ 0 };

  /* Generate fake values from above */
  OBDVector getFakeReading() const;

  // The per-car init*() declarations are gone with their definitions - see the note at the foot of
  // obdwrapper.cpp. Profiles arrive over ABD2 and are applied with setVehicleCommands() + initialize().

private:
  json vehicle_commands{};
  std::vector<std::string> init_commands{};
  std::vector<std::string> pids{};
  std::map<std::string, json> car_models{};
  std::string raw_220105_reply;
  /* Parallel to pids: the reply ID each command expects, and whether its last retry failed */
  std::vector<std::string> reply_headers{};
  std::vector<bool> retry_held{};
  /* See setPollGuard */
  bool (*poll_guard)(){ nullptr };

  bool readBuffer(std::string pid_string, char* buffer, int bufsize, bool print);

  /* Fill missing universal speed/ext_temp via standard Mode 01 PIDs on the
     functional broadcast address, then restore the profile's ELM state */
  void pollStandardFallback(OBDVector& results, const std::string& protocol);
};

#endif