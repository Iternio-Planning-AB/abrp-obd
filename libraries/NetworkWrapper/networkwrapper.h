#ifndef NETWORKWRAPPER_H
#define NETWORKWRAPPER_H

/*  
  Abstract base class for communication over network
*/

#include <string>
#include <vector>
#include <json.hpp>
#include <FreematicsPlus.h>

using json = nlohmann::json;
using HeaderVector = std::vector<std::pair<std::string, std::string>>;
using OBDVector = std::vector<std::pair<std::string, double>>;

struct HttpResponse {
  int responseCode;
  std::string payload;
};

class NetworkWrapper {
public:
  virtual ~NetworkWrapper() = default;

  NetworkWrapper(FreematicsESP32 sys) : sys{ sys } { };

  virtual bool init(const bool retry = false) = 0;

  virtual void set_car(const std::string new_token) {
    carToken = new_token;
  }
  virtual void set_apikey(const std::string new_api_key) {
    api_key = new_api_key;
  }
  virtual void set_url(const std::string new_url) {
    url = new_url;
  }

  virtual std::string getUrl() {
    return url;
  }

  virtual void restart() = 0;

  // Fully power the radio down before deep sleep so it draws ~nothing (WiFi: radio off; cellular: AT+CPOWD
  // modem power-off). Leaving the modem powered was draining the car battery overnight. Default no-op.
  virtual void powerOff() {}

  // Non-blocking attempt to re-establish the link when offline. Default no-op; transports that can cheaply
  // kick a reconnect (WiFi) override it. The loop calls this each transmit interval while disconnected.
  virtual void reconnect() {}

  // The server's "Date" response header from the most recent request (RFC-1123 string), or "" if none.
  // Used as a coarse UTC time source. Default empty; transports that can read it (WiFi) override.
  virtual std::string getDateHeader() { return std::string(); }

  // Do a lightweight request purely to read the server's "Date" header (a coarse UTC time source) WITHOUT
  // sending telemetry - used to acquire a clock at session start so buffered readings get a real utc.
  // Returns the RFC-1123 date string, or "" on failure. Default no-op; WiFi overrides.
  virtual std::string fetchDate() { return std::string(); }

  // One-line signal + modem-voltage snapshot for upload diagnostics (e.g. "csq=20(-73dBm) vbat=4.05"). Used
  // in the field to tell apart the failure modes: low csq -> RF/signal, sagging vbat -> power brownout during
  // TX, both healthy -> session/protocol issue. Default empty; cellular overrides.
  virtual std::string netDiag() { return std::string(); }

  /* How the last connect() went (empty if the transport doesn't say) */
  virtual std::string lastOpenTrace() { return std::string(); }

  /* ms since connected() last became true, or -1 if it isn't / the transport doesn't track it */
  virtual long upForMs() { return -1; }

  virtual void set_headers(const HeaderVector new_headers) = 0;
  virtual bool connect() = 0;
  virtual bool connected() = 0;
  virtual void disconnect() = 0;

  virtual HttpResponse getEpoch() = 0;
  virtual HttpResponse sendData(std::string payload) = 0;

  // Send a pre-built /tlm/bulk body verbatim. Unlike sendData(), the token is already embedded
  // inside the body (per vehicle), so no per-call token mutation is applied. Default: not supported
  // by this transport; overridden where implemented (WiFi).
  virtual HttpResponse sendBulk(std::string payload) { return HttpResponse{ 0, std::string{} }; }

protected:
  FreematicsESP32 sys;
  std::string carToken{};
  std::string api_key{};
  std::string url;
  const int timeout{ 5000 };
};

#endif

