#if defined(BC250_BLUEPAD32)
#include "bt_host.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_bt.h>
#include <esp_mac.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "bt/uni_bt_bredr.h"
#include "board.h"

namespace {
constexpr char SETUP_NAME[] = "BC250 Power Setup";
std::atomic<bool> spoofed{false}, incomingAllowed{false}, learning{false};
std::atomic<bool> logRequested{false};
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
BtHostIncoming incomingCallback = nullptr;
char learned[18]{};
const char *identityReason = "identity not decided";
uint8_t factoryBase[6]{};
bd_addr_t rejected{};
bool haveRejected = false;
btstack_timer_source_t timer{};
int appliedConnectable = -1, appliedDiscoverable = -1;
bool loggedAddress = false;

void formatAddress(const uint8_t *addr, char out[18]) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseAddress(const char *text, uint8_t *addr) {
  if (strlen(text) != 17) return false;
  bool nonzero = false;
  for (int i = 0; i < 6; ++i) {
    int hi = hexDigit(text[i * 3]), lo = hexDigit(text[i * 3 + 1]);
    if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':')) return false;
    addr[i] = (hi << 4) | lo;
    nonzero |= addr[i] != 0;
  }
  return nonzero && !(addr[0] & 1);
}

void decideIdentity() {
  // This runs before Arduino setup / Preferences. Never erase user NVS on error.
  esp_err_t macError = esp_efuse_mac_get_default(factoryBase);
  if (nvs_flash_init() != ESP_OK) {
    identityReason = "NVS initialization failed";
    return;
  }
  char host[18]{};
  uint8_t forceSetup = 0, psLearn = 0;
  nvs_handle_t handle;
  if (nvs_open("bc250", NVS_READONLY, &handle) != ESP_OK) {
    identityReason = "no saved host";
    return;
  }
  size_t length = sizeof(host);
  esp_err_t hostError = nvs_get_str(handle, "btHost", host, &length);
  nvs_get_u8(handle, "forceSetup", &forceSetup);
  nvs_get_u8(handle, "psLearn", &psLearn);
  nvs_close(handle);
  uint8_t base[6];
  if (hostError != ESP_OK || !parseAddress(host, base)) {
    identityReason = "no valid saved host";
    return;
  }
  analogSetPinAttenuation(BOARD_SENSE, ADC_11db);
  uint32_t mv = 0;
  for (int i = 0; i < SENSE_OVERSAMPLE; ++i) mv += analogReadMilliVolts(BOARD_SENSE);
  if (mv / SENSE_OVERSAMPLE > SENSE_HIGH_MV) {
    identityReason = "board is on";
    return;
  }
  if (forceSetup && !psLearn) {
    identityReason = "setup requested";
    return;
  }
  if (macError != ESP_OK) {
    identityReason = "factory MAC unavailable";
    return;
  }
  // Four universal MACs: Bluetooth uses base + 2. Propagate borrow across bytes.
  unsigned borrow = 2;
  for (int i = 5; i >= 0 && borrow; --i) {
    unsigned value = base[i];
    base[i] = static_cast<uint8_t>(value - borrow);
    borrow = value < borrow ? 1 : 0;
  }
  // Do not alter a multicast result: that would silently change the target MAC.
  if (borrow || (base[0] & 1)) {
    identityReason = "host cannot use a unicast base";
    return;
  }
  if (esp_base_mac_addr_set(base) != ESP_OK) {
    identityReason = "base MAC change failed";
    return;
  }
  identityReason = psLearn ? "PS learning with saved host" : "board off with saved host";
  spoofed.store(true);
}

void applyScanState(bool force = false) {
  bool spoof = spoofed.load();
  int connectable = spoof ? incomingAllowed.load() : 1;
  int discoverable = !spoof && learning.load();
  if (force || connectable != appliedConnectable) {
    gap_connectable_control(connectable);
    appliedConnectable = connectable;
  }
  if (force || discoverable != appliedDiscoverable) {
    gap_discoverable_control(discoverable);
    appliedDiscoverable = discoverable;
  }
}

int connectionFilter(bd_addr_t addr, hci_link_type_t) {
  // This device is never a real Classic HID host. Every incoming Classic
  // connection is rejected before authentication, so a controller's existing
  // bond with the BC250 is never disturbed; we only harvest the address the
  // request carries. Rejecting unconditionally also means a stray device cannot
  // bond while the portal is up as "BC250 Power Setup".
  bd_addr_copy(rejected, addr);
  haveRejected = true;
  char text[18];
  formatAddress(addr, text);
  portENTER_CRITICAL(&stateMux);
  if (learning.load()) memcpy(learned, text, sizeof(learned));
  BtHostIncoming callback = incomingCallback;
  portEXIT_CRITICAL(&stateMux);
  if (spoofed.load() && incomingAllowed.load() && callback) callback(text);
  return 0;
}

void tick(btstack_timer_source_t *source) {
  applyScanState();
  if (hci_get_state() == HCI_STATE_WORKING) {
    bool requested = logRequested.exchange(false);
    if (!loggedAddress || requested) {
      bd_addr_t address;
      char text[18];
      gap_local_bd_addr(address);
      formatAddress(address, text);
      Serial.printf("[BTHOST] addr=%s spoofed=%d reason=%s connectable=%d discoverable=%d\n",
                    text, spoofed.load(), identityReason, appliedConnectable, appliedDiscoverable);
      loggedAddress = true;
    }
  }
  btstack_run_loop_set_timer(source, 200);
  btstack_run_loop_add_timer(source);
}
}  // namespace

extern "C" uint8_t __real_btstack_init();
extern "C" uint8_t __wrap_btstack_init() {
  decideIdentity();
  uint8_t result = __real_btstack_init();
  gap_register_classic_connection_filter(connectionFilter);
  if (spoofed.load()) uni_bt_bredr_set_enabled(false);
  else {
    gap_set_local_name(SETUP_NAME);
    gap_set_class_of_device(0x002508);
  }
  btstack_run_loop_set_timer_handler(&timer, tick);
  btstack_run_loop_set_timer(&timer, 200);
  btstack_run_loop_add_timer(&timer);
  return result;
}

extern "C" esp_err_t __real_esp_bt_controller_enable(esp_bt_mode_t mode);
extern "C" esp_err_t __wrap_esp_bt_controller_enable(esp_bt_mode_t mode) {
  esp_err_t result = __real_esp_bt_controller_enable(mode);
  if (spoofed.load()) {
    esp_err_t restored = esp_base_mac_addr_set(factoryBase);
    if (restored != ESP_OK) printf("[BTHOST] factory base restore failed: %d\n", restored);
  }
  return result;
}

extern "C" void __real_uni_bt_bredr_setup();
extern "C" void __wrap_uni_bt_bredr_setup() {
  __real_uni_bt_bredr_setup();
  applyScanState(true);
}

extern "C" void __real_uni_bt_bredr_on_hci_connection_request(uint16_t, const uint8_t *, uint16_t);
extern "C" void __wrap_uni_bt_bredr_on_hci_connection_request(uint16_t channel, const uint8_t *packet, uint16_t size) {
  if (!packet || size < 12) return;
  bd_addr_t address;
  hci_event_connection_request_get_bd_addr(packet, address);
  bool reject = haveRejected && bd_addr_cmp(address, rejected) == 0;
  haveRejected = false;  // Only suppress the event that the filter just rejected.
  if (!reject) __real_uni_bt_bredr_on_hci_connection_request(channel, packet, size);
}

bool btHostSpoofed() { return spoofed.load(); }
const char *btHostIdentityReason() { return identityReason; }
void btHostLogStatus() { logRequested.store(true); }
void btHostSetIncoming(BtHostIncoming callback) {
  portENTER_CRITICAL(&stateMux);
  incomingCallback = callback;
  portEXIT_CRITICAL(&stateMux);
}
void btHostAllowIncoming(bool allow) { incomingAllowed.store(allow); }
void btHostSetLearning(bool learn) { learning.store(!spoofed.load() && learn); }
bool btHostLearnedAddr(char out[18]) {
  if (!out) return false;
  portENTER_CRITICAL(&stateMux);
  memcpy(out, learned, sizeof(learned));
  portEXIT_CRITICAL(&stateMux);
  return out[0] != '\0';
}
void btHostClearLearned() {
  portENTER_CRITICAL(&stateMux);
  learned[0] = '\0';
  portEXIT_CRITICAL(&stateMux);
}
#endif
