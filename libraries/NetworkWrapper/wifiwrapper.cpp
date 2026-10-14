#include "wifiwrapper.h"

WiFiWrapper::WiFiWrapper(FreematicsESP32 sys, std::string ssid, std::string password) : NetworkWrapper(sys), ssid{ ssid }, password{ password }, WiFi{}, client{}, https{} { };

// Bounded boot-time connect attempt. Never blocks setup() forever, and never re-issues WiFi.begin() while
// an association is mid-flight (doing so throws 0x3007 "sta is connecting" and wedges reconnection after a
// warm reset - the original bug). The radio keeps trying / auto-reconnecting in the background regardless.
// A healthy association completes in ~1-2s; only a glitched radio runs the whole window. Keep this short
// so a dirty-power-up boot bails out and triggers the RF-flush recovery quickly instead of stalling 15s.
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 7000;

// Stop esp_wifi and restart it (WIFI_OFF -> WIFI_STA), forcing a fresh RF/PHY calibration. A dirty USB
// unplug/replug can back-power the chip through the serial bridge and leave the WiFi MAC/PHY in a glitched
// "half-alive" state that a plain WiFi.disconnect() does NOT clear - which is why a power-cycle failed to
// reconnect while a deep-sleep wake (which powers the RF domain down fully) always worked.
void WiFiWrapper::radioReset() {
  WiFi.disconnect(true);   // stop STA association and power the radio off
  WiFi.mode(WIFI_OFF);     // ensure the modem is fully stopped (esp_wifi_stop)
  delay(250);              // let the RF domain settle before restarting
  WiFi.mode(WIFI_STA);     // restart esp_wifi -> re-runs PHY calibration, flushing the glitched state
}

bool WiFiWrapper::init(const bool retry/* = false */) {
  WiFi.persistent(false);        // don't thrash flash with every begin(); avoids stale-config issues
  radioReset();                  // full radio power-cycle to clear any glitch from a dirty power-up
  WiFi.setAutoReconnect(true);   // let the stack transparently re-associate when the AP drops/returns
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str()); // begin ONCE - re-begin() mid-connect is what wedged it

  if (retry) {
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(500);
      Serial.print(".");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected to WiFi network with IP Address: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.printf("\nWiFi not connected within boot timeout - continuing; will keep retrying in background\n");
  return false;
}

void WiFiWrapper::powerOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void WiFiWrapper::reconnect() {
  // Retry kick from the main loop when offline. Do a FULL radio power-cycle (not just disconnect) so a
  // glitched MAC/PHY from a dirty power-up is flushed on every attempt, not only at boot. We don't wait
  // here for association - the loop keeps logging and uploads resume once status() reports connected.
  // The loop rate-limits this (WIFI_RETRY_MS, ~20s) so the ~250ms cycle never aborts an attempt mid-flight.
  if (WiFi.status() == WL_CONNECTED) return;
  radioReset();
  WiFi.begin(ssid.c_str(), password.c_str());
}


bool WiFiWrapper::connect() {
  if (!url.c_str()) {
    Serial.printf("No url found\n");
    return false;
  }
  if (https.connected()) return true;
  client.setInsecure();
  https.setTimeout(timeout);
  bool success = https.begin(client, url.c_str());
  if (success) {
    // Ask HTTPClient to retain the server's Date header so we can read it after the POST (a coarse UTC
    // time source). collectHeaders must be set after begin() and before the request.
    static const char* collect[] = { "Date" };
    https.collectHeaders(collect, 1);
  }
  // Serial.printf("Connection to %s %s\n", url.c_str(), success ? "succeeded" : "refused");
  return success;
}

void WiFiWrapper::disconnect() {
  return https.end();
}

bool WiFiWrapper::set_headers(HeaderVector headers) {
  // addHeader() accumulates until https.end(), and connect() reuses a connected session: call https.end()
  // per POST before re-enabling this lane, or every request carries every earlier request's signature.
  for (auto header : headers) {
    // Serial.printf("Adding header %s, %s\n", header.first.c_str(), header.second.c_str());
    https.addHeader(header.first.c_str(), header.second.c_str());
  }
  return true;
}

void WiFiWrapper::set_credentials(std::string ssid, std::string password) {
  this->ssid = ssid;         // was `ssid = ssid;` - assigned the parameter to itself (members were shadowed)
  this->password = password;
}

void WiFiWrapper::restart() { 
  https.end();
  WiFi.disconnect();
  init();
  connect();
}

HttpResponse WiFiWrapper::sendBulk(std::string payload) {
  // The headers logNetwork set authenticate the request, so the body goes as-is.
  // BY LENGTH, never as a C string: the body may be gzip, which contains NULs - c_str() would truncate
  // it at the first one and post a body the server cannot inflate. This lane is dormant (ENABLE_WIFI 0)
  // and would not have shown the fault until someone turned it back on.
  Serial.printf("Sent bulk data: %u bytes\n", (unsigned)payload.size());
  int responseCode = https.POST((uint8_t*)payload.data(), payload.size());
  std::string response = https.getString().c_str();
  last_date_header = https.header("Date").c_str(); // coarse UTC time source (see logNetwork)
  return HttpResponse{ responseCode, response };
}

std::string WiFiWrapper::getDateHeader() {
  return last_date_header;
}

std::string WiFiWrapper::fetchDate() {
  // Lightweight GET purely to read the server's Date header (coarse UTC), without sending telemetry.
  // connect() applies collectHeaders("Date"); any response - even 4xx/405 - still carries a Date.
  if (!connect()) return std::string();
  https.GET();
  last_date_header = https.header("Date").c_str();
  https.end();
  return last_date_header;
}

bool WiFiWrapper::connected() {
  return WiFi.status() == WL_CONNECTED;
}

HttpResponse WiFiWrapper::getEpoch() {
  // Time is sourced from GPS in the WiFi build, so this is not used.
  return HttpResponse{ 0, std::string{} };
}