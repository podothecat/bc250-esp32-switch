#include "controller_scan.h"
#include "board.h"

static ControllerFound found;
static bool setupScan;
static bool coexScan;

#if defined(CONFIG_IDF_TARGET_ESP32)
void controllerScanPrepare() {}
#include <esp32-hal-bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gap_bt_api.h>

// WROOM uses one dual-mode Bluedroid host. NimBLE initialization would release
// Classic memory permanently until reboot, so never initialize it on this target.
static bool initialized;
static volatile bool wanted;
static volatile bool blePending;
static volatile bool bleRunning;
static volatile bool classicRunning;
static volatile int bleError;
static volatile int classicError;
static volatile uint32_t lastBleAttempt;
static volatile uint32_t lastClassicAttempt;
static const uint32_t RETRY_MS = 3000;

static void report(const uint8_t *bda, const char *name, int rssi, bool classic) {
  if (!wanted || !found) return;
  char addr[18];
  snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
  found(addr, name, rssi, classic);
}

// Bounded TLV parsing for advertising/scan response and Classic EIR names.
static void readName(const uint8_t *data, size_t size, char *name, size_t capacity) {
  for (size_t i = 0; i < size;) {
    size_t len = data[i];
    if (!len || i + len + 1 > size) break;
    uint8_t type = data[i + 1];
    if (type == 0x09 || (type == 0x08 && !name[0])) {
      size_t n = len - 1;
      if (n >= capacity) n = capacity - 1;
      memcpy(name, data + i + 2, n);
      name[n] = 0;
    }
    i += len + 1;
  }
}

static void bleCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (p->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        bleError = p->scan_param_cmpl.status;
        blePending = false;
      } else if (wanted) {
        esp_err_t err = esp_ble_gap_start_scanning(0);
        if (err != ESP_OK) { bleError = err; blePending = false; }
      } else blePending = false;
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      bleError = p->scan_start_cmpl.status;
      bleRunning = p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
      blePending = false;
      if (!wanted && bleRunning) esp_ble_gap_stop_scanning();
      break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      bleError = p->scan_stop_cmpl.status;
      if (p->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) bleRunning = false;
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        char name[33] = {};
        readName(p->scan_rst.ble_adv, p->scan_rst.adv_data_len, name, sizeof(name));
        readName(p->scan_rst.ble_adv + p->scan_rst.adv_data_len,
                 p->scan_rst.scan_rsp_len, name, sizeof(name));
        report(p->scan_rst.bda, name, p->scan_rst.rssi, false);
      } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        bleRunning = false;
      }
      break;
    default: break;
  }
}

static void classicCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p) {
  if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
    classicRunning = p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
    if (!classicRunning) lastClassicAttempt = millis();
    return;
  }
  if (event != ESP_BT_GAP_DISC_RES_EVT) return;
  char name[33] = {};
  int rssi = -127;
  for (int i = 0; i < p->disc_res.num_prop; ++i) {
    const auto &prop = p->disc_res.prop[i];
    if (!prop.val || prop.len <= 0) continue;
    if (prop.type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *(int8_t *)prop.val;
    else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
      size_t n = prop.len < 32 ? prop.len : 32;
      memcpy(name, prop.val, n); name[n] = 0;
    } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR) {
      readName((const uint8_t *)prop.val, prop.len, name, sizeof(name));
    }
  }
  report(p->disc_res.bda, name, rssi, true);
}

static bool initialize() {
  if (initialized) return true;
  if (!btStartMode(BT_MODE_BTDM)) { bleError = classicError = ESP_FAIL; return false; }
  esp_err_t err = ESP_OK;
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
    err = esp_bluedroid_init();
  if (err == ESP_OK && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
    err = esp_bluedroid_enable();
  if (err == ESP_OK) err = esp_ble_gap_register_callback(bleCallback);
  if (err == ESP_OK) err = esp_bt_gap_register_callback(classicCallback);
  if (err != ESP_OK) { bleError = classicError = err; return false; }
  // Inquiry only: no pairing, ACL/HID connections or change to the pad's host.
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  initialized = true;
  return true;
}

void controllerScanLoop() {
  if (!wanted) return;
  uint32_t now = millis();
  if (!initialized) {
    if (uint32_t(now - lastBleAttempt) < RETRY_MS) return;
    lastBleAttempt = now;
    if (!initialize()) return;
  }
  if (!bleRunning && !blePending && uint32_t(now - lastBleAttempt) >= RETRY_MS) {
    lastBleAttempt = now;
    esp_ble_scan_params_t params{};
    params.scan_type = setupScan ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    uint16_t interval = setupScan ? 500 : coexScan ? BLE_SCAN_INTERVAL_COEX_MS : BLE_SCAN_INTERVAL_SOLO_MS;
    uint16_t window = setupScan ? 45 : coexScan ? BLE_SCAN_WINDOW_COEX_MS : BLE_SCAN_WINDOW_SOLO_MS;
    // Bluedroid uses 0.625ms units; existing board settings use milliseconds.
    params.scan_interval = interval * 8 / 5;
    params.scan_window = window * 8 / 5;
    params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    blePending = true;
    bleError = esp_ble_gap_set_scan_params(&params);
    if (bleError != ESP_OK) blePending = false;
  }
  if (!classicRunning && uint32_t(now - lastClassicAttempt) >= RETRY_MS) {
    lastClassicAttempt = now;
    classicRunning = true;
    // 4 * 1.28 seconds, unlimited responses. Repeat after a 3-second radio break.
    classicError = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 4, 0);
    if (classicError != ESP_OK) classicRunning = false;
  }
}

bool controllerScanBegin(ControllerFound callback, bool setup, bool coex) {
  found = callback; setupScan = setup; coexScan = coex;
  return controllerScanStart();
}
bool controllerScanStart() {
  wanted = true;
  lastBleAttempt = lastClassicAttempt = millis() - RETRY_MS;
  if (!initialize()) return false;
  controllerScanLoop();
  return bleError == ESP_OK || classicError == ESP_OK;
}
bool controllerScanStop() {
  wanted = false;
  esp_err_t ble = ESP_OK, classic = ESP_OK;
  if (bleRunning) ble = esp_ble_gap_stop_scanning();
  if (classicRunning) classic = esp_bt_gap_cancel_discovery();
  bool stopped = ble == ESP_OK && classic == ESP_OK;
  if (!stopped) wanted = true;
  return stopped;
}
bool controllerScanRunning() { return wanted && (bleRunning || blePending || classicRunning); }
ControllerScanStatus controllerScanStatus() {
  return {true, bleRunning, classicRunning, bleError, classicError};
}

#else
#include <NimBLEDevice.h>
void controllerScanPrepare() {}
static bool initialized;
static bool wanted;
static bool initFailed;
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    if (!wanted || !found) return;
    const auto addr = d->getAddress().toString();
    const auto name = d->getName();
    found(addr.c_str(), name.c_str(), d->getRSSI(), false);
  }
};
static ScanCallbacks callbacks;
bool controllerScanBegin(ControllerFound callback, bool setup, bool coex) {
  found = callback; setupScan = setup; coexScan = coex;
  initialized = NimBLEDevice::init("");
  initFailed = !initialized;
  if (!initialized) return false;
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&callbacks, true);
  scan->setActiveScan(setup);
  scan->setInterval(setup ? 500 : coex ? BLE_SCAN_INTERVAL_COEX_MS : BLE_SCAN_INTERVAL_SOLO_MS);
  scan->setWindow(setup ? 45 : coex ? BLE_SCAN_WINDOW_COEX_MS : BLE_SCAN_WINDOW_SOLO_MS);
  scan->setMaxResults(0);
  return controllerScanStart();
}
bool controllerScanStart() {
  wanted = true;
  if (!initialized) return controllerScanBegin(found, setupScan, coexScan);
  return NimBLEDevice::getScan()->start(0, false);
}
bool controllerScanStop() { wanted = false; return !initialized || NimBLEDevice::getScan()->stop(); }
void controllerScanLoop() {}
bool controllerScanRunning() { return initialized && wanted && NimBLEDevice::getScan()->isScanning(); }
ControllerScanStatus controllerScanStatus() { return {false, controllerScanRunning(), false, initFailed ? -1 : 0, 0}; }
#endif
