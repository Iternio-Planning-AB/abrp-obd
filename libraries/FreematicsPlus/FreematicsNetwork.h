/*************************************************************************
* Telematics Data Logger Class
* Distributed under BSD license
* Developed by Stanley Huang https://www.facebook.com/stanleyhuangyc
*************************************************************************/

#ifndef FREEMATICS_NETWORK
#define FREEMATICS_NETWORK

#include <Arduino.h>
#include <vector>

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "FreematicsBase.h"

#define XBEE_BAUDRATE 115200
#define HTTP_CONN_TIMEOUT 5000

// Root-CA install (HTTPClientSIM::ensureCa). Retries of a failed write per boot; and once CONVERT has
// refused the same bundle twice, how many boots pass between attempts - about a day at the 30-minute
// parked cadence - so a bundle the modem rejects does not rewrite its flash every wake.
#define CA_INSTALL_TRIES 3
#define CA_CONVERT_RETRY_WAKES 48
// AT+CFSWFILE <input time>: 4.3 KB takes ~0.4 s at 115200; kept short because a missed prompt is waited out.
#define CA_INPUT_MS 2000

#define RECV_BUF_SIZE 512

using HeaderVector = std::vector<std::pair<std::string, std::string>>;

typedef enum {
  METHOD_GET = 0,
  METHOD_POST,
} HTTP_METHOD;

typedef enum {
    HTTP_DISCONNECTED = 0,
    HTTP_CONNECTED,
    HTTP_SENT,
    HTTP_ERROR,
} HTTP_STATES;

typedef struct {
    float lat;
    float lng;
    uint8_t year; /* year past 2000, e.g. 15 for 2015 */
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} NET_LOCATION;

struct commandResponse{
  int returnValue;
  const char* buffer;
  bool operator==(const int otherValue) const {
    return returnValue == otherValue;
  }
  operator bool() const {
    // xbReceive: 1 = first expected string matched (OK / the success pattern), 2 = "\r\nERROR"
    // matched, -1 = timeout with unmatched data, 0 = timeout with nothing. Only 1 is success -
    // the old `returnValue != 0` treated ERROR and timeout-with-data as true, which let a bad
    // SIM pass checkSIM (+CME ERROR) and let open() proceed past a rejected SHCONF.
    return returnValue == 1;
  }
};

struct HTTPTransferDiagnostics {
    int prompt_result = -1;
    int body_ack_result = -1;
    int post_result = -1;
    int declared_response_length = -1;
    int shread_length = -1;
    int captured_response_length = -1;
    int8_t response_incomplete = -1;
};

class HTTPClientClass
{
public:
    HTTP_STATES state() { return m_state; }
    uint16_t code() { return m_code; }
protected:
    String genHeader(HTTP_METHOD method, const char* path, bool keepAlive, const char* payload, int payloadSize);
    HTTP_STATES m_state = HTTP_DISCONNECTED;
    uint16_t m_code = 0;
    String m_host;
};

typedef enum {
    CELL_SIM7600 = 0,
    CELL_SIM7070 = 1,
    CELL_SIM5360 = 2
} CELL_TYPE;

class CellSIMCOM
{
public:
    virtual bool begin(CFreematics* device);
    virtual void end();
    virtual bool setup(const char* apn, const char* username = 0, const char* password = 0, unsigned int timeout = 30000);
    virtual bool setGPS(bool on);
    virtual String getIP();
    int RSSI();
    String getOperatorName();
    bool checkSIM(const char* pin = 0);
    virtual String queryIP(const char* host);
    virtual bool getLocation(GPS_DATA** pgd);
    bool check(unsigned int timeout = 0);
    char* getBuffer();
    const char* deviceName() { return m_model; }
    char IMEI[16] = {0};
protected:
    commandResponse sendCommand(const char* cmd, unsigned int timeout = 1000, const char* expected = 0);
    virtual void checkGPS();
    float parseDegree(const char* s);
    char* m_buffer = 0;
    char m_model[12] = {0};
    CFreematics* m_device = 0;
    GPS_DATA* m_gps = 0;
    CELL_TYPE m_type = CELL_SIM7600;
};

class HTTPClientSIM : public HTTPClientClass, public CellSIMCOM
{
public:
    bool setup(const char* apn, unsigned int timeout = 30000);
    void end();
    bool setGPS(bool on);
    void gpsTest();
    bool open(const char* host = 0, uint16_t port = 0);
    void setHeaders(const HeaderVector &headers);
    bool close();
    bool send(HTTP_METHOD method, const char* path, bool keepAlive, const char* payload = 0, int payloadSize = 0);
    commandResponse send(const char* api_page, const char* payload, const int payloadSize, const char* car_token);
    commandResponse getEpoch(const char* api_page);
    char* receive(int* pbytes = 0, unsigned int timeout = HTTP_CONN_TIMEOUT);
    // Outcome, step results and bearer state of the last open(); see open() for the fields.
    const char* openTrace() { return m_open_trace; }
    const HTTPTransferDiagnostics& transferDiagnostics() const { return m_transfer_diagnostics; }
    // Root CAs (DER PKCS#7) the modem verifies port-443 servers against. Without them open() refuses
    // TLS rather than connecting unverified. The bytes must outlive the client.
    void setCaBundle(const uint8_t* p7b, size_t len);
    // The last open() failed on trust (no or refused bundle, or a chain the roots refuse), not the network.
    bool trustRefused() const { return m_trust_refused; }
private:
    enum CaState : uint8_t { CA_UNCHECKED, CA_READY, CA_FAILED };
    HTTPTransferDiagnostics m_transfer_diagnostics;
    int bearerState(char* out, size_t len);
    bool writeCaFile(const char* ext, const uint8_t* data, size_t len);
    bool ensureCa();
    void dropCaMarker();
    bool configureTls(const char* host);
    const char* probeTls(const char* host);
    void recordOpenTrace(const char* fail_step, char req, const char* bo, const char* sh, unsigned long t0);
    char m_open_trace[128] = {0};
    const uint8_t* m_ca = nullptr;
    size_t m_ca_len = 0;
    uint32_t m_ca_hash = 0;
    CaState m_ca_state = CA_UNCHECKED;
    uint8_t m_ca_tries = 0;
    bool m_ca_dropped = false;
    char m_ca_name[11] = {0};         // "rc" + 8 hex of the bundle; files are <name>.p7b and <name>.ok
    const char* m_ca_trace = "none";  // this boot's install outcome, or the step that failed
    const char* m_tls_verdict = nullptr;
    bool m_trust_refused = false;
    char m_probe_trace[20] = {0};
};
#endif
