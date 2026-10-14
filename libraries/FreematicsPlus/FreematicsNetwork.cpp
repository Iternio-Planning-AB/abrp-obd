/*************************************************************************
* Helper classes for various network communication devices
* Distributed under BSD license
* Visit https://freematics.com for more inform
on
* (C)2012-2021 Developed by Stanley Huang <stanley@freematics.com.au>
*************************************************************************/

#include <Arduino.h>
#include <cerrno>
#include <climits>
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

// Deep sleep is a reboot: CONVERT refusals must survive it, counted for the bundle they were against.
RTC_DATA_ATTR static uint32_t s_ca_refused_hash = 0;
RTC_DATA_ATTR static uint8_t s_ca_refusals = 0;
RTC_DATA_ATTR static uint8_t s_ca_held_wakes = 0;

void HTTPClientSIM::setCaBundle(const uint8_t* p7b, size_t len) {
  m_ca = p7b;
  m_ca_len = p7b ? len : 0;
  m_ca_state = CA_UNCHECKED;
  m_ca_tries = 0;
  uint32_t h = 2166136261u;  // FNV-1a: a changed bundle gets a new modem-side name and installs itself
  for (size_t i = 0; i < m_ca_len; ++i) h = ( h ^ p7b[i] ) * 16777619u;
  m_ca_hash = h;
  snprintf(m_ca_name, sizeof(m_ca_name), "rc%08lx", (unsigned long)h);
}

// A prompt or ACK that comes late would leave the modem taking our next command as file bytes, so on
// any miss wait out the (short) input window and purge before carrying on.
bool HTTPClientSIM::writeCaFile(const char* ext, const uint8_t* data, size_t len) {
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+CFSWFILE=3,\"%s%s\",0,%u,%d\r", m_ca_name, ext, (unsigned)len, CA_INPUT_MS);
  if (sendCommand(cmd, 3000, "DOWNLOAD")) {
    m_device->xbWrite((const char*)data, len);
    if (sendCommand(0, CA_INPUT_MS + 3000)) return true;
  }
  delay(CA_INPUT_MS + 500);
  m_device->xbPurge();
  return false;
}

// Puts the bundle into the modem's certificate store unless this exact bundle is already there. The
// converted store and /customer/ persist in modem flash across CPOWD, so this writes once per bundle,
// not per wake. <name>.ok is written only after CONVERT succeeds: a power cut mid-install leaves no
// marker and the next attempt redoes it. A failed write is retried on later opens, CA_INSTALL_TRIES per
// boot; a CONVERT refusal is deterministic, so it latches for the boot, and after two it is retried only
// every CA_CONVERT_RETRY_WAKES boots rather than rewriting the modem's flash on every wake.
bool HTTPClientSIM::ensureCa() {
  if (m_ca_state != CA_UNCHECKED) return m_ca_state == CA_READY;
  if (!m_ca_len) { m_ca_state = CA_FAILED; return false; }
  if (s_ca_refused_hash != m_ca_hash) { s_ca_refused_hash = m_ca_hash; s_ca_refusals = 0; }
  if (s_ca_refusals >= 2 && ++s_ca_held_wakes < CA_CONVERT_RETRY_WAKES) {
    m_ca_state = CA_FAILED;
    m_ca_trace = "FAIL:convert(held)";
    return false;
  }
  s_ca_held_wakes = 0;
  char cmd[64];
  sendCommand("AT+CFSTERM\r"); // CFSINIT errors while an earlier buffer is still allocated
  m_ca_trace = "FAIL:cfsinit";
  if (sendCommand("AT+CFSINIT\r")) do {
    snprintf(cmd, sizeof(cmd), "AT+CFSGFIS=3,\"%s.ok\"\r", m_ca_name);
    const commandResponse marker = sendCommand(cmd);
    if (marker && strstr(m_buffer, "+CFSGFIS:")) { m_ca_trace = "present"; break; }
    m_ca_trace = "FAIL:gfis";
    if (marker.returnValue != 1 && marker.returnValue != 2) break; // no answer is not "absent"
    m_ca_trace = "FAIL:write";
    if (!writeCaFile(".p7b", m_ca, m_ca_len)) break;
    m_ca_trace = "FAIL:convert";
    snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"CONVERT\",2,\"%s.p7b\"\r", m_ca_name);
    const commandResponse converted = sendCommand(cmd, 10000);
    if (!converted) {
      if (converted.returnValue != 2) m_ca_trace = "FAIL:convert_to"; // a slow modem is not a refusal
      break;
    }
    s_ca_refusals = 0;
    m_ca_trace = "FAIL:marker";
    if (!writeCaFile(".ok", (const uint8_t*)"1", 1)) break;
    m_ca_trace = "loaded";
  } while (0);
  sendCommand("AT+CFSTERM\r");
  if (m_ca_trace[0] != 'F') {
    m_ca_state = CA_READY;
    return true;
  }
  if (!strcmp(m_ca_trace, "FAIL:convert")) {
    if (s_ca_refusals < 255) s_ca_refusals++;
    m_ca_state = CA_FAILED;
  } else {
    m_ca_state = ++m_ca_tries < CA_INSTALL_TRIES ? CA_UNCHECKED : CA_FAILED;
  }
  return false;
}

// The marker says installed but the modem refused or cannot find the roots (its certificate store
// reset, a modem firmware update). Drop the marker so the next open reinstalls. Once per boot.
void HTTPClientSIM::dropCaMarker() {
  if (m_ca_dropped) return;
  m_ca_dropped = true;
  char cmd[64];
  sendCommand("AT+CFSTERM\r");
  sendCommand("AT+CFSINIT\r");
  snprintf(cmd, sizeof(cmd), "AT+CFSDFILE=3,\"%s.ok\"\r", m_ca_name);
  sendCommand(cmd);
  sendCommand("AT+CFSTERM\r");
  m_ca_state = CA_UNCHECKED;
  m_ca_trace = "stale";
}

// The SSL context the SH session uses. The empty-CA form (AT+SHSSL=1,"") skips server verification;
// it is never sent - no bundle means no TLS. A CA name the store does not hold is accepted here and
// refused at SHCONN (measured), so a stale marker shows up as a refused open, not a silent downgrade.
bool HTTPClientSIM::configureTls(const char* host) {
  if (!ensureCa()) return false;
  sprintf(m_buffer, "AT+SHSSL=1,\"%s.p7b\"\r", m_ca_name);
  if (!sendCommand(m_buffer)) {
    if (!strcmp(m_ca_trace, "present")) dropCaMarker();
    return false;
  }
  // Validity dates are not checked, so an expired leaf is accepted. The modem judges them by its own RTC,
  // which is right only once NITZ or our NTP sync has set it since power-up, and neither is guaranteed
  // before the first upload (NITZ is absent on roaming SIMs; NTP is skipped while the ESP32 clock is
  // fresh). The chain is verified; the certificate's name is not (the SIM7070 has no option for it).
  sendCommand("AT+CSSLCFG=\"ignorertctime\",1,1\r");
  sendCommand("AT+CSSLCFG=\"SSLVERSION\",1,3\r");
  // Send SNI (Server Name Indication) with the TLS handshake. api.iternio.com is served behind a CDN /
  // load-balancer that routes by SNI, so WITHOUT this the handshake can land on the wrong virtual host /
  // certificate and fail intermittently.
  sprintf(m_buffer, "AT+CSSLCFG=\"sni\",1,\"%s\"\r", host);
  sendCommand(m_buffer);
  return true;
}

// SHCONN only answers ERROR. Ask the CA (TCP) stack the same question in two halves - plain TCP, then
// TLS against the same roots - so a refused chain is told apart from a network failure. Measured
// +CAOPEN results: 27 for a chain that fails the roots, 1 when the named CA is not in the store, 0 for
// plain TCP that connects. No application data is sent. Probes until one reaches a trust verdict; that
// verdict then stands for the boot's later refusals, so they stay off the stale-bearer ladder.
const char* HTTPClientSIM::probeTls(const char* host) {
  if (m_tls_verdict) return m_tls_verdict;
  auto caopen = [&](bool ssl) {
    sprintf(m_buffer, "AT+CASSLCFG=0,\"SSL\",%d\r", ssl ? 1 : 0);
    sendCommand(m_buffer);
    if (ssl) {
      sendCommand("AT+CASSLCFG=0,\"CRINDEX\",1\r");
      sprintf(m_buffer, "AT+CASSLCFG=0,\"CACERT\",\"%s.p7b\"\r", m_ca_name);
      sendCommand(m_buffer);
    }
    sprintf(m_buffer, "AT+CAOPEN=0,0,\"TCP\",\"%s\",443\r", host);
    int result = -1;
    if (sendCommand(m_buffer, 20000)) {
      const char* p = strstr(m_buffer, "+CAOPEN: 0,");
      if (p) result = atoi(p + 11);
    }
    sendCommand("AT+CACLOSE=0\r", 2000);
    return result;
  };
  const int tcp = caopen(false);
  const int tls = tcp == 0 ? caopen(true) : -1;
  snprintf(m_probe_trace, sizeof(m_probe_trace), " tcp=%d tls=%d", tcp, tls);
  if (tcp != 0 || tls == 0) return 0;  // the network, or a chain that verifies: not a trust verdict
  if (tls < 0) return "tlsfail";       // no answer says nothing about the chain
  m_tls_verdict = tls == 1 ? "noca" : "untrusted";
  return m_tls_verdict;
}

bool HTTPClientSIM::open(const char* host, uint16_t port) {
  // Trace of this open() for the caller to log: activate-request result, bearer before SHCONF, each SHCONN
  // result, bearer at failure, duration. Kept for successes too, so failures have a baseline.
  unsigned long t0 = millis();
  m_trust_refused = false;
  sendCommand("AT+SHDISC\r", 2000); // never adopt a session left open by an earlier image or session
  char req = rvCode(sendCommand("AT+CNACT=0,1\r")); // ERROR here means context 0 was already active
  sendCommand("AT+CACID=0\r");
  // delay(5000);

#if !0 //TESTING
  bool useSSL = ( port == 443 );
  char bo[8];
  bearerState(bo, sizeof(bo));
  char sh[3] = "--";
  if (useSSL && !configureTls(host)) {
    m_state = HTTP_ERROR;
    // A missing or refused bundle is a trust failure; a transient install step is left to the recovery ladder.
    m_trust_refused = !m_ca_len || !strcmp(m_ca_trace, "FAIL:convert") ||
      !strcmp(m_ca_trace, "FAIL:convert(held)");
    recordOpenTrace("tls", req, bo, sh, t0);
    return false;
  }
  sprintf(m_buffer, "AT+SHCONF=\"URL\",\"%s://%s\"\r", useSSL ? "https" : "http", host);
  if (!sendCommand(m_buffer)) {
    recordOpenTrace("shconf", req, bo, sh, t0);
    return false;
  }
  // The modem's maximum (AT+SHCONF=? says 0-350), cumulative over all headers. The signed upload needs
  // nearly all of it, so a session that did not take it is not one to send on.
  if (!sendCommand("AT+SHCONF=\"HEADERLEN\",350\r")) {
    recordOpenTrace("hdrlen", req, bo, sh, t0);
    return false;
  }
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
      recordOpenTrace(0, req, bo, sh, t0);
      return true;
    }
    sendCommand("AT+SHDISC\r", 2000);
    delay(500);
  }
  checkGPS();
  Serial.println(m_buffer);
  m_state = HTTP_ERROR;
  if (useSSL && sh[0] == 'e' && sh[1] == 'e') { // a refused chain reads ee; timeouts are the network
    const char* verdict = probeTls(host);
    if (verdict) sh_step = verdict;
    m_trust_refused = verdict && strcmp(verdict, "tlsfail");
    // Result 1 is the store missing the roots the marker vouches for; 27 is the server, not the store.
    if (verdict && !strcmp(verdict, "noca") && !strcmp(m_ca_trace, "present")) dropCaMarker();
  }
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
  snprintf(m_open_trace, sizeof(m_open_trace), "%s req=%c bo=%s sh=%s%s%s ca=%s t=%.1fs",
    verdict, req, bo, sh, bf, m_probe_trace, m_ca_trace, (millis() - t0) / 1000.0f);
  m_probe_trace[0] = 0;
}

// Only the caller's headers: the signed upload spends nearly all of the modem's header budget. Names,
// values and the budget are checked before anything is sent, and a refused header fails the whole call, so
// a request never goes up with part of its headers - unsigned in particular. The budget is enforced here,
// not left to the modem, because it accepts a few bytes past what SHREQ can then send (see
// SIM7070_HEADER_BUDGET).
// Inside the quoted AT value a quote is sent as \" (measured to arrive as a real quote); a backslash has no
// measured escape, so it is refused along with anything outside printable ASCII.
static bool headerNameOk(const std::string& name) {
  if (name.empty()) return false;
  for (unsigned char c : name)
    if (!c || ( !isalnum(c) && !strchr("!#$%&'*+-.^_`|~", c) )) return false; // RFC 7230 token
  return true;
}

bool HTTPClientSIM::setHeaders(const HeaderVector& headers) {
#if PRINT
  Serial.printf("Setting headers\n");
#endif
  for (const auto& header : headers) {
    if (!headerNameOk(header.first)) return false;
    for (unsigned char c : header.second)
      if (c < 0x20 || c > 0x7e || c == '\\') return false;
  }
  if (headerBytes(headers) > SIM7070_HEADER_BUDGET) return false;
  if (!sendCommand("AT+SHCHEAD\r")) return false;
  for (const auto& header : headers) {
    std::string value;
    for (char c : header.second) {
      if (c == '"') value += '\\';
      value += c;
    }
    const int n = snprintf(m_buffer, RECV_BUF_SIZE, "AT+SHAHEAD=\"%s\",\"%s\"\r", header.first.c_str(),
      value.c_str());
    if (n < 0 || n >= RECV_BUF_SIZE || !sendCommand(m_buffer)) return false;
  }
  return true;
}

bool HTTPClientSIM::close() {
  // Tear down the SH session with AT+SHDISC - the SH-stack disconnect that matches open()'s AT+SHCONN.
  // The old code sent AT+CHTTPSCLSE, which belongs to the SIM7070's *separate* CHTTPS HTTP application and
  // does NOTHING to an SH session. Result: the SH/TLS connection was never actually closed, so it was
  // reused across uploads until the server/NAT silently dropped it. AT+SHSTATE? then still reported "1"
  // (connected) on the half-open socket, so the next SHREQ POST fired into a dead connection and hung the
  // full 15s -> http=0 with an empty body. Closing cleanly each cycle makes the next open() build a genuinely
  // fresh session, which is the fix for the http=0 storms.
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

commandResponse HTTPClientSIM::send(const char* api_page, const char* payload, const int payloadSize) {
  m_transfer_diagnostics = {};
  // Start the transaction clean: leftovers from a previous exchange (a late URC, an unread body
  // tail) would otherwise satisfy this transaction's expected-string matches.
  m_device->xbPurge();

  // Body in DOWNLOAD mode (confirmed against the SIM7070G via standalone test): AT+SHBOD=<len>,<timeout>
  // replies ">", then we stream EXACTLY <len> raw bytes (no trailing CR), then the modem replies OK.
  // (The inline AT+SHBOD="...",<len> form returns ERROR on this modem.) Streaming raw also avoids the
  // 512-byte command buffer limit for the body - it's bounded only by AT+SHCONF "BODYLEN".
  sprintf(m_buffer, "AT+SHBOD=%d,10000\r", payloadSize);
  auto prompt = sendCommand(m_buffer, 3000, ">");
  m_transfer_diagnostics.prompt_result = prompt.returnValue;
  if (!prompt) {   // wait for the download prompt
    // No prompt = the modem REFUSED the body (dead session, or payloadSize over the SHCONF
    // BODYLEN=1024 cap). Writing the payload anyway would feed raw JSON straight into the AT
    // command parser and desynchronize the command stream for everything that follows - this was
    // the failure mode that killed uploads the moment frames got bigger. Abort instead; the
    // caller sees http=0 and runs the normal failed-upload path.
    return { 0, "" };
  }
  m_device->xbWrite(payload, payloadSize);   // exactly payloadSize bytes
  m_transfer_diagnostics.body_ack_result = sendCommand(0, 3000).returnValue;                       // consume the "OK" emitted after the body is received

  // Issue the POST and wait for the unsolicited result line "+SHREQ: \"POST\",<code>,<len>". It arrives
  // AFTER the HTTP round-trip (not in AT+SHREAD=?, which only reports the readable range), so capture it
  // directly here rather than from a later command's buffer. The headers authenticate the request, so
  // the path goes up bare.
  sprintf(m_buffer, "AT+SHREQ=\"%s\",\"3\"\r", api_page);
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
  m_transfer_diagnostics.post_result = req.returnValue;
  auto size = handlePair(req.buffer, "+SHREQ: \"POST\",");   // {http_code, body_len}
  auto diagnosticNumber = [](const char*& s, int& number) {
    while (*s == ' ' || *s == '\t') ++s;
    if (*s < '0' || *s > '9') return false;
    char* end;
    errno = 0;
    const long value = strtol(s, &end, 10);
    if (errno == ERANGE || value > INT_MAX) return false;
    number = (int)value;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    return true;
  };
  const char* post_header = strstr(req.buffer, "+SHREQ: \"POST\",");
  if (post_header) {
    const char* s = post_header + strlen("+SHREQ: \"POST\",");
    int status, length;
    if (diagnosticNumber(s, status) && *s++ == ',' && diagnosticNumber(s, length) &&
        (*s == '\r' || *s == '\n') && status >= 100 && status <= 599) {
      m_transfer_diagnostics.declared_response_length = length;
      m_transfer_diagnostics.captured_response_length = 0;
      m_transfer_diagnostics.response_incomplete = length > 0;
    }
  }
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
  const char* read_header = strstr(response.buffer, "+SHREAD:");
  if (read_header) {
    const char* s = read_header + 8;
    int length;
    if (diagnosticNumber(s, length) && (*s == '\r' || *s == '\n')) m_transfer_diagnostics.shread_length = length;
  }
  auto buf = handleResponse(response.buffer, "+SHREAD");
  m_transfer_diagnostics.captured_response_length = strlen(buf);
  if (m_transfer_diagnostics.declared_response_length >= 0 || m_transfer_diagnostics.shread_length >= 0) {
    m_transfer_diagnostics.response_incomplete =
      m_transfer_diagnostics.captured_response_length < m_transfer_diagnostics.declared_response_length ||
      m_transfer_diagnostics.captured_response_length < m_transfer_diagnostics.shread_length;
  }
#if PRINT
  Serial.printf("Response body: [omitted]\n");
#endif
  return { size.first, buf };
}

bool HTTPClientSIM::send(HTTP_METHOD method, const char* path, bool keepAlive, const char* payload, int payloadSize) {
  m_transfer_diagnostics = {};
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
