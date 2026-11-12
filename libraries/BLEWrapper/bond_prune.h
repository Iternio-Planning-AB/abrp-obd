#pragma once
#include <stdint.h>
#include <string.h>

/*
  Bond-table policy for BLEWrapper::pruneBonds and rememberAuthenticatedPeer, free of IDF types so
  tests/relay_checks.cpp runs it on a host.

  esp_ble_get_bond_device_list is most-recently-connected FIRST. Bluedroid (IDF 4.4 osi/config.c) prepends a new
  bond's bt_config section, and every connection moves the peer's section to the front
  (btc_storage_update_active_device; encryption alone does not). The order reaches NVS with any bond write and at
  esp_bluedroid_disable before sleep; a reset that skips that (the OTA reboot, a crash, a brownout) loses only that
  wake's moves, and the authenticated-peer record below does not depend on it.

  Recency alone does not identify the owner: pairing is Just Works, so a phone can hold a bond without ever
  passing ABD8. So the bonds of peers that passed ABD8 are recorded (BOND_AUTH_PEERS, newest first, entry 0 = the
  owner) and pruning ranks by that first. An evicted phone is left holding a one-sided bond, which iOS does not
  recover from until the user forgets the dongle (DONGLE_CONTEXT §3), so the owner's bond is never the one to go.
*/
static const int BOND_AUTH_PEERS = 3;
#define BOND_AUTH_PEERS_KEY "bondpeers" // in the "auth" NVS namespace: AUTHCLEAR and the in-app key reset clear it
typedef uint8_t BondAddr[6];

/* Index of addr in the authenticated-peer record (0 = owner), or -1. */
inline int bondAuthRank(const BondAddr* peers, int count, const uint8_t* addr) {
  for (int i = 0; i < count; i++) {
    if (memcmp(peers[i], addr, sizeof(BondAddr)) == 0) return i;
  }
  return -1;
}

/* Move addr to the front of the record (adding it, dropping the oldest when full). False when it already led the
   record, so the caller writes NVS only on a change. */
inline bool bondAuthNote(BondAddr* peers, int& count, const uint8_t* addr) {
  int at = bondAuthRank(peers, count, addr);
  if (at == 0) return false;
  if (at < 0) at = count < BOND_AUTH_PEERS ? count++ : BOND_AUTH_PEERS - 1;
  memmove(peers[1], peers[0], sizeof(BondAddr) * at);
  memcpy(peers[0], addr, sizeof(BondAddr));
  return true;
}

/*
  Free one slot in a table at `limit`, evicting in this order: bonds that never passed ABD8, least recently connected
  first; then recorded peers other than the owner, least recently connected first. The owner's bond is never evicted.
  With no owner in the list (none recorded yet, or its bond is already gone) recency is all there is, and the most
  recently connected bond (entry 0) is kept instead. `rank[i]` is bondAuthRank of list entry i. `evict(i)` queues the
  removal of entry i; a refusal stops pruning rather than moving on to a more valued bond.
*/
template <typename Evict>
int pruneBondList(int num, int limit, const int* rank, Evict evict) {
  if (limit <= 0) return 0;
  bool owner_listed = false;
  for (int i = 0; i < num; i++) owner_listed |= rank[i] == 0;
  int removed = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = num - 1; i >= 0 && num - removed >= limit; i--) {
      bool authenticated = rank[i] >= 0;
      if (authenticated != (pass == 1) || rank[i] == 0 || (!owner_listed && i == 0)) continue;
      if (!evict(i)) return removed;
      removed++;
    }
  }
  return removed;
}
