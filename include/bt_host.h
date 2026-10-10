#pragma once
#include <Arduino.h>

// PS-button wake (WROOM / Bluepad32 only).
//
// A DualSense or DualShock 4 woken with the PS button only pages the host it is
// bonded with; it never advertises or answers inquiries. While the BC250 is off,
// the ESP32 therefore boots with the BC250's Bluetooth address and listens for
// those pages. Each page is rejected before authentication, so the controller's
// bond with the BC250 is never touched. The identity is chosen before BTstack
// starts (see src/bt_host.cpp); everything here only drives page scan.
//
// Without a spoofed identity the ESP32 can instead appear as "BC250 Power
// Setup" so the BC250 can be selected from Steam's Bluetooth menu: the BC250's
// connection attempt reveals its address.

// Called on the Bluetooth task with a lower-case "aa:bb:cc:dd:ee:ff" address.
using BtHostIncoming = void (*)(const char *addr);

#if defined(BC250_BLUEPAD32)
bool btHostSpoofed();                // this boot answers pages for btHost
const char *btHostIdentityReason();  // why this boot chose its identity
void btHostLogStatus();
void btHostSetIncoming(BtHostIncoming callback);
// Spoofed only: page scan on. Callers keep it off unless the BC250 is off.
void btHostAllowIncoming(bool allow);
// Not spoofed only: discoverable and connectable as "BC250 Power Setup".
void btHostSetLearning(bool learn);
bool btHostLearnedAddr(char out[18]);  // last BC250 connection attempt
void btHostClearLearned();
#else
inline bool btHostSpoofed() { return false; }
inline const char *btHostIdentityReason() { return "not supported on this board"; }
inline void btHostLogStatus() {}
inline void btHostSetIncoming(BtHostIncoming) {}
inline void btHostAllowIncoming(bool) {}
inline void btHostSetLearning(bool) {}
inline bool btHostLearnedAddr(char *) { return false; }
inline void btHostClearLearned() {}
#endif
