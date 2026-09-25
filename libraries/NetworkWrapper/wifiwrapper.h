#ifndef WIFIWRAPPER_H
#define WIFIWRAPPER_H

/*
  Wrapper for Arduino HTTPClient with custom functions
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <networkwrapper.h>

class WiFiWrapper : public NetworkWrapper {
public:
  WiFiWrapper(FreematicsESP32 sys, std::string ssid, std::string password);

  /* Establish connection with WiFi accesspoint */
  bool init(const bool retry = false) override;

  /* True if connection established with wifi accesspoint */
  bool connected() override;

  /* Connect to network server */
  bool connect() override;

  /* Disconnect from network server */
  void disconnect() override;

  /* Set headers of https packet  */
  void set_headers(const HeaderVector headers) override;

  /* Set credentials for connecting to wifi accesspoint */
  void set_credentials(std::string ssid, std::string password);

  /* Disconnect from server and accesspoint, then try to establish connection again */
  void restart() override;

  /* Non-blocking: kick a fresh association attempt when offline (used by the loop) */
  void reconnect() override;

  /* Turn the WiFi radio off before deep sleep */
  void powerOff() override;
  /* Send data to Iternio API */
  HttpResponse sendData(const std::string payload) override;

  /* Send a pre-built /tlm/bulk body verbatim (token already embedded in the body) */
  HttpResponse sendBulk(const std::string payload) override;

  /* Fetch UTC epoch. Unused for WiFi (time comes from GPS); returns a failed response */
  HttpResponse getEpoch() override;

  /* The "Date" response header captured from the most recent POST (coarse UTC time source) */
  std::string getDateHeader() override;

  /* Lightweight GET purely to read the server's "Date" header (acquire a clock before polling) */
  std::string fetchDate() override;

private:
  WiFiClass WiFi;
  WiFiClientSecure client;
  HTTPClient https;

  std::string ssid;
  std::string password;
  std::string last_date_header; // "Date" response header from the most recent POST (coarse UTC source)

  /* Fully power-cycle the radio (OFF -> STA) to flush a glitched MAC/PHY state left by a dirty
     power-cycle, re-running RF calibration the way a deep-sleep wake does. */
  void radioReset();
};

#endif

