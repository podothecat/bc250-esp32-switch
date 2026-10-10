#if defined(BC250_BLUEPAD32)
#include <Arduino.h>
#include <Bluepad32.h>
#include "controller_diag.h"
#include "bt/uni_bt.h"
#include "bt/uni_bt_le.h"
#include "bt/uni_bt_setup.h"
#include "bt/uni_bt_allowlist.h"

// Observe HCI events without changing pairing, scan state, or stored keys.
static btstack_packet_callback_registration_t observer{};
static bool registered;
static ControllerAdvertisement advertisementFound;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
struct Stats {
  uint32_t advertisements = 0;
  uint32_t connections = 0;
  uint32_t disconnects = 0;
  uint32_t commandErrors = 0;
  uint32_t classicRequests = 0;
  uint32_t classicCompletions = 0;
  uint32_t authentications = 0;
  uint8_t classicStatus = 0;
  uint8_t authStatus = 0;
  bd_addr_t classicAddress{};
  uint8_t connectionStatus = 0;
  uint8_t disconnectReason = 0;
  uint8_t commandStatus = 0;
  uint16_t commandOpcode = 0;
};
static Stats stats;

// BLE controllers such as the Xbox Wireless Controller advertise their name in a
// scan response and their gamepad appearance / HID service UUID in the primary
// advertisement, arriving as two separate reports. A spoofed learning session
// cannot bond to read the model name, so merge both halves here, keyed by
// address, and remember whether a device ever looked like a controller. Only the
// Bluetooth task touches this table.
struct AdvCache {
  bd_addr_t addr{};
  char name[33] = {};
  bool gamepad = false;
  bool used = false;
  uint32_t lastSeen = 0;
};
static AdvCache advCache[24];

// Bounded TLV parse of advertising / scan-response data for a Complete (0x09) or
// Shortened (0x08) Local Name, a controller hint from the Appearance (0x19) or a
// 16-bit HID service UUID (0x1812) in a service-UUID list (0x02/0x03), and the
// Manufacturer Specific (0xFF) company id so a nameless controller can still be
// labeled by its maker.
static void parseAdvData(const uint8_t *data, uint8_t len, char *name,
                         size_t capacity, bool *gamepad, uint16_t *company) {
  for (uint8_t i = 0; i + 1 < len;) {
    uint8_t fieldLen = data[i];
    if (!fieldLen || uint16_t(i) + 1 + fieldLen > len) break;
    uint8_t adType = data[i + 1];
    const uint8_t *val = data + i + 2;
    uint8_t vlen = fieldLen - 1;
    if (adType == 0x09 || (adType == 0x08 && !name[0])) {
      uint8_t n = vlen < capacity - 1 ? vlen : uint8_t(capacity - 1);
      memcpy(name, val, n);
      name[n] = 0;
    } else if (adType == 0x19 && vlen >= 2) {
      uint16_t appearance = uint16_t(val[0]) | uint16_t(val[1]) << 8;
      if (appearance == 0x03C4 || appearance == 0x03C3) *gamepad = true;
    } else if ((adType == 0x02 || adType == 0x03) && vlen >= 2) {
      for (uint8_t j = 0; j + 1 < vlen; j += 2) {
        uint16_t uuid = uint16_t(val[j]) | uint16_t(val[j + 1]) << 8;
        if (uuid == 0x1812) *gamepad = true;
      }
    } else if (adType == 0xFF && vlen >= 2 && !*company) {
      *company = uint16_t(val[0]) | uint16_t(val[1]) << 8;
    }
    i += fieldLen + 1;
  }
}

// Bluetooth SIG company identifiers for controller makers whose BLE pads do not
// put a name in the advertisement (the name is in a scan response we cannot read
// in a spoofed, no-bond session). Used only to label an otherwise-nameless pad.
static const char *gamepadLabelForCompany(uint16_t company) {
  switch (company) {
    case 0x0006: return "Xbox controller";       // Microsoft
    case 0x054C: return "PlayStation controller"; // Sony
    case 0x057E: return "Nintendo controller";    // Nintendo
    default: return "Gamepad";
  }
}

static AdvCache *advCacheFor(const bd_addr_t address, uint32_t now) {
  int free = -1, oldest = 0;
  for (int i = 0; i < 24; ++i) {
    if (advCache[i].used) {
      if (memcmp(advCache[i].addr, address, 6) == 0) return &advCache[i];
      if (uint32_t(now - advCache[i].lastSeen) >
          uint32_t(now - advCache[oldest].lastSeen)) oldest = i;
    } else if (free < 0) {
      free = i;
    }
  }
  int idx = free >= 0 ? free : oldest;
  advCache[idx] = AdvCache{};
  memcpy(advCache[idx].addr, address, 6);
  advCache[idx].used = true;
  return &advCache[idx];
}

static void observe(uint8_t type, uint16_t, uint8_t *packet, uint16_t size) {
  if (type != HCI_EVENT_PACKET || size < 2) return;
  portENTER_CRITICAL(&statsMux);
  switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_CONNECTION_REQUEST:
      if (size >= 12) {
        ++stats.classicRequests;
        hci_event_connection_request_get_bd_addr(packet, stats.classicAddress);
      }
      break;
    case HCI_EVENT_CONNECTION_COMPLETE:
      if (size >= 13) {
        ++stats.classicCompletions;
        stats.classicStatus = packet[2];
        hci_event_connection_complete_get_bd_addr(packet, stats.classicAddress);
      }
      break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
      if (size >= 5) {
        ++stats.authentications;
        stats.authStatus = packet[2];
      }
      break;
    case GAP_EVENT_ADVERTISING_REPORT:
      ++stats.advertisements;
      break;
    case HCI_EVENT_LE_META:
      if (size >= 4 && packet[2] == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
        ++stats.connections;
        stats.connectionStatus = packet[3];
      }
      break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
      if (size >= 6) {
        ++stats.disconnects;
        stats.disconnectReason = packet[5];
      }
      break;
    case HCI_EVENT_COMMAND_COMPLETE:
      if (size >= 6 && packet[5]) {
        ++stats.commandErrors;
        stats.commandStatus = packet[5];
        stats.commandOpcode = uint16_t(packet[3]) | uint16_t(packet[4]) << 8;
      }
      break;
    case HCI_EVENT_COMMAND_STATUS:
      if (size >= 6 && packet[2]) {
        ++stats.commandErrors;
        stats.commandStatus = packet[2];
        stats.commandOpcode = uint16_t(packet[4]) | uint16_t(packet[5]) << 8;
      }
      break;
  }
  portEXIT_CRITICAL(&statsMux);
  // Deliver sightings independently of HID pairing/encryption success. Keep
  // application callbacks outside the statistics critical section.
  if (hci_event_packet_get_type(packet) == GAP_EVENT_ADVERTISING_REPORT &&
      size >= 12 && advertisementFound) {
    bd_addr_t address;
    gap_event_advertising_report_get_address(packet, address);
    const uint8_t *advData = gap_event_advertising_report_get_data(packet);
    uint8_t advLen = gap_event_advertising_report_get_data_length(packet);
    char name[33] = {};
    bool gamepad = false;
    uint16_t company = 0;
    parseAdvData(advData, advLen, name, sizeof(name), &gamepad, &company);
    uint32_t now = millis();
    AdvCache *entry = advCacheFor(address, now);
    if (gamepad) entry->gamepad = true;
    // A real advertised name always wins. Otherwise, the first time a device looks
    // like a controller, synthesize a label from its maker so it is not listed as
    // "unknown"; a real name arriving later (e.g. in a scan response) replaces it.
    if (name[0]) {
      strncpy(entry->name, name, sizeof(entry->name) - 1);
      entry->name[sizeof(entry->name) - 1] = 0;
    } else if (entry->gamepad && !entry->name[0]) {
      strncpy(entry->name, gamepadLabelForCompany(company), sizeof(entry->name) - 1);
      entry->name[sizeof(entry->name) - 1] = 0;
    }
    entry->lastSeen = now;
    char addr[18];
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    advertisementFound(addr, entry->name,
                       static_cast<int8_t>(gap_event_advertising_report_get_rssi(packet)),
                       entry->gamepad);
  }
}

void controllerDiagBegin(ControllerAdvertisement advertisement) {
  advertisementFound = advertisement;
  if (registered) return;
  observer.callback = observe;
  hci_add_event_handler(&observer);
  registered = true;
}

void controllerDiagPoll(bool active) {
  static uint32_t lastLog;
  if (!active || uint32_t(millis() - lastLog) < 5000) return;
  lastLog = millis();
  portENTER_CRITICAL(&statsMux);
  Stats snapshot = stats;
  portEXIT_CRITICAL(&statsMux);
  Serial.printf("[BTDIAG] ready=%d enabled=%d ble=%d allowlist=%d adv=%lu le_conn=%lu status=0x%02x disconnects=%lu reason=0x%02x cmd_errors=%lu opcode=0x%04x cmd_status=0x%02x\n",
                uni_bt_setup_is_ready(), uni_bt_enable_new_connections_is_enabled(),
                uni_bt_le_is_enabled(), uni_bt_allowlist_is_enabled(),
                (unsigned long)snapshot.advertisements, (unsigned long)snapshot.connections,
                snapshot.connectionStatus, (unsigned long)snapshot.disconnects,
                snapshot.disconnectReason, (unsigned long)snapshot.commandErrors,
                snapshot.commandOpcode, snapshot.commandStatus);
  Serial.printf("[BTCLASSIC] requests=%lu completions=%lu status=0x%02x last_addr=%02x:%02x:%02x:%02x:%02x:%02x auth=%lu auth_status=0x%02x\n",
                (unsigned long)snapshot.classicRequests,
                (unsigned long)snapshot.classicCompletions, snapshot.classicStatus,
                snapshot.classicAddress[0], snapshot.classicAddress[1], snapshot.classicAddress[2],
                snapshot.classicAddress[3], snapshot.classicAddress[4], snapshot.classicAddress[5],
                (unsigned long)snapshot.authentications, snapshot.authStatus);
}
#endif
