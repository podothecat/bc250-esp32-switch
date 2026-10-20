#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include "controller_scan.h"
#include <esp_mac.h>

#include "board.h"
#include "config.h"
#include "bt_host.h"

static DNSServer       dnsServer;
static AsyncWebServer  server(80);

// Deferred restart (so the HTTP response can flush before we reboot).
static unsigned long   g_restartAt = 0;

// --- BLE device discovery (bounded, recent-device cache for the picker) ---
struct BleDev {
  char addr[18];
  char name[33];
  int  rssi;
  bool used;
  uint32_t lastSeen;
  bool classic;
  bool logged;   // reported on serial; cleared again when a name arrives
};
static const int       MAX_DEVS = 48;
static BleDev          devs[MAX_DEVS];
static portMUX_TYPE    devsMux = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t BLE_DEVICE_TTL_MS = 30000;
static const uint32_t BLE_SCAN_RETRY_MS = 3000;
static const uint32_t BLE_PICKER_IDLE_MS = 12000;
// Protected by devsMux: the HTTP task only queues scan operations for loop().
static bool           bleRescanRequested = false;
static bool           blePickerRequested = false;
static uint32_t       blePickerLastRequest = 0;
static bool           bleScannerRunning = false;
static const char    *bleScanError = "";
static bool           bleReceived = false;
static uint32_t       bleLastReceived = 0;
static uint32_t       bleEvictions = 0;
// Only accessed by the Arduino loop task.
static uint32_t       bleLastScanAttempt = 0;
static bool           bleScanInitialized = false;
static bool           bleScanRequested = false;

// Caller holds devsMux. Unsigned subtraction also handles millis() rollover.
static void expireBleDevices(uint32_t now) {
  for (auto &dev : devs)
    if (dev.used && uint32_t(now - dev.lastSeen) >= BLE_DEVICE_TTL_MS)
      dev.used = false;
}

// --- WiFi network list (scanned once, before the AP exists) ---
// A live scanNetworks() in AP mode takes the SoftAP off-channel for seconds and
// drops the phone's association, so the list is captured at startup and served
// from this cache. "Rescan" in the UI is re-enter-setup-and-reboot, not a live
// scan. Manual SSID entry stays available for hidden networks.
struct WifiNet {
  char ssid[33];
  int  rssi;
  bool open;
};
static const int MAX_NETS = 24;
static WifiNet   nets[MAX_NETS];
static int       netCount = 0;

static void setupControllerFound(const char *addr, const char *name, int rssi,
                               bool classic) {
  portENTER_CRITICAL(&devsMux);
  uint32_t now = millis();
  bleReceived = true;
  bleLastReceived = now;
  expireBleDevices(now);
  int idx = -1, free = -1, oldest = -1;
  for (int i = 0; i < MAX_DEVS; i++) {
    if (devs[i].used) {
      if (strcmp(devs[i].addr, addr) == 0) { idx = i; break; }
      if (oldest < 0 || uint32_t(now - devs[i].lastSeen) >
                        uint32_t(now - devs[oldest].lastSeen)) oldest = i;
    } else if (free < 0) {
      free = i;
    }
  }
  if (idx < 0) {
    idx = free >= 0 ? free : oldest;
    if (devs[idx].used) ++bleEvictions;
    devs[idx].used = true;
    strncpy(devs[idx].addr, addr, sizeof(devs[idx].addr) - 1);
    devs[idx].addr[sizeof(devs[idx].addr) - 1] = 0;
    devs[idx].name[0] = 0;
    devs[idx].lastSeen = 0;
    devs[idx].logged = false;
  }
  if (rssi != -127 || !devs[idx].lastSeen) devs[idx].rssi = rssi;
  devs[idx].classic = classic;
  devs[idx].lastSeen = now;
  if (name[0] && strncmp(devs[idx].name, name, sizeof(devs[idx].name) - 1) != 0) {
    strncpy(devs[idx].name, name, sizeof(devs[idx].name) - 1);
    devs[idx].name[sizeof(devs[idx].name) - 1] = 0;
    devs[idx].logged = false;
  }
  portEXIT_CRITICAL(&devsMux);
}

// Reports picker entries on serial, for the power-log page's registration view.
// The callback above runs on the Bluetooth task under a spinlock, so printing
// happens here on the Arduino task. A device is reported when it enters the
// list and again when its name changes.
static void logNewBleDevices() {
  BleDev fresh[4];
  int n = 0;
  portENTER_CRITICAL(&devsMux);
  for (auto &dev : devs) {
    if (!dev.used || dev.logged) continue;
    dev.logged = true;
    fresh[n++] = dev;
    if (n == 4) break;
  }
  portEXIT_CRITICAL(&devsMux);
  for (int i = 0; i < n; ++i)
    Serial.printf("[PORTAL] device %s %s rssi=%d name=%s\n", fresh[i].addr,
                  fresh[i].classic ? "classic" : "ble", fresh[i].rssi,
                  fresh[i].name[0] ? fresh[i].name : "-");
}

// --- Helpers ---

static void sendJsonError(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

static void serveIndex(AsyncWebServerRequest *req) {
  if (SPIFFS.exists("/index.html")) {
    req->send(SPIFFS, "/index.html", "text/html");
  } else {
    req->send(200, "text/html",
              "<h1>BC250 Switch</h1><p>Filesystem image missing. Flash it with "
              "<code>pio run -t uploadfs</code>.</p>");
  }
}

// Scan for access points and cache the result. Must run BEFORE the SoftAP is
// started — see the WifiNet comment above. Costs ~3s at startup, which is
// before anyone can have connected to the AP.
static void scanWifiNetworks() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  netCount = 0;
  for (int i = 0; i < n && netCount < MAX_NETS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden; use manual entry for these
    int  rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

    // Dedupe by SSID, keeping the strongest signal (mesh/repeater networks
    // advertise the same SSID from several BSSIDs).
    int found = -1;
    for (int j = 0; j < netCount; j++) {
      if (ssid == nets[j].ssid) { found = j; break; }
    }
    if (found >= 0) {
      if (rssi > nets[found].rssi) nets[found].rssi = rssi;
      continue;
    }
    strncpy(nets[netCount].ssid, ssid.c_str(), sizeof(nets[netCount].ssid) - 1);
    nets[netCount].ssid[sizeof(nets[netCount].ssid) - 1] = 0;
    nets[netCount].rssi = rssi;
    nets[netCount].open = open;
    netCount++;
  }
  WiFi.scanDelete();
  Serial.printf("[PORTAL] wifi scan: %d found, %d cached\n", n, netCount);
}

// --- PlayStation host (BC250 Bluetooth identity) setup ---
//
// Two jobs, both console-free:
//   1. Learn the BC250's Bluetooth address. In a normal (non-spoofed) session
//      the device advertises as "BC250 Power Setup"; selecting it in Steam's
//      Bluetooth menu makes the BC250 page us, and its address is captured from
//      the (rejected) connection request. The learning window is only open while
//      the host screen is polling, like the controller picker.
//   2. Register the PlayStation controllers. "Continue to controller step"
//      reboots into a spoofed session (psLearn) with the BC250 off; pressing a
//      controller's PS button then pages our spoofed address and the controller
//      is listed in the normal picker.
static bool        g_psSession = false;     // this boot spoofs the BC250
static volatile bool g_boardUp  = false;    // debounced-free TPMS1 snapshot
static volatile bool g_psuOn    = false;    // PS_ON# asserted via the portal
static portMUX_TYPE  hostMux    = portMUX_INITIALIZER_UNLOCKED;
static bool        g_hostLearnRequested  = false;   // guarded by hostMux
static uint32_t    g_hostLearnLastRequest = 0;      // guarded by hostMux
static bool        g_hostLearning = false;          // last applied, loop only
static const uint32_t HOST_LEARN_IDLE_MS = 12000;

static bool readPortalBoardSense() {
  uint32_t acc = 0;
  for (int i = 0; i < SENSE_OVERSAMPLE; ++i) acc += analogReadMilliVolts(BOARD_SENSE);
  return acc / SENSE_OVERSAMPLE > (uint32_t)SENSE_HIGH_MV;
}

// --- Endpoint handlers ---

static void handleStatus(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["wakeAddr"]    = config.wakeAddr;
  JsonArray addrs = doc["wakeAddrs"].to<JsonArray>();
  for (uint8_t i = 0; i < config.wakeAddrCount; ++i) addrs.add(config.wakeAddrs[i]);
  doc["psSession"]   = g_psSession;
  doc["classicSupported"] = controllerScanStatus().classicSupported;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// Current settings for the UI to prefill. Authed, unlike /api/status: this
// exposes the network the device is joined to and the broker it talks to.
// Passwords are NEVER returned — only whether one is stored.
static void handleGetSettings(AsyncWebServerRequest *req) {
  uint8_t mac[6];
  esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    Serial.printf("[PORTAL] failed to read WiFi MAC: %s\n", esp_err_to_name(err));
    sendJsonError(req, 500, "could not read WiFi MAC");
    return;
  }
  char wolMac[18];
  snprintf(wolMac, sizeof(wolMac), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  JsonDocument doc;
  doc["wifiSsid"]    = config.wifiSsid;
  doc["wifiPassSet"] = config.wifiPass.length() > 0;
  doc["wakeOnLan"]   = config.wakeOnLan;
  doc["wolMac"]      = wolMac;
  doc["mqttHost"]    = config.mqttHost;
  doc["mqttPort"]    = mqttPort();
  doc["mqttUser"]    = config.mqttUser;
  doc["mqttPassSet"] = config.mqttPass.length() > 0;
  doc["deviceName"]  = config.deviceName;
  doc["wakeAddr"]    = config.wakeAddr;
  doc["btHost"]      = config.btHost;
  JsonArray addrs = doc["wakeAddrs"].to<JsonArray>();
  for (uint8_t i = 0; i < config.wakeAddrCount; ++i) addrs.add(config.wakeAddrs[i]);
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleWifiScan(AsyncWebServerRequest *req) {
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < netCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["open"] = nets[i].open;
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// Password sentinel, shared by /api/wifi and /api/mqtt:
//
//   key ABSENT            -> keep whatever is already stored
//   key PRESENT and ""    -> clear the stored password
//   key PRESENT, non-""   -> replace
//
// This is what lets the UI show "password saved" and submit the form without
// ever round-tripping the secret. Do NOT "simplify" this to `o["password"] |
// config.wifiPass` — that collapses the clear case into the keep case, and a
// user who deletes their password would silently keep the old one.
static bool keyPresent(JsonObject o, const char *key) {
  return !o[key].isNull();
}

static void handleWifi(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String ssid = o["ssid"] | "";
  ssid.trim();

  if (ssid.length() > 32) { sendJsonError(req, 400, "ssid too long"); return; }

  // An empty SSID disables station mode entirely; drop the password with it so
  // no orphaned secret is left in NVS.
  if (ssid.isEmpty()) {
    setWifiSettings("", "", false);
    Serial.println("[PORTAL] wifi cleared (station mode disabled)");
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  String pass = keyPresent(o, "password") ? (o["password"] | "") : config.wifiPass;
  if (pass.length() > 63) { sendJsonError(req, 400, "password too long"); return; }

  bool wakeOnLan = o["wakeOnLan"] | config.wakeOnLan;
  setWifiSettings(ssid, pass, wakeOnLan);
  Serial.printf("[PORTAL] wifi set: ssid='%s' pass=%s wol=%s\n", ssid.c_str(),
                pass.length() ? "(set)" : "(open)",
                config.wakeOnLan ? "on" : "off");
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleMqtt(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String host = o["host"] | "";
  host.trim();

  // An empty host disables MQTT; clear the credentials with it.
  if (host.isEmpty()) {
    setMqttSettings("", mqttPort(), "", "");
    Serial.println("[PORTAL] mqtt cleared (disabled)");
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (host.length() > 64) { sendJsonError(req, 400, "host too long"); return; }

  uint32_t port = o["port"] | (uint32_t)mqttPort();
  if (port < 1 || port > 65535) { sendJsonError(req, 400, "invalid port"); return; }

  String user = o["user"] | "";
  String pass = keyPresent(o, "password") ? (o["password"] | "") : config.mqttPass;

  setMqttSettings(host, (uint16_t)port, user, pass);
  Serial.printf("[PORTAL] mqtt set: %s:%u user=%s pass=%s\n", host.c_str(),
                (unsigned)port, user.length() ? user.c_str() : "(none)",
                pass.length() ? "(set)" : "(none)");
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleDevice(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String name = o["name"] | "";
  name.trim();
  if (name.length() > 32) { sendJsonError(req, 400, "name too long"); return; }
  setDeviceName(name);
  Serial.printf("[PORTAL] device name: '%s'\n",
                name.length() ? name.c_str() : haDeviceName());
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleBleDevices(AsyncWebServerRequest *req) {

  // Snapshot under the lock, then build JSON without holding it.
  BleDev snap[MAX_DEVS];
  JsonDocument doc;
  portENTER_CRITICAL(&devsMux);
  uint32_t now = millis();
  blePickerRequested = true;
  blePickerLastRequest = now;
  expireBleDevices(now);
  memcpy(snap, devs, sizeof(devs));
  bool scanning = bleScannerRunning;
  bool restarting = bleRescanRequested || !bleScannerRunning;
  const char *error = bleScanError;
  bool received = bleReceived;
  uint32_t lastAge = now - bleLastReceived;
  uint32_t evictions = bleEvictions;
  portEXIT_CRITICAL(&devsMux);

  ControllerScanStatus bt = controllerScanStatus();
  doc["classicSupported"] = bt.classicSupported;
  doc["psSession"] = g_psSession;
  doc["bleScanning"] = bt.bleRunning;
  doc["classicScanning"] = bt.classicRunning;
  doc["bleError"] = bt.bleError;
  doc["classicError"] = bt.classicError;
  doc["scanning"] = scanning;
  doc["restarting"] = restarting;
  doc["scanError"] = error;
  doc["capacity"] = MAX_DEVS;
  doc["evictions"] = evictions;
  if (received) doc["lastSeenAgeMs"] = lastAge;
  else doc["lastSeenAgeMs"] = nullptr;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVS; i++) {
    if (!snap[i].used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["addr"] = snap[i].addr;
    o["name"] = snap[i].name;
    o["rssi"] = snap[i].rssi;
    o["ageMs"] = uint32_t(now - snap[i].lastSeen);
    o["transport"] = snap[i].classic ? "classic" : "ble";
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleBleRescan(AsyncWebServerRequest *req) {
  portENTER_CRITICAL(&devsMux);
  bleRescanRequested = true;
  blePickerRequested = true;
  blePickerLastRequest = millis();
  portEXIT_CRITICAL(&devsMux);
  req->send(202, "application/json", "{\"ok\":true}");
}

// All Bluetooth lifecycle operations run on the Arduino task, never the HTTP task.
static void servicePortalBleScan() {
  uint32_t now = millis();
  portENTER_CRITICAL(&devsMux);
  bool pickerActive = blePickerRequested &&
      uint32_t(now - blePickerLastRequest) < BLE_PICKER_IDLE_MS;
  bool rescan = bleRescanRequested;
  expireBleDevices(now);
  portEXIT_CRITICAL(&devsMux);
  // Do not compete with phone association/DHCP. Only authenticated picker
  // polling activates Bluetooth, and loss of all AP clients stops it immediately.
  if (!pickerActive || WiFi.softAPgetStationNum() == 0) {
    if (bleScanRequested && controllerScanStop()) {
      bleScanRequested = false;
      Serial.println("[PORTAL] Bluetooth paused; WiFi setup only");
    }
    portENTER_CRITICAL(&devsMux);
    bleScannerRunning = false;
    portEXIT_CRITICAL(&devsMux);
    return;
  }
  if (bleScanRequested) controllerScanLoop();
  logNewBleDevices();
  bool running = controllerScanRunning();
  portENTER_CRITICAL(&devsMux);
  bleScannerRunning = running;
  portEXIT_CRITICAL(&devsMux);
  if (!rescan && running) return;
  if (uint32_t(now - bleLastScanAttempt) < BLE_SCAN_RETRY_MS) return;
  bleLastScanAttempt = now;

  if (rescan && running && !controllerScanStop()) {
    portENTER_CRITICAL(&devsMux);
    bleScanError = "Could not stop scan; retrying";
    portEXIT_CRITICAL(&devsMux);
    Serial.println("[PORTAL] BLE stop failed; retrying in 3s");
    return;
  }
  if (rescan) {
    portENTER_CRITICAL(&devsMux);
    memset(devs, 0, sizeof(devs));
    bleReceived = false;
    bleEvictions = 0;
    bleRescanRequested = false;
    portEXIT_CRITICAL(&devsMux);
  }
  bool started;
  if (!bleScanInitialized) {
    started = controllerScanBegin(setupControllerFound, true, true);
    bleScanInitialized = true;
  } else started = controllerScanStart();
  bleScanRequested = true;
  portENTER_CRITICAL(&devsMux);
  bleScannerRunning = started && controllerScanRunning();
  bleScanError = bleScannerRunning ? "" : "Could not start scan; retrying";
  portEXIT_CRITICAL(&devsMux);
  Serial.printf("[PORTAL] BLE scan %s%s\n", started ? "started" : "failed",
                started ? "" : "; retrying in 3s");
}

static void handleBleSelect(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String addrs[MAX_WAKE_CONTROLLERS];
  uint8_t count = 0;
  if (o["addrs"].is<JsonArray>()) {
    JsonArray list = o["addrs"].as<JsonArray>();
    if (list.size() > MAX_WAKE_CONTROLLERS) {
      sendJsonError(req, 400, "maximum 4 controllers"); return;
    }
    for (JsonVariant value : list) {
      if (!value.is<const char *>()) {
        sendJsonError(req, 400, "invalid address"); return;
      }
      addrs[count++] = value.as<String>();
    }
  } else if (!o["addrs"].isUnbound() && !o["addrs"].is<JsonArray>()) {
    sendJsonError(req, 400, "expected address list"); return;
  } else if (o["addr"].is<const char *>()) {
    // Accept the old portal's single-address request.
    String addr = o["addr"].as<String>();
    addr.trim();
    if (!addr.isEmpty()) addrs[count++] = addr;
  } else {
    sendJsonError(req, 400, "expected address list"); return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    addrs[i].trim();
    addrs[i].toLowerCase();
    if (!validWakeAddr(addrs[i])) {
      sendJsonError(req, 400, "invalid address"); return;
    }
    for (uint8_t j = 0; j < i; ++j) {
      if (addrs[i] == addrs[j]) {
        sendJsonError(req, 400, "duplicate address"); return;
      }
    }
  }
  if (!setWakeAddrs(addrs, count)) {
    sendJsonError(req, 500, "could not save controllers"); return;
  }
  Serial.printf("[PORTAL] bound %u controller(s)\n", count);
  req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/host[?learn=1] — host-identity status. learn=1 opens (and keeps
// alive) the BC250 discovery window, so "BC250 Power Setup" only shows in Steam
// while this screen is polling.
static void handleHost(AsyncWebServerRequest *req) {
  if (req->hasParam("learn") && req->getParam("learn")->value() == "1") {
    portENTER_CRITICAL(&hostMux);
    g_hostLearnRequested = true;
    g_hostLearnLastRequest = millis();
    portEXIT_CRITICAL(&hostMux);
  }
  JsonDocument doc;
  doc["spoofed"]   = btHostSpoofed();
  doc["psSession"] = g_psSession;
  doc["reason"]    = btHostIdentityReason();
  doc["hostAddr"]  = config.btHost;
  doc["boardUp"]   = g_boardUp;
  doc["psuOn"]     = g_psuOn;
  doc["learning"]  = g_hostLearning;
  char learned[18];
  if (btHostLearnedAddr(learned)) doc["learned"] = learned;
  else doc["learned"] = nullptr;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// POST /api/host/power {on} — assert or release PS_ON# so the BC250 can be
// powered on from the setup screen (setup mode keeps it released otherwise).
static void handleHostPower(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  bool on = o["on"] | false;
  digitalWrite(PS_ON_PIN, on ? PS_ON_ASSERT : PS_ON_RELEASE);
  g_psuOn = on;
  Serial.printf("[PORTAL] host power %s\n", on ? "ON" : "OFF");
  req->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/host/select {addr} — save the learned BC250 address (""" clears).
static void handleHostSelect(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String addr = o["addr"] | "";
  addr.trim();
  addr.toLowerCase();
  if (!setBtHost(addr)) { sendJsonError(req, 400, "invalid address"); return; }
  btHostClearLearned();
  Serial.printf("[PORTAL] BC250 host %s\n",
                addr.isEmpty() ? "cleared" : addr.c_str());
  req->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/host/controller-step — reboot into a spoofed session so PS-button
// pages can be registered. Requires a saved BC250 and the board powered down.
static void handleHostControllerStep(AsyncWebServerRequest *req) {
  if (config.btHost.isEmpty()) { sendJsonError(req, 409, "save the BC250 first"); return; }
  if (g_boardUp) { sendJsonError(req, 409, "turn the BC250 off first"); return; }
  setPsLearn(true);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  g_psuOn = false;
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[PORTAL] controller step requested; rebooting into spoofed session");
  g_restartAt = millis() + 800;
}

static void handleFinish(AsyncWebServerRequest *req) {
  setForceSetup(false);
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[PORTAL] setup complete, restarting into normal mode");
  g_restartAt = millis() + 800;
}

// POST /api/factory-reset — wipe every stored setting and reboot into a fresh
// setup portal, for handing the board to someone else.
static void handleFactoryReset(AsyncWebServerRequest *req) {
  factoryReset();
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[PORTAL] factory reset requested; wiping settings and rebooting");
  g_restartAt = millis() + 800;
}

// --- Lifecycle ---

void portalBegin() {
  Serial.println("=== BC250 PSU controller : SETUP MODE ===");

  // Keep the PSU off and the button readable while configuring.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);
  if (BOOT_BUTTON_PIN >= 0) pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  analogRead(BOARD_SENSE);  // attach to the ADC first (Arduino 3)
  analogSetPinAttenuation(BOARD_SENSE, ADC_11db);

  // A spoofed boot (chosen in bt_host.cpp from the psLearn flag) is a one-shot
  // controller-registration session. Clear the flag now so a later ordinary
  // reboot returns to the "BC250 Power Setup" identity.
  g_psSession = btHostSpoofed();
  if (config.psLearn) setPsLearn(false);
  g_boardUp = readPortalBoardSense();
  Serial.printf("[PORTAL] host identity: %s (%s), board %s\n",
                g_psSession ? "spoofing BC250" : "BC250 Power Setup",
                btHostIdentityReason(), g_boardUp ? "UP" : "down");

  if (!SPIFFS.begin(true)) {
    Serial.println("[PORTAL] WARNING: SPIFFS mount failed");
  }

  // Scan while we're still in STA mode and nothing is associated. Doing this
  // after softAP() would knock any connected phone off the AP for seconds.
  scanWifiNetworks();

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED ||
        event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
      Serial.printf("[PORTAL] AP client %s; clients=%u heap=%u\n",
                    event == ARDUINO_EVENT_WIFI_AP_STACONNECTED ? "connected" : "disconnected",
                    WiFi.softAPgetStationNum(), ESP.getFreeHeap());
    }
  });
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID);  // open network
  // Keep the configured TX power consistent across targets. Must be set AFTER
  // softAP().
  WiFi.setTxPower(AP_TX_POWER);
  Serial.printf("[PORTAL] softAP ret=%d ssid='%s' ip=%s txpwr=%d\n", ok, AP_SSID,
                WiFi.softAPIP().toString().c_str(), WiFi.getTxPower());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Keep Bluetooth uninitialized until the phone is connected and the
  // authenticated controller picker starts polling. Preserve Classic memory.
  bleLastScanAttempt = millis() - BLE_SCAN_RETRY_MS;
  Serial.println("[PORTAL] Bluetooth deferred until controller picker opens");

  // The portal has no password: the setup AP is only up on an explicit button
  // request, and anyone with physical access can reflash anyway, so every
  // endpoint is open. Keep it that way only because the AP exposure window is
  // short and deliberate.
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/ble/devices", HTTP_GET, handleBleDevices);
  server.on("/api/ble/rescan", HTTP_POST, handleBleRescan);
  server.on("/api/host", HTTP_GET, handleHost);
  server.on("/api/host/controller-step", HTTP_POST, handleHostControllerStep);
  server.on("/api/finish", HTTP_POST, handleFinish);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi", handleWifi));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mqtt", handleMqtt));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/device", handleDevice));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/ble/select", handleBleSelect));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/host/power", handleHostPower));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/host/select", handleHostSelect));

  server.on("/", HTTP_GET, serveIndex);
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/api/")) {
      sendJsonError(req, 404, "not found");
    } else {
      serveIndex(req);  // captive-portal catch-all
    }
  });

  server.begin();
  Serial.println("[PORTAL] web server up. Connect to the AP and open http://192.168.4.1");
}

// Host-identity page-scan control, run on the Arduino task. Mirrors the picker's
// idle window: the BC250 discovery identity ("BC250 Power Setup") is only
// connectable while the host screen is actively polling with learn=1.
static void servicePortalHost() {
  uint32_t now = millis();
  g_boardUp = readPortalBoardSense();
  portENTER_CRITICAL(&hostMux);
  bool learnActive = g_hostLearnRequested &&
      uint32_t(now - g_hostLearnLastRequest) < HOST_LEARN_IDLE_MS;
  portEXIT_CRITICAL(&hostMux);
  learnActive = learnActive && WiFi.softAPgetStationNum() > 0;
  // Report each new BC250 candidate (a Classic connection attempt while the
  // host screen is learning) so a missing "BC250 Power Setup" pick shows in the log.
  static char lastLearned[18];
  char learned[18];
  if (btHostLearnedAddr(learned) && strcmp(learned, lastLearned) != 0) {
    memcpy(lastLearned, learned, sizeof(lastLearned));
    Serial.printf("[PORTAL] BC250 candidate seen: %s\n", learned);
  }
  if (g_psSession) {
    // Spoofed session: answer PS-button pages only while the BC250 is down.
    btHostSetLearning(false);
    btHostAllowIncoming(!g_boardUp);
    g_hostLearning = false;
  } else {
    btHostSetLearning(learnActive);
    g_hostLearning = learnActive;
  }
}

void portalLoop() {
  dnsServer.processNextRequest();
  servicePortalHost();
#if defined(BC250_BLUEPAD32)
  // Paired devices can still reconnect after discovery stops. Drain callbacks
  // so they are released even when no Wi-Fi client or picker is active.
  controllerScanLoop();
#endif
  servicePortalBleScan();

  // Button held in setup mode = escape back to normal mode (only useful once
  // configured; an unconfigured device just re-enters setup). Either the main
  // power switch or the onboard BOOT button (where present) works.
  static unsigned long pressStart = 0;
  static bool wasDown = false;
  bool down = (digitalRead(BUTTON_SENSE) == LOW) ||
              (BOOT_BUTTON_PIN >= 0 && digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (down && !wasDown) {
    pressStart = millis();
  } else if (down && (millis() - pressStart) >= SETUP_HOLD_MS) {
    Serial.println("[PORTAL] button held, leaving setup mode");
    setForceSetup(false);
    delay(50);
    ESP.restart();
  }
  wasDown = down;

  if (g_restartAt && (int32_t)(millis() - g_restartAt) >= 0) {
    ESP.restart();
  }
}
