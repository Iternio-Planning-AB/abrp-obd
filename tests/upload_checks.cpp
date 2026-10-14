#include "upload_transport_fixture.h"

void bodyAckRed() {
    CFreematics uart;
    uart.replies = {{1, ">"}, {2, "\r\nERROR\r\n"}, {1, "+SHREQ: \"POST\",200,0\r\n"}};
    HTTPClientSIM client(&uart);
    const auto response = client.send("/2/app/tlm", "abc", 3);
    check(response.returnValue == 200 && uart.writes.size() == 3, "real send still POSTs despite missing body ACK");
    check(client.transferDiagnostics().body_ack_result == 2, "real send records discarded body ACK ERROR (expected 2, got " + std::to_string(client.transferDiagnostics().body_ack_result) + ")");
}

void stages() {
    for (int prompt : {0, 2, -1}) {
        CFreematics uart;
        uart.replies = {{prompt, "\r\nERROR\r\n"}};
        HTTPClientSIM client(&uart);
        const auto response = client.send("/2/app/tlm", "abc", 3);
        const auto d = client.transferDiagnostics();
        check(response.returnValue == 0 && std::string(response.buffer).empty(), "prompt refusal baseline result");
        check(uart.writes == std::vector<std::string>{"AT+SHBOD=3,10000\r"} && uart.purges == 1, "no POST or payload without prompt");
        check(d.prompt_result == (byte)prompt && d.body_ack_result == -1 && d.post_result == -1, "prompt result exact, later stages unknown");
        check(d.declared_response_length == -1 && d.captured_response_length == -1 && d.response_incomplete == -1, "no invented HTTP body or completeness");
        check(uart.timeouts == std::vector<unsigned>{3000} && uart.patterns[0] == std::vector<std::string>{">"}, "baseline prompt wait");
    }
    for (int ack : {0, 1, 2, -1}) {
        for (int code : {0, 200, 400, 401, 403, 404, 500}) {
            for (size_t length : {0, 1, 511, 512, 1023, 1024, 1025}) {
                CFreematics uart;
                const int post = code ? 1 : 0;
                uart.replies = {{1, ">"}, {ack, ack == 1 ? "\r\nOK" : "\r\nERROR"},
                    {post, code ? "+SHREQ: \"POST\"," + std::to_string(code) + ",0\r\n" : ""}};
                HTTPClientSIM client(&uart);
                std::string payload(length, 'x');
                for (size_t i = 0; i < length; ++i) payload[i] = static_cast<char>(i % 256);
                const auto response = client.send("/2/app/tlm", payload.data(), payload.size());
                const auto d = client.transferDiagnostics();
                check(response.returnValue == code && std::string(response.buffer).empty(), "POST acceptance unchanged even without body ACK");
                check(uart.writes == std::vector<std::string>{"AT+SHBOD=" + std::to_string(length) + ",10000\r", payload, "AT+SHREQ=\"/2/app/tlm\",\"3\"\r"}, "baseline exact AT sequence and binary payload at size boundary");
                check(uart.timeouts == std::vector<unsigned>{3000, 3000, 8000}, "no new waits or AT commands");
                check(uart.patterns == std::vector<std::vector<std::string>>{{">"}, {"\r\nOK", "\r\nERROR"}, {"+SHREQ:"}}, "baseline expected matches");
                check(d.prompt_result == 1 && d.body_ack_result == (byte)ack && d.post_result == post, "factual prompt ACK and POST fields");
                check(d.declared_response_length == (code ? 0 : -1) && d.captured_response_length == (code ? 0 : -1) &&
                      d.response_incomplete == (code ? 0 : -1), "measured empty response differs from unknown response");
                uart.replies = {{0, ""}};
                client.send("/2/app/tlm", "x", 1);
                const auto reset = client.transferDiagnostics();
                check(reset.prompt_result == 0 && reset.body_ack_result == -1 && reset.declared_response_length == -1 && reset.response_incomplete == -1, "all diagnostics reset on next send");
            }
        }
    }
}

void responses() {
    struct Case { int declared, read_length, result; std::string burst, tail, expected; bool cut; };
    const std::vector<Case> cases = {
        {3, 3, 1, "+SHREAD: 3\r\nabc\r\nOK", "", "abc", false},
        {3, 3, 1, "+SHREAD: 3\r\na", "bc", "abc", false},
        {10, 10, 1, "+SHREAD: 10\r\nabc", "", "abc", true},
        {10, -1, 0, "", "", "", true},
        {10, 10, 2, "+SHREAD: 10\r\nabc", "", "abc", true},
        {3, 3, 1, std::string("+SHREAD: 3\r\n") + std::string("a\0b", 3), "", "a", true},
        {600, 496, 1, "+SHREAD: 496\r\n" + std::string(496, 'x'), "", std::string(496, 'x'), true},
        {600, 600, 1, "+SHREAD: 600\r\n" + std::string(600, 'x'), "", std::string(497, 'x'), true},
        {496, 496, 1, "+SHREAD: 496\r\n" + std::string(496, 'x'), "", std::string(496, 'x'), false},
        {3, 3, 1, "+SHREAD: 3\r\n\r\n\1", "", "\r\n\1", false},
        {3, -1, 2, "+SHREAD: bad\r\nabc", "", "", true},
        {3, -1, 2, "+SHREAD: -1\r\nabc", "", "", true},
        {3, -1, 2, "+SHREAD: 3junk\r\nabc", "", "abc", false},
        {3, -1, 2, "+SHREAD: 2147483648\r\nabc", "", "", true},
        {3, -1, 2, "+SHREAD: 999999999999999999999999\r\nabc", "", "", true},
    };
    for (const auto& c : cases) {
        CFreematics uart;
        uart.replies = {{1, ">"}, {0, ""}, {1, "+SHREQ: \"POST\",400," + std::to_string(c.declared) + "\r\n"}, {c.result, c.burst}};
        if (!c.tail.empty()) uart.replies.push_back({0, c.tail});
        HTTPClientSIM client(&uart);
        const auto response = client.send("/2/app/tlm", "abc", 3);
        const auto d = client.transferDiagnostics();
        check(response.returnValue == 400 && std::string(response.buffer) == c.expected, "baseline body extraction for " + c.burst.substr(0, 48) + " expected size=" + std::to_string(c.expected.size()) + " actual size=" + std::to_string(strlen(response.buffer)));
        check(d.declared_response_length == c.declared && d.shread_length == c.read_length && d.captured_response_length == (int)c.expected.size() && d.response_incomplete == c.cut, "declared SHREAD captured length and incomplete metadata");
        check(d.body_ack_result == 0, "response read still happens after absent ACK");
        check(uart.writes.size() == 4 && uart.writes.back() == "AT+SHREAD=0," + std::to_string(std::min(c.declared, 496)) + "\r", "baseline capped read with no extra AT");
        check(uart.timeouts[3] == 5000 && uart.patterns[3] == std::vector<std::string>{"+SHREAD:"}, "baseline read wait");
    }
    CFreematics uart;
    uart.replies = {{1, ">"}, {1, "\r\nOK"}, {1, "+SHREQ: \"POST\",200,"}, {1, "0\r\n"}};
    HTTPClientSIM client(&uart);
    check(client.send("/2/app/tlm", "x", 1).returnValue == 200, "split POST URC still topped up");
    check(uart.writes.back() == "AT+SHREQ=\"/2/app/tlm\",\"3\"\r" && uart.timeouts.back() == 1000, "bare path, no token in the URL; topup unchanged");
    check(client.transferDiagnostics().declared_response_length == 0 && client.transferDiagnostics().response_incomplete == 0, "POST metadata captured after topup");
}
void unknownMetadata() {
    for (const auto& reply : {"", "\r\nERROR\r\n", "+SHREQ: \"POST\",200,broken\r\n",
                             "+SHREQ: \"POST\",200,-1\r\n", "+SHREQ: \"POST\",200,0junk\r\n",
                             "+SHREQ: \"POST\",200,0", "+SHREQ: \"POST\",99,0\r\n",
                             "+SHREQ: \"POST\",600,0\r\n", "+SHREQ: \"POST\",200,2147483648\r\n",
                             "+SHREQ: \"POST\",200,999999999999999999999999\r\n"}) {
        CFreematics uart;
        uart.replies = {{1, ">"}, {1, "\r\nOK"}, {0, reply}};
        HTTPClientSIM client(&uart);
        client.send("/2/app/tlm", "abc", 3);
        const auto d = client.transferDiagnostics();
        check(d.declared_response_length == -1 && d.captured_response_length == -1,
              "missing or invalid SHREQ metadata remains unknown, not measured empty");
        check(static_cast<int>(d.response_incomplete) == -1, "missing response cannot be labelled complete");
    }
}
int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "unknown") unknownMetadata();
    if (group == "all" || group == "body-ack") bodyAckRed();
    if (group == "all" || group == "stages") stages();
    if (group == "all" || group == "responses") responses();
    check(group == "all" || group == "body-ack" || group == "stages" || group == "responses" || group == "unknown", "known upload group");
    std::cout << "PASS upload " << group << '\n';
}
