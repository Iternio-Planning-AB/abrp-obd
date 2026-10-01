/*************************************************************************
* Helper classes for various network communication devices
* Distributed under BSD license
* Visit https://freematics.com for more inform
on
* (C)2012-2021 Developed by Stanley Huang <stanley@freematics.com.au>
*************************************************************************/

#include <Arduino.h>
#include "FreematicsBase.h"
#include "FreematicsNetwork.h"

#ifndef PRINT
#define PRINT 0
#endif

String HTTPClientClass::genHeader(HTTP_METHOD method, const char* path, bool keepAlive, const char* payload, int payloadSize) {
  String header;
  // generate a simplest HTTP header
  header = method == METHOD_GET ? "GET " : "POST ";
  header += path;
  header += " HTTP/1.1\r\nConnection: ";
  header += keepAlive ? "keep-alive" : "close";
  header += "\r\nHost: ";
  header += m_host;
  if (method != METHOD_GET) {
    header += "\r\nContent-length: ";
    header += String(payloadSize);
  }
  header += "\r\n\r\n";
  return header;
}

/*******************************************************************************
  SIM7600/SIM7070/SIM5360
*******************************************************************************/
bool CellSIMCOM::begin(CFreematics* device) {
  getBuffer();
  m_device = device;
  for (byte n = 0; n < 30; n++) {
    device->xbTogglePower(200);
    device->xbPurge();
    if (!check(2000)) continue;
    if (sendCommand("ATE0\r") && sendCommand("ATI\r")) {
      // retrieve module info
      //Serial.print(m_buffer);
      char* p = strstr(m_buffer, "Model:");
      if (!p) {
        sendCommand("AT+SIMCOMATI\r");
        p = strstr(m_buffer, "QCN:");
        if (p) {
          char* q = strchr(p += 4, '_');
          if (q) {
            int l = q - p;
            if (l >= sizeof(m_model)) l = sizeof(m_model) - 1;
            memcpy(m_model, p, l);
            m_model[l] = 0;
          }
        }
        m_type = CELL_SIM7070;
      } else {
        p = strchr(p, '_');
        if (p++) {
          int i = 0;
          while (i < sizeof(m_model) - 1 && p[i] && p[i] != '\r' && p[i] != '\n') {
            m_model[i] = p[i];
            i++;
          }
          m_model[i] = 0;
        }
        m_type = strstr(m_model, "5360") ? CELL_SIM5360 : CELL_SIM7600;
      }
      p = strstr(m_buffer, "IMEI:");
      if (p) strncpy(IMEI, p[5] == ' ' ? p + 6 : p + 5, sizeof(IMEI) - 1);
      return true;
    }
  }
  end();
  return false;
}

void CellSIMCOM::end() {
  setGPS(false);
  if (m_type == CELL_SIM7070) {
    if (!sendCommand("AT+CPOWD=1\r", 1000, "NORMAL POWER DOWN")) {
      if (m_device) m_device->xbTogglePower(2510);
    }
  } else {
    if (!sendCommand("AT+CPOF\r")) {
      if (m_device) m_device->xbTogglePower(2510);
    }
  }
  // Settle (upstream fix): the modem takes ~1.5s to actually finish powering down after CPOWD -
  // without this, an immediately-following begin()'s PWRKEY toggle can hit it mid-shutdown and
  // leave it in a half-on state.
  delay(1500);
}

bool CellSIMCOM::setup(const char* apn, const char* username, const char* password, unsigned int timeout) {
  uint32_t t = millis();
  bool success = false;

  if (m_type == CELL_SIM7070) {
    do {
      do {
        success = false;
        sendCommand("AT+CFUN=1\r");
        do {
          delay(500);
          if (sendCommand("AT+CGREG?\r", 1000, "+CGREG: 0,")) {
            char* p = strstr(m_buffer, "+CGREG: 0,");
            if (p) {
              char ret = *( p + 10 );
              success = ret == '1' || ret == '5';
            }
          }
        } while (!success && millis() - t < timeout);
        if (!success) break;
        success = sendCommand("AT+CGACT?\r", 1000, "+CGACT: 1,");
        break;
      } while (millis() - t < timeout);
      if (!success) break;

      sendCommand("AT+CGNAPN\r");
      if (apn && *apn) {
        if (username && password) {
          sprintf(m_buffer, "AT+CNCFG=0,0,\"%s\",\"%s\",\"%s\",3\r", apn, username, password);
        } else {
          sprintf(m_buffer, "AT+CNCFG=0,0,\"%s\"\r", apn);
        }
        sendCommand(m_buffer);
      }
      sendCommand("AT+CNACT=0,1\r");
      sendCommand("AT+CNSMOD?\r");
      sendCommand("AT+CSCLK=0\r");
    } while (0);
  }
  if (!success) Serial.println(m_buffer);
  return success;
}

bool CellSIMCOM::setGPS(bool on) {
  if (on) {
    if (m_type == CELL_SIM7070) {
      sendCommand("AT+CGNSPWR=1\r");
      sendCommand("AT+CGNSMOD=1,1,0,0,0\r");
      if (sendCommand("AT+CGNSINF\r", 1000, "+CGNSINF:")) {
        if (!m_gps) {
          m_gps = new GPS_DATA;
          memset(m_gps, 0, sizeof(GPS_DATA));
        }
        return true;
      }
    } else {
      sendCommand("AT+CVAUXV=61\r", 100);
      sendCommand("AT+CVAUXS=1\r", 100);
      for (byte n = 0; n < 3; n++) {
        if (( sendCommand("AT+CGPS=1,1\r") && sendCommand("AT+CGPSINFO=1\r") ) || sendCommand("AT+CGPS?\r", 100, "+CGPS: 1")) {
          if (!m_gps) {
            m_gps = new GPS_DATA;
            memset(m_gps, 0, sizeof(GPS_DATA));
          }
          return true;
        }
        sendCommand("AT+CGPS=0\r", 100);
      }
    }
  } else if (m_gps) {
    if (m_type == CELL_SIM7070) {
      sendCommand("AT+CGNSPWR=0\r");
    } else {
      //sendCommand("AT+CVAUXS=0\r");
      sendCommand("AT+CGPS=0\r", 100);
    }
    GPS_DATA* g = m_gps;
    m_gps = 0;
    delete g;
    return true;
  }
  return false;
}

bool CellSIMCOM::getLocation(GPS_DATA** pgd) {
  if (m_gps) {
    if (pgd) *pgd = m_gps;
    return m_gps->ts != 0;
  } else {
    return false;
  }
}

String CellSIMCOM::getIP() {
  if (m_type == CELL_SIM7070) {
    sendCommand("AT+CNACT=0,1\r");
    for (int i = 0; i < 30; i++) {
      delay(500);
      if (sendCommand("AT+CNACT?\r", 1000)) {
        char* ip = strstr(m_buffer, "+CNACT:");
        if (ip) {
          ip = strchr(ip, '\"');
          if (ip++ && *ip != '0') {
            char* q = strchr(ip, '\"');
            if (q) *q = 0;
            return ip;
          }
        }
      }
    }
  } else {
    uint32_t t = millis();
    do {
      if (sendCommand("AT+IPADDR\r", 3000, "\r\nOK\r\n")) {
        char* p = strstr(m_buffer, "+IPADDR:");
        if (p) {
          char* ip = p + 9;
          if (*ip != '0') {
            char* q = strchr(ip, '\r');
            if (q) *q = 0;
            return ip;
          }
        }
      }
      delay(500);
    } while (millis() - t < 15000);
  }
  return "";
}

int CellSIMCOM::RSSI() {
  if (sendCommand("AT+CSQ\r")) {
    char* p = strchr(m_buffer, ':');
    if (p) {
      int csq = atoi(p + 2);
      if (csq != 99) {
        return csq * 2 - 113;
      }
    }
  }
  return 0;
}

String CellSIMCOM::getOperatorName() {
  if (sendCommand("AT+COPS?\r")) {
    char* p = strstr(m_buffer, ",\"");
    if (p) {
      p += 2;
      char* s = strchr(p, '\"');
      if (s) *s = 0;
      return p;
    }
  }
  return "";
}

bool CellSIMCOM::check(unsigned int timeout) {
  uint32_t t = millis();
  do {
    // Doubled "AT" per the upstream Freematics fix: right after a PWRKEY toggle the modem's UART
    // can garble the first byte(s), so a single AT may never be answered; the second one lands
    // cleanly and still yields the OK we wait for.
    if (sendCommand("AT\rAT\r", 250)) return true;
  } while (millis() - t < timeout);
  return false;
}

bool CellSIMCOM::checkSIM(const char* pin) {
  bool success;
  if (pin && *pin) {
    snprintf(m_buffer, RECV_BUF_SIZE, "AT+CPIN=\"%s\"\r", pin);
    sendCommand(m_buffer);
  }
  for (byte n = 0; n < 20 && !( success = sendCommand("AT+CPIN?\r", 500, ": READY") ); n++);
  return success;
}

String CellSIMCOM::queryIP(const char* host) {
  if (m_type == CELL_SIM7070) {
    sprintf(m_buffer, "AT+CDNSGIP=\"%s\",1,3000\r", host);
    if (sendCommand(m_buffer, 10000, "+CDNSGIP:")) {
      char* p = strstr(m_buffer, host);
      if (p) {
        p = strstr(p, "\",\"");
        if (p) {
          char* ip = p + 3;
          p = strchr(ip, '\"');
          if (p) *p = 0;
          return ip;
        }
      }
    }
  } else {
    sprintf(m_buffer, "AT+CDNSGIP=\"%s\"\r", host);
    if (sendCommand(m_buffer, 10000)) {
      char* p = strstr(m_buffer, host);
      if (p) {
        p = strstr(p, ",\"");
        if (p) {
          char* ip = p + 2;
          p = strchr(ip, '\"');
          if (p) *p = 0;
          return ip;
        }
      }
    }
  }
  return "";
}

commandResponse CellSIMCOM::sendCommand(const char* cmd, unsigned int timeout, const char* expected) {
  bool print = false;
if (print){
  Serial.printf("Buffer sent: %s\n", cmd);
}
  if (cmd) {
    m_device->xbWrite(cmd);
    delay(10);
  }
  char* temp;
  m_buffer[0] = 0;
  const char* answers [ ] = { "\r\nOK", "\r\nERROR" };
  // byte ret;
  // while (true) {
  //   sprintf(temp, "%s", m_buffer);
  //   byte ret = m_device->xbReceive(m_buffer, RECV_BUF_SIZE, timeout, expected ? &expected : answers, expected ? 1 : 2);
  //   if (!strcmp(temp, m_buffer)) break;
  //   Serial.printf("   %s\n", m_buffer);
  // }
  byte ret = m_device->xbReceive(m_buffer, RECV_BUF_SIZE, timeout, expected ? &expected : answers, expected ? 1 : 2);
if(print){
  Serial.printf("ret = %d, buffer = %s\n", ret, m_buffer);
}
  return commandResponse{ ret, m_buffer };
}

float CellSIMCOM::parseDegree(const char* s) {
  char* p;
  unsigned long left = atol(s);
  unsigned long tenk_minutes = ( left % 100UL ) * 100000UL;
  if (( p = strchr(s, '.') )) {
    unsigned long mult = 10000;
    while (isdigit(*++p)) {
      tenk_minutes += mult * ( *p - '0' );
      mult /= 10;
    }
  }
  return ( left / 100 ) + (float)tenk_minutes / 6 / 1000000;
}

void CellSIMCOM::checkGPS() {
  // check and parse GPS data
  char* p;
  if (m_type == CELL_SIM7070) {
    if (m_gps && sendCommand("AT+CGNSINF\r", 100, "+CGNSINF:")) do {
      if (!( p = strchr(m_buffer, ':') )) break;
      p += 2;
      if (strncmp(p, "1,1,", 4)) break;
      p += 4;
      m_gps->time = atol(p + 8) * 100 + atoi(p + 15);
      *( p + 8 ) = 0;
      int day = atoi(p + 6);
      *( p + 6 ) = 0;
      int month = atoi(p + 4);
      *( p + 4 ) = 0;
      int year = atoi(p + 2);
      m_gps->date = year + month * 100 + day * 10000;
      if (!( p = strchr(p + 9, ',') )) break;
      m_gps->lat = atof(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->lng = atof(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->alt = atof(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->speed = atof(++p) * 1000 / 1852;
      if (!( p = strchr(p, ',') )) break;
      m_gps->heading = atoi(++p);
      m_gps->ts = millis();
    } while (0);
  } else {
    if (m_gps && ( p = strstr(m_buffer, "+CGPSINFO:") )) do {
      if (!( p = strchr(p, ':') )) break;
      if (*( ++p ) == ',') break;
      m_gps->lat = parseDegree(p);
      if (!( p = strchr(p, ',') )) break;
      if (*( ++p ) == 'S') m_gps->lat = -m_gps->lat;
      if (!( p = strchr(p, ',') )) break;
      m_gps->lng = parseDegree(++p);
      if (!( p = strchr(p, ',') )) break;
      if (*( ++p ) == 'W') m_gps->lng = -m_gps->lng;
      if (!( p = strchr(p, ',') )) break;
      m_gps->date = atoi(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->time = atof(++p) * 100;
      if (!( p = strchr(p, ',') )) break;
      m_gps->alt = atof(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->speed = atof(++p);
      if (!( p = strchr(p, ',') )) break;
      m_gps->heading = atoi(++p);
      m_gps->ts = millis();
    } while (0);
  }
}

char* CellSIMCOM::getBuffer() {
  if (!m_buffer) m_buffer = (char*)malloc(RECV_BUF_SIZE);
  return m_buffer;
}

/*************************************************************************************************
  SIM7600
*************************************************************************************************/

void HTTPClientSIM::end() {
  setGPS(false);
  // AT+CPOF is the SIM7600 power-off and just errors on the SIM7070, so this used to leave the
  // modem running (it's reached via begin()'s failure path). Use the SIM7070's CPOWD with the
  // long-PWRKEY hardware fallback, then settle so a follow-up begin() doesn't catch the modem
  // mid-shutdown.
  if (!sendCommand("AT+CPOWD=1\r", 1000, "NORMAL POWER DOWN")) {
    if (m_device) m_device->xbTogglePower(2510);
  }
  delay(1500);
}

bool HTTPClientSIM::setup(const char* apn, unsigned int timeout) {
  uint32_t t = millis();
  bool success = false;
  //sendCommand("AT+CNMP=13\r"); // GSM only
  //sendCommand("AT+CNMP=14\r"); // WCDMA only
  //sendCommand("AT+CNMP=38\r"); // LTE only
  // Full functionality, per the canonical Freematics SIM7070 setup. A no-op when the radio is
  // already on (it is NOT CFUN=1,1, which would reset), but it recovers a modem left in minimum
  // functionality / airplane state by a previous run.
  sendCommand("AT+CFUN=1\r");
  do {
    do {
      m_device->xbWrite("AT+CPSI?\r");
      m_buffer[0] = 0;
      const char* answers [ ] = { "NO SERVICE", ",Online", ",Offline", ",Low Power Mode" };
      int ret = m_device->xbReceive(m_buffer, RECV_BUF_SIZE, 500, answers, 4);
      if (ret == 2) {
        success = true;
        break;
      }
      if (ret == -1 || ret == 4) break;
      delay(500);
    } while (millis() - t < timeout);
    if (!success) break;

    // Registration check across ALL radio access technologies. The stock code required CREG (2G/3G
    // circuit-switched) AND CGREG (2G/3G GPRS packet). On an LTE-M / CAT-M1 attach - what a SIM7070 uses on a
    // modern (roaming) network - the packet domain is EPS, reported by CEREG, and CREG/CGREG stay at 0. That
    // made a perfectly-attached LTE modem fail setup() (false negative: NITZ time arrives + NTP/data work,
    // yet setup returns false). We now accept registration on ANY domain: CEREG (LTE/EPS), CGREG (GPRS),
    // or CREG (GSM); state 1 = registered home, 5 = registered roaming.
    success = false;
    do {
      delay(100);
      if (sendCommand("AT+CEREG?\r", 1000, "+CEREG: 0,")) {
        char* p = strstr(m_buffer, "+CEREG: 0,");
        if (p && ( *( p + 10 ) == '1' || *( p + 10 ) == '5' )) { success = true; break; }
      }
      if (sendCommand("AT+CGREG?\r", 1000, "+CGREG: 0,")) {
        char* p = strstr(m_buffer, "+CGREG: 0,");
        if (p && ( *( p + 10 ) == '1' || *( p + 10 ) == '5' )) { success = true; break; }
      }
      if (sendCommand("AT+CREG?\r", 1000, "+CREG: 0,")) {
        char* p = strstr(m_buffer, "+CREG: 0,");
        if (p && ( *( p + 9 ) == '1' || *( p + 9 ) == '5' )) { success = true; break; }
      }
    } while (!success && millis() - t < timeout);
    if (!success) break;

    if (apn && *apn) {
      sendCommand("AT+CSQ\r");
      sprintf(m_buffer, "AT+CGDCONT=1,\"IP\",\"%s\"\r", apn);
      sendCommand(m_buffer);
      sendCommand("AT+CNACT=0,1\r");
      sendCommand("AT+CNACT?\r");   // read PDN state (was "AT+CNACT=?", the capability TEST command)
      sendCommand("AT+CGNAPN\r");
    }
    if (!success) break;

    // Keep the modem UART awake permanently (canonical Freematics setup ends with this). Without
    // it, a modem that enables autosleep swallows the first AT of each burst after an idle gap.
    sendCommand("AT+CSCLK=0\r");

    // sendCommand("AT+CSOCKSETPN=1\r");
    // sendCommand("AT+CIPMODE=0\r");
    // sendCommand("AT+NETOPEN\r");
  } while (0);
  return success;
}

bool HTTPClientSIM::setGPS(bool on) {
  if (on) {
    sendCommand("AT+CVAUXV=61\r");
    sendCommand("AT+CVAUXS=1\r");
    for (byte n = 0; n < 3; n++) {
      if (( sendCommand("AT+CGPS=1,1\r") && sendCommand("AT+CGPSINFO=1\r") ) || sendCommand("AT+CGPS?\r", 100, "+CGPS: 1")) {
        if (!m_gps) {
          m_gps = new GPS_DATA;
          memset(m_gps, 0, sizeof(GPS_DATA));
        }
        return true;
      }
      sendCommand("AT+CGPS=0\r", 100);
    }
  } else if (m_gps) {
    //sendCommand("AT+CVAUXS=0\r", 100);
    sendCommand("AT+CGPS=0\r", 100);
    delete m_gps;
    m_gps = 0;
    return true;
  }
  return false;
}

// PDP context 0 from AT+CNACT? as "<status>,<ip|noip>" ("?" if unanswered). Returns 1 if active with an IP,
// 0 if not, -1 if the modem didn't answer.
int HTTPClientSIM::bearerState(char* out, size_t len) {
  snprintf(out, len, "?");
  if (!sendCommand("AT+CNACT?\r", 1000)) return -1;
  char* p = strstr(m_buffer, "+CNACT: 0,");
  if (!p || p[10] < '0' || p[10] > '9') return -1;
  char st = p[10];
  char* eol = strpbrk(p, "\r\n");
  char* ip = strchr(p, '\"');
  bool has_ip = ip && (!eol || ip < eol) && ip[1] && ip[1] != '0' && ip[1] != '\"';
  snprintf(out, len, "%c,%s", st, has_ip ? "ip" : "noip");
  return st == '1' && has_ip ? 1 : 0;
}

static char rvCode(const commandResponse& r) {
  return r.returnValue == 1 ? 'o' : r.returnValue == 2 ? 'e' : 't'; // OK / ERROR / timeout
}

bool HTTPClientSIM::open(const char* host, uint16_t port) {
  // Trace of this open() for the caller to log: activate-request result, bearer before SHCONF, each SHCONN
  // result, bearer at failure, duration. Kept for successes too, so failures have a baseline.
  unsigned long t0 = millis();
  char req = rvCode(sendCommand("AT+CNACT=0,1\r")); // ERROR here means context 0 was already active
  sendCommand("AT+CACID=0\r");
  // delay(5000);

#if !0 //TESTING
  bool useSSL = ( port == 443 );
  if (useSSL) {
    sendCommand("AT+SHSSL=1,\"\"\r");
    sendCommand("AT+CSSLCFG=\"ignorertctime\",1,1\r");
    sendCommand("AT+CSSLCFG=\"SSLVERSION\",1,3\r");
    // Send SNI (Server Name Indication) with the TLS handshake. api.iternio.com is served behind a CDN /
    // load-balancer that routes by SNI, so WITHOUT this the handshake can land on the wrong virtual host /
    // certificate and fail intermittently. The canonical Freematics CellHTTP::open() sets this for SIM7070;
    // our port left it commented out. (Enabled per the reference library.)
    sprintf(m_buffer, "AT+CSSLCFG=\"sni\",1,\"%s\"\r", host);
    sendCommand(m_buffer);
  }

  char bo[8];
  bearerState(bo, sizeof(bo));
  char sh[3] = "--";
  sprintf(m_buffer, "AT+SHCONF=\"URL\",\"%s://%s\"\r", useSSL ? "https" : "http", host);
  if (!sendCommand(m_buffer)) {
    recordOpenTrace("shconf", req, bo, sh, t0);
    return false;
  }
  sendCommand("AT+SHCONF=\"HEADERLEN\",256\r");
  sendCommand("AT+SHCONF=\"BODYLEN\",1024\r");
  // Two attempts: SHCONN can fail if a half-open session from a previous cycle hasn't fully cleared. If the
  // first attempt doesn't reach SHSTATE:1, force a clean SHDISC, settle briefly, and try once more. (This is
  // why bare SHDISC recovery "sometimes failed to open" before - a back-to-back disconnect/connect needs a
  // moment to settle; the retry here absorbs that.)
  const char* sh_step = "shconn";
  for (int attempt = 0; attempt < 2; attempt++) {
    commandResponse conn = sendCommand("AT+SHCONN\r", HTTP_CONN_TIMEOUT); // HTTP_CONN_TIMEOUT);
    sh[attempt] = rvCode(conn);
    sh_step = conn ? "shstate" : "shconn";
    if (sendCommand("AT+SHSTATE?\r") && strstr(m_buffer, "+SHSTATE: 1")) {
      m_state = HTTP_CONNECTED;
      m_host = host;
      sendCommand("AT+SHCHEAD\r");
      sendCommand("AT+SHAHEAD=\"User-Agent\",\"curl/7.47.0\"\r");
      sendCommand("AT+SHAHEAD=\"Cache-control\",\"no-cache\"\r");
      sendCommand("AT+SHAHEAD=\"Connection\",\"keep-alive\"\r");
      sendCommand("AT+SHAHEAD=\"Accept\",\"*/*\"\r");
      m_state = HTTP_CONNECTED;
      recordOpenTrace(0, req, bo, sh, t0);
      return true;
    }
    sendCommand("AT+SHDISC\r", 2000);
    delay(500);
  }
  checkGPS();
  Serial.println(m_buffer);
  m_state = HTTP_ERROR;
  recordOpenTrace(sh_step, req, bo, sh, t0);
#endif
  return false;
}

// fail_step is 0 on success. A failure with the bearer down reports as cnact whatever SH step tripped over
// it; nores means AT+CNACT? itself went unanswered (wedged modem, not a bearer verdict).
void HTTPClientSIM::recordOpenTrace(const char* fail_step, char req, const char* bo, const char* sh,
    unsigned long t0) {
  char verdict[20] = "ok", bf[16] = "";
  if (fail_step) {
    char b[8];
    int up = bearerState(b, sizeof(b));
    if (up == 1) snprintf(verdict, sizeof(verdict), "%s", fail_step);
    else snprintf(verdict, sizeof(verdict), "%s (%s)", up == 0 ? "cnact" : "nores", fail_step);
    snprintf(bf, sizeof(bf), " bf=%s", b);
  }
  snprintf(m_open_trace, sizeof(m_open_trace), "%s req=%c bo=%s sh=%s%s t=%.1fs",
    verdict, req, bo, sh, bf, (millis() - t0) / 1000.0f);
}

void HTTPClientSIM::setHeaders(const HeaderVector& headers) {
#if PRINT
  Serial.printf("Setting headers\n");
#endif
  sendCommand("AT+SHCHEAD\r");
  sendCommand("AT+SHAHEAD=\"User-Agent\",\"curl/7.47.0\"\r");
  sendCommand("AT+SHAHEAD=\"Cache-control\",\"no-cache\"\r");
  sendCommand("AT+SHAHEAD=\"Connection\",\"keep-alive\"\r");
  sendCommand("AT+SHAHEAD=\"Accept\",\"*/*\"\r");
  for (auto header : headers) {
    sprintf(m_buffer, "AT+SHAHEAD=\"%s\",\"%s\"\r", header.first.c_str(), header.second.c_str());
    sendCommand(m_buffer);
  }
}

bool HTTPClientSIM::close() {
  // Tear down the SH session with AT+SHDISC - the SH-stack disconnect that matches open()'s AT+SHCONN.
  // The old code sent AT+CHTTPSCLSE, which belongs to the SIM7070's *separate* CHTTPS HTTP application and
  // does NOTHING to an SH session. Result: the keep-alive SH/TLS connection (open() sets Connection:
  // keep-alive) was never actually closed, so it was reused across uploads until the server/NAT silently
  // dropped it. AT+SHSTATE? then still reported "1" (connected) on the half-open socket, so the next SHREQ
  // POST fired into a dead connection and hung the full 15s -> http=0 with an empty body. Closing cleanly
  // each cycle makes the next open() build a genuinely fresh session, which is the fix for the http=0 storms.
  m_state = HTTP_DISCONNECTED;
  return sendCommand("AT+SHDISC\r", 2000);
}

std::pair<int, int> handlePair(const char* response, const char* pattern) {
  // Hardened parse of "<pattern><code>,<len>\r\n". The previous std::string/std::stoi version
  // crashed the whole ESP32 when the URC was truncated by the UART read window (xbReceive returns
  // the instant the match string appears, so the ",<code>,<len>" tail can still be in flight):
  // stoi("") throws an uncaught std::invalid_argument -> abort -> panic reset, and ++it after the
  // first loop could walk past end(). strtol never throws; requiring both numbers AND a line
  // terminator makes a half-received URC yield {0,0} (soft failure) instead of a bogus HTTP code
  // or a reboot.
  if (!response || !pattern) return { 0, 0 };
  const char* p = strstr(response, pattern);
  if (!p) return { 0, 0 };
  p += strlen(pattern);
  char* end = nullptr;
  long first = strtol(p, &end, 10);
  if (end == p || *end != ',') return { 0, 0 };
  p = end + 1;
  long second = strtol(p, &end, 10);
  if (end == p || ( *end != '\r' && *end != '\n' )) return { 0, 0 };
  return { (int)first, (int)second };
}

const char* handleResponse(const char* response, const char* pattern) {
  // Extract the response body using the EXACT byte count the modem reports in "+SHREAD: <len>", mirroring the
  // canonical Freematics CellHTTP::receive() for the SIM7070. Two reasons this is the right approach:
  //   1) The old version returned res.c_str() from a LOCAL std::string - a dangling pointer into freed memory,
  //      so json::parse downstream read garbage ~1-in-5 times (the "ok_noparse" path) and a real API rejection
  //      (200 + {"status":"error"}) could be misparsed and logged as accepted "ok".
  //   2) Reading exactly <len> bytes (instead of scanning to the next '\n') is robust to bodies that contain
  //      newlines and to any trailing "\r\nOK" the modem appends after the payload.
  // A static buffer outlives the return; the cellular path is single-threaded and sequential, so reuse is safe.
  static char resbuf[RECV_BUF_SIZE];
  resbuf[0] = 0;
  const char* p = strstr(response, "+SHREAD:");
  if (!p) return resbuf;
  int bytes = atoi(p + 8);                          // length after "+SHREAD:" (atoi skips the leading space)
  if (bytes <= 0) return resbuf;
  if (bytes > (int)sizeof(resbuf) - 1) bytes = sizeof(resbuf) - 1;
  const char* body = strchr(p, '\n');               // body begins right after the "+SHREAD: <len>" header line
  if (!body) return resbuf;
  ++body;
  int n = 0;
  while (n < bytes && body[n]) { resbuf[n] = body[n]; ++n; }
  resbuf[n] = 0;
  return resbuf;
}

commandResponse HTTPClientSIM::getEpoch(const char* api_page) {
  sprintf(m_buffer, "AT+SHREQ=\"%s\",\"1\"\r", api_page); //1: GET, 3: POST
  auto req_response = sendCommand(m_buffer);

  delay(2000);
  auto sizemsg = sendCommand("AT+SHREAD=?\r");
  auto size = handlePair(sizemsg.buffer, "+SHREQ: \"GET\",");
  sprintf(m_buffer, "AT+SHREAD=0,%d\r", size.second);
  auto response = sendCommand(m_buffer);
#if PRINT 
  Serial.printf("Response is: \n%s\n", response.buffer);
#endif
  auto buf = handleResponse(response.buffer, "+SHREAD");
#if PRINT 
  Serial.printf("RETURN VALUE: %s\n", buf);
#endif
  // sendCommand("AT+SHDISC\r");
  close();

  return { size.first, buf };
}

commandResponse HTTPClientSIM::send(const char* api_page, const char* payload, const int payloadSize, const char* car_token) {
  // Start the transaction clean: leftovers from a previous exchange (a late URC, an unread body
  // tail) would otherwise satisfy this transaction's expected-string matches.
  m_device->xbPurge();

  // Body in DOWNLOAD mode (confirmed against the SIM7070G via standalone test): AT+SHBOD=<len>,<timeout>
  // replies ">", then we stream EXACTLY <len> raw bytes (no trailing CR), then the modem replies OK.
  // (The inline AT+SHBOD="...",<len> form returns ERROR on this modem.) Streaming raw also avoids the
  // 512-byte command buffer limit for the body - it's bounded only by AT+SHCONF "BODYLEN".
  sprintf(m_buffer, "AT+SHBOD=%d,10000\r", payloadSize);
  if (!sendCommand(m_buffer, 3000, ">")) {   // wait for the download prompt
    // No prompt = the modem REFUSED the body (dead session, or payloadSize over the SHCONF
    // BODYLEN=1024 cap). Writing the payload anyway would feed raw JSON straight into the AT
    // command parser and desynchronize the command stream for everything that follows - this was
    // the failure mode that killed uploads the moment frames got bigger. Abort instead; the
    // caller sees http=0 and runs the normal failed-upload path.
    return { 0, "" };
  }
  m_device->xbWrite(payload, payloadSize);   // exactly payloadSize bytes
  sendCommand(0, 3000);                       // consume the "OK" emitted after the body is received

  // Issue the POST and wait for the unsolicited result line "+SHREQ: \"POST\",<code>,<len>". It arrives
  // AFTER the HTTP round-trip (not in AT+SHREAD=?, which only reports the readable range), so capture it
  // directly here rather than from a later command's buffer.
  // car_token == nullptr: the endpoint authenticates by HEADER, not by query string (v2 /2/app/tlm takes
  // X-TLM-TOKEN), so the path goes up bare. Appending an empty ?token= would be harmless to the server
  // but spends body-cap-adjacent URL bytes and puts a credential-shaped empty param in every access log.
  if (car_token != nullptr && *car_token != '\0') {
    sprintf(m_buffer, "AT+SHREQ=\"%s?token=%s\",\"3\"\r", api_page, car_token);
  } else {
    sprintf(m_buffer, "AT+SHREQ=\"%s\",\"3\"\r", api_page);
  }
  auto req = sendCommand(m_buffer, 8000, "+SHREQ:");         // 8s (was 15s): a healthy POST answers in 1-2.5s;
                                                             // a frozen LTE-M stack never answers, so fail fast
                                                             // and let the recovery ladder run sooner
  // The match fires the instant "+SHREQ:" enters the buffer - the ",<code>,<len>" tail can still
  // be in flight on the UART. Top the buffer up until the URC line is complete, or handlePair
  // sees a half line and reports a false http=0.
  {
    char* u = strstr(m_buffer, "+SHREQ:");
    if (u && !strchr(u, '\n')) {
      size_t used = strlen(m_buffer);
      if (used < RECV_BUF_SIZE - 1) {
        const char* nl[] = { "\n" };
        m_device->xbReceive(m_buffer + used, RECV_BUF_SIZE - used, 1000, nl, 1);
      }
    }
  }
  auto size = handlePair(req.buffer, "+SHREQ: \"POST\",");   // {http_code, body_len}
#if PRINT
  Serial.printf("POST result: code=%d len=%d\n", size.first, size.second);
#endif
  if (size.second <= 0) {
    return { size.first, "" };                // request done, no body to read (size.first carries the code)
  }
  int want = size.second;
  if (want > (int)RECV_BUF_SIZE - 16) want = RECV_BUF_SIZE - 16; // never ask for more than the buffer holds
  sprintf(m_buffer, "AT+SHREAD=0,%d\r", want);
  // The modem answers OK FIRST and delivers "+SHREAD: <len>" + body as a separate burst. The old
  // default wait ("\r\nOK", 1s) returned before the body had arrived whenever the two landed in
  // different UART read windows -> empty body (the ~22% "ok_noparse" events), with the unread body
  // left in the UART buffer to pollute the NEXT command. Wait for the URC itself, then keep
  // reading until the full reported body is actually in the buffer.
  auto response = sendCommand(m_buffer, 5000, "+SHREAD:");
  if (response.returnValue == 1) {
    uint32_t t = millis();
    while (millis() - t < 1500) {
      char* h = strstr(m_buffer, "+SHREAD:");
      if (h) {
        int need = atoi(h + 8);
        char* body = strchr(h, '\n');       // '\n' only arrives once the length digits are complete
        if (body && need > 0 && (int)strlen(body + 1) >= need) break;
      }
      size_t used = strlen(m_buffer);
      if (used >= RECV_BUF_SIZE - 1) break;
      m_device->xbReceive(m_buffer + used, RECV_BUF_SIZE - used, 50); // collect the tail
    }
  }
  auto buf = handleResponse(response.buffer, "+SHREAD");
#if PRINT
  Serial.printf("Response body: %s\n", buf);
#endif
  return { size.first, buf };
}

bool HTTPClientSIM::send(HTTP_METHOD method, const char* path, bool keepAlive, const char* payload, int payloadSize) {
  String header = genHeader(method, path, keepAlive, payload, payloadSize);
  int len = header.length();
  sprintf(m_buffer, "AT+CHTTPSSEND=%u\r", len + payloadSize);
  if (!sendCommand(m_buffer, 100, ">")) {
    m_state = HTTP_DISCONNECTED;
    return commandResponse{ 0, m_buffer };
  }
  // send HTTP header
  m_device->xbWrite(header.c_str());
  // send POST payload if any
  if (payload) m_device->xbWrite(payload, payloadSize);
  if (auto a = sendCommand(0, 200, "+CHTTPSSEND:")) {
    m_state = HTTP_SENT;
    return a;
  }
  //Serial.println(m_buffer);
  m_state = HTTP_ERROR;
  return commandResponse{ 0, m_buffer };
}

char* HTTPClientSIM::receive(int* pbytes, unsigned int timeout) {
  // start receiving
  int received = 0;
  char* payload = 0;

  // wait for +CHTTPS:RECV EVENT
  if (!sendCommand(0, timeout, "RECV EVENT")) {
    checkGPS();
    return 0;
  }

  bool legacy = false;
  char* p = strstr(m_buffer, "RECV EVENT");
  if (p) {
    if (*( p - 1 ) == ' ')
      legacy = true;
    else if (*( p - 1 ) != ':')
      return 0;
  }
  // FIXME
  if (strstr(m_buffer, " 200 ")) {
    m_code = 200;
  }

  checkGPS();

  /*
    +CHTTPSRECV: DATA,XX\r\n
    [XX bytes from server]\r\n
    +CHTTPSRECV:0\r\n
  */
  // TODO: implement for multiple chunks of data
  // only deals with first chunk now
  sprintf(m_buffer, "AT+CHTTPSRECV=%u\r", RECV_BUF_SIZE - 36);
  if (sendCommand(m_buffer, timeout, legacy ? "\r\n+CHTTPSRECV: 0" : "\r\n+CHTTPSRECV:0")) {
    char* p = strstr(m_buffer, "\r\n+CHTTPSRECV: DATA");
    if (p) {
      if (( p = strchr(p, ',') )) {
        received = atoi(p + 1);
        char* q = strchr(p, '\n');
        payload = q ? ( q + 1 ) : p;
        if (m_buffer + RECV_BUF_SIZE - payload > received) {
          payload[received] = 0;
        }
      }
    }
  }
  if (received == 0) {
    m_state = HTTP_ERROR;
    return 0;
  } else {
    m_state = HTTP_CONNECTED;
    if (pbytes) *pbytes = received;
    return payload;
  }
}
