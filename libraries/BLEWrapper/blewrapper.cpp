#include "blewrapper.h"
#include <stdarg.h>

/*  
  Initialize all BLE essentials
*/
void BLEWrapper::init() {
  // Advertised name shown in the app's device picker only; identity/matching is by service UUID + iOS
  // peripheral ID, never the name (contract §2). Dropped the stray '\n' that leaked into the shown name.
  BLEDevice::init("ABRP Dongle");
  if (bonding_enabled) {
    // Program SMP, then let the CENTRAL drive every security procedure. EVERY attribute value and
    // CCCD in the service is gated (*_ENCRYPTED): a phone pairs at the first thing it touches -
    // normally ABD2's CCCD, the setup subscribe - and an unpaired peer can connect but read/write
    // NOTHING. ABD1/ABD2 gating is the "cellular until paired" behaviour; the rollout went
    // ABD7 -> ABD1/ABD2 -> everything, each step field-validated before the next. The app maps the
    // ATT insufficient-authentication refusal to an "awaiting pairing" state (listener/setup thunks)
    // instead of a teardown-reconnect loop - keep dongle and app in step if gating ever changes.
    // BLEDevice keeps this as a STATIC pointer, so it must outlive init() - and must not be re-allocated
    // if init() ever runs twice in one boot (today it runs once, from setup()).
    static SecurityCallbacks* pSecurityCallbacks = NULL;
    if (!pSecurityCallbacks) pSecurityCallbacks = new SecurityCallbacks(this);
    BLEDevice::setSecurityCallbacks(pSecurityCallbacks);
    BLESecurity sec;                                              // stack-local: set_security_param copies
    sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);           // LE Secure Connections + store the LTK
    sec.setCapability(ESP_IO_CAP_NONE);                           // no display, no keypad -> Just Works
    sec.setKeySize(16);                                           // full AES-128 link key
    // ID_KEY (the peer's IRK) is what makes bonding actually WORK against a phone, and leaving it out is a
    // silent, confusing failure. Android (and iOS) advertise/connect with a ROTATING resolvable private
    // address - every address in our logs starts with binary 01, which is what an RPA looks like. Without
    // the peer's IRK we cannot resolve a rotated RPA back to the phone we bonded with, so the stack sees a
    // brand-new device: the phone offers the LTK it already has, we have no matching bond, encryption fails
    // (SEC failed reason=0x63), and after the user toggles Bluetooth it pairs again into a SECOND bond slot
    // for the same phone. Measured exactly that: bonds went 1 -> 2 across one RPA rotation.
    // These take a key-distribution MASK, not a key size - the library's own @param comment says
    // "value between 7 and 16", which is a copy-paste error; setStaticPIN() in the same file passes masks.
    sec.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK); // keys the phone hands us
    sec.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK); // keys we hand the phone
    // Prune BEFORE advertising, on the loop task, with nothing connected - see pruneBonds in blewrapper.h
    // for why that placement removes the need for any "don't evict the active peer" logic.
    int pruned = pruneBonds(bond_limit);
    logEvt("SEC on: bonds=%d/%d pruned=%d", esp_ble_get_bond_device_num(), bond_limit, pruned);
  }
  // Our MTU ceiling, 247 = the largest value where every notification still fits ONE link-layer
  // radio packet (244B ATT payload + L2CAP/ATT headers = the 251B DLE maximum). This is a MEASURED
  // limit, not a guess: at 517 the small frames (headers, ACKs, telemetry) still arrived but every
  // 514-byte log chunk was deterministically lost - multi-packet fragmented notifications do not
  // survive this stack/phone combination. The negotiated value is min(central request, this): iOS
  // lands at ~185 on its own, Android gets 247 when the app's transfer engines request more -
  // 239-byte log chunks instead of 177, single-packet, ~35% faster. Idle Android connections that
  // never ask stay at 23 and keep the documented notify-then-read telemetry fallback.
  BLEDevice::setMTU(247);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks(this));
  // 32 attribute handles, NOT the default 15. Cost: service declaration 1, each characteristic 2, each
  // CCCD 1. Today: 1 + (10 chars x 2) + 7 CCCDs = 28, leaving 4 spare.
  //   ABD1 +CCCD, ABD2 +CCCD, ABD3 +CCCD, ABD4 +CCCD, ABD5 +CCCD, ABD6, ABD7 +CCCD, ABD8 +CCCD,
  //   ABD9, ABDA
  // History, so nobody trims this back: with the DEFAULT budget the table filled up at ABD5's value
  // handle (15/15) and ABD5's CCCD plus ALL of ABD6 were silently dropped from the ATT table - Bluedroid
  // reports out-of-handles only in a GATT event the Arduino core ignores, so phones saw "notify change
  // failed" on ABD5 and no ABD6 at all, which killed OTA and cost days to find.
  // RAISE THIS NUMBER FIRST if the characteristic list ever grows past the spare 4 - but read the FROZEN
  // TABLE note in blewrapper.h before adding anything, because changing the table's shape after bonding
  // is enabled has a user-visible cost. Must use the BLEUUID overload - the const char* one takes no
  // handle count.
  pService = pServer->createService(BLEUUID(SERVICE_UUID), 32);

  preferences.begin("BLE");
  // READ the persisted profile-version tag. This was putULong("hash", 0), which (a) overwrote the stored
  // hash with 0 on every boot so the HASH OK fast path could never match across a reboot / deep-sleep wake,
  // and (b) assigned putULong's return value (the byte-count written, 4) into crc_hash. getULong restores
  // the intended behaviour: the fast path now survives sleep, so an unchanged profile isn't re-pushed.
  crc_hash = preferences.getULong("hash", 0);
  preferences.end();

  pTelemetryCharacteristic = pService->createCharacteristic(
    TELEMETRY_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  if (bonding_enabled) {
    // ABD1 gated = "cellular until paired": an unpaired phone can connect but gets no telemetry -
    // the dongle's own cellular uplink carries the data meanwhile. The app understands the refusal
    // (its listener/setup thunks map ATT insufficient-authentication to an "awaiting pairing" state
    // instead of a teardown-reconnect loop) - ship dongle and app together, an OLD app against this
    // firmware would loop reconnect->prompt on a declined dialog.
    // Same _ENCRYPTED / never _ENC_MITM reasoning as ABD7 below. No write perm on ABD1 - it has no
    // write property.
    pTelemetryCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  }

  pStartupCharacteristic = pService->createCharacteristic(
    STARTUP_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE
  );
  if (bonding_enabled) {
    // ABD2 carries the pushed profile INCLUDING the tlm push token - the single most sensitive
    // write in the protocol, and the reason this pair is gated ahead of ABD3-ABD6 (deviating from
    // the plan's original ABD3/4-first order: those carry far less than the token does).
    pStartupCharacteristic->setAccessPermissions(
      ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  }

  pDeveloperCharacteristic = pService->createCharacteristic(
    DEVELOPER_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE
  );

  pErrorCharacteristic = pService->createCharacteristic(
    ERROR_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  // OTA control (ABD5): the app writes START/END/ABORT/INFO here and we notify 20-byte ACK/status frames
  // back. READ is supported so the app can fetch the installed-version JSON (published by setOtaInfo)
  // without starting a transfer - note that DURING a transfer the readable value holds the most recent ACK
  // frame, since a BLE notify necessarily carries the characteristic's value.
  pOtaControlCharacteristic = pService->createCharacteristic(
    OTA_CONTROL_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  // OTA data (ABD6): WRITE_NR (write-without-response) is the throughput path - the client can stream
  // packets back-to-back without waiting for an ATT response, and our per-sector ACK on ABD5 provides the
  // flow control instead. No NOTIFY/descriptor: this channel is inbound only.
  pOtaDataCharacteristic = pService->createCharacteristic(
    OTA_DATA_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  // Log dump (ABD7): the app writes JSON commands (START / <offset> / END) and we notify back binary
  // log chunks + JSON window markers (see Helpers/log_dump.h for the framing). READ is harmless and kept
  // for debugging (returns the last notified frame). Handle cost: 3 (decl+value+CCCD).
  pLogDumpCharacteristic = pService->createCharacteristic(
    LOGDUMP_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  if (bonding_enabled) {
    // THE FIRST characteristic that required encryption (validated on Android + iOS before ABD1/ABD2
    // joined). Chosen to lead because it has the smallest blast radius in the table: the log dump is
    // developer-gated in the app, so a mistake here breaks nothing a normal user touches.
    // The gate is also what makes the phone, not us, start pairing: an unpaired access gets ATT 0x05
    // (insufficient authentication) and the central pairs and retries on its own. That replaces the
    // requestEncryption() push, which on Android >=12 produced TWO system dialogs for one procedure
    // (peripheral-initiated bonding: one on the security request, one when the stack re-evaluates).
    // _ENCRYPTED, never the _ENC_MITM variants: Just Works CANNOT satisfy an MITM requirement, so
    // ENC_MITM here would make the log dump permanently unreachable rather than merely encrypted.
    esp_gatt_perm_t enc = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;
    pLogDumpCharacteristic->setAccessPermissions(enc);
  }
  // Access control (ABD8): the dongle notifies a random nonce, the app writes back an HMAC proof
  // (BLE_SECURITY_PLAN.md phase 2). Live since phase 2: AuthCallbacks forwards writes to the sketch's
  // auth gate (Helpers/auth_gate.h) via set_auth_hook - same forward-only pattern as ABD6/ABD7. It was
  // created one release earlier, inert, so the attribute table reached its final shape BEFORE bonding
  // was enabled - see the FROZEN TABLE note in blewrapper.h.
  pAuthCharacteristic = pService->createCharacteristic(
    AUTH_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  // RESERVED (ABD9/ABDA): placeholders so a future feature can be added WITHOUT changing the table's
  // shape. READ|WRITE only, no CCCD - a notify-capable descriptor can be added later without shifting
  // any other handle, whereas a whole new characteristic cannot. Inert: no callbacks, no hooks.
  pReservedACharacteristic = pService->createCharacteristic(
    RESERVED_A_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pReservedBCharacteristic = pService->createCharacteristic(
    RESERVED_B_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  // NOTE: deliberately NOT compiled out behind the sketch's ENABLE_OTA. This library is its own translation
  // unit and never sees config.h (logger.ino includes blewrapper.h BEFORE config.h), so an #if here would
  // silently evaluate to 0 and the characteristics would never exist. They are inert anyway: without hooks
  // wired via set_ota_hooks() the dispatchers are no-ops, so an ENABLE_OTA 0 build can do nothing with them.

  if (bonding_enabled) {
    // Gate the REST of the table - with this block, EVERY value in the service requires an encrypted
    // link (the CCCDs are gated at the descriptor block below). The original rollout order kept
    // ABD5/ABD6 (OTA) open until last, fearing a permission mistake would brick updates; that caution
    // is retired on evidence: the identical pattern moved full 2.8 MB log dumps on both platforms with
    // retries=0, comfortably above OTA's 1.7 MB stop-and-wait transfer. What gating OTA actually buys:
    // an unpaired stranger could otherwise write START to ABD5 and spam the staging slot. ABD6 is
    // WRITE_NR - per ATT, write commands to an encryption-required attribute are silently dropped on a
    // plaintext link (no error response), which is fine: ABD6 is only ever written after START is
    // ACKed on ABD5, and that already forced encryption.
    // ABD8: pairing-before-authorization is the intended layering for the phase-2 HMAC handshake (the
    // link encrypts, THEN the peer proves it holds the secret). ABD9/ABDA: inert, but an open window
    // is an open window - a future use that needs pre-pairing access can flip a permission then
    // (permissions are per-attribute metadata, not table shape).
    esp_gatt_perm_t enc_rw = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;
    pDeveloperCharacteristic->setAccessPermissions(enc_rw);
    pErrorCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED); // no write property
    pOtaControlCharacteristic->setAccessPermissions(enc_rw);
    pOtaDataCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED); // write-only channel
    pAuthCharacteristic->setAccessPermissions(enc_rw);
    pReservedACharacteristic->setAccessPermissions(enc_rw);
    pReservedBCharacteristic->setAccessPermissions(enc_rw);
  }

  // Register the callback for the Startup characteristic
  pStartupCharacteristic->setCallbacks(new StartupCallbacks(this));
  // Register the callback for the Developer characteristic
  pDeveloperCharacteristic->setCallbacks(new DeveloperCallbacks(this));
  // Register the callback for the Telemetry characteristic (logs the app's notify-then-READ of ABD1)
  pTelemetryCharacteristic->setCallbacks(new TelemetryCallbacks(this));
  // OTA characteristics: forward-only callbacks (no OTA logic here - see set_ota_hooks)
  pOtaControlCharacteristic->setCallbacks(new OtaControlCallbacks(this));
  pOtaDataCharacteristic->setCallbacks(new OtaDataCallbacks(this));
  // Log-dump characteristic: forward-only callback (no log logic here - see set_logdump_hook)
  pLogDumpCharacteristic->setCallbacks(new LogDumpCallbacks(this));
  // Auth-gate characteristic: forward-only callback (no auth logic here - see set_auth_hook)
  pAuthCharacteristic->setCallbacks(new AuthCallbacks(this));

  // https://www.bluetooth.com/specifications/gatt/viewer?attributeXmlFile=org.bluetooth.descriptor.gatt.client_characteristic_configuration.xml
  // Create a BLE Descriptor
  BLE2902* pTelemetryCccd = new BLE2902();
  pTelemetryCharacteristic->addDescriptor(pTelemetryCccd);
  BLE2902* pStartupCccd = new BLE2902();
  pStartupCharacteristic->addDescriptor(pStartupCccd);
  if (bonding_enabled) {
    // Gate the ABD1/ABD2 CCCDs like their values (subscribing IS access - see the ABD7 CCCD note
    // below). ABD2's CCCD is the FIRST attribute the app touches in a session (its setup flow
    // subscribes before writing AUTH), so this is where the OS pairing prompt fires for telemetry.
    pTelemetryCccd->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
    pStartupCccd->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  }
  BLE2902* pDeveloperCccd = new BLE2902();
  pDeveloperCharacteristic->addDescriptor(pDeveloperCccd);
  BLE2902* pErrorCccd = new BLE2902();
  pErrorCharacteristic->addDescriptor(pErrorCccd);
  BLE2902* pOtaControlCccd = new BLE2902();                // CCCD - the app must subscribe to get OTA ACKs
  pOtaControlCharacteristic->addDescriptor(pOtaControlCccd);
  BLE2902* pLogDumpCccd = new BLE2902();                   // CCCD - the app must subscribe to get log chunks
  pLogDumpCharacteristic->addDescriptor(pLogDumpCccd);
  BLE2902* pAuthCccd = new BLE2902();                      // CCCD - phase 2 notifies the nonce on ABD8
  pAuthCharacteristic->addDescriptor(pAuthCccd);
  if (bonding_enabled) {
    // Gate every CCCD like its value, for two reasons. Correctness: subscribing IS access, and an open
    // CCCD would let an unpaired peer arm the notify channel. Behaviour: the app subscribes BEFORE it
    // writes any command, so gating these makes pairing trigger on the FIRST operation of each flow
    // instead of part-way through it. Same _ENCRYPTED / not _ENC_MITM reasoning as the values above.
    esp_gatt_perm_t enc_cccd = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;
    pDeveloperCccd->setAccessPermissions(enc_cccd);
    pErrorCccd->setAccessPermissions(enc_cccd);
    pOtaControlCccd->setAccessPermissions(enc_cccd);
    pLogDumpCccd->setAccessPermissions(enc_cccd);
    pAuthCccd->setAccessPermissions(enc_cccd);
  }
  // ABD9/ABDA get no descriptor on purpose (see their creation above)

  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  // Put the device NAME + TX-power in the SCAN RESPONSE, not the primary advertisement packet.
  // WHY (Android reconnect fix): the primary BLE adv PDU is only 31 bytes. With scan response OFF
  // this library packs flags + full name ("ABRP Dongle") + tx-power + the ABD0 service UUID + the
  // connection-interval hints (setMinPreferred/setMaxPreferred) into that one packet - which
  // overflows, and Bluedroid silently DROPS the service UUID. So ABD0 lived only in the GATT table
  // (visible post-connect) and never in the broadcast. The app's Android reconnect uses a
  // SERVICE-FILTERED scan (startDeviceScan([ABD0,...])) - required for scanning with the screen
  // off - so it never saw this dongle and stuck at "Not connected", while pairing worked (it uses
  // an unfiltered scan). Turning scan response ON moves name+tx-power to the scan-response packet,
  // leaving flags + ABD0 + conn-interval hints (~13B) in the primary packet, so ABD0 is always
  // advertised - matching how a generic ELM327 advertises FFF0. iOS is unaffected (it reads both
  // packets and connects already); the device picker still shows the name (active scan reads the
  // scan response). Additive change: no GATT/characteristic/connection-param behaviour changes.
  pAdvertising->setScanResponse(true);
  // iOS-friendly preferred connection parameters. This is THE documented fix for "an iPhone connects to an
  // ESP32 then drops after a few seconds" - exactly the flap we see (disconnect reason 0x13 = phone-initiated,
  // in BOTH the old and new firmware, both of which had these disabled at 0x0). 0x06 = 7.5ms, 0x12 = 22.5ms;
  // advertising these hints lets iOS pick an interval it's willing to hold instead of terminating the link.
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
#if PRINT
  Serial.println("Waiting a client connection to notify...");
#endif
}

/*
  Fully tear down the BLE stack before deep sleep. A left-initialized Bluedroid controller keeps the radio
  domain clocked and holds tens of KB of heap, raising deep-sleep current (which undoes the battery work).
  BLEDevice::deinit(true) releases both. The next wake's init() brings it back up; the advertised identity is
  preserved because the BLE address derives from the stable eFuse base MAC and we never enable RPA/privacy.
*/
void BLEWrapper::deinit() {
  set_device_connected(false);
  set_authenticated(false);   // a reconnecting phone re-runs AUTH (the app re-runs the setup handshake anyway)
  setup_unlocked = false;     // and re-runs it from the top: the ABD2 latch is per-session too
  att_mtu = 23;               // per-connection state - the next wake's connection starts unnegotiated
  BLEDevice::deinit(true);
  pServer = NULL;             // pointers are stale after deinit; init() recreates the server/service/chars
  pService = NULL;
  pTelemetryCharacteristic = NULL;
  pStartupCharacteristic = NULL;
  pDeveloperCharacteristic = NULL;
  pErrorCharacteristic = NULL;
  pOtaControlCharacteristic = NULL; // notifyOta/setOtaInfo null-check these - never cache them elsewhere
  pOtaDataCharacteristic = NULL;
  pLogDumpCharacteristic = NULL;    // notifyLogDump null-checks this too
  pAuthCharacteristic = NULL;
  pReservedACharacteristic = NULL;
  pReservedBCharacteristic = NULL;
}

/*
  Bonding. See blewrapper.h for the placement rules that keep pruneBonds safe.

  There is deliberately NO "request encryption on connect" here any more. It existed only because nothing
  in the table required encryption, so no phone had a reason to pair - and it cost us a real bug: a
  PERIPHERAL-initiated security request makes Android >=12 show TWO system pairing dialogs for one
  procedure (one when the request arrives, one when its stack re-evaluates the encryption state). iOS
  bundles them into one, which is why this only ever reproduced on Android. Measured: a single "SEC req"
  and a single "SEC bonded" with ~8s of human-tap latency between them, both dialogs inside that window.
  Encryption is now driven by the CENTRAL, which is the normal BLE pattern: an unpaired access to a
  *_ENCRYPTED attribute (ABD7) returns ATT 0x05 and the phone pairs and retries by itself - one dialog.
  Do not reintroduce a peripheral-side push to "help" a phone pair; gate an attribute instead.
*/
int BLEWrapper::pruneBonds(int limit) {
  if (limit <= 0) return 0;
  int num = esp_ble_get_bond_device_num();
  if (num < limit) return 0; // room for one more pairing - leave the table alone
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * num);
  if (!list) return 0;
  int removed = 0;
  if (esp_ble_get_bond_device_list(&num, list) == ESP_OK) {
    // Evict from the front of whatever order the stack hands back (arbitrary, deliberately not "oldest
    // first" - the stack stores no timestamps and an evicted phone just re-pairs silently). Stop as soon
    // as one slot is free, so a full table costs exactly one eviction, not a wipe.
    for (int i = 0; i < num && (num - removed) >= limit; i++) {
      if (esp_ble_remove_bond_device(list[i].bd_addr) == ESP_OK) removed++;
    }
  }
  free(list);
  return removed;
}

/*
  Serial-console maintenance. Physical access only - neither is reachable over BLE, so an attacker gains
  nothing they could not already do by reflashing over USB. See the declarations in blewrapper.h.
*/
int BLEWrapper::clearAllBonds() {
  // Bonds live in NVS and survive deinit()/deep sleep, so a full bond table stays full until something
  // removes entries. The Arduino BLE library exposes no bond API at all, hence the direct IDF calls.
  int num = esp_ble_get_bond_device_num();
  if (num <= 0) return 0;
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * num);
  if (!list) return 0;
  int removed = 0;
  if (esp_ble_get_bond_device_list(&num, list) == ESP_OK) {
    for (int i = 0; i < num; i++) {
      if (esp_ble_remove_bond_device(list[i].bd_addr) == ESP_OK) removed++;
    }
  }
  free(list);
  return removed;
}

void BLEWrapper::clearAuthSecrets() {
  // Clear the NAMESPACE, not named keys: phase 2 owns the slot layout (current/previous secret, labels)
  // and this must keep working when that layout changes. A namespace that does not exist yet is a no-op.
  Preferences p;
  if (!p.begin("auth", false)) return;
  p.clear();
  p.end();
}

/*
  Format an event and fan it out to Serial (always) and, if wired, the sketch's flash logger (log_cb). Lets
  the connection/handshake trace land in the DUMP so a BLE session in the car is debuggable, not just on USB.
*/
void BLEWrapper::logEvt(const char* fmt, ...) {
  char buf[100];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("%s\n", buf);
  if (log_cb) log_cb(buf);
}

/*
  Reset all initialized variable
*/
void BLEWrapper::clean() {
  preferences.begin("BLE");
  preferences.clear();
  preferences.putString("auth_token", auth_token.c_str());
  preferences.end();
  config_keys.clear();
  configJson.clear();
  tlm_push_token = "";
  hash_ok = false;
#if TIME
  cleanTime = millis();
  lastTime = millis();
#endif
}

/*  
  Send message on Error characteristic
*/
bool BLEWrapper::logError(std::string error) {
  logEvt("%s", error.c_str()); // surface every handshake error in the DUMP (the app reads ABD4 on ERROR)
  if (device_connected) {
    pErrorCharacteristic->setValue(error);
    pErrorCharacteristic->notify();
    return true;
  }
  return false;
}

/*  
  Send telemetry on telemetry characteristic
*/
bool BLEWrapper::logData(std::string payload) {

  if (device_connected && get_authenticated()) {
    pTelemetryCharacteristic->setValue(payload);
    pTelemetryCharacteristic->notify();
#if PRINT 
    Serial.printf("New value notified: %s\n", payload.c_str());
#endif
    return true;
  }
  return false;
}

/*  
  Set connected variable and start advertising
*/
void BLEWrapper::disconnect() {
  set_device_connected(false);
  pServer->startAdvertising(); // restart advertising
}

/*  
  Compare authentication tokens, set new if in setup phase
*/
bool BLEWrapper::authenticate(std::string token) {
  Serial.printf("Starting authorization\n");
  auth_token = token;
  // Contract 4e - current (non-signature) scheme: accept the token the app sends and return AUTH OK. The
  // AUTH token IS the tlm_push_token, so we just persist it. Real link-level security (MITM passkey + bonding)
  // is the later signed-pairing phase.
  //
  // Removed the old 60s-from-boot "setup window": it trusted only the first token seen within 60s of boot and
  // otherwise required a match against a previously stored token. In the field the device boots on a wake and
  // the phone typically connects well after 60s, so a legitimate first pairing was rejected as "unauthorized".
  preferences.begin("BLE");
  preferences.putString("auth_token", auth_token.c_str());
  preferences.end();
  // The LATCH always advances: the app's next ABD2 write is a profile packet, not another AUTH, and
  // that must hold or the profile push breaks (every packet would re-parse as AUTH).
  setup_unlocked = true;
  // The GRANT is deliberately NOT set here. This token is the account's tlm_push_token - anyone who
  // can read it, or sniff one write of it, would otherwise get telemetry, OTA and the log dump. Only
  // a valid ABD8 HMAC proof authorizes now; holding the token unlocks setup and nothing else.
  return true;
}

/*  
  Set variables and activate callback function
*/
void BLEWrapper::completeSetup() {
  confirmHash();
  json data;
  data["configJson"] = configJson;
  data["tlm_push_token"] = tlm_push_token;
  configCallback(data);
}

/*
  Validate if received hash is same as stored, update to new hash if not
*/
bool BLEWrapper::validate_hash(unsigned long hash) {
  // DIAGNOSTIC: force a full re-push (HASH NOT OK) so the app's Save runs the complete clean->bulk->SETUP OK
  // path instead of the fast-path no-op. See BLE_FORCE_REPUSH.
  if (force_repush) { new_hash = hash; hash_ok = false; return false; }
  hash_ok = get_crc_hash() == hash;
  if (hash_ok) {
    return true;
  } else {
    new_hash = hash;
    return false;
  }
}

/*  
  Store completed hash
*/
void BLEWrapper::confirmHash() {
  crc_hash = new_hash;
  preferences.begin("BLE");
  preferences.putULong("hash", crc_hash);
  preferences.end();
}

/*
  Parse JSON containing:
    tlm_push_token
    VEHICLE_JSON
*/
void StartupCallbacks::onWrite(BLECharacteristic* pStartupCharacteristic) {
  std::string input = pStartupCharacteristic->getValue();
  json j;
  std::string message_key{};
  if (input.length() > 0) {
    // Log the RAW bytes of every app write to ABD2. The push packets are expected; what we're hunting for is
    // ANY write that arrives AFTER SETUP OK (a keep-alive, nonce/challenge, or signed-auth message the app
    // expects us to answer). If one shows up and we don't respond correctly, that's why the app drops us (0x13).
    wrapper->logEvt("ABD2 rx: %.70s", input.c_str());
    // AUTHORIZATION GATE on the profile push. ABD2 is the highest-value write in the protocol: it
    // carries the vehicle profile (the PID list) and the tlm push token, so an unauthorized peer that
    // could write here could inject arbitrary PIDs at the CAN bus. Pairing alone does NOT earn that -
    // it only proves the link is private, not that the peer is allowed. So the whole channel needs
    // the ABD8 session grant, and the app proves BEFORE it pushes (see the poller's connect
    // sequence). Refused writes get a distinct reply so the app can tell "not authorized" from
    // "malformed".
    if (!wrapper->get_authenticated()) {
      wrapper->logEvt("ABD2 refused: not authorized (no ABD8 proof yet)");
      pStartupCharacteristic->setValue("NOAUTH");
      pStartupCharacteristic->notify();
      return;
    }
    /*
      Dispatch AUTH on its CONTENT, not on the latch.

      The latch (below) only sequences the ABD2 conversation - first write is the AUTH token, later
      writes are profile packets - and gating it on the grant would re-parse every profile packet as
      an AUTH token. But keying the AUTH handler on `!setup_unlocked` alone meant a SECOND
      `{"AUTH":...}` on the same connection was never seen as AUTH: clean() does not reset the latch
      (only clearAuthentication() on disconnect and deinit() do), so a retried push fell through to
      the generic package path, answered ACK, and was then refused UNSIGNED by the
      PROFILE_REQUIRE_SIGNATURE lockdown. Two contradictory replies to one write, and the push never
      completed - five refused re-AUTHs over ~22 s in the field DUMP.

      That cascaded a long way: no completeSetup() -> no config-apply -> no setup permit -> the
      wizard's PID test denied -> Save impossible. On a car with the alarm flag set that made the
      flag UNRECOVERABLE from the app, since clearing it needs a Save. Which is exactly why this had
      to be fixed before the flag became the enforcement switch.

      authenticate() is idempotent, so answering a repeat AUTH is free.

      Detected by PARSING and looking for the key, not by substring: ABD2 also carries bare-string
      commands ("clean") that must not be parsed, and a JWS chunk is attacker-influenced base64 that
      should never be able to steer this dispatch by containing the right bytes.
    */
    bool is_auth_write = false;
    if (!input.empty() && input.front() == '{') {
      try {
        is_auth_write = json::parse(input).contains("AUTH");
      }
      catch (const json::exception&) {
        is_auth_write = false; // malformed: fall through to the paths below, which report it
      }
    }
    if (is_auth_write || !wrapper->get_setup_unlocked()) {
      try {
        j = json::parse(input);
        auto ok = wrapper->authenticate(j.at("AUTH").get<std::string>());
        wrapper->logEvt("AUTH %s", ok ? "OK" : "NOT OK");
        pStartupCharacteristic->setValue(ok ? "AUTH OK" : "AUTH NOT OK");
        pStartupCharacteristic->notify();
        return;
      }
      catch (json::out_of_range err) {
        Serial.printf("Authentication error: %s\n", err.what());
        pStartupCharacteristic->setValue("ERROR");
        pStartupCharacteristic->notify();
        wrapper->logError(std::string("ERROR: Expected authentication token, received " + input));
        return;
      }
      catch (json::parse_error err) {
        Serial.printf("Parsing error: %s\n", err.what());
        pStartupCharacteristic->setValue("ERROR");
        pStartupCharacteristic->notify();
        wrapper->logError(std::string("ERROR: failed to parse " + input));
        return;
      }
      catch (...) {
        pStartupCharacteristic->setValue("ERROR");
        pStartupCharacteristic->notify();
        wrapper->logError(std::string("ERROR: unknown error " + input));
        return;
      }
    }
    // Signed profile chunk (phase B): {"JWS":{"i":<idx>,"n":<total>,"d":"<base64url slice>"}}.
    // Handled BEFORE the generic one-key package path below because its value is an object, not a
    // scalar, and it must never reach setJsonValue - the assembled JWS is not profile data until its
    // signature has been verified on the loop task. Copy-and-return only: no crypto here.
    if (wrapper->has_profile_sig_hook() && input.rfind("{\"JWS\"", 0) == 0) {
      json jc = json::parse(input, nullptr, false);
      bool ok = false;
      if (!jc.is_discarded() && jc.contains("JWS") && jc["JWS"].is_object()) {
        json c = jc["JWS"];
        if (c.contains("i") && c.contains("n") && c.contains("d") && c["d"].is_string()) {
          const std::string d = c["d"].get<std::string>();
          ok = wrapper->dispatchProfileSig(c["i"].get<int>(), c["n"].get<int>(), d.c_str(), d.size());
        }
      }
      // Per-chunk ACK keeps the app's flow control identical to the package push it replaces. The
      // OK/BAD verdict for the whole JWS arrives later, from the loop task (see profileSigService).
      pStartupCharacteristic->setValue(std::string(ok ? "ACK" : "JWS BAD"));
      pStartupCharacteristic->notify();
      return;
    }

    if (input == "clean") {
      wrapper->clean();
      pStartupCharacteristic->setValue(std::string("CLEAN OK"));
      pStartupCharacteristic->notify();
      wrapper->logEvt("CLEAN OK (config cleared)");
      return;
    }

    try {
      j = json::parse(input);
      if (j.size() != 1) {
        throw std::out_of_range("OOR");
      }
      for (auto key : j.items()) {
        message_key = key.key();
      }
      // ACK is per bulk-push packet (contract §4c). Do NOT ACK the HASH write - it gets HASH OK / HASH NOT OK,
      // and an extra ACK before that puts an unexpected response on the channel the app is strictly matching.
      if (message_key != "HASH") {
        pStartupCharacteristic->setValue(std::string("ACK"));
        pStartupCharacteristic->notify();
      }
    }
    catch (json::parse_error& ex) {
      Serial.printf("ERROR: Failed to parse string: \n%s\nas JSON\n", input.c_str()); // Print the string value
      pStartupCharacteristic->setValue("ERROR");
      pStartupCharacteristic->notify();
      wrapper->logError(std::string("ERROR: Failed to parse: " + input));
      return;
    }
    catch (std::out_of_range& ex) {
      Serial.printf("ERROR: Invalid JSON size\n");
      pStartupCharacteristic->setValue("ERROR");
      pStartupCharacteristic->notify();
      wrapper->logError(std::string("ERROR: Invalid JSON size, expected size 1"));
      return;
    }

    if (message_key == "keys") {
      for (auto key : j[message_key]) {
        auto s = std::string(key);
        wrapper->add_key(s);
      }
#if TIME
      Serial.printf("Received %s: %d, %d\n", "keys", millis() - wrapper->lastTime, millis() - wrapper->cleanTime);
      wrapper->lastTime = millis();
#endif
    } else {
#if TIME
      Serial.printf("Received %s: %d, %d\n", message_key.c_str(), millis() - wrapper->lastTime, millis() - wrapper->cleanTime);
      wrapper->lastTime = millis();
#endif
      if (message_key == "tlm_push_token") {
        wrapper->set_tlm_push_token(j.at(message_key).get<std::string>());
    } else if (message_key == "utc") {
      wrapper->setTime(j.at(message_key).get<unsigned long>());
      if (wrapper->hash_ok) {
        wrapper->logEvt("SETUP OK (fast path, hash matched)");
        pStartupCharacteristic->setValue(std::string("SETUP OK"));
        pStartupCharacteristic->notify();
        return;
      }
    } else if (message_key == "HASH") {
      bool hok = wrapper->validate_hash(j.at(message_key).get<unsigned long>());
      pStartupCharacteristic->setValue(std::string(hok ? "HASH OK" : "HASH NOT OK"));
      pStartupCharacteristic->notify();
      wrapper->logEvt("HASH %s", hok ? "OK" : "NOT OK");
      return;
    } else if (message_key == "obd_alarm") {
      // Per-vehicle "OBD port alarmed" flag from the app's save-screen toggle (see
      // Helpers/alarm_gate.h). Handshake/config data like tlm_push_token - NOT profile content -
      // and it must RETURN here: reaching setJsonValue would fold it into configJson and perturb
      // the profile HASH, forcing a re-push on every reconnect. Bluedroid task: the hook only
      // stashes; the NVS persist runs on the loop task.
      int v = j.at(message_key).get<int>() ? 1 : 0;
      wrapper->dispatchAlarmFlag(v);
      wrapper->logEvt("ABD2 obd_alarm=%d received (persisted by loop task)", v);
      return;
    }
    // PHASE B LOCKDOWN. Everything reaching this line is a profile VALUE - the protocol packages
    // (keys / HASH) returned above, and tlm_push_token / utc are explicitly allowed through because
    // they are handshake data, not profile content. With PROFILE_REQUIRE_SIGNATURE the only legitimate
    // source of a profile value is a JWS whose signature has been verified on the loop task, so refuse
    // the raw form outright rather than running CAN commands nobody signed for.
    if (wrapper->profile_signature_required() &&
        message_key != "tlm_push_token" && message_key != "utc") {
      wrapper->logEvt("ABD2 refused unsigned profile key '%s' (signature required)",
        message_key.c_str());
      pStartupCharacteristic->setValue(std::string("UNSIGNED"));
      pStartupCharacteristic->notify();
      return;
    }
    wrapper->setJsonValue(message_key, j.at(message_key));
  }

#if PRINT 
    Serial.printf("Successfully parsed string: \n%s\nas JSON\n", j.dump().c_str()); // Print the string value
    Serial.printf("Complete config: \n%s\n", wrapper->getConfig().dump().c_str());
#endif
    wrapper->logEvt("push progress: keys=%d config=%d", (int)wrapper->keys().size(), (int)wrapper->getConfig().size());
    if (wrapper->keys().size() == wrapper->getConfig().size() && wrapper->keys().size() > 0) {
      wrapper->logEvt("SETUP OK (push complete)");
      wrapper->completeSetup();
      pStartupCharacteristic->setValue(std::string("SETUP OK"));
      pStartupCharacteristic->notify();
    }
}
}

/*  
  Handle connection
*/
void ServerCallbacks::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  BLEAddress addr = BLEAddress(param->connect.remote_bda);
  wrapper->logEvt("CONNECT %s", addr.toString().c_str());
  wrapper->set_device_connected(true);
};

/*  
  Handle disconnection
*/
void ServerCallbacks::onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  BLEAddress addr = BLEAddress(param->disconnect.remote_bda);
  // reason is a BLE HCI disconnect code: 0x13=remote user terminated, 0x08=connection timeout,
  // 0x3D=MIC failure, 0x16=local host terminated, 0x22=LMP timeout. Key to diagnosing the ~20s flap.
  wrapper->logEvt("DISCONNECT %s reason=0x%02x", addr.toString().c_str(), param->disconnect.reason);
  wrapper->clearAuthentication();
  wrapper->set_device_connected(false);
  // ATT MTU is PER-CONNECTION state: every new connection restarts at 23 until the central
  // renegotiates. Without this reset a reconnect inherits the previous connection's value, and the
  // log dump would size chunks (and pass its mtu>=64 guard) on stale state - broken transfers after
  // any reconnect, which the poller does on every dongle wake.
  wrapper->set_mtu(23);
  // ABD8's CCCD is per-connection state too: BLE2902 stores a plain RAM byte that nothing resets, so
  // without this a reconnect would look already-subscribed and the auth gate's greet-on-subscribe
  // (its rising-edge trigger) would fire before the app's CCCD write actually armed the pipe - the
  // exact race the greeting exists to eliminate. Safe on the Bluedroid task: one RAM byte, no flash.
  // Deliberately only ABD8: the other CCCDs have no edge-triggered behaviour riding on them.
  wrapper->clearAuthSubscription();
  pServer->startAdvertising(); // restart advertising
}

/*
  Log the negotiated ATT MTU. Our ceiling is 517 (BLEDevice::setMTU); iOS settles at ~185 on its own, Android
  gets whatever the app requested (the transfer engines ask for 517). This matters for the app's notify path:
  a NOTIFY can't exceed MTU-3 (no fragmentation), so at the 23-byte default our ~150B telemetry frame would be
  truncated in the notification. (Reads are unaffected - they use long/blob reads - but if the app parses the
  notify payload directly, the MTU must fit the frame.)
*/
void ServerCallbacks::onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  wrapper->set_mtu(param->mtu.mtu); // the log-dump chunk size is derived from this (notify max = MTU-3)
  wrapper->logEvt("MTU negotiated=%d (iOS ~185, Android 247 = single-radio-packet ceiling)", param->mtu.mtu);
}

/*
  Pairing finished, one way or the other. THE line to look for when a transfer misbehaves after enabling
  bonding: "SEC bonded" means the link is encrypted, "SEC failed" means it is not - and because no
  characteristic requires encryption yet, a failure is a security downgrade, NOT a broken dongle.
  fail_reason is NOT a raw SMP status: BTA offsets SMP codes by a base (HCI_ERR_MAX_ERR + 0x10) so they
  land in roughly 0x53-0x6B, which is why the useful values all look like 0x5x/0x6x. The exact mnemonic
  depends on the base in this IDF build, so treat the number as a fingerprint to compare across runs
  rather than a name. Empirically: 0x63 = the phone offered an LTK we have no bond for, i.e. one side
  forgot the pairing - a rotated RPA we cannot resolve (fixed by distributing ID_KEY, see init), or a
  BONDCLEAR here without a matching forget/unpair on the phone.
*/
void SecurityCallbacks::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
  if (cmpl.success) {
    // Do NOT reach for cmpl.key_present to tell "new pairing" from "reused LTK": it reports whether a
    // BR/EDR classic link key is attached to this event, and an LE pairing never attaches one (the LTK
    // arrives via the separate key-exchange events). It read false immediately after a BONDCLEAR + fresh
    // pair, which is how we found that out. The bond COUNT is the honest signal - it steps up on a real
    // pairing and stays put on a reconnect that reused the stored key.
    wrapper->logEvt("SEC bonded mode=0x%02x bonds=%d", (int)cmpl.auth_mode, esp_ble_get_bond_device_num());
  } else {
    wrapper->logEvt("SEC failed reason=0x%02x (link unencrypted, transfers still work)", (int)cmpl.fail_reason);
  }
}

/*
  Print any message recieved
*/
void DeveloperCallbacks::onWrite(BLECharacteristic* pDeveloperCharacteristic) {
  std::string value = pDeveloperCharacteristic->getValue();
  if (value.length() > 0) {
    // The app may use the "developer"/aux characteristic (ABD3) for a keep-alive or challenge. Log whatever
    // arrives here to flash so a post-setup write on this channel shows up in the DUMP too.
    wrapper->logEvt("ABD3 rx: %.70s", value.c_str());
  }
}

/*
  The app's notify-then-READ: after each ABD1 notification it issues a separate GATT read and parses that.
  logData()'s setValue() keeps ABD1's readable value = the latest {"tlm":{...}} frame, so the read here returns
  current data with no work needed. Logging it confirms the app is exercising the read path (if this never
  appears while connected, the app switched to parsing the notify payload directly - which is fine, our notify
  already carries the full frame).
*/
void TelemetryCallbacks::onRead(BLECharacteristic* pTelemetryCharacteristic) {
  wrapper->logEvt("ABD1 read by app");
}

/*
  Send a 20-byte OTA ACK/status frame to the app (ABD5). A BLE notify carries the characteristic's value, so
  this necessarily overwrites ABD5's readable value - which is why the installed-version JSON published by
  setOtaInfo() is only guaranteed readable while idle (during a transfer a read returns the latest ACK).
  Null-checked because deinit() nulls the pointer before deep sleep.
*/
bool BLEWrapper::notifyOta(const uint8_t* frame, size_t len) {
  if (!device_connected || !pOtaControlCharacteristic || !frame || !len) return false;
  pOtaControlCharacteristic->setValue((uint8_t*)frame, len);
  pOtaControlCharacteristic->notify();
  return true;
}

/*
  Publish the readable value of ABD5 so the app can read the installed firmware version + OTA state with a
  plain GATT read, without starting a transfer. No notify - this is a passive value.
*/
void BLEWrapper::setOtaInfo(const std::string& info) {
  if (!pOtaControlCharacteristic) return;
  pOtaControlCharacteristic->setValue(info);
}

/*
  Forward-only OTA characteristic callbacks (Bluedroid task). getData()/getLength() are used rather than
  getValue() so binary firmware bytes pass through untouched.
*/
void OtaControlCallbacks::onWrite(BLECharacteristic* pOtaControlCharacteristic) {
  wrapper->dispatchOtaCtrl(pOtaControlCharacteristic->getData(), pOtaControlCharacteristic->getLength());
}

void OtaDataCallbacks::onWrite(BLECharacteristic* pOtaDataCharacteristic) {
  wrapper->dispatchOtaData(pOtaDataCharacteristic->getData(), pOtaDataCharacteristic->getLength());
}

/* Forward-only log-dump callback (Bluedroid task) - all handling happens in Helpers/log_dump.h. */
void LogDumpCallbacks::onWrite(BLECharacteristic* pLogDumpCharacteristic) {
  wrapper->dispatchLogDump(pLogDumpCharacteristic->getData(), pLogDumpCharacteristic->getLength());
}

void AuthCallbacks::onWrite(BLECharacteristic* pAuthCharacteristic) {
  wrapper->dispatchAuth(pAuthCharacteristic->getData(), pAuthCharacteristic->getLength());
}

/* Fires synchronously inside notify(): ERROR_GATT = Bluedroid's TX queue was full and the frame was
   dropped - the one failure notifyLogDump retries. Runs on the caller's (loop) task. */
void LogDumpCallbacks::onStatus(BLECharacteristic* pLogDumpCharacteristic, Status s, uint32_t code) {
  if (s == BLECharacteristicCallbacks::Status::ERROR_GATT) {
    wrapper->set_logdump_notify_err(true);
  }
}

/* Send one log-dump frame (binary chunk or JSON reply) to the app on ABD7. Null-checked because
   deinit() nulls the pointer before deep sleep - never cache it outside the wrapper. */
/* Auth-gate replies are single small JSON notifications at human cadence (one per handshake step), so
   unlike notifyLogDump there is no backpressure loop - a full TX queue is not a normal event here, and
   the app re-requests a challenge if a reply is lost. */
bool BLEWrapper::notifySetup(const std::string& msg) {
  if (!device_connected || !pStartupCharacteristic) return false;
  pStartupCharacteristic->setValue(msg);
  pStartupCharacteristic->notify();
  return true;
}

bool BLEWrapper::notifyAuth(const uint8_t* frame, size_t len) {
  if (!device_connected || !pAuthCharacteristic || !frame || !len) return false;
  pAuthCharacteristic->setValue((uint8_t*)frame, len);
  pAuthCharacteristic->notify();
  return true;
}

bool BLEWrapper::authSubscribed() {
  if (!pAuthCharacteristic) return false;
  BLE2902* cccd = (BLE2902*)pAuthCharacteristic->getDescriptorByUUID((uint16_t)0x2902);
  return cccd != nullptr && cccd->getNotifications();
}

void BLEWrapper::clearAuthSubscription() {
  if (!pAuthCharacteristic) return;
  BLE2902* cccd = (BLE2902*)pAuthCharacteristic->getDescriptorByUUID((uint16_t)0x2902);
  if (cccd) cccd->setNotifications(false);
}

bool BLEWrapper::notifyLogDump(const uint8_t* frame, size_t len) {
  if (!device_connected || !pLogDumpCharacteristic || !frame || !len) return false;
  if (!logDumpSubscribed()) return false; // notify() would silently drop it anyway
  pLogDumpCharacteristic->setValue((uint8_t*)frame, len);
  // Send with backpressure: Bluedroid returns ESP_FAIL when its TX queue is full (it never blocks),
  // and at the log dump's production rate that is a normal event, not an error - measured: clustered
  // drops costing the app a recovery timeout each. notify() discards that status, but reports it via
  // the onStatus callback (LogDumpCallbacks flags ERROR_GATT for us) - wait for the radio to drain a
  // connection event and resend, self-pacing the dump to the true link rate. Bounded (~250ms) so a
  // dead link can't wedge the loop task; a chunk given up on is recovered by the app's offset
  // re-request (frames are idempotent by design).
  for (int attempt = 0; attempt < 25; ++attempt) {
    logdump_notify_err = false;
    pLogDumpCharacteristic->notify();
    if (!logdump_notify_err) return true;
    delay(10); // queue full - let the radio drain, then resend
  }
  return false;
}

bool BLEWrapper::logDumpSubscribed() {
  if (!pLogDumpCharacteristic) return false;
  BLE2902* cccd = (BLE2902*)pLogDumpCharacteristic->getDescriptorByUUID((uint16_t)0x2902);
  return cccd != nullptr && cccd->getNotifications();
}