#pragma once
/*
  BLE protocol versions - what a phone's session may do, chosen once per session grant in the ABD8 handshake.
  Contains: the proof's "proto" parse and the selection (bleProtoParse, bleProtoSelect, bleProtoAccept, the nonce
  frame's range field), the per-version tables (kProtoV0, kProtoV1, kProtoNone) and the current session's table
  (bleProto()).

  Every feature used to add its own capability flag and its own "only if you saw that" rule (the "kex":1 greet, the
  ABD5 "relay_ack":1 of #90's first cut). Instead each side states the versions it speaks once per session, and the
  session runs at the highest one both speak:
    dongle -> {"nonce":"..","id":"..","proto":[MIN,MAX]}   every nonce frame (auth_gate.h authSendNonce)
    app    -> {"proof":"..","proto":v}                    the version the app chose, only if the nonce offered a range
    dongle -> {"auth":"ok","proto":v}                     the session's version, only if the proof carried one
  Absent means 0, and malformed means absent: a version is a JSON unsigned integer token 0..255 ("1", -1, 1.5, 1.0 and
  2^40 are not). A version above BLE_PROTO_MAX runs at BLE_PROTO_MAX; one below BLE_PROTO_MIN gets a session with no
  protocol (kProtoNone): OTA and the log dump work, every ABD2 write is answered PROTO, nothing is relayed on ABD1. A
  negotiating session also needs MTU >= 96 when the proof lands, the app's own floor, so v1's seq always fits the
  notify; below it the dongle picks v0.

  The version belongs to the grant: it is chosen when the proof that creates a grant is accepted, and a further proof
  on the same grant (the wizard, the connection screen and the poller can all run the handshake on one link) keeps it.
  There are no reset sites: bleProto() answers the table only while the grant it was chosen for is the live one, so a
  disconnect, authreset or AUTHCLEAR (all of which clear authenticated) and any later grant fall back to v0.

  Callers never test a version: they call the table. The relay machinery stays in ble_relay.h and provides v1's
  entries. A later version builds its table from the one below and overrides only what it changes; adding a field
  is a compile error at every table, which is the point.

  This file is #included by logger.ino into the single sketch translation unit - it is NOT a standalone compilation
  unit - straight after ble_relay.h (whose functions the tables name) and before auth_gate.h (which selects).
*/

// What differs between protocol versions. Plain parameters, positional: Arduino-ESP32 builds with -std=gnu++11.
struct BleProtocol {
  uint8_t version;
  uint32_t (*nextSeq)();                                  // the seq the next ABD1 frame carries; 0: none
  void (*frameNotified)(uint32_t seq);                    // a frame went out on ABD1 under seq
  bool (*phoneIsTransport)();                             // may cellular stand down for this authenticated phone?
  bool (*sessionMessage)(const char* key, const json& value); // an ABD9 message; false: not in this version
  bool setupAllowed;                                      // false: no-protocol session (no ABD1 relay, ABD2 PROTO)
  bool relayNeedsSeq;                                     // a frame counts as relayed only if it carries a seq
};

static uint32_t bleNoSeq() { return 0; }
static void bleNoFrame(uint32_t) {}
static bool bleNever() { return false; }
static bool bleNoMessages(const char*, const json&) { return false; }

static const uint8_t BLE_PROTO_NONE = 0xFF; // the version of a session with no protocol

constexpr BleProtocol kProtoNone = { BLE_PROTO_NONE, bleNoSeq, bleNoFrame, bleNever, bleNoMessages, false, false };
// v0 is today's protocol. Until the relay session moves to ABD9, v0 and v1 both still take #90's ABD2 announce.
constexpr BleProtocol kProtoV0 = { 0, relayNextSeq, relayFrameNotified, relayTransportLive, bleNoMessages, true, false };
// v1 (#90): the frames of a subscribed app carry "seq" beside "tlm", it acks them, and stops being the transport when
// its acks stall.
constexpr BleProtocol kProtoV1 = { 1, relayNextSeq, relayFrameNotified, relayTransportLive, bleNoMessages, true, true };

constexpr const BleProtocol* kProtocols[] = { &kProtoV0, &kProtoV1 };
static const uint8_t BLE_PROTO_MAX = sizeof(kProtocols) / sizeof(kProtocols[0]) - 1;
constexpr bool bleProtoTablesInOrder(size_t i) {
  return i >= sizeof(kProtocols) / sizeof(kProtocols[0]) || (kProtocols[i]->version == i && bleProtoTablesInOrder(i + 1));
}
static_assert(bleProtoTablesInOrder(0), "kProtocols[v] is the table of version v");
static_assert(BLE_PROTO_MIN <= BLE_PROTO_MAX, "BLE_PROTO_MIN names a version this firmware has no table for");
static_assert(BLE_PROTO_MAX < BLE_PROTO_NONE, "0xFF is the no-protocol session's");

// The session's table and the grant it was chosen for (bleProtoAccept). Loop task only.
static const BleProtocol* g_proto = &kProtoV0;
static uint32_t g_proto_grant = 0;

// The current session's table: v0 when no session is live, or the live one is not the grant it was chosen for.
const BleProtocol* bleProto() {
  return bleAuthenticated() && bleAuthGrant() == g_proto_grant ? g_proto : &kProtoV0;
}

// Through a forward declaration in ble.h (included long before this file): bleRelaying() asks it.
bool bleProtoPhoneIsTransport() {
  return bleProto()->phoneIsTransport();
}

// A version: a JSON unsigned integer token, 0..255. True and asked set when the proof offers one; anything else, absent
// or malformed, offers none (rule 2: malformed means absent, and never fails the proof).
bool bleProtoParse(const json& proof, uint8_t& asked) {
  if (!proof.is_object()) return false;
  const auto it = proof.find("proto");
  if (it == proof.end() || !it->is_number_unsigned() || it->get<uint64_t>() > 255) return false;
  asked = (uint8_t)it->get<uint64_t>();
  return true;
}

// The session's version for a proof that offered asked (or nothing: 0) at this MTU; BLE_PROTO_NONE below the minimum.
uint8_t bleProtoSelect(bool offered, uint8_t asked, uint16_t mtu) {
  uint8_t v = offered ? asked : 0;
  if (v > BLE_PROTO_MAX) v = BLE_PROTO_MAX;
  if (mtu < 96) v = 0; // the notify budget a negotiating session relies on (v1's seq) is not there
  return v < BLE_PROTO_MIN ? BLE_PROTO_NONE : v;
}

// The range every nonce frame carries: ,"proto":[BLE_PROTO_MIN,BLE_PROTO_MAX]. 14 B today, 18 B at worst.
size_t bleProtoRangeField(char* out, size_t size) {
  const int n = snprintf(out, size, ",\"proto\":[%u,%u]", (unsigned)BLE_PROTO_MIN, (unsigned)BLE_PROTO_MAX);
  return n < 0 ? 0 : (size_t)n;
}

// Loop task, once a proof has passed the HMAC check (auth_gate.h). A proof that creates a grant selects its version;
// one on a live grant keeps it. reply gets the ok frame to notify, which names the session's version only when the
// proof offered one, so an app that does not negotiate reads exactly what it always has.
void bleProtoAccept(const json& proof, char* reply, size_t size) {
#if ENABLE_BLE
  uint8_t asked = 0;
  const bool offered = bleProtoParse(proof, asked);
  char app[6];
  if (offered) snprintf(app, sizeof(app), "%u", (unsigned)asked);
  else snprintf(app, sizeof(app), "%s", proof.contains("proto") ? "bad" : "none");
  if (!ble.get_authenticated()) {
    const uint8_t v = bleProtoSelect(offered, asked, ble.mtu());
    const BleProtocol* table = v == BLE_PROTO_NONE ? &kProtoNone : kProtocols[v];
    // Before the grant: ABD2's PROTO refusal is decided on the Bluedroid task, which reads this bit once authenticated
    // is set, and never sees this table.
    ble.set_setup_allowed(table->setupAllowed);
    // The proof IS the session grant: the legacy AUTH write only unlocks the ABD2 profile push, so
    // telemetry/OTA/log-dump are withheld until this line.
    ble.set_authenticated(true);
    g_proto_grant = bleAuthGrant();
    g_proto = table;
    if (v == BLE_PROTO_NONE)
      logEvent("AUTH2 ok - no protocol (app=%s below min=%u)%s", app, (unsigned)BLE_PROTO_MIN, ble.mtu() < 96 ? " mtu<96" : "");
    else
      logEvent("AUTH2 ok - session granted proto=%u app=%s range=%u-%u mtu=%u", (unsigned)v, app,
        (unsigned)BLE_PROTO_MIN, (unsigned)BLE_PROTO_MAX, (unsigned)ble.mtu());
  } else {
    logEvent("AUTH2 ok - session kept proto=%u (asked %s)", (unsigned)bleProto()->version, app);
  }
  const uint8_t version = bleProto()->version;
  if (offered && version != BLE_PROTO_NONE) snprintf(reply, size, "{\"auth\":\"ok\",\"proto\":%u}", (unsigned)version);
  else snprintf(reply, size, "{\"auth\":\"ok\"}");
#else
  snprintf(reply, size, "{\"auth\":\"ok\"}");
#endif
}
