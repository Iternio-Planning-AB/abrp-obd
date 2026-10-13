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

int main() {
    naming();
    present();
    install();
    transientFailures();
    convertBackoff();
    noBundle();
    openRefusesTls();
    openVerified();
    probe();
    std::cout << "PASS upload tls-ca\n";
}
