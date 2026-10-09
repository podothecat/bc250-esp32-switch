#if defined(BC250_NAME_PROBE)
#include <Arduino.h>
#include <Bluepad32.h>
#include "bt/uni_bt.h"
#include "board.h"
#include "config.h"

// Standalone experiment. All radio operations and query state belong to the
// BT task. No application power-on callback or NVS setters are used here.
// Interpose startup too: the packaged BT stack starts before Arduino setup().
extern "C" void __wrap_uni_bt_le_on_gap_event_advertising_report(const uint8_t *, uint16_t) {}
extern "C" uni_error_t __wrap_uni_hid_device_on_device_discovered(bd_addr_t, const char *, uint16_t, uint8_t) {
  return UNI_ERROR_IGNORE_DEVICE;
}
static int rejectConnection(bd_addr_t, hci_link_type_t) { return 0; }
extern "C" void __real_uni_bt_bredr_setup();
extern "C" void __wrap_uni_bt_bredr_setup() {
  __real_uni_bt_bredr_setup();
  gap_register_classic_connection_filter(rejectConnection);
  gap_set_bondable_mode(0);
  gap_connectable_control(0);
}
// The stock handler assumes every name response belongs to a HID device.
// Our observer handles the diagnostic responses without creating HID devices.
extern "C" void __wrap_uni_bt_bredr_on_hci_remote_name_request_complete(uint16_t, const uint8_t *, uint16_t) {}

static bd_addr_t targets[MAX_WAKE_CONTROLLERS];
static uint8_t targetCount;
static uint8_t nextTarget;
static bd_addr_t currentTarget;
static btstack_timer_source_t timer{};
static btstack_packet_callback_registration_t observer{};
static btstack_context_callback_registration_t startCommand{};
static bool pending;
static bool stalled;
static uint32_t sentAt;
static uint32_t nextAt;
static uint32_t sequence;

static void observed(uint8_t type, uint16_t, uint8_t *packet, uint16_t size) {
  if (type != HCI_EVENT_PACKET || size < 9 ||
      packet[0] != HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE || !pending) return;
  bd_addr_t address;
  hci_event_remote_name_request_complete_get_bd_addr(packet, address);
  if (memcmp(address, currentTarget, sizeof(bd_addr_t))) return;
  uint8_t status = hci_event_remote_name_request_complete_get_status(packet);
  // The remote name is not trusted to have a terminating NUL in the event.
  unsigned nameLength = size - 9;
  if (nameLength > 248) nameLength = 248;
  Serial.printf("[NAMEPROBE] #%lu result addr=%s status=0x%02x elapsed=%lums name=%.*s\n",
                (unsigned long)sequence, bd_addr_to_str(address), status,
                (unsigned long)(millis() - sentAt), status ? 0 : (int)nameLength,
                size > 9 ? (const char *)(packet + 9) : "");
  pending = false;
  nextAt = millis() + 1000;
}

static void tick(btstack_timer_source_t *) {
  uint32_t now = millis();
  if (pending && uint32_t(now - sentAt) >= 15000 && !stalled) {
    // Never overlap a new query with an outstanding controller command.
    stalled = true;
    Serial.println("[NAMEPROBE] no completion for 15s; probes stopped, reset ESP32 to retry");
  }
  if (!stalled && !pending && targetCount && int32_t(now - nextAt) >= 0) {
    memcpy(currentTarget, targets[nextTarget], sizeof(bd_addr_t));
    nextTarget = (nextTarget + 1) % targetCount;
    sentAt = now;
    pending = true;
    ++sequence;
    int result = gap_remote_name_request(currentTarget, 2, 0);
    Serial.printf("[NAMEPROBE] #%lu request addr=%s queued_status=0x%02x\n",
                  (unsigned long)sequence, bd_addr_to_str(currentTarget), result);
    if (result) { pending = false; nextAt = now + 1000; }
  }
  btstack_run_loop_set_timer(&timer, 100);
  btstack_run_loop_add_timer(&timer);
}

static void startProbe(void *) {
  uni_bt_enable_new_connections_unsafe(false);
  gap_set_bondable_mode(0);
  gap_connectable_control(0);
  gap_set_page_timeout(0x1000); // 2.56 s; bounds each unavailable target's wait.
  observer.callback = observed;
  hci_add_event_handler(&observer);
  nextAt = millis() + 2000; // let periodic inquiry cancellation settle
  btstack_run_loop_set_timer_handler(&timer, tick);
  btstack_run_loop_set_timer(&timer, 2000);
  btstack_run_loop_add_timer(&timer);
}

void setup() {
  // Bench test with BC250 OFF. This firmware never asserts PS_ON#.
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  Serial.begin(115200);
  BP32.setup([](ControllerPtr) {}, [](ControllerPtr) {});
  delay(1000);
  loadConfig();
  Serial.println("[NAMEPROBE] diagnostic only: no pairing, no power-on, no WiFi/portal");
  for (uint8_t i = 0; i < config.wakeAddrCount; ++i) {
    unsigned b[6];
    if (sscanf(config.wakeAddrs[i].c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;
    for (uint8_t j = 0; j < 6; ++j) targets[targetCount][j] = b[j];
    ++targetCount;
    Serial.printf("[NAMEPROBE] target=%s\n", config.wakeAddrs[i].c_str());
  }
  if (!targetCount) Serial.println("[NAMEPROBE] no saved controller addresses; restore normal firmware to register one");
  startCommand.callback = startProbe;
  btstack_run_loop_execute_on_main_thread(&startCommand);
}

void loop() { delay(10); }
#endif
