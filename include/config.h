#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS via the Preferences library.
//
//   wakeAddrs  - up to four bound Bluetooth MACs, lower-case colon form ("aa:bb:..").
//   wakeAddr   - first bound MAC, retained for single-controller compatibility.
//   passHash   - SHA-256 hex of the portal password ("" => not set yet).
//   forceSetup - request that the next boot enters the WiFi setup portal.
//   wifiSsid   - station-mode SSID ("" => WiFi disabled, AP-only at setup).
//   wifiPass   - station-mode passphrase ("" => open network).
//   wakeOnLan  - listen for WoL magic packets while connected to WiFi.
//   mqttHost   - broker hostname or IP ("" => MQTT disabled).
//   mqttPort   - broker port (default 1883).
//   mqttUser   - broker username ("" => anonymous).
//   mqttPass   - broker password.
//   deviceName - friendly name shown in Home Assistant.
//   btHost     - the BC250's Bluetooth Classic address ("" => PS-button wake off).
//                WROOM boots with this identity while the BC250 is off.
//   psLearn    - one-shot: the next setup boot uses btHost so PS-button pages
//                can be registered. Cleared when that setup session starts.
//
// Every field is optional. A device with no WiFi and no MQTT configured must
// behave exactly as it did before those fields existed: the button is the
// primary control and has to work standalone.
constexpr uint8_t MAX_WAKE_CONTROLLERS = 4;

struct Config {
  String   wakeAddr;  // first address, retained for older portal clients
  String   wakeAddrs[MAX_WAKE_CONTROLLERS];
  uint8_t  wakeAddrCount;
  String   passHash;
  bool     forceSetup;
  String   wifiSsid;
  String   wifiPass;
  bool     wakeOnLan;
  String   mqttHost;
  uint16_t mqttPort;
  String   mqttUser;
  String   mqttPass;
  String   deviceName;
  String   btHost;
  bool     psLearn;
};

extern Config config;

// Default friendly name when the user hasn't set one.
extern const char *const DEFAULT_DEVICE_NAME;

void loadConfig();

bool validWakeAddr(const String &addr);
bool setWakeAddrs(const String *addrs, uint8_t count);
void setPassHash(const String &hash);
void setForceSetup(bool force);

// Grouped setters. Each writes all of its keys in a single NVS transaction —
// the per-key putString() below opens and closes the namespace every call.
void setWifiSettings(const String &ssid, const String &pass, bool wakeOnLan);
void setMqttSettings(const String &host, uint16_t port, const String &user,
                     const String &pass);
void setDeviceName(const String &name);
bool setBtHost(const String &addr);  // "" clears; false on invalid input
void setPsLearn(bool learn);

// Provisioned enough to leave the setup portal: a password has been set.
// Deliberately password-only — WiFi, MQTT and the bound controller are each
// independently skippable, and nothing in setup() consults this.
bool isConfigured();

bool wifiConfigured();  // an SSID is set
bool mqttConfigured();  // wifiConfigured() and a broker host is set

uint16_t    mqttPort();        // config.mqttPort, or 1883 if unset/invalid
const char *haDeviceName();    // config.deviceName, or DEFAULT_DEVICE_NAME
