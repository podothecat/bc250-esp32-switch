#include "controller_scan.h"
#include "board.h"
#include "bt_host.h"

static ControllerFound found;
static bool setupScan;
static bool coexScan;

#if defined(BC250_BLUEPAD32)
#include <atomic>
#include <Bluepad32.h>
#include "controller_diag.h"
#include "bt/uni_bt.h"
static bool initialized;
static bool wanted;
// Bluepad32 4.1.0 normally stops scanning and initiates a BLE connection from
// its advertising handler. Gate that handler, not the radio scan: our separate
// HCI observer still receives advertisements. Default closed also covers the
// package's Bluetooth startup before Arduino setup(). Classic HID is unchanged.
static std::atomic<bool> bleHidAllowed{false};
extern "C" void __real_uni_bt_le_on_gap_event_advertising_report(const uint8_t *, uint16_t);
extern "C" void __wrap_uni_bt_le_on_gap_event_advertising_report(const uint8_t *packet, uint16_t size) {
  if (bleHidAllowed.load())
    __real_uni_bt_le_on_gap_event_advertising_report(packet, size);
}
static uint32_t lastDiscoveryCommand;
static constexpr uint32_t DISCOVERY_SETTLE_MS = 500;
// Bluepad32 4.1.0 shares one callback registration between its safe APIs.
// Use independent registrations, and never enqueue the same one twice.
struct BtCommand {
  btstack_context_callback_registration_t registration{};
  bool queued = false;
  int index = -1;
  bool enabled = false;
};
static BtCommand discoveryCommand;
static BtCommand disconnectCommands[BP32_MAX_CONTROLLERS];
// Bluepad32 4.1.0 announces a ready device before its first input report.
// isGamepad() reads that report, so classifying in onConnect can reject a pad.
static ControllerPtr pendingControllers[BP32_MAX_CONTROLLERS]{};
static uint32_t pendingSince[BP32_MAX_CONTROLLERS]{};
static constexpr uint32_t FIRST_REPORT_TIMEOUT_MS = 10000;
static portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
static void advertisementSeen(const char *addr, const char *name, int rssi,
                              bool gamepad) {
  if (!wanted || !found) return;
  // Normal wake matches the saved address in main.cpp and must not depend on a
  // successful HID bond with this host, nor on the pad advertising a recognizable
  // appearance, so report every sighting on that path.
  if (!setupScan) { found(addr, name, rssi, false); return; }
  // A spoofed controller-learning session disables BLE HID pairing (a new bond
  // would re-pair the pad against the BC250 address), so BLE pads such as Xbox
  // can only be registered by their advertised address. Filter to likely game
  // controllers (gamepad appearance or HID service) so the picker is not buried
  // under every unrelated BLE advertiser; the diag layer gives each one a name or
  // a maker label. A pad that advertises neither marker can still be added with
  // the manual MAC field. An ordinary (non-spoofed) picker lists HID-verified
  // pads only, handled by the connect path.
  if (btHostSpoofed() && gamepad) found(addr, name, rssi, false);
}
// A Classic page captured by the host-identity filter (bt_host.cpp): a
// controller paging the BC250's address while we spoof it, or, in the picker,
// a PlayStation controller offering itself for registration. Reported as a
// Classic sighting; main.cpp matches it against the saved addresses to wake,
// and the picker lists it under a friendly label.
static void incomingSeen(const char *addr) {
  if (wanted && found)
    found(addr, setupScan ? "PlayStation (PS button)" : "", -127, true);
}
static void runBtCommand(void *context) {
  auto *command = static_cast<BtCommand *>(context);
  portENTER_CRITICAL(&commandMux);
  int index = command->index;
  bool enabled = command->enabled;
  portEXIT_CRITICAL(&commandMux);
  if (index < 0) {
    controllerDiagBegin(advertisementSeen);
    uni_bt_enable_new_connections_unsafe(enabled);
  } else {
    auto *device = uni_hid_device_get_instance_for_idx(index);
    if (device && device->conn.connected) {
      // uni_hid_device_disconnect() invalidates the connection handle before
      // the HCI event arrives. Request only the ACL close here; the native
      // HCI handler must retain ownership of notification and device deletion.
      gap_disconnect(device->conn.handle);
    }
  }
  portENTER_CRITICAL(&commandMux);
  command->queued = false;
  portEXIT_CRITICAL(&commandMux);
}
static void enqueueBtCommand(BtCommand &command, int index, bool enabled) {
  portENTER_CRITICAL(&commandMux);
  if (command.queued) { portEXIT_CRITICAL(&commandMux); return; }
  command.queued = true;
  command.index = index;
  command.enabled = enabled;
  command.registration.callback = runBtCommand;
  command.registration.context = &command;
  portEXIT_CRITICAL(&commandMux);
  btstack_run_loop_execute_on_main_thread(&command.registration);
}
static void releaseController(ControllerPtr controller) {
  int index = controller->index();
  if (index >= 0 && index < BP32_MAX_CONTROLLERS)
    enqueueBtCommand(disconnectCommands[index], index, false);
}
static void requestDiscovery(bool enabled) {
  enqueueBtCommand(discoveryCommand, -1, enabled);
}
static void reportController(ControllerPtr controller) {
  if (!controller->isGamepad()) { releaseController(controller); return; }
  auto properties = controller->getProperties();
  char addr[18];
  snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
           properties.btaddr[0], properties.btaddr[1], properties.btaddr[2],
           properties.btaddr[3], properties.btaddr[4], properties.btaddr[5]);
  Serial.printf("[HID] connected %s (%s), disconnecting\n", addr, controller->getModelName().c_str());
  if (wanted && found) found(addr, controller->getModelName().c_str(), -127, true);
  releaseController(controller);
}
static void connected(ControllerPtr controller) {
  int index = controller->index();
  if (index < 0 || index >= BP32_MAX_CONTROLLERS) return;
  if (!wanted) { releaseController(controller); return; }
  if (controller->hasData()) { reportController(controller); return; }
  pendingControllers[index] = controller;
  pendingSince[index] = millis();
  Serial.printf("[HID] connected slot=%d; waiting for first input report\n", index);
}
static void disconnected(ControllerPtr controller) {
  int index = controller->index();
  if (index >= 0 && index < BP32_MAX_CONTROLLERS) pendingControllers[index] = nullptr;
  Serial.println("[HID] disconnected");
}
void controllerScanPrepare() {
  if (initialized) return;
  BP32.setup(connected, disconnected);
  btHostSetIncoming(incomingSeen);
  // The package starts its first periodic inquiry before Arduino setup. Wait
  // for that command to leave BTstack's pending state before cancelling it.
  // Virtual devices are disabled by default; avoid another queued command here.
  delay(1000);
  requestDiscovery(false);
  lastDiscoveryCommand = millis();
  initialized = true;
}
bool controllerScanBegin(ControllerFound callback, bool setup, bool coex) {
  found = callback; setupScan = setup; coexScan = coex;
  controllerScanPrepare();
  return controllerScanStart();
}
bool controllerScanStart() {
  // Never start BLE HID pairing under a spoofed BC250 identity: a new bond would
  // be written against the BC250's address and could overwrite a real console
  // bond (e.g. an Xbox controller's). Spoofed sessions only capture Classic
  // pages via the host-identity filter.
  bleHidAllowed.store(setupScan && !btHostSpoofed());
  wanted = true;
  Serial.printf("[BT] BLE mode: %s\n", setupScan ? "HID setup" : "advertisements only (no BLE connections)");
  // Discovery commands execute asynchronously on the BT task. Allow an
  // initial stop to finish before requesting another inquiry in loop().
  return initialized;
}
bool controllerScanStop() {
  bleHidAllowed.store(false);
  wanted = false;
  if (initialized) {
    requestDiscovery(false);
    lastDiscoveryCommand = millis();
  }
  return true;
}
void controllerScanLoop() {
  if (!initialized) return;
  BP32.update();
  controllerDiagPoll(wanted);
  for (int i = 0; i < BP32_MAX_CONTROLLERS; ++i) {
    ControllerPtr controller = pendingControllers[i];
    if (!controller) continue;
    if (!wanted) {
      pendingControllers[i] = nullptr;
      releaseController(controller);
    } else if (controller->hasData()) {
      pendingControllers[i] = nullptr;
      reportController(controller);
    } else if (uint32_t(millis() - pendingSince[i]) >= FIRST_REPORT_TIMEOUT_MS) {
      pendingControllers[i] = nullptr;
      Serial.printf("[HID] slot=%d first input report timed out; disconnecting\n", i);
      releaseController(controller);
    }
  }
  if (wanted != uni_bt_enable_new_connections_is_enabled() &&
      uint32_t(millis() - lastDiscoveryCommand) >= DISCOVERY_SETTLE_MS) {
    requestDiscovery(wanted);
    lastDiscoveryCommand = millis();
  }
}
bool controllerScanRunning() { return initialized && wanted && uni_bt_enable_new_connections_is_enabled(); }
ControllerScanStatus controllerScanStatus() { return {true, false, controllerScanRunning(), 0, 0}; }

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
