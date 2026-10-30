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
#include "../OBDWrapper/obd_vector.h"

struct HttpResponse {
  int responseCode;
  std::string payload;
};

class NetworkWrapper {
public:
  virtual ~NetworkWrapper() = default;

  NetworkWrapper(FreematicsESP32 sys) : sys{ sys } { };

  virtual bool init(const bool retry = false) = 0;

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
  // The csq the last netDiag() read: 0-31, 99 when the modem said "unknown" or did not answer; -1 before any
  // netDiag() or on a transport without one. Read by the upload's weak-signal byte budget (#52).
  virtual int lastCsq() const { return -1; }
  virtual HTTPTransferDiagnostics transferDiagnostics() const { return {}; }

  /* Close and reopen a kept-alive session idle longer than this instead of reusing it (0 = always reuse) */
  virtual void set_session_idle_max_ms(unsigned long ms) {}

  /* Root CAs (DER PKCS#7) the server must chain to; the cellular transport refuses TLS without them */
  virtual void set_server_roots(const uint8_t* p7b, size_t len) {}

  /* The last connect() failed on certificate trust rather than the network (not a stale bearer) */
  virtual bool trustRefused() { return false; }

  /* How the last connect() went (empty if the transport doesn't say) */
  virtual std::string lastOpenTrace() { return std::string(); }

  /* ms since connected() last became true, or -1 if it isn't / the transport doesn't track it */
  virtual long upForMs() { return -1; }

  /* false if the transport refused a header: the request must not be sent */
  virtual bool set_headers(const HeaderVector new_headers) = 0;
  virtual bool connect() = 0;
  virtual bool connected() = 0;
  virtual void disconnect() = 0;

  virtual HttpResponse getEpoch() = 0;

  // POST a pre-built body verbatim; the caller has set the headers that authenticate it. Default: not
  // supported by this transport.
  virtual HttpResponse sendBulk(std::string payload) { return HttpResponse{ 0, std::string{} }; }

protected:
  FreematicsESP32 sys;
  std::string api_key{};
  std::string url;
  const int timeout{ 5000 };
};

#endif

