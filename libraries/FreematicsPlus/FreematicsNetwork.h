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
};
#endif
