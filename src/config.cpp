#include "config.h"
#include <Preferences.h>

const char *const DEFAULT_DEVICE_NAME = "BC250";

// Broker port used when nothing is stored (or a stored value is nonsense).
static const uint16_t DEFAULT_MQTT_PORT = 1883;

Config config{};

static Preferences prefs;
static const char *NS = "bc250";

// NVS keys are capped at 15 characters, so the stored names don't always match
// the struct field names ("devName" vs deviceName). Keep them here, once.
static const char *K_WAKE_ADDR  = "wakeAddr";
static const char *K_WAKE_ADDRS = "wakeAddrs";
static const char *K_FORCE_SETUP = "forceSetup";
static const char *K_WIFI_SSID  = "wifiSsid";
static const char *K_WIFI_PASS  = "wifiPass";
static const char *K_WAKE_ON_LAN = "wakeOnLan";
static const char *K_MQTT_HOST  = "mqttHost";
static const char *K_MQTT_PORT  = "mqttPort";
static const char *K_MQTT_USER  = "mqttUser";
static const char *K_MQTT_PASS  = "mqttPass";
static const char *K_DEV_NAME   = "devName";
// Also read before Arduino starts (src/bt_host.cpp); keep the names in sync.
static const char *K_BT_HOST    = "btHost";
static const char *K_PS_LEARN   = "psLearn";

void loadConfig() {
  prefs.begin(NS, true);  // read-only
  // Every read carries a default, so a device provisioned before these keys
  // existed upgrades in place — no NVS wipe, no reprovisioning.
  config.wakeAddrCount = 0;
  for (auto &addr : config.wakeAddrs) addr = "";
  // New list takes precedence, including an explicitly empty list.
  String stored = prefs.isKey(K_WAKE_ADDRS)
      ? prefs.getString(K_WAKE_ADDRS, "") : prefs.getString(K_WAKE_ADDR, "");
  while (!stored.isEmpty() && config.wakeAddrCount < MAX_WAKE_CONTROLLERS) {
    int comma = stored.indexOf(',');
    String addr = comma < 0 ? stored : stored.substring(0, comma);
    addr.trim();
    addr.toLowerCase();
    bool duplicate = false;
    for (uint8_t i = 0; i < config.wakeAddrCount; ++i)
      duplicate |= config.wakeAddrs[i] == addr;
    if (validWakeAddr(addr) && !duplicate)
      config.wakeAddrs[config.wakeAddrCount++] = addr;
    if (comma < 0) break;
    stored = stored.substring(comma + 1);
  }
  config.wakeAddr = config.wakeAddrCount ? config.wakeAddrs[0] : "";
  config.forceSetup = prefs.getBool(K_FORCE_SETUP, false);
  config.wifiSsid   = prefs.getString(K_WIFI_SSID, "");
  config.wifiPass   = prefs.getString(K_WIFI_PASS, "");
  config.wakeOnLan  = prefs.getBool(K_WAKE_ON_LAN, false);
  config.mqttHost   = prefs.getString(K_MQTT_HOST, "");
  config.mqttPort   = prefs.getUShort(K_MQTT_PORT, DEFAULT_MQTT_PORT);
  config.mqttUser   = prefs.getString(K_MQTT_USER, "");
  config.mqttPass   = prefs.getString(K_MQTT_PASS, "");
  config.deviceName = prefs.getString(K_DEV_NAME, "");
  config.btHost     = prefs.getString(K_BT_HOST, "");
  if (!validWakeAddr(config.btHost)) config.btHost = "";
  config.psLearn    = prefs.getBool(K_PS_LEARN, false);
  prefs.end();
}

static void putString(const char *key, const String &val) {
  prefs.begin(NS, false);
  prefs.putString(key, val);
  prefs.end();
}

bool validWakeAddr(const String &addr) {
  if (addr.length() != 17) return false;
  for (unsigned i = 0; i < 17; ++i) {
    char c = addr[i];
    if (i % 3 == 2) { if (c != ':') return false; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

bool setWakeAddrs(const String *addrs, uint8_t count) {
  if (count > MAX_WAKE_CONTROLLERS) return false;
  String normalized[MAX_WAKE_CONTROLLERS];
  String stored;
  for (uint8_t i = 0; i < count; ++i) {
    normalized[i] = addrs[i];
    normalized[i].trim();
    normalized[i].toLowerCase();
    if (!validWakeAddr(normalized[i])) return false;
    for (uint8_t j = 0; j < i; ++j)
      if (normalized[i] == normalized[j]) return false;
    if (i) stored += ',';
    stored += normalized[i];
  }
  if (!prefs.begin(NS, false)) return false;
  bool saved = prefs.putString(K_WAKE_ADDRS, stored) == stored.length() &&
      prefs.isKey(K_WAKE_ADDRS) && prefs.getString(K_WAKE_ADDRS, "") == stored;
  // Keep the first address for rollback to single-controller firmware.
  if (saved) prefs.putString(K_WAKE_ADDR, count ? normalized[0] : String(""));
  prefs.end();
  if (!saved) return false;
  for (uint8_t i = 0; i < MAX_WAKE_CONTROLLERS; ++i)
    config.wakeAddrs[i] = normalized[i];
  config.wakeAddrCount = count;
  config.wakeAddr = count ? normalized[0] : String("");
  return true;
}

void setForceSetup(bool force) {
  config.forceSetup = force;
  prefs.begin(NS, false);
  prefs.putBool(K_FORCE_SETUP, force);
  prefs.end();
}

void setWifiSettings(const String &ssid, const String &pass, bool wakeOnLan) {
  config.wifiSsid = ssid;
  config.wifiPass = pass;
  config.wakeOnLan = !ssid.isEmpty() && wakeOnLan;
  prefs.begin(NS, false);
  prefs.putString(K_WIFI_SSID, ssid);
  prefs.putString(K_WIFI_PASS, pass);
  prefs.putBool(K_WAKE_ON_LAN, config.wakeOnLan);
  prefs.end();
}

void setMqttSettings(const String &host, uint16_t port, const String &user,
                     const String &pass) {
  config.mqttHost = host;
  config.mqttPort = port ? port : DEFAULT_MQTT_PORT;
  config.mqttUser = user;
  config.mqttPass = pass;
  prefs.begin(NS, false);
  prefs.putString(K_MQTT_HOST, config.mqttHost);
  prefs.putUShort(K_MQTT_PORT, config.mqttPort);
  prefs.putString(K_MQTT_USER, config.mqttUser);
  prefs.putString(K_MQTT_PASS, config.mqttPass);
  prefs.end();
}

void setDeviceName(const String &name) {
  config.deviceName = name;
  putString(K_DEV_NAME, name);
}

bool setBtHost(const String &addr) {
  String normalized = addr;
  normalized.trim();
  normalized.toLowerCase();
  if (!normalized.isEmpty() && !validWakeAddr(normalized)) return false;
  config.btHost = normalized;
  putString(K_BT_HOST, normalized);
  return true;
}

void setPsLearn(bool learn) {
  config.psLearn = learn;
  prefs.begin(NS, false);
  prefs.putBool(K_PS_LEARN, learn);
  prefs.end();
}

void factoryReset() {
  // Wipe every stored value so the board can be handed on clean: WiFi, portal
  // password, MQTT, bound controllers and the spoofed BC250 host all live in
  // this one NVS namespace (raw-NVS keys set by bt_host.cpp included), so a
  // single clear() removes the lot. Arm the setup portal for the next boot so
  // the new owner lands straight in it. The caller is expected to reboot.
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();
  setForceSetup(true);
}

bool wifiConfigured() {
  return config.wifiSsid.length() > 0;
}

bool mqttConfigured() {
  return wifiConfigured() && config.mqttHost.length() > 0;
}

uint16_t mqttPort() {
  return config.mqttPort ? config.mqttPort : DEFAULT_MQTT_PORT;
}

const char *haDeviceName() {
  return config.deviceName.length() ? config.deviceName.c_str()
                                    : DEFAULT_DEVICE_NAME;
}
