// The real CellWrapper::syncNtp and readClock (extracted by run_clock_range_checks.py) against a scripted UART with
// the FreematicsESP32::xbReceive contract: it returns 1 + the index of the expected string matched, or 0/-1.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

void check(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
unsigned long fake_ms = 0;
unsigned long millis() { return fake_ms; }
struct { template<typename... Args> void printf(const char*, Args...) {} } Serial;
struct Reply { int result; std::string bytes; };
struct UartFake {
    std::deque<Reply> replies;
    std::vector<std::string> writes;
    std::vector<std::vector<std::string>> patterns;
    void xbPurge() {}
    void xbWrite(const char* data) { writes.emplace_back(data); }
    int xbReceive(char* out, int capacity, unsigned timeout, const char** expected = nullptr, int count = 0) {
        patterns.emplace_back();
        for (int i = 0; i < count; ++i) patterns.back().emplace_back(expected[i]);
        fake_ms += 100;
        if (replies.empty()) { out[0] = 0; (void)timeout; return 0; }
        const Reply reply = replies.front(); replies.pop_front();
        const size_t n = std::min(size_t(capacity - 1), reply.bytes.size());
        std::memcpy(out, reply.bytes.data(), n); out[n] = 0;
        return reply.result;
    }
};
#include "cell_ntp_result.inc"
struct CellWrapper {
    UartFake sys;
    bool syncNtp(NtpResult* result = nullptr);
    std::string readClock();
};
#include "cell_ntp.inc"

const std::string kLong = "AT+CNTP=\"pool.ntp.org\",0,0,1\r", kShort = "AT+CNTP=\"pool.ntp.org\",0\r";

CellWrapper modem(std::vector<Reply> replies) {
    CellWrapper cell;
    for (const auto& r : replies) cell.sys.replies.push_back(r);
    return cell;
}

int main() {
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {1, "OK"}, {1, "\r\n+CNTP: 1,\"2026/10/21,10:00:00\"\r\n"}});
        NtpResult r;
        check(cell.syncNtp(&r) && r.code == 1 && r.utc == "2026/10/21,10:00:00" && r.ms > 0,
              "+CNTP: 1 with a time: ok, and the time is returned");
        check(cell.sys.writes.size() == 4 && cell.sys.writes[2] == kLong && cell.sys.writes[3] == "AT+CNTP\r",
              "the long form is sent, then the sync");
    }
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {1, "OK"}, {1, "+CNTP: 1,"}, {1, "\"2026/10/21,10:00:00\"\r\n"}});
        NtpResult r;
        check(cell.syncNtp(&r) && r.utc == "2026/10/21,10:00:00", "a result line split across reads is completed");
        check(cell.sys.patterns.back() == std::vector<std::string>{"\r\n"}, "the rest is read up to the line end");
    }
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {1, "OK"}, {1, "+CNTP: 62\r\n"}});
        NtpResult r;
        check(!cell.syncNtp(&r) && r.code == 62 && r.utc.empty(), "+CNTP: 62 (DNS): not ok, code 62, no time");
    }
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {1, "OK"}, {1, "+CNTP: 1\r\n+CPSI: LTE,Online\r\n"}});
        NtpResult r;
        check(cell.syncNtp(&r) && r.utc.empty(), "a comma on a later line is not taken for the time");
    }
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {2, "ERROR"}, {1, "OK"}, {1, "+CNTP: 1\r\n"}});
        NtpResult r;
        check(cell.syncNtp(&r) && r.code == 1 && r.utc.empty() && cell.sys.writes.size() == 5 &&
              cell.sys.writes[2] == kLong && cell.sys.writes[3] == kShort, "ERROR on the long form falls back to the short one");
    }
    {
        CellWrapper cell = modem({{1, "OK"}, {1, "OK"}, {1, "OK"}});
        NtpResult r;
        check(!cell.syncNtp(&r) && r.code == -1, "no result line: not ok, code -1");
    }
    {
        CellWrapper cell = modem({{1, "\r\n+CCLK: \"26/10/21,12:00:00+08\"\r\n\r\nOK\r\n"}});
        check(cell.readClock() == "26/10/21,12:00:00+08" &&
              cell.sys.patterns.back() == std::vector<std::string>{"OK", "ERROR"},
              "readClock returns the quoted time and stops on OK/ERROR instead of the full timeout");
        CellWrapper none = modem({{2, "\r\nERROR\r\n"}});
        check(none.readClock().empty(), "readClock: ERROR gives an empty time");
    }
    std::cout << "PASS clock modem\n";
}
