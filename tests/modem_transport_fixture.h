#pragma once
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using byte = unsigned char;
using String = std::string;
using HeaderVector = std::vector<std::pair<std::string, std::string>>;
inline uint64_t fake_ms = 1000;
inline bool overshoot_delays = false;
inline uint32_t millis() { return static_cast<uint32_t>(fake_ms); }
inline void delay(unsigned long ms) { fake_ms += ms + (overshoot_delays && ms ? 1 : 0); }

struct SerialStub {
    std::string output;
    std::vector<std::pair<uint64_t, std::string>> messages;
    void printf(const char* text) { output += text; messages.emplace_back(fake_ms, text); }
    void flush() {}
    template<typename... Args> void printf(const char* format, Args... args) {
        char text[2048];
        std::snprintf(text, sizeof(text), format, args...);
        printf(text);
    }
};
inline SerialStub Serial;

#include "modem_types.inc"
#define RECV_BUF_SIZE 512
enum CELL_TYPE { CELL_SIM7600, CELL_SIM7070, CELL_SIM5360 };
enum HTTP_STATES { HTTP_DISCONNECTED, HTTP_CONNECTED };

struct ModemEvent {
    uint64_t ms;
    std::string command;
    unsigned timeout;
};

struct ModemUart {
    bool alive = true;
    bool can_power_on = true;
    bool online_without_registration = false;
    uint64_t attach_at = std::numeric_limits<uint64_t>::max();
    uint64_t detach_at = std::numeric_limits<uint64_t>::max();
    unsigned data_reply_ms = 5;
    int csq = 15;
    char domain = 'E';
    char registered_state = '5';
    std::set<std::string> silent;
    std::vector<ModemEvent> events;
    std::vector<unsigned> power_pulses;
    std::vector<std::string> accepted;
    size_t max_events = std::numeric_limits<size_t>::max();
    std::string last_command;

    bool registered() const { return fake_ms >= attach_at && fake_ms < detach_at; }
    std::string reply() const {
        if (!alive || silent.count(last_command)) return "";
        if (last_command == "ATI\r") return "SIMCOM\r\nOK\r\n";
        if (last_command == "AT+SIMCOMATI\r") return "QCN:SIM7070_REV\r\nOK\r\n";
        if (last_command == "AT+CPIN?\r") return "+CPIN: READY\r\nOK\r\n";
        if (last_command == "AT+CNMP?\r") return "+CNMP: 51\r\nOK\r\n";
        if (last_command == "AT+CMNB?\r") return "+CMNB: 1\r\nOK\r\n";
        if (last_command == "AT+CBANDCFG?\r") return "+CBANDCFG: \"CAT-M\",1,3,8,20,28\r\nOK\r\n";
        if (last_command == "AT+CPSI?\r")
            return registered() || online_without_registration ? "+CPSI: LTE CAT-M1,Online\r\nOK\r\n" : "+CPSI: NO SERVICE\r\nOK\r\n";
        for (const auto& pair : std::vector<std::pair<std::string, char>>{
                 {"AT+CEREG?\r", 'E'}, {"AT+CGREG?\r", 'G'}, {"AT+CREG?\r", 'C'}}) {
            if (last_command == pair.first)
                return last_command.substr(2, last_command.size() - 4) + ": 0," +
                       (registered() && domain == pair.second ? registered_state : '2') + "\r\nOK\r\n";
        }
        if (last_command == "AT+CSQ\r") return "+CSQ: " + std::to_string(csq) + ",99\r\nOK\r\n";
        if (last_command == "AT+CBC\r") return "+CBC: 4.05\r\nOK\r\n";
        return "\r\nOK\r\n";
    }
};

struct CFreematics {
    std::shared_ptr<ModemUart> uart = std::make_shared<ModemUart>();
    void xbPurge() {}
    void xbTogglePower(unsigned duration = 200) {
        uart->power_pulses.push_back(duration);
        delay(duration);
        if (duration < 1000 && uart->can_power_on) uart->alive = true;
    }
    void xbWrite(const char* command) {
        if (uart->events.size() >= uart->max_events) {
            std::fputs("FAIL: modem exceeded the scenario's bounded UART command count\n", stderr);
            std::exit(1);
        }
        uart->last_command = command;
        uart->events.push_back({fake_ms, command, 0});
    }
    int xbReceive(char* out, size_t capacity, unsigned timeout, const char** expected = nullptr, int count = 0) {
        uart->events.push_back({fake_ms, uart->last_command, timeout});
        const bool data = uart->last_command.find("CGDCONT=") != std::string::npos ||
                          uart->last_command.find("CNACT=") != std::string::npos;
        const unsigned response_ms = data ? uart->data_reply_ms : 5U;
        const std::string response = uart->reply();
        int result = response.empty() ? 0 : -1;
        if (response_ms <= timeout) {
            for (int i = 0; i < count; ++i) {
                if (response.find(expected[i]) != std::string::npos) {
                    result = i + 1;
                    break;
                }
            }
        }
        const unsigned elapsed = result > 0 ? response_ms : timeout;
        if (result > 0) uart->accepted.push_back(uart->last_command);
        fake_ms += elapsed;
        const std::string received = response_ms <= timeout ? response : "";
        const size_t n = std::min(capacity - 1, received.size());
        std::memcpy(out, received.data(), n);
        out[n] = 0;
        return result;
    }
};
struct FreematicsESP32 : CFreematics {};

class CellSIMCOM {
public:
    virtual ~CellSIMCOM() = default;
    bool begin(CFreematics* device);
    virtual void end();
    virtual bool setGPS(bool) { return false; }
    bool check(unsigned int timeout = 0);
    bool checkSIM(const char* pin = nullptr);
    char* getBuffer() { return m_buffer; }
    char IMEI[16] = {};
protected:
    commandResponse sendCommand(const char* command, unsigned timeout = 1000, const char* expected = nullptr);
    char storage[RECV_BUF_SIZE] = {};
    char* m_buffer = storage;
    char m_model[12] = {};
    CFreematics* m_device = nullptr;
    CELL_TYPE m_type = CELL_SIM7600;
};

inline bool g_fixture_session_opens = false; // a test sets it for the connect() that must succeed
class HTTPClientSIM : public CellSIMCOM {
public:
    bool setup(const char* apn, unsigned int timeout = 30000);
    void end() override;
    HTTP_STATES state() const { return HTTP_DISCONNECTED; }
    bool open(const char*, unsigned) { return g_fixture_session_opens; }
    bool close() { return true; }
    bool setHeaders(const HeaderVector&) { return false; }
    void setCaBundle(const uint8_t*, size_t) {}
    bool trustRefused() const { return false; }
    const char* openTrace() const { return ""; }
    HTTPTransferDiagnostics transferDiagnostics() const { return {}; }
    commandResponse send(const char*, const char*, size_t) { return {0, ""}; }
    commandResponse getEpoch(const char*) { return {0, ""}; }
};
