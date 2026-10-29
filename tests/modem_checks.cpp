#include <iostream>
#include <limits>
#include <new>
#include "cellwrapper.h"
#include "modem_production.inc"
#include "modem_config.inc"
#include "modem_cadence.inc"

#define ENABLE_LOGGING 0
#define ENABLE_CELL 1
#define ENABLE_NETWORK 1
#define ENABLE_GPS 0

#include "../libraries/OBDWrapper/obd_vector.h"
#define MAX_UPLOAD_BUFFER 120
unsigned long upload_capped = 0, upload_heap_dropped = 0;
bool park_marker_owed = false;
struct UploadHeapStub {
    unsigned long getFreeHeap() const { return 500000; }
    unsigned long getMaxAllocHeap() const { return 250000; }
    unsigned long getMaxAllocPsram() const { return 4000000; }
    unsigned long getPsramSize() const { return 4194304; }
    unsigned long getFreePsram() const { return 4000000; }
} ESP;
#include "drive_marker_restored.inc"
#include "drive_marker_queued.inc"
#include "upload_queue.inc"
struct ICM_42627 {};
struct SleepReached {};

void check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct Wake {
    FreematicsESP32 sys;
    CellWrapper network{sys, "test-apn"};
    bool g_sample_poke = false;
    bool g_weak_rail_cold_boot = false;
    bool g_ota_active = false;
    bool g_logdump_active = false;
    bool relaying = false;
    unsigned long last_reconnect_try = 0;
    unsigned long sleepCount = 0;
    bool is_currently_parked = true, clean_sleep_marker = false;
    std::vector<OBDVector> obd_result_vector;
    int upload_fail_count = 0, upload_attempts = 0, ble_teardowns = 0;
    uint32_t clock_sleep_enter_epoch = 0;
    static constexpr int MAX_UPLOAD_FAILS_PER_WAKE = 3, WOM_THRESHOLD_LSB = 4, WOM_INT_PIN = 39;
    static constexpr float WOM_THRESHOLD_G = 0.05f;
    struct { uint32_t getEpoch() const { return 0; } } rtc;
    ICM_42627 sensor;
    void* mems = &sensor;

    Wake() { fake_ms = 1000; overshoot_delays = false; park_marker_owed = false; Serial.output.clear(); Serial.messages.clear(); }
    bool bleRelaying() const { return relaying; }
    void esp_task_wdt_reset() {}
    bool uploadBuffered(bool) { ++upload_attempts; return false; }
    void bleDeinitForSleep() { ++ble_teardowns; }
    bool wdtStreakModemMayBeOn() { return false; }
    void wdtStreakModem(bool) {}
    void wdtStreakMarker(bool) {}
    bool uploadBacklogPending() { return false; }
    bool uploadQueued() { return !obd_result_vector.empty(); }
    bool uploadBacklogUnread() { return false; }
    void loadUploadBacklog() {}
    void configureWOM(ICM_42627&) {}
    void esp_sleep_enable_ext0_wakeup(int, int) {}
    void esp_deep_sleep_start() { throw SleepReached{}; }
    bool boot() {
#include "modem_boot.inc"
        return net_ok;
    }
    void tick() {
#include "modem_reconnect.inc"
    }
#include "modem_sleep.inc"
    bool sleep(bool modem_on, const char* reason = nullptr) {
        try { enterDeepSleep(modem_on, reason); }
        catch (const SleepReached&) { return true; }
        return false;
    }
    size_t writes(const std::string& command) const {
        return std::count_if(sys.uart->events.begin(), sys.uart->events.end(),
            [&](const ModemEvent& e) { return e.timeout == 0 && e.command == command; });
    }
};

void ordinary() {
    Wake wake;
    check(!wake.boot(), "no coverage continues offline");
    check(!wake.network.connected(), "offline continuation does not claim a usable data link");
    check(wake.writes("AT+CPSI?\r") > 2, "registration really polls instead of returning immediately");
    check(wake.writes("AT+CNACT=0,1\r") == 0, "unregistered boot does not activate data");
    check(wake.sys.uart->power_pulses.size() == 1, "one cold initialization only");
    const auto start = std::find_if(wake.sys.uart->events.begin(), wake.sys.uart->events.end(),
        [](const ModemEvent& e) { return e.command == "AT+CFUN=1\r" && e.timeout == 0; });
    const auto end = std::find_if(Serial.messages.begin(), Serial.messages.end(),
        [](const auto& message) { return message.second.find("Failed to connect to server") == 0; });
    check(start != wake.sys.uart->events.end() && end != Serial.messages.end() &&
          end->first - start->ms == 30000, "ordinary wake exhausts the selected 30s search, not a shorter allowance");
}

void late() {
    Wake wake;
    wake.sys.uart->attach_at = 110000;
    check(!wake.boot(), "slow attach exceeds the initial search allowance");
    const size_t pulses = wake.sys.uart->power_pulses.size();
    auto radioCommands = [&]() {
        std::vector<std::string> commands;
        for (const auto& event : wake.sys.uart->events)
            if (!event.timeout && (event.command.find("AT+CNMP") == 0 ||
                                  event.command.find("AT+CMNB") == 0 ||
                                  event.command.find("AT+CBANDCFG") == 0))
                commands.push_back(event.command);
        return commands;
    };
    const auto radio_before = radioCommands();
    wake.tick();
    const size_t probes = wake.writes("AT+CEREG?\r");
    delay(WIFI_RETRY_MS);
    wake.tick();
    check(wake.writes("AT+CEREG?\r") == probes, "reconnect cadence is unchanged at the exact 20s boundary");
    delay(1);
    wake.tick();
    check(wake.writes("AT+CEREG?\r") > probes, "offline search is probed on the existing cadence");
    while (!wake.network.connected() && fake_ms < 180000) {
        delay(WIFI_RETRY_MS + 1);
        wake.tick();
    }
    check(wake.network.connected(), "attachment after 45s recovers on a later poll");
    check(wake.sys.uart->power_pulses.size() == pulses, "late attach must not pulse the modem power key");
    check(radioCommands() == radio_before, "late recovery neither reads nor writes any scan-mode or band setting");
    check(wake.writes("AT+CFUN=0\r") == 0 && wake.writes("AT+CFUN=1,1\r") == 0,
          "late attach never cycles or reboots the radio");
    check(wake.writes("AT+CNCFG=0,0,\"test-apn\"\r") == 1, "late attach completes data binding once");
    const size_t events = wake.sys.uart->events.size();
    delay(WIFI_RETRY_MS + 1);
    wake.tick();
    check(wake.sys.uart->events.size() == events, "ready links do not repeat data initialization");
}

void fast() {
    for (char domain : {'E', 'G', 'C'}) {
        Wake wake;
        wake.sys.uart->attach_at = 0;
        wake.sys.uart->domain = domain;
        wake.sys.uart->registered_state = domain == 'E' ? '5' : '1';
        check(wake.boot() && wake.network.connected(), "home/roaming registration in each legacy domain works");
        std::vector<std::string> data;
        for (const auto& event : wake.sys.uart->events) {
            if (event.timeout != 0) continue;
            for (const char* prefix : {"AT+CGDCONT=", "AT+CNACT", "AT+CGNAPN", "AT+CSCLK=",
                                      "AT+CNCFG=", "AT+CTZU=", "AT+CPSMS=", "AT+CEDRXS="}) {
                if (event.command.find(prefix) == 0) data.push_back(event.command);
            }
        }
        check(data == std::vector<std::string>{
            "AT+CGDCONT=1,\"IP\",\"test-apn\"\r", "AT+CNACT=0,1\r", "AT+CNACT?\r",
            "AT+CGNAPN\r", "AT+CSCLK=0\r", "AT+CNCFG=0,0,\"test-apn\"\r",
            "AT+CTZU=1\r", "AT+CPSMS=0\r", "AT+CEDRXS=0\r"},
            "fast attach retains the existing data-setup command order");
    }
    FreematicsESP32 sys;
    HTTPClientSIM client;
    check(client.begin(&sys), "near-deadline fixture starts the real modem client");
    sys.uart->attach_at = fake_ms + 29000;
    sys.uart->data_reply_ms = 900;
    const uint64_t before = fake_ms;
    check(client.setup("test-apn", 30000), "registration near the deadline still completes data setup");
    check(fake_ms - before > 30000, "successful data setup is not capped by the search deadline");
    for (const std::string command : {"AT+CGDCONT=1,\"IP\",\"test-apn\"\r", "AT+CNACT=0,1\r"}) {
        check(std::count(sys.uart->accepted.begin(), sys.uart->accepted.end(), command) == 1,
              "near-deadline data setup receives each delayed configuration ACK");
        check(std::any_of(sys.uart->events.begin(), sys.uart->events.end(),
            [&](const ModemEvent& e) { return e.command == command && e.timeout == 1000; }),
            "near-deadline data setup keeps the full per-command timeout");
    }
    check(std::any_of(sys.uart->events.begin(), sys.uart->events.end(),
        [&](const ModemEvent& e) { return e.command == "AT+CNACT=0,1\r" && !e.timeout && e.ms > before + 30000; }),
        "data activation is actually issued after the registration deadline");
}

void parked() {
    Wake wake;
    wake.g_sample_poke = true;
    const uint64_t before = fake_ms;
    check(!wake.boot(), "parked no-coverage sampling continues offline as before");
    check(fake_ms - before >= 45000 && fake_ms - before < 51000,
          "single-poll parked wakes retain their 45s search allowance");
    Wake weak;
    weak.g_weak_rail_cold_boot = true;
    check(!weak.boot() && weak.sys.uart->events.empty(), "weak-rail BLE-only wake never touches the modem");
    weak.sys.uart->attach_at = 0;
    delay(WIFI_RETRY_MS + 1);
    weak.tick();
    check(weak.sys.uart->events.empty(), "weak-rail BLE-only wake also suppresses later reconnect attempts");
    for (const char* reason : std::vector<const char*>{nullptr, "backstop"}) {
        Wake skip;
        skip.g_weak_rail_cold_boot = true;
        skip.sys.uart->alive = false;
        skip.relaying = true;
        check(!skip.boot(), "weak-rail sleep fixture skips modem startup");
        check(skip.sleep(true, reason) && skip.clean_sleep_marker && skip.ble_teardowns == 1,
              "weak-rail STANDBY/backstop still complete deep sleep and BLE teardown");
        check(skip.writes("AT+CPOWD=1\r") == 0 && skip.sys.uart->power_pulses.empty(),
              "weak-rail sleep sends neither CPOWD nor a PWRKEY pulse to the off modem");
    }
    Wake normal_sleep;
    check(normal_sleep.sleep(true) && normal_sleep.writes("AT+CPOWD=1\r") == 1,
          "normal sleeps still invoke the modem power-off path");
    Wake relayed;
    relayed.sys.uart->attach_at = 0;
    check(relayed.boot() && relayed.network.connected(), "relay-session sleep fixture has a usable link");
    relayed.relaying = true;
    relayed.obd_result_vector = {{{"utc", 1}, {"soc", 50}}};
    g_fixture_session_opens = true;
    check(relayed.sleep(true) && relayed.upload_attempts == 1,
          "a pre-sleep flush during a relay session still sends the queue over cellular (#91)");
    g_fixture_session_opens = false;
    Wake legacy;
    const uint64_t legacy_before = fake_ms;
    check(!legacy.network.init(false), "existing init(bool) still returns offline without coverage");
    check(fake_ms - legacy_before >= 45000, "other init callers retain the legacy 45s allowance");
}

void safety() {
    for (bool online : {false, true}) {
        for (uint64_t start : std::vector<uint64_t>{1000, static_cast<uint64_t>(UINT32_MAX) - 15000}) {
            FreematicsESP32 sys;
            HTTPClientSIM client;
            fake_ms = start;
            check(client.begin(&sys), "deadline fixture starts the production client");
            sys.uart->online_without_registration = online;
            if (online) {
                sys.uart->silent = {"AT+CEREG?\r", "AT+CGREG?\r", "AT+CREG?\r"};
            }
            sys.uart->max_events = 2000;
            const uint64_t before = fake_ms;
            check(!client.setup("test-apn", 30000), "no registration never configures data");
            check(fake_ms - before <= 30000, "30s registration budget includes waits across native millis wrap");
            check(fake_ms - before >= 30000, "registration does not expire early when its deadline wraps");
            check(std::count_if(sys.uart->events.begin(), sys.uart->events.end(), [&](const ModemEvent& e) {
                return !e.timeout && e.command == (online ? "AT+CEREG?\r" : "AT+CPSI?\r");
            }) > 2, "registration repeatedly probes through the full search window, including wrap");
            overshoot_delays = true;
            const uint64_t overshoot_start = fake_ms;
            sys.uart->max_events += sys.uart->events.size();
            check(!client.setup("test-apn", 30000), "late clock wakeup remains offline without registration");
            overshoot_delays = false;
            check(fake_ms - overshoot_start >= 30000 && fake_ms - overshoot_start <= 30001,
                  "a 1ms deadline overshoot ends the search instead of underflowing into a new window: " +
                  std::to_string(fake_ms - overshoot_start) + "ms");
        }
    }
    Wake lost;
    check(!lost.boot(), "signal-loss fixture begins offline");
    lost.sys.uart->attach_at = 0;
    lost.sys.uart->detach_at = fake_ms + 10;
    const size_t pulses = lost.sys.uart->power_pulses.size();
    const uint64_t before = fake_ms;
    lost.network.reconnect();
    check(!lost.network.connected(), "registration lost after the probe returns to offline recovery");
    check(fake_ms - before < 5000, "registered-then-lost recovery makes one pass, not another long search");
    check(lost.sys.uart->power_pulses.size() == pulses, "registration loss does not trigger cold init");

    Wake lost_online;
    check(!lost_online.boot(), "online-but-deregistered fixture begins offline");
    lost_online.sys.uart->online_without_registration = true;
    lost_online.sys.uart->attach_at = 0;
    lost_online.sys.uart->detach_at = fake_ms + 10;
    lost_online.sys.uart->max_events = lost_online.sys.uart->events.size() + 40;
    const size_t ereg = lost_online.writes("AT+CEREG?\r");
    const size_t greg = lost_online.writes("AT+CGREG?\r");
    const size_t creg = lost_online.writes("AT+CREG?\r");
    const size_t online_pulses = lost_online.sys.uart->power_pulses.size();
    lost_online.network.reconnect();
    check(!lost_online.network.connected(), "online radio without registration stays offline");
    check(lost_online.writes("AT+CEREG?\r") == ereg + 2 &&
          lost_online.writes("AT+CGREG?\r") == greg + 1 &&
          lost_online.writes("AT+CREG?\r") == creg + 1,
          "zero-budget warm setup makes exactly one domain-check pass after the successful probe");
    check(lost_online.writes("AT+CNACT=0,1\r") == 0 &&
          lost_online.sys.uart->power_pulses.size() == online_pulses,
          "lost registration activates no data and never pulses power");

    Wake holds;
    holds.relaying = true;
    delay(30000);
    holds.tick();
    holds.relaying = false;
    holds.g_ota_active = true;
    holds.tick();
    check(holds.sys.uart->events.empty(), "authenticated BLE and OTA retain cellular stand-down");

    Wake dead;
    dead.sys.uart->alive = false;
    dead.sys.uart->can_power_on = false;
    dead.network.reconnect();
    check(!dead.sys.uart->power_pulses.empty() && !dead.network.connected(),
          "unanswered modem retains full cold-init recovery");

    Wake uninitialized;
    uninitialized.sys.uart->attach_at = 0;
    uninitialized.network.reconnect();
    check(uninitialized.network.connected() && uninitialized.sys.uart->power_pulses.size() == 1,
          "registered modem without initialized client takes the safe cold-init path");
    uninitialized.network.powerOff();
    const size_t powered_off_pulses = uninitialized.sys.uart->power_pulses.size();
    uninitialized.sys.uart->alive = true;
    uninitialized.network.reconnect();
    check(uninitialized.network.connected() && uninitialized.sys.uart->power_pulses.size() == powered_off_pulses + 1,
          "a modem answering again after powerOff requires cold initialization, not stale warm completion");

    Wake rf;
    check(!rf.boot(), "dead-RF fixture starts offline");
    rf.sys.uart->csq = 99;
    rf.network.reconnect();
    delay(299000);
    rf.network.reconnect();
    check(rf.writes("AT+CFUN=0\r") == 0, "healthy-length searches never trigger premature dead-RF recovery");
    delay(2000);
    rf.network.reconnect();
    check(rf.writes("AT+CFUN=0\r") == 1, "five-minute dead-RF radio cycle remains available");
}

int main(int argc, char** argv) {
    const std::string group = argc > 1 ? argv[1] : "all";
    for (const auto& test : std::vector<std::pair<std::string, void(*)()>>{
             {"ordinary", ordinary}, {"late", late}, {"fast", fast}, {"parked", parked}, {"safety", safety}}) {
        if (group == "all" || group == test.first) {
            test.second();
            std::cout << "PASS modem " << test.first << '\n';
        }
    }
}
