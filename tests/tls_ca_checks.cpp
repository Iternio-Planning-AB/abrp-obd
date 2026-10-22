#define TLS_CHECKS 1
#include "upload_transport_fixture.h"
#include "../baseLogger/Helpers/server_roots.h"

namespace {
const Reply OK{1, "\r\nOK\r\n"};
const Reply ERR{2, "\r\nERROR\r\n"};
const Reply TIMEOUT{0, ""};
const Reply PROMPT{1, "\r\nDOWNLOAD\r\n"};
const Reply PRESENT{1, "\r\n+CFSGFIS: 1\r\n\r\nOK\r\n"};
const Reply BEARER{1, "\r\n+CNACT: 0,1,\"10.1.2.3\"\r\n\r\nOK\r\n"};
const Reply CONNECTED{1, "\r\n+SHSTATE: 1\r\n\r\nOK\r\n"};
Reply caopen(int result) { return {1, "\r\n+CAOPEN: 0," + std::to_string(result) + "\r\n\r\nOK\r\n"}; }

const std::string BUNDLE(reinterpret_cast<const char*>(SERVER_ROOTS_P7B), SERVER_ROOTS_LEN);
using Replies = std::vector<Reply>;

#define BUNDLED(client, uart) HTTPClientSIM client(&uart); client.setCaBundle(SERVER_ROOTS_P7B, SERVER_ROOTS_LEN)

void newBoot() { s_ca_refused_hash = 0; s_ca_refusals = 0; s_ca_held_wakes = 0; }

void push(CFreematics& uart, const Replies& replies) { for (const Reply& r : replies) uart.replies.push_back(r); }

bool any(const std::vector<std::string>& writes, const std::string& needle) {
    for (const auto& w : writes) if (w.find(needle) != std::string::npos) return true;
    return false;
}

void noUnverifiedContext(const CFreematics& uart) {
    check(!any(uart.writes, "AT+SHSSL=1,\"\""), "the empty-CA (unverified) SSL context is never selected");
}

// Replies for each phase of an open: SHDISC, CNACT, CACID, bearer | install check | SSL context | SHCONF x3.
const Replies OPEN_HEAD = {OK, OK, OK, BEARER};
const Replies MARKER_PRESENT = {OK, OK, PRESENT, OK};
const Replies INSTALL = {OK, OK, ERR, PROMPT, OK, OK, PROMPT, OK, OK};
const Replies TLS_CONTEXT = {OK, OK, OK, OK};
const Replies SHCONF = {OK, OK, OK};
const Replies SHCONN_REFUSED = {ERR, OK, OK, ERR, OK, OK}; // SHCONN, SHSTATE (not 1), SHDISC, twice

void naming() {
    HTTPClientSIM client;
    client.setCaBundle(reinterpret_cast<const uint8_t*>("a"), 1);
    check(std::string(client.m_ca_name) == "rce40c292c", "bundle name is rc + FNV-1a-32 of the bytes (FNV(\"a\") = e40c292c)");
    const uint8_t nul[] = {'a', 0, 'b'};
    client.setCaBundle(nul, sizeof(nul));
    check(std::string(client.m_ca_name) != "rce40c292c", "the hash covers bytes past a NUL (DER is binary)");
    client.setCaBundle(SERVER_ROOTS_P7B, SERVER_ROOTS_LEN);
    check(std::strlen(client.m_ca_name) + 4 <= 20, "file name fits AT+SHSSL's 20-character <ca list>");
}

void present() {
    newBoot();
    CFreematics uart;
    push(uart, MARKER_PRESENT);
    push(uart, TLS_CONTEXT);
    BUNDLED(client, uart);
    const std::string name = client.m_ca_name;
    check(client.configureTls("api.iternio.com"), "installed bundle configures TLS");
    check(!any(uart.writes, "CFSWFILE") && !any(uart.writes, "CONVERT"), "marker present: nothing written or converted");
    check(uart.writes[2] == "AT+CFSGFIS=3,\"" + name + ".ok\"\r" && std::string(client.m_ca_trace) == "present", "presence is the marker");
    check(any(uart.writes, "AT+SHSSL=1,\"" + name + ".p7b\"\r") && any(uart.writes, "AT+CSSLCFG=\"sni\",1,\"api.iternio.com\"\r"),
          "verified context and SNI selected");
    uart.writes.clear();
    push(uart, TLS_CONTEXT);
    check(client.configureTls("api.iternio.com") && !any(uart.writes, "AT+CFS"), "later opens this boot skip the filesystem");
    noUnverifiedContext(uart);
}

void install() {
    newBoot();
    CFreematics uart;
    push(uart, INSTALL);
    push(uart, TLS_CONTEXT);
    BUNDLED(client, uart);
    const std::string name = client.m_ca_name;
    check(client.configureTls("api.iternio.com"), "missing bundle is installed and TLS configured");
    const std::vector<std::string> expected = {
        "AT+CFSTERM\r", "AT+CFSINIT\r", "AT+CFSGFIS=3,\"" + name + ".ok\"\r",
        "AT+CFSWFILE=3,\"" + name + ".p7b\",0," + std::to_string(SERVER_ROOTS_LEN) + ",2000\r", BUNDLE,
        "AT+CSSLCFG=\"CONVERT\",2,\"" + name + ".p7b\"\r",
        "AT+CFSWFILE=3,\"" + name + ".ok\",0,1,2000\r", "1", "AT+CFSTERM\r"};
    check(std::vector<std::string>(uart.writes.begin(), uart.writes.begin() + 9) == expected,
          "exact install sequence: whole binary bundle, convert, then marker");
    const std::vector<unsigned> waits(uart.timeouts.begin(), uart.timeouts.begin() + 9);
    check(waits == std::vector<unsigned>{1000, 1000, 1000, 3000, 5000, 10000, 3000, 5000, 1000},
          "install timeouts: 3 s prompt, input window + 3 s ACK, 10 s CONVERT");
    check(uart.patterns[3] == std::vector<std::string>{"DOWNLOAD"} && uart.patterns[6] == std::vector<std::string>{"DOWNLOAD"},
          "file bytes are sent only after the DOWNLOAD prompt");
    check(std::string(client.m_ca_trace) == "loaded", "trace says loaded");

    newBoot();
    CFreematics bare;
    push(bare, {OK, OK, OK, PROMPT});
    BUNDLED(other, bare);
    other.configureTls("api.iternio.com");
    check(any(bare.writes, "CFSWFILE=3,\"" + name + ".p7b\""), "a CFSGFIS that answers a bare OK is absent: install");
}

void transientFailures() {
    struct Case { Replies replies; const char* trace; };
    const std::vector<Case> cases = {
        {{OK, ERR}, "FAIL:cfsinit"},
        {{OK, OK, TIMEOUT}, "FAIL:gfis"},
        {{OK, OK, ERR, TIMEOUT}, "FAIL:write"},
        {{OK, OK, ERR, PROMPT, TIMEOUT}, "FAIL:write"},
        {{OK, OK, ERR, PROMPT, OK, TIMEOUT}, "FAIL:convert_to"},
        {{OK, OK, ERR, PROMPT, OK, OK, ERR}, "FAIL:marker"},
    };
    for (const auto& c : cases) {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        for (int attempt = 0; attempt < CA_INSTALL_TRIES; ++attempt) {
            uart.writes.clear();
            uart.replies.clear();
            push(uart, c.replies);
            push(uart, {OK}); // closing CFSTERM
            const unsigned long before = fake_ms;
            const size_t waits_before = uart.timeouts.size();
            check(!client.configureTls("api.iternio.com") && std::string(client.m_ca_trace) == c.trace,
                  std::string("transient install failure refuses TLS: ") + c.trace);
            check(!any(uart.writes, "AT+SHSSL") && uart.writes.back() == "AT+CFSTERM\r", "no SSL context; buffer released");
            if (!std::strcmp(c.trace, "FAIL:gfis")) check(!any(uart.writes, "CFSWFILE"), "an unanswered CFSGFIS is not absent");
            if (!std::strcmp(c.trace, "FAIL:write") || !std::strcmp(c.trace, "FAIL:marker")) {
                unsigned long receive_ms = 0;
                for (size_t i = waits_before; i < uart.timeouts.size(); ++i) receive_ms += uart.timeouts[i];
                check(fake_ms - before - receive_ms >= CA_INPUT_MS + 500 && uart.purges > 0,
                      "a missed prompt or ACK is waited out and purged");
            }
            if (!std::strcmp(c.trace, "FAIL:gfis") || !std::strcmp(c.trace, "FAIL:write") || !std::strcmp(c.trace, "FAIL:cfsinit"))
                check(!any(uart.writes, ".ok\",0,1"), "no marker without a successful CONVERT");
            if (!std::strcmp(c.trace, "FAIL:convert_to")) check(s_ca_refusals == 0, "a CONVERT timeout is not a refusal");
        }
        uart.writes.clear();
        check(!client.configureTls("api.iternio.com") && uart.writes.empty(), "transient failures stop after CA_INSTALL_TRIES per boot");
    }
}

void convertBackoff() {
    newBoot();
    const Replies refused = {OK, OK, ERR, PROMPT, OK, ERR, OK};
    for (int wake = 0; wake < 2; ++wake) {
        CFreematics uart;
        push(uart, refused);
        BUNDLED(client, uart);
        check(!client.configureTls("api.iternio.com") && std::string(client.m_ca_trace) == "FAIL:convert", "CONVERT refusal");
        uart.writes.clear();
        check(!client.configureTls("api.iternio.com") && uart.writes.empty(), "a CONVERT refusal latches for the boot");
    }
    for (int wake = 1; wake < CA_CONVERT_RETRY_WAKES; ++wake) {
        CFreematics uart;
        BUNDLED(client, uart);
        check(!client.configureTls("api.iternio.com") && uart.writes.empty() &&
              std::string(client.m_ca_trace) == "FAIL:convert(held)", "after two refusals later wakes do not rewrite the flash");
    }
    CFreematics retry;
    push(retry, INSTALL);
    push(retry, TLS_CONTEXT);
    BUNDLED(client, retry);
    check(client.configureTls("api.iternio.com") && any(retry.writes, "CONVERT"), "retried once CA_CONVERT_RETRY_WAKES pass");
    check(s_ca_refusals == 0, "a successful CONVERT clears the refusal count");

    newBoot();
    s_ca_refused_hash = 1;
    s_ca_refusals = 2;
    CFreematics changed;
    push(changed, INSTALL);
    BUNDLED(fresh, changed);
    fresh.configureTls("api.iternio.com");
    check(any(changed.writes, "CONVERT"), "refusals of another bundle do not hold this one");
}

void noBundle() {
    for (int variant = 0; variant < 3; ++variant) {
        CFreematics uart;
        HTTPClientSIM client(&uart);
        if (variant == 1) client.setCaBundle(nullptr, 100);
        if (variant == 2) client.setCaBundle(SERVER_ROOTS_P7B, 0);
        check(!client.configureTls("api.iternio.com") && uart.writes.empty(), "no bundle: TLS refused without touching the modem");
    }
}

void openRefusesTls() {
    newBoot();
    CFreematics uart;
    push(uart, OPEN_HEAD);
    push(uart, {BEARER});
    HTTPClientSIM client(&uart);
    check(!client.open("api.iternio.com", 443), "open() fails closed without trust anchors");
    check(uart.writes[0] == "AT+SHDISC\r" && uart.timeouts[0] == 2000, "SHDISC first: no session from an earlier image is adopted");
    check(!any(uart.writes, "AT+SHCONF") && !any(uart.writes, "AT+SHCONN") && !any(uart.writes, "AT+SHSSL"),
          "no session is configured or connected without verification");
    check(std::string(client.openTrace()).rfind("tls ", 0) == 0 && client.trustRefused(), "no bundle is a trust failure");

    CFreematics flaky;
    push(flaky, OPEN_HEAD);
    push(flaky, {OK, ERR, OK, BEARER});
    BUNDLED(transient, flaky);
    check(!transient.open("api.iternio.com", 443) && !transient.trustRefused(),
          "a transient install failure is left to the recovery ladder");

    CFreematics refused;
    push(refused, OPEN_HEAD);
    push(refused, {OK, OK, ERR, PROMPT, OK, ERR, OK, BEARER});
    BUNDLED(convert, refused);
    check(!convert.open("api.iternio.com", 443) && convert.trustRefused(), "a CONVERT refusal is a trust failure");
    push(refused, OPEN_HEAD);
    push(refused, {BEARER});
    check(!convert.open("api.iternio.com", 443) && convert.trustRefused(), "so is the latched refusal on later opens");

    newBoot();
    CFreematics slow;
    push(slow, OPEN_HEAD);
    push(slow, {OK, OK, ERR, PROMPT, OK, TIMEOUT, OK, BEARER});
    BUNDLED(timed_out, slow);
    check(!timed_out.open("api.iternio.com", 443) && !timed_out.trustRefused(),
          "a CONVERT timeout is left to the recovery ladder");
}

int g_http0_streak = 0;

void openFailStreak(const HTTPClientSIM& network) {
#include "open_streak.inc"
}

void openStreak() {
    newBoot();
    CFreematics uart;
    push(uart, OPEN_HEAD);
    push(uart, {BEARER});
    HTTPClientSIM unbundled(&uart);
    g_http0_streak = 2;
    unbundled.open("api.iternio.com", 443);
    openFailStreak(unbundled);
    check(g_http0_streak == 2, "a refused certificate leaves the http=0 streak alone");

    CFreematics flaky;
    push(flaky, OPEN_HEAD);
    push(flaky, {OK, ERR, OK, BEARER});
    BUNDLED(transient, flaky);
    transient.open("api.iternio.com", 443);
    openFailStreak(transient);
    check(g_http0_streak == 3, "any other failed open counts toward it");
}

void openVerified() {
    newBoot();
    CFreematics uart;
    push(uart, OPEN_HEAD);
    push(uart, MARKER_PRESENT);
    push(uart, TLS_CONTEXT);
    push(uart, SHCONF);
    push(uart, {OK, CONNECTED});
    BUNDLED(client, uart);
    const std::string name = client.m_ca_name;
    check(client.open("api.iternio.com", 443), "verified open succeeds");
    const auto at = [&](const std::string& w) { return std::find(uart.writes.begin(), uart.writes.end(), w) - uart.writes.begin(); };
    check(at("AT+SHSSL=1,\"" + name + ".p7b\"\r") < at("AT+SHCONN\r"), "verified context selected before SHCONN");
    check(!any(uart.writes, "AT+CAOPEN") && !client.trustRefused(), "a successful open runs no probe");
    check(std::string(client.openTrace()).find(" ca=present ") != std::string::npos, "trace carries the CA state");
    noUnverifiedContext(uart);
}

// A first open with the marker present whose SHCONN is refused, then the probe, then the trace's bearer read.
std::string refusedOpen(HTTPClientSIM& client, CFreematics& uart, const Replies& install, const Replies& shconn,
                        const Replies& probe) {
    uart.writes.clear();
    uart.replies.clear();
    push(uart, OPEN_HEAD);
    push(uart, install);
    push(uart, TLS_CONTEXT);
    push(uart, SHCONF);
    push(uart, shconn);
    push(uart, probe);
    push(uart, {BEARER});
    check(!client.open("api.iternio.com", 443), "refused SHCONN fails the open");
    noUnverifiedContext(uart);
    return client.openTrace();
}

const Replies PROBE_UNTRUSTED = {OK, caopen(0), OK, OK, OK, OK, caopen(27), OK};
const Replies MARKER_DROP = {OK, OK, OK, OK}; // CFSTERM, CFSINIT, CFSDFILE, CFSTERM

void probe() {
    {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        const std::string name = client.m_ca_name;
        const std::string trace = refusedOpen(client, uart, MARKER_PRESENT, SHCONN_REFUSED, PROBE_UNTRUSTED);
        check(trace.rfind("untrusted ", 0) == 0 && trace.find(" tcp=0 tls=27 ") != std::string::npos && client.trustRefused(),
              "a reachable server whose chain fails the roots is untrusted: " + trace);
        check(any(uart.writes, "AT+CASSLCFG=0,\"CACERT\",\"" + name + ".p7b\"\r"), "probe TLS uses the same bundle");
        const auto caopens = std::count(uart.writes.begin(), uart.writes.end(), "AT+CAOPEN=0,0,\"TCP\",\"api.iternio.com\",443\r");
        check(caopens == 2, "probe: plain TCP, then TLS");
        for (size_t i = 0; i < uart.writes.size(); ++i)
            if (uart.writes[i].rfind("AT+CAOPEN", 0) == 0) check(uart.timeouts[i] == 20000, "CAOPEN waits 20 s");
        check(!any(uart.writes, "CFSDFILE") && trace.find(" ca=present ") != std::string::npos,
              "an untrusted server keeps the marker: reinstalling cannot fix it and would rewrite flash each wake");

        // The second refusal this boot is not probed, and nothing is reinstalled.
        const std::string again = refusedOpen(client, uart, {}, SHCONN_REFUSED, {});
        check(!any(uart.writes, "AT+CAOPEN") && !any(uart.writes, "CFSWFILE") && !any(uart.writes, "CFSDFILE"),
              "one probe per boot, no reinstall: " + again);
        check(again.rfind("untrusted ", 0) == 0 && client.trustRefused(), "the boot's verdict stands for later refusals: " + again);
    }
    {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        Replies probe = {OK, caopen(0), OK, OK, OK, OK, caopen(1), OK};
        for (const Reply& r : MARKER_DROP) probe.push_back(r);
        const std::string trace = refusedOpen(client, uart, MARKER_PRESENT, SHCONN_REFUSED, probe);
        check(trace.rfind("noca ", 0) == 0 && client.trustRefused() && any(uart.writes, "CFSDFILE"),
              "a CA the store does not hold (result 1) is a stale marker: " + trace);
    }
    {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        const std::string trace = refusedOpen(client, uart, MARKER_PRESENT, SHCONN_REFUSED,
                                              {OK, caopen(0), OK, OK, OK, OK, TIMEOUT, OK});
        check(trace.rfind("tlsfail ", 0) == 0 && trace.find(" tcp=0 tls=-1 ") != std::string::npos,
              "TCP ok but TLS unanswered is not called untrusted: " + trace);
        check(!client.trustRefused() && !any(uart.writes, "CFSDFILE"), "and neither keeps the frames off the ladder nor drops the marker");
        const std::string later = refusedOpen(client, uart, {}, SHCONN_REFUSED, PROBE_UNTRUSTED);
        check(later.rfind("untrusted ", 0) == 0 && client.trustRefused(), "a tlsfail is re-probed on the next refusal: " + later);
    }
    {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        const std::string trace = refusedOpen(client, uart, MARKER_PRESENT, SHCONN_REFUSED, {OK, caopen(27), OK});
        check(trace.rfind("shconn ", 0) == 0 && trace.find(" tcp=27 tls=-1 ") != std::string::npos && !client.trustRefused(),
              "an unreachable server is the network: " + trace);
        check(!any(uart.writes, "CACERT"), "no TLS probe when plain TCP fails");
        const std::string later = refusedOpen(client, uart, {}, SHCONN_REFUSED, PROBE_UNTRUSTED);
        check(later.rfind("untrusted ", 0) == 0 && client.trustRefused(),
              "a refused chain after a network failure is still classified: " + later);
    }
    for (const Replies& shconn : {Replies{TIMEOUT, OK, OK, TIMEOUT, OK, OK}, Replies{ERR, OK, OK, TIMEOUT, OK, OK}}) {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        const std::string trace = refusedOpen(client, uart, MARKER_PRESENT, shconn, {});
        check(!any(uart.writes, "AT+CAOPEN") && !client.trustRefused(), "a timed-out SHCONN is not probed: " + trace);
    }
    {
        newBoot();
        CFreematics uart;
        BUNDLED(client, uart);
        uart.replies.clear();
        push(uart, OPEN_HEAD);
        push(uart, MARKER_PRESENT);
        push(uart, {ERR});    // SHSSL refuses the name
        push(uart, MARKER_DROP);
        push(uart, {BEARER});
        check(!client.open("api.iternio.com", 443) && !any(uart.writes, "AT+SHCONN"), "a refused SHSSL never reaches SHCONN");
        check(any(uart.writes, "CFSDFILE") && std::string(client.m_ca_trace) == "stale", "and drops a present marker");
        uart.writes.clear();
        push(uart, INSTALL);
        client.configureTls("api.iternio.com");
        check(any(uart.writes, "CFSWFILE"), "so the next open reinstalls");

        // A marker the delete did not remove reads present again; it is not dropped a second time this boot.
        newBoot();
        CFreematics stuck;
        BUNDLED(again, stuck);
        push(stuck, MARKER_PRESENT);
        push(stuck, {ERR});
        push(stuck, MARKER_DROP);
        again.configureTls("api.iternio.com");
        stuck.writes.clear();
        push(stuck, MARKER_PRESENT);
        push(stuck, {ERR});
        again.configureTls("api.iternio.com");
        check(!any(stuck.writes, "CFSDFILE"), "at most one marker drop per boot");
    }
}
}

void headers() {
    // The real setHeaders(): only what the caller asked for, everything checked before anything is sent,
    // quotes escaped, every reply checked.
    CFreematics uart;
    push(uart, {OK, OK, OK});
    HTTPClientSIM client(&uart);
    check(client.setHeaders({{"X-API-KEY", "k"}, {"Signature-Input", "s=(\"content-digest\");keyid=\"ab\""}}), "headers set");
    check(uart.writes == std::vector<std::string>{"AT+SHCHEAD\r", "AT+SHAHEAD=\"X-API-KEY\",\"k\"\r",
              "AT+SHAHEAD=\"Signature-Input\",\"s=(\\\"content-digest\\\");keyid=\\\"ab\\\"\"\r"},
          "no cosmetic defaults; a quote goes out as \\\" (measured to arrive as a real quote)");

    const auto refusedOutright = [](const HeaderVector& headers, const std::string& why) {
        CFreematics none;
        HTTPClientSIM c(&none);
        check(!c.setHeaders(headers) && none.writes.empty(), why + ": refused with nothing sent");
    };
    for (const char* bad : {"a\\b", "a\rb", "a\nb", "a\x08" "b", "a\x7f" "b"})
        refusedOutright({{"X-A", "ok"}, {"X-B", bad}}, "a value with a backslash or outside printable ASCII");
    for (const char* bad : {"X\"B", "X B", "X:B", ""})
        refusedOutright({{"X-A", "ok"}, {bad, "v"}}, "a name that is not an RFC 7230 token");

    // The budget, counted as name: value CRLF, at the measured edge: 354 goes out, 355 does not.
    {
        CFreematics edge;
        push(edge, {OK, OK});
        HTTPClientSIM c(&edge);
        check(SIM7070_HEADER_BUDGET == 354, "the measured budget");
        check(c.setHeaders({{"X-F", std::string(354 - 7, 'f')}}) && edge.writes.size() == 2, "354 bytes of headers are sent");
    }
    refusedOutright({{"X-F", std::string(355 - 7, 'f')}}, "355 bytes of headers");
    {
        CFreematics full;
        push(full, {OK});
        HTTPClientSIM c(&full);
        check(!c.setHeaders({{"X-Q", std::string(300, '"')}}) && full.writes.size() == 1,
              "a value whose escaped command does not fit the buffer is refused");
    }
    {
        CFreematics refused;
        push(refused, {OK, OK, ERR, OK});
        HTTPClientSIM c(&refused);
        check(!c.setHeaders({{"X-A", "1"}, {"X-B", "2"}, {"X-C", "3"}}), "a refused SHAHEAD fails the call");
        check(!any(refused.writes, "X-C"), "and stops there");
        CFreematics cleared;
        push(cleared, {ERR});
        HTTPClientSIM d(&cleared);
        check(!d.setHeaders({{"X-A", "1"}}) && cleared.writes.size() == 1, "a refused SHCHEAD fails the call");
    }
    {
        newBoot();
        CFreematics opened;
        push(opened, OPEN_HEAD);
        push(opened, MARKER_PRESENT);
        push(opened, TLS_CONTEXT);
        push(opened, SHCONF);
        push(opened, {OK, CONNECTED});
        BUNDLED(c, opened);
        check(c.open("api.iternio.com", 443), "open");
        check(any(opened.writes, "AT+SHCONF=\"HEADERLEN\",350\r"), "HEADERLEN is the modem's maximum, 350");
        check(!any(opened.writes, "AT+SHAHEAD") && !any(opened.writes, "AT+SHCHEAD"), "open() adds no headers of its own");
    }
    {
        newBoot();
        CFreematics opened;
        push(opened, OPEN_HEAD);
        push(opened, MARKER_PRESENT);
        push(opened, TLS_CONTEXT);
        push(opened, {OK, ERR, BEARER}); // SHCONF URL ok, HEADERLEN refused
        BUNDLED(c, opened);
        check(!c.open("api.iternio.com", 443) && !any(opened.writes, "AT+SHCONN"), "a refused HEADERLEN fails the open");
        check(std::string(c.openTrace()).rfind("hdrlen ", 0) == 0, "with its own trace step: " + std::string(c.openTrace()));
    }
}

// Session reuse (#8): the real CellWrapper::connect()/disconnect()/sendBulk()/lastOpenTrace() on the real SH
// open/close/send, against the production idle limit and upload cadence.
#include <networkwrapper.h>
#include "cellwrapper.h"
#include "cell_session.inc"
bool CellWrapper::init(const bool) { check(false, "init unused"); return false; }
bool CellWrapper::connected() { return true; }
bool CellWrapper::set_headers(const HeaderVector) { check(false, "set_headers unused"); return false; }
void CellWrapper::restart() { check(false, "restart unused"); }
void CellWrapper::reconnect() { check(false, "reconnect unused"); }
void CellWrapper::powerOff() { check(false, "powerOff unused"); }
HttpResponse CellWrapper::getEpoch() { check(false, "getEpoch unused"); return {}; }
std::string CellWrapper::netDiag() { return {}; }
long CellWrapper::upForMs() { return -1; }
#include "session_config.inc"
#include "cadence.inc"

// Field (#8): the earliest POST that got the prompt and body ACK and then nothing went out 61.3 s after the
// session's last request. The upload fires on the first poll past its delay (952 of 954 cycles within 1.8 s of
// that poll's own delay), and the POST leaves connect() at most 2.0 s later (1110 uploads: signing, headers).
const unsigned long EARLIEST_FAILED_IDLE_MS = 61300, POLL_OVERHEAD_MS = 2000, PRESEND_MS = 2000;
const unsigned long WORST_CYCLE_IDLE_MS = getTransmissionDelay(ACTIVE) + getPollingDelay(ACTIVE) + POLL_OVERHEAD_MS;

// ending: the replies after SHCONF, from SHCONN on.
void sessionOpen(CFreematics& uart, bool first_of_boot, const Replies& ending = {OK, CONNECTED}) {
    push(uart, OPEN_HEAD);
    if (first_of_boot) push(uart, MARKER_PRESENT);
    push(uart, TLS_CONTEXT);
    push(uart, SHCONF);
    push(uart, ending);
}

// A silent POST is the field signature: prompt, body ACK, the SHREQ command's OK, then no +SHREQ URC.
int sessionPost(CellWrapper& cell, CFreematics& uart, bool answered) {
    push(uart, {{1, ">"}, {1, "\r\nOK"}, answered ? Reply{1, "+SHREQ: \"POST\",200,0\r\n"} : Reply{-1, "\r\nOK\r\n"}});
    const int code = cell.sendBulk("{}").responseCode;
    check(uart.replies.empty(), "the POST used exactly its scripted replies");
    return code;
}

bool reusedAfter(CellWrapper& cell, CFreematics& uart, unsigned long idle_ms) {
    check(sessionPost(cell, uart, true) == 200, "POST");
    fake_ms += idle_ms;
    uart.writes.clear();
    return cell.connect() && uart.writes.empty() && cell.lastOpenTrace() == "reused idle=" + std::to_string(idle_ms / 1000) + "s";
}

void session() {
    check(WORST_CYCLE_IDLE_MS <= SESSION_IDLE_MAX_MS, "a normal driving cycle (cadence + one poll) reuses the session");
    check(SESSION_IDLE_MAX_MS + PRESEND_MS < EARLIEST_FAILED_IDLE_MS,
          "and nothing reused can reach its POST idle as long as the earliest field failure");

    newBoot();
    CFreematics uart;
    default_uart = &uart;
    CellWrapper cell(FreematicsESP32{}, "hologram");
    default_uart = nullptr;
    cell.set_url("api.iternio.com");
    cell.set_api_page("/2/app/tlm");
    cell.set_server_roots(SERVER_ROOTS_P7B, SERVER_ROOTS_LEN);
    cell.set_session_idle_max_ms(SESSION_IDLE_MAX_MS);
    sessionOpen(uart, true);
    check(cell.connect() && cell.lastOpenTrace().rfind("ok ", 0) == 0, "first connect opens: " + cell.lastOpenTrace());

    check(reusedAfter(cell, uart, WORST_CYCLE_IDLE_MS), "the next driving cycle reuses with no AT traffic: " + cell.lastOpenTrace());
    check(reusedAfter(cell, uart, SESSION_IDLE_MAX_MS), "so does a session idle exactly the limit");

    check(sessionPost(cell, uart, false) == 0 && cell.transferDiagnostics().post_result == 255,
          "a silent POST is http=0, logged p/a/r=1/1/255 as in the field");
    push(uart, {OK});
    cell.disconnect(); // what the loop does after a failed POST
    fake_ms += 1000;
    sessionOpen(uart, false);
    check(cell.connect() && cell.lastOpenTrace().rfind("ok ", 0) == 0 && uart.replies.empty(),
          "the retry opens a fresh session, however short the gap: " + cell.lastOpenTrace());

    check(sessionPost(cell, uart, true) == 200, "POST");
    fake_ms += SESSION_IDLE_MAX_MS + 1; // a slow poll, or any CHARGING upload (its poll is 60 s)
    uart.writes.clear();
    push(uart, {OK}); // close()
    sessionOpen(uart, false);
    check(cell.connect() && uart.replies.empty() && uart.writes[0] == "AT+SHDISC\r" && uart.writes[1] == "AT+SHDISC\r",
          "1 ms past the idle limit the session is torn down and opened fresh");
    check(cell.lastOpenTrace().rfind("idle=" + std::to_string(SESSION_IDLE_MAX_MS / 1000) + "s ok ", 0) == 0,
          "with the idle time in front of the open trace: " + cell.lastOpenTrace());

    check(sessionPost(cell, uart, true) == 200, "POST");
    fake_ms += SESSION_IDLE_MAX_MS + 1;
    push(uart, {OK});
    sessionOpen(uart, false, {TIMEOUT, TIMEOUT, OK, TIMEOUT, TIMEOUT, OK, BEARER}); // SHCONN, SHSTATE, SHDISC, twice
    check(!cell.connect() && uart.replies.empty() &&
          cell.lastOpenTrace().rfind("idle=" + std::to_string(SESSION_IDLE_MAX_MS / 1000) + "s shconn ", 0) == 0,
          "a reopen that fails keeps its idle prefix: " + cell.lastOpenTrace());
}

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group == "signing") { headers(); std::cout << "PASS upload signing headers\n"; return 0; }
    if (group == "session") { session(); std::cout << "PASS upload session\n"; return 0; }
    headers();
    naming();
    present();
    install();
    transientFailures();
    convertBackoff();
    noBundle();
    openRefusesTls();
    openStreak();
    openVerified();
    probe();
    session();
    std::cout << "PASS upload tls-ca\n";
}
