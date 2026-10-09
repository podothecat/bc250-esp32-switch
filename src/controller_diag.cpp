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
    char addr[18];
    snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    advertisementFound(addr, static_cast<int8_t>(gap_event_advertising_report_get_rssi(packet)));
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
