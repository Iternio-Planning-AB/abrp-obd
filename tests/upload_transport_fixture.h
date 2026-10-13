#pragma once
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef PRINT
#define PRINT 0
#endif
#define RECV_BUF_SIZE 512
using byte = unsigned char;
unsigned long fake_ms = 1000;
unsigned long millis() { return fake_ms; }
void delay(unsigned long ms) { fake_ms += ms; }
struct SerialStub {
    std::string output;
    void printf(const char* text) { output += text; }
    void println(const char* text) { output += text; output += '\n'; }
    template<typename... Args> void printf(const char* format, Args... args) {
        char text[2048]; std::snprintf(text, sizeof(text), format, args...); output += text;
    }
} Serial;
void check(bool value, const std::string& message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
#include "upload_types.inc"
struct Reply { int result; std::string bytes; };
struct CFreematics {
    std::deque<Reply> replies;
    std::vector<std::string> writes;
    std::vector<unsigned> timeouts;
    std::vector<std::vector<std::string>> patterns;
    int purges = 0;
    void xbPurge() { ++purges; }
    void xbWrite(const char* data, int length = -1) {
        writes.emplace_back(data, length < 0 ? std::strlen(data) : static_cast<size_t>(length));
    }
    int xbReceive(char* out, size_t capacity, unsigned timeout, const char** expected = nullptr, int count = 0) {
        timeouts.push_back(timeout);
        patterns.emplace_back();
        for (int i = 0; i < count; ++i) patterns.back().emplace_back(expected[i]);
        fake_ms += timeout;
        if (replies.empty()) { out[0] = 0; return 0; }
        Reply reply = replies.front(); replies.pop_front();
        const size_t n = std::min(capacity - 1, reply.bytes.size());
        std::memcpy(out, reply.bytes.data(), n); out[n] = 0;
        return reply.result;
    }
};
class CellSIMCOM {
public:
    char storage[RECV_BUF_SIZE] = {};
    char* m_buffer = storage;
    CFreematics* m_device;
    explicit CellSIMCOM(CFreematics* device = nullptr) : m_device(device) {}
    commandResponse sendCommand(const char* cmd, unsigned int timeout = 1000, const char* expected = nullptr);
};
#ifdef TLS_CHECKS
#define HTTP_CONN_TIMEOUT 5000
#define RTC_DATA_ATTR
#include "tls_defines.inc"
enum HTTP_STATES { HTTP_DISCONNECTED, HTTP_CONNECTED, HTTP_SENT, HTTP_ERROR };
#endif
class HTTPClientSIM : public CellSIMCOM {
public:
    using CellSIMCOM::CellSIMCOM;
    commandResponse send(const char* api_page, const char* payload, const int payloadSize, const char* car_token);
    void setCaBundle(const uint8_t* p7b, size_t len);
    bool trustRefused() const;
#include "upload_getter.inc"
#ifdef TLS_CHECKS
    bool open(const char* host = 0, uint16_t port = 0);
    const char* openTrace() { return m_open_trace; }
    bool writeCaFile(const char* ext, const uint8_t* data, size_t len);
    bool ensureCa();
    void dropCaMarker();
    bool configureTls(const char* host);
    const char* probeTls(const char* host);
    int bearerState(char* out, size_t len);
    void recordOpenTrace(const char* fail_step, char req, const char* bo, const char* sh, unsigned long t0);
    void checkGPS() {}
    HTTP_STATES m_state = HTTP_DISCONNECTED;
    std::string m_host;
#include "tls_members.inc"
#endif
private:
#include "upload_member.inc"
};
#include "upload_command.inc"
#include "upload_parse.inc"
#include "upload_send.inc"
#ifdef TLS_CHECKS
bool HTTPClientSIM::trustRefused() const { return m_trust_refused; }
#include "tls_open.inc"
#endif
