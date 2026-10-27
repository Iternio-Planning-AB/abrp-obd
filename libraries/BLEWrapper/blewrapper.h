#ifndef BLEWRAPPER_H
#define BLEWRAPPER_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <Preferences.h>
#include <ESP32Time.h>
#include "../nlohmann/json.hpp"

using json = nlohmann::json;

#define SERVICE_UUID                  "ABD0"
#define TELEMETRY_CHARACTERISTIC_UUID "ABD1"
#define STARTUP_CHARACTERISTIC_UUID   "ABD2"
#define DEVELOPER_CHARACTERISTIC_UUID "ABD3"
#define ERROR_CHARACTERISTIC_UUID     "ABD4"
// Firmware OTA (see Helpers/ota_update.h + ota-update-architecture-v4.md). Deliberately on the EXISTING
// ABD0 service so the app's connection/auth/bonding is reused - no second service, no second connection.
// ABD5 = control + ACK/status notify (small frames). ABD6 = firmware data (write-no-response, throughput).
// Kept on their own binary characteristics so image bytes never reach ABD2's JSON parser.
#define OTA_CONTROL_CHARACTERISTIC_UUID "ABD5"
#define OTA_DATA_CHARACTERISTIC_UUID    "ABD6"
// Log retrieval (see Helpers/log_dump.h): the app pulls the DataLogger CSV over BLE instead of the
// serial DUMP. One characteristic: JSON commands in (write), binary chunks + JSON replies out (notify).
#define LOGDUMP_CHARACTERISTIC_UUID     "ABD7"
// Access control (see BLE_SECURITY_PLAN.md phase 2): nonce out / HMAC proof in. Created here in phase 1
// but not wired to anything yet - see the FROZEN TABLE note below for why it exists this early.
#define AUTH_CHARACTERISTIC_UUID        "ABD8"
/*
  FROZEN TABLE - read before adding any characteristic.

  iOS caches a peripheral's GATT table across connections, and BONDED devices cache it harder: once
  bonding is enabled (phase 1.2), a table that changes shape can leave a phone unable to see the new
  attributes until the user manually forgets the device in system settings. This firmware implements
  neither Service Changed nor the GATT Database Hash (the Arduino BLE library provides neither, and
  hand-rolling Database Hash means an AES-CMAC over a spec-defined serialization of the attribute
  table), so there is no way to tell a phone "re-discover me".

  The mitigation is to never change the shape again: ABD9/ABDA are RESERVED, created now, unused. A
  future feature repurposes one of them instead of appending a new UUID. Handles are assigned in
  creation order, so appending at the END of this list keeps ABD1-ABD8 at their existing handles - which
  is what makes the phase-1.1 update safe to deliver over OTA to already-paired phones.
*/
#define RESERVED_A_CHARACTERISTIC_UUID  "ABD9"
#define RESERVED_B_CHARACTERISTIC_UUID  "ABDA"

#ifndef PRINT
#define PRINT 0
#endif
#ifndef TIME
#define TIME 0
#endif

class BLEWrapper {
public:

  /*
    public fields, needs to be accessed by callbacks outside class
  */
  bool hash_ok;
  Preferences preferences;
  unsigned long new_hash{ 0 };
  /*
    Timing operations
  */
#if TIME
  unsigned long cleanTime{};
  unsigned long lastTime{};
#endif

  BLEWrapper(std::function<void(json)> cb, Preferences preferences, ESP32Time rtc) : configCallback(cb), preferences{ preferences }, rtc{ rtc } { };

  void init();
  void deinit();
  void clean();
  bool logError(std::string error);
  bool logData(const std::string& notify_payload, const std::string& read_value);
  // Optional flash-logging hook: routes handshake/connection events to the sketch's DUMP (logEvent), not just
  // Serial, so a BLE session in the car is debuggable from the log. null = Serial only. logEvt() fans to both.
  void set_log_callback(void (*cb)(const char*)) { log_cb = cb; }
  void logEvt(const char* fmt, ...);

  /* Handle fields regarding connection */
  void set_device_connected(bool status) {
    device_connected = status;
  }
  bool connected() { return device_connected; }
  void disconnect();

  /*
    Authentication. TWO flags, deliberately, because they answer different questions:

      setup_unlocked - ABD2 STATE-MACHINE latch. While false, the next ABD2 write is parsed as an
                       {"AUTH":...} token; once true, writes are profile packets. Purely protocol
                       sequencing - it says nothing about whether the peer is allowed to be here.
      authenticated  - the SESSION GRANT. Gates telemetry notify, OTA, the log dump and the cellular
                       stand-down (bleRelaying). This is the one that means "authorized".

    They used to be one flag, which made real enforcement impossible: the legacy token write set it,
    so it both unlocked the profile push AND authorized everything - and leaving it false to withhold
    authorization would have re-parsed every profile packet as an AUTH attempt, breaking setup.

    Now: the legacy AUTH write sets only the LATCH (setup still works, the profile still pushes), and
    ONLY a valid ABD8 HMAC proof sets the GRANT. There is no longer a mode switch - the audit mode
    that let the token grant a session existed to spare apps without the handshake, and no such app
    is in the field.
  */
  bool authenticate(std::string auth_token);
  void clearAuthentication() { auth_token = "false"; set_authenticated(false); setup_unlocked = false; }
  bool get_authenticated() { return authenticated; }
  void set_authenticated(bool auth) { if (auth && !authenticated) ++auth_grants; authenticated = auth; };
  /* Counts session grants: a new one on every false -> true. The relay acks (Helpers/ble_relay.h) are per session, and
     a disconnect, reconnect and AUTH can all land between two loop iterations, where authenticated alone looks unchanged. */
  uint32_t auth_grant() const { return auth_grants; }
  bool get_setup_unlocked() { return setup_unlocked; }
  /* Phase B lockdown, passed in from config.h PROFILE_REQUIRE_SIGNATURE the same way - see there for
     what it refuses and the single lockout scenario. */
  void set_profile_require_signature(bool r) { profile_require_signature = r; }
  bool profile_signature_required() { return profile_require_signature; }

  /* complete setup and activate callback hook */
  std::function<void(json)> configCallback;
  void completeSetup();

  /* Compare hashvalue, true if same as stored */
  bool validate_hash(unsigned long hash);
  unsigned long get_crc_hash() { return crc_hash; }
  void set_force_repush(bool f) { force_repush = f; }
  // The three below run on the loop task, so each opens its own NVS handle: the member one belongs to the
  // Bluedroid task (clean, authenticate), and a shared handle opened on both tasks at once loses a write.
  // Clear the stored profile hash (RAM + NVS) so the next connect takes HASH NOT OK and the app re-pushes.
  // Called when the dongle can no longer guarantee hash == running profile (persisted profile unreadable,
  // config apply failed) - keeps the fast path truthful instead of claiming a profile we aren't running.
  void invalidateHash() { commitHash(0, true); }
  // Commit a push's hash once its profile is on flash (persist), or for this boot only when nothing is
  // persisted: the fast path may only ever claim a profile the dongle can restore.
  void commitHash(unsigned long hash, bool persist) {
    crc_hash = hash;
    if (!persist) return;
    Preferences p;
    if (!p.begin("BLE", false)) return;
    p.putULong("hash", hash);
    p.end();
  }
  // Key reset: forget the stored push - the hash and the AUTH copy of the push token (the whole namespace).
  // The staging copies in RAM stay: they belong to the Bluedroid task and the next push's clean() resets them.
  void forgetStoredPush() {
    crc_hash = 0;
    Preferences p;
    if (!p.begin("BLE", false)) return;
    p.clear();
    p.end();
  }

  /* Store config values */
  void add_key(std::string key) { config_keys.push_back(key); }
  void resetPushStaging() { new_hash = 0; hash_ok = false; config_keys.clear(); configJson.clear(); tlm_push_token.clear(); }
  std::vector<std::string> keys() { return config_keys; }
  json getConfig() { return configJson; }
  void setJsonValue(std::string key, json value) { configJson[key] = value; };

  /* set/get car_token for Iternio API telemetry */
  void set_tlm_push_token(std::string apikey) { tlm_push_token = apikey; }
  std::string get_tlm_push_token() { return tlm_push_token; } // getter not currently used

  // Counted so the loop task can tell the clock was set behind the drift-learning window's back.
  void setTime(unsigned long time) { rtc.setTime(time); time_pushes = time_pushes + 1; }
  uint32_t timePushes() const { return time_pushes; }

  /*
    OTA plumbing. This wrapper contains NO OTA logic on purpose: the two characteristics only forward raw
    bytes to sketch-supplied hooks (same pattern as set_log_callback), so all update state and every flash
    operation lives in Helpers/ota_update.h on the loop task. Hooks run on the Bluedroid task and must
    therefore only copy bytes and set flags - never touch flash.
  */
  void set_ota_hooks(void (*ctrl)(const uint8_t*, size_t), void (*data)(const uint8_t*, size_t)) {
    ota_ctrl_cb = ctrl;
    ota_data_cb = data;
  }
  void dispatchOtaCtrl(const uint8_t* d, size_t n) { if (ota_ctrl_cb) ota_ctrl_cb(d, n); }
  void dispatchOtaData(const uint8_t* d, size_t n) { if (ota_data_cb) ota_data_cb(d, n); }

  /* Log-dump plumbing (ABD7). Same forward-only pattern as the OTA hooks: the callback runs on the
     Bluedroid task and must only copy bytes and set flags - all LittleFS reads and replies happen in
     Helpers/log_dump.h on the loop task. */
  void set_logdump_hook(void (*ctrl)(const uint8_t*, size_t)) { logdump_cb = ctrl; }
  void dispatchLogDump(const uint8_t* d, size_t n) { if (logdump_cb) logdump_cb(d, n); }
  bool notifyLogDump(const uint8_t* frame, size_t len);

  /* Reply on ABD2 from the LOOP TASK. The profile-signature path needs this: RSA verification and the
     apply cannot run on the Bluedroid task, so the answer to a {"JWS":...} push is sent later, from
     Helpers/profile_sig.h, rather than returned inline from StartupCallbacks::onWrite like every other
     ABD2 reply. Null-checked because deinit() nulls the pointer before deep sleep. */
  bool notifySetup(const std::string& msg);

  /* Profile-signature chunk hook (ABD2 {"JWS":...}). Forward-only, same contract as the OTA/log-dump
     hooks: it runs on the Bluedroid task and may only copy bytes. All RSA verification, base64url
     decoding and the apply happen in Helpers/profile_sig.h on the loop task. Null until the sketch
     wires it, so a build without the helper simply has no signed path. */
  void set_profile_sig_hook(bool (*cb)(int, int, const char*, size_t)) { profile_sig_cb = cb; }
  bool dispatchProfileSig(int i, int n, const char* d, size_t len) {
    return profile_sig_cb ? profile_sig_cb(i, n, d, len) : false;
  }
  bool has_profile_sig_hook() { return profile_sig_cb != nullptr; }

  /* Auth-gate plumbing (ABD8). Same forward-only pattern: the callback copies bytes on the Bluedroid
     task; all NVS and HMAC work happens in Helpers/auth_gate.h on the loop task (phase 2). */
  void set_auth_hook(void (*ctrl)(const uint8_t*, size_t)) { auth_cb = ctrl; }
  void dispatchAuth(const uint8_t* d, size_t n) { if (auth_cb) auth_cb(d, n); }
  /* OBD-alarm flag plumbing (ABD2 key "obd_alarm" - the app's save-screen toggle). Same forward-only
     pattern: the hook stashes the value on the Bluedroid task; Helpers/alarm_gate.h persists it to
     NVS on the loop task. Reaches here only past ABD2's authorization gate. */
  void set_alarm_flag_hook(void (*h)(int v)) { alarm_flag_cb = h; }
  void dispatchAlarmFlag(int v) { if (alarm_flag_cb) alarm_flag_cb(v); }
  /* Relay-ack plumbing (ABD2 key "relay_ack", #90): 0 announces that the app acks, n > 0 acks every ABD1 frame
     up to seq n. Same forward-only pattern: the hook stashes on the Bluedroid task; Helpers/ble_relay.h acts on
     the loop task. Reaches here only past ABD2's authorization gate. */
  void set_relay_ack_hook(void (*h)(uint32_t v)) { relay_ack_cb = h; }
  void dispatchRelayAck(uint32_t v) { if (relay_ack_cb) relay_ack_cb(v); }
  bool notifyAuth(const uint8_t* frame, size_t len);
  /* TRUE when the central has armed ABD8's CCCD. The auth gate GREETS on this flag's rising edge -
     the dongle speaks first, so the app's first frame arrives exactly when its subscription is
     armed and there is no subscribe-vs-write race to retry around. Trustworthy across sessions only
     because onDisconnect clears the flag (a BLE2902's value is a plain RAM byte that otherwise
     survives disconnects and would fake an already-armed pipe on reconnect). */
  bool authSubscribed();
  /* Clear ABD8's CCCD flag. Called from onDisconnect so authSubscribed() is honest per-connection
     (see the note there). RAM-only, safe on the Bluedroid task. */
  void clearAuthSubscription();
  /* Set by LogDumpCallbacks::onStatus when Bluedroid's TX queue rejected a notify (ERROR_GATT) -
     notifyLogDump's retry loop clears and polls it around each attempt. */
  void set_logdump_notify_err(bool v) { logdump_notify_err = v; }
  /* TRUE when the central has written 0x0001 to ABD7's CCCD. Diagnostic: notify() silently drops
     frames when this is false (log_v only), so the log-dump start logs it explicitly - it separates
     "app never subscribed" from "phone dropped the delivery". */
  bool logDumpSubscribed();

  /* Negotiated ATT MTU (23 until the central negotiates). The log-dump chunk size is derived from it:
     a notify carries at most MTU-3 bytes and is never fragmented. */
  uint16_t mtu() const { return att_mtu; }
  void set_mtu(uint16_t m) { att_mtu = m; }

  /* Send an ACK/status frame to the app on ABD5. False if not connected or BLE is down (deinit nulls the
     characteristic pointers, so this is null-checked - never cache the pointer outside the wrapper). */
  bool notifyOta(const uint8_t* frame, size_t len);

  /* Publish the readable value of ABD5 (installed firmware version + OTA state, as JSON) so the app can
     read what's installed without starting a transfer. */
  void setOtaInfo(const std::string& info);

  /*
    Pairing/bonding policy, passed in from config.h (BLE_ENABLE_BONDING / BLE_MAX_BONDS) the same way
    set_force_repush passes BLE_FORCE_REPUSH. Unlike that one this MUST be called BEFORE init(), because
    init() is what programs the SMP parameters and prunes the bond table.
  */
  void set_bonding(bool enabled, int max_bonds) { bonding_enabled = enabled; bond_limit = max_bonds; }

  /*
    Maintenance, reachable ONLY from the serial console (physical access) - never over BLE. Both exist
    before the thing they recover from: bond storage arrives in phase 1.2 and the auth secret in phase 2,
    so shipping these first means recovery is already in the field when the risk appears.
    Kept SEPARATE on purpose: a full bond table is a routine annoyance and must not cost authorization.
  */
  /* Forget every bonded phone. Returns how many bonds were removed. The next connect re-pairs. */
  int clearAllBonds();
  /* Wipe the whole auth NVS namespace, returning the dongle to trust-on-first-use so the app can
     provision a fresh secret. Clears the namespace rather than named keys so it stays correct when
     phase 2 defines the slot layout. */
  void clearAuthSecrets();

private:
  ESP32Time rtc;
  volatile uint32_t time_pushes = 0;

  /*
    BLE Essentials
  */
  BLEServer* pServer = NULL;
  BLEService* pService = NULL;
  BLECharacteristic* pTelemetryCharacteristic = NULL;
  BLE2902* pTelemetryCccd = NULL;                       // ABD1's CCCD: is the direct notify wanted?
  BLECharacteristic* pStartupCharacteristic = NULL;
  BLECharacteristic* pDeveloperCharacteristic = NULL;
  BLECharacteristic* pErrorCharacteristic = NULL;
  BLECharacteristic* pOtaControlCharacteristic = NULL;  // ABD5: control in, ACK/status notify out
  BLECharacteristic* pOtaDataCharacteristic = NULL;     // ABD6: firmware data in (write-no-response)
  BLECharacteristic* pLogDumpCharacteristic = NULL;     // ABD7: commands in, log chunks notify out
  BLECharacteristic* pAuthCharacteristic = NULL;        // ABD8: nonce out, HMAC proof in (phase 2)
  BLECharacteristic* pReservedACharacteristic = NULL;   // ABD9: reserved, see FROZEN TABLE above
  BLECharacteristic* pReservedBCharacteristic = NULL;   // ABDA: reserved, see FROZEN TABLE above

  /*
    Authentication, no new device connected without correct token
  */
  std::string auth_token{ "false" };
  bool authenticated = false;   // the SESSION GRANT - see the authenticate() block above
  volatile uint32_t auth_grants = 0; // see auth_grant()
  bool setup_unlocked = false;  // the ABD2 state-machine latch - see the same block
  bool profile_require_signature{ false }; // set from config.h PROFILE_REQUIRE_SIGNATURE, before init

  /* Pairing/bonding policy (set from config.h via set_bonding, before init) */
  bool bonding_enabled{ false };
  int bond_limit{ 0 };

  /* Evict bonds until the table is below `limit`, returning how many went. Called from init() ONLY, and
     that placement is what makes it safe: init() runs on the loop task (bond removal writes NVS, which
     must never happen on the Bluedroid task) and nothing is connected yet, so it cannot possibly evict
     the peer we are talking to - no exclusion logic needed. */
  int pruneBonds(int limit);

  /* used for fast setup if nothing has changed */
  unsigned long crc_hash{ 0 };
  bool force_repush{ false }; // DIAGNOSTIC (set from BLE_FORCE_REPUSH): always answer HASH NOT OK -> full push

  /*
    Config sent to main program
  */
  json configJson;
  std::vector<std::string> config_keys{};
  std::string tlm_push_token{};

  /* Value for whether BLE is connected */
  bool device_connected = false;

  /* Optional flash-log hook (see set_log_callback) - routes handshake events to the main DUMP */
  void (*log_cb)(const char*) = nullptr;

  /* OTA byte-forwarding hooks (see set_ota_hooks) - null until the sketch wires them */
  void (*ota_ctrl_cb)(const uint8_t*, size_t) = nullptr;
  void (*ota_data_cb)(const uint8_t*, size_t) = nullptr;

  /* Log-dump byte-forwarding hook (see set_logdump_hook) - null until the sketch wires it */
  void (*logdump_cb)(const uint8_t*, size_t) = nullptr;

  /* Auth-gate byte-forwarding hook (see set_auth_hook) - null until the sketch wires it */
  void (*auth_cb)(const uint8_t*, size_t) = nullptr;
  /* OBD-alarm flag hook (see set_alarm_flag_hook) */
  void (*alarm_flag_cb)(int) = nullptr;
  /* Relay-ack hook (see set_relay_ack_hook) */
  void (*relay_ack_cb)(uint32_t) = nullptr;

  /* Profile-signature chunk hook (see set_profile_sig_hook) - null until the sketch wires it */
  bool (*profile_sig_cb)(int, int, const char*, size_t) = nullptr;

  /* Negotiated ATT MTU, updated in onMtuChanged (23 = BLE default before negotiation) */
  uint16_t att_mtu{ 23 };

  /* TX-queue-full flag for notifyLogDump's backpressure retry (see set_logdump_notify_err) */
  volatile bool logdump_notify_err{ false };

};

class StartupCallbacks : public BLECharacteristicCallbacks {
public:
  StartupCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pStartupCharacteristic);
  BLEWrapper* wrapper;
};

/*
  SMP/pairing callbacks. We run Just Works (ESP_IO_CAP_NONE - the dongle has no display and no keypad), so
  every passkey hook here is dead code the base class forces us to define; only onAuthenticationComplete
  does anything, and only logging - it is the ONLY way to see whether a link actually got encrypted or why
  it did not, which is the whole point of shipping bonding in support-only mode first.
  Just Works means no MITM protection AT PAIRING TIME: an attacker present at that exact moment could
  interpose. Accepted deliberately - it needs physical proximity during the one-off pairing, and the
  phase-2 HMAC gate is what actually authorizes a peer.
*/
class SecurityCallbacks : public BLESecurityCallbacks {
public:
  SecurityCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  uint32_t onPassKeyRequest() { return 0; }        // no keypad - never reached with IO_CAP_NONE
  void onPassKeyNotify(uint32_t pass_key) { }      // no display - never reached with IO_CAP_NONE
  bool onSecurityRequest() { return true; }        // accept the phone's pairing request
  bool onConfirmPIN(uint32_t pin) { return true; } // no display to compare on - Just Works auto-confirms
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl);
  BLEWrapper* wrapper;
};

class ServerCallbacks : public BLEServerCallbacks {
public:

  ServerCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param);
  void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param);
  void onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param); // log negotiated ATT MTU (want ~185, not 23)
  BLEWrapper* wrapper;
};

class DeveloperCallbacks : public BLECharacteristicCallbacks {
public:
  DeveloperCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pDeveloperCharacteristic);
  BLEWrapper* wrapper;
};

// ABD1 (telemetry). The app does notify-then-READ: after each notification it issues a separate GATT read and
// parses THAT. onRead logs that the app is exercising the read path (and we're serving the current frame).
class TelemetryCallbacks : public BLECharacteristicCallbacks {
public:
  TelemetryCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onRead(BLECharacteristic* pTelemetryCharacteristic);
  BLEWrapper* wrapper;
};

/*
  ABD5 (OTA control) and ABD6 (OTA data). These forward raw bytes to the sketch's OTA engine and do NOTHING
  else. Two deliberate omissions, both of which caused real problems elsewhere in this firmware:
    - they never call logEvt() - a 1.7MB image is ~9,400 packets, and one flash-log line per packet would
      swamp the BLE event queue and hammer LittleFS while esp_ota_write is using the same flash chip;
    - they never touch flash themselves - they run on the Bluedroid task, so the hooks only copy bytes and
      set flags, and the loop task does every flash operation (the same hand-off that fixed the config-push
      PANICs).
*/
class OtaControlCallbacks : public BLECharacteristicCallbacks {
public:
  OtaControlCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pOtaControlCharacteristic);
  BLEWrapper* wrapper;
};

class OtaDataCallbacks : public BLECharacteristicCallbacks {
public:
  OtaDataCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pOtaDataCharacteristic);
  BLEWrapper* wrapper;
};

/* ABD7 (log dump). Forward-only like the OTA callbacks: no logEvt (the dump itself IS the log - writing
   to flash while streaming it would both contend and grow the file mid-read) and no LittleFS access on
   the Bluedroid task. */
class LogDumpCallbacks : public BLECharacteristicCallbacks {
public:
  LogDumpCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pLogDumpCharacteristic);
  void onStatus(BLECharacteristic* pLogDumpCharacteristic, Status s, uint32_t code);
  BLEWrapper* wrapper;
};

/* ABD8 (auth gate). Forward-only: provisioning writes NVS and proofs run HMAC - both belong on the
   loop task (Helpers/auth_gate.h), never here on the Bluedroid task. */
class AuthCallbacks : public BLECharacteristicCallbacks {
public:
  AuthCallbacks(BLEWrapper* wrapper) : wrapper{ wrapper } { };

private:
  void onWrite(BLECharacteristic* pAuthCharacteristic);
  BLEWrapper* wrapper;
};

#endif