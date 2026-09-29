#ifndef CELLWRAPPER_H
#define CELLWRAPPER_H

/*
  Wrapper for Freematics HTTPClient with custom functions
  Substantial changes have also been made to Freematics class
*/

#include <networkwrapper.h>
#include <utility>

class CellWrapper : public NetworkWrapper {
public:
  CellWrapper(FreematicsESP32 sys, std::string apn);

  /* Establish network connection (hologram) */
  bool init(const bool retry = false) override;

  /* Connect to network server (telemetry API) */
  bool connect() override;

  /* True if connection established */
  bool connected() override;

  /* Set headers for https packet */
  void set_headers(const HeaderVector headers) override;

  /* Close connection to network server */
  void disconnect() override;

  /* Disconnect and reestablish connection with network server */
  void restart() override;

  /* Non-blocking re-establish when offline (re-inits the modem/APN) */
  void reconnect() override;

  /* Power the modem fully down (AT+CPOWD) before deep sleep so it stops drawing */
  void powerOff() override;

  /* Fetch current time from iternio API */
  HttpResponse getEpoch() override;

  /* Send data over Iternio API */
  HttpResponse sendData(std::string payload) override;

  /* Send a pre-built /tlm/bulk body (token already embedded in the body) */
  HttpResponse sendBulk(std::string payload) override;

  /* Set variable api_page */
  void set_api_page(const char* new_api_page);

  /* Read the modem's network clock via AT+CCLK ("yy/MM/dd,hh:mm:ss±zz"), or "" if unavailable */
  std::string readClock();

  /* Sync the modem RTC from an NTP server over the data link (AT+CNTP). Reliable when carrier NITZ is
     absent. tz is set to 0 so AT+CCLK then reads UTC. Returns true on a reported success. */
  bool syncNtp();

  /* One-line signal (AT+CSQ) + modem voltage (AT+CBC) + CEREG state snapshot for diagnostics */
  std::string netDiag() override;
  std::pair<std::string, std::string> initFailureDiag();
  char registrationState() const { return m_registration; }

  /* Raw AT passthrough for bench debugging (serial console "AT..." commands): sends cmd + CR and
     returns the raw modem reply (everything received before OK/ERROR/timeout). */
  std::string sendRawAt(const char* cmd, unsigned int timeout_ms = 10000);

  /* READ-ONLY dump of every NVM-persisted radio setting that can differ between two "identical"
     units and survive a power pull (CNMP/CMNB/CBANDCFG/PSM/eDRX/COPS mode/...). Run on a failing
     and a working unit and diff the output to find a poisoned setting. Writes nothing. */
  void dumpRadioConfig();

private:
  /* Dead-RF revival ladder step (called from reconnect() when unregistered with csq=99). */
  void reviveDeadRadio();

  std::string apn;
  std::string api_page;
  HTTPClientSIM net;
  bool m_ready = false;  // modem set up + APN attached; coarse "link up" flag for connected()
  const char* m_init_stage = "begin";
  char m_registration = '?';
  unsigned long m_last_cfun_ms = 0; // last AT+CFUN radio cycle; used to rate-limit re-attach (roaming throttle guard)
  unsigned long m_dead_rf_since = 0; // millis() when csq=99-while-unregistered was first seen (0 = radio alive)
  uint8_t m_revive_stage = 0;        // dead-RF ladder: 0 none, 1 CFUN cycled, 2 modem rebooted, 3 power-cycled
};

#endif

