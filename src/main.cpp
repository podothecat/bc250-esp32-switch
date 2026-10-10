#include <Arduino.h>
#include "controller_scan.h"
#include <esp_system.h>
#include "board.h"
#include "config.h"
#include "ha.h"
#include "portal.h"
#include "bt_host.h"

// The ESP is permanently powered from the ATX connector (5VSB), so it runs
// independently of the PSU's main rails. It controls the SFX PSU through PS_ON#
// and watches the BC250's TPMS1 line to know whether the board itself is up.
//
// Control model
// -------------
//   * Asserting PS_ON# turns the PSU on; the BC250 is expected to power up from
//     applied power (BIOS "restore on AC power"), so PSU on == board boots.
//   * We have no wire to the board's power button, so "turning the board off"
//     means cutting PSU power via PS_ON#. This is a hard power-off, not a
//     graceful OS shutdown.
//   * If the board shuts itself down (e.g. OS shutdown), TPMS1 drops to 0 while
//     the PSU is still energized. We detect that and release PS_ON# so the PSU
//     follows the board down.
//   * A background BLE scan watches for the bound controller advertising; when
//     it appears while we're OFF, that wakes the machine just like a button tap.
//     After a power-off the wake stays disarmed until the controller has gone
//     quiet, so its reconnect burst can't switch the machine straight back on.

enum PowerState {
  STATE_OFF,      // PSU released, board down
  STATE_BOOTING,  // PSU asserted, waiting for TPMS1 to go HIGH
  STATE_ON,       // PSU asserted, board up (TPMS1 HIGH)
};

static PowerState state = STATE_OFF;

// --- Button debounce / press tracking ---
static bool          buttonStable      = false;  // debounced "pressed" level
static bool          buttonLastRaw     = false;
static unsigned long buttonLastChange  = 0;
static unsigned long pressStart        = 0;
static bool          pressStartedOff   = false;  // press began while OFF
static bool          longPressFired    = false;
static bool          setupFired        = false;

// Independent BOOT shortcut: never acts as a power button.
static bool          bootButtonStable = false;
static bool          bootButtonLastRaw = false;
static unsigned long bootButtonLastChange = 0;
static unsigned long bootButtonPressStart = 0;
static bool          bootButtonArmed = false;

// --- Board-sense debounce ---
static bool          boardSenseStable  = false;  // debounced TPMS1 HIGH
static bool          boardSenseLastRaw = false;
static unsigned long boardSenseChange  = 0;

// --- BLE wake ---
// Written from the Bluetooth task, read from loop(). 32-bit aligned scalars
// are atomic on the ESP32, so a plain volatile is enough here.
static volatile bool          bleSeenEver = false;
static volatile unsigned long bleLastSeen = 0;
static unsigned long          bleInhibitUntil = 0;  // wakes ignored until this

// Arm/disarm latch (see BLE_REARM_SILENCE_MS). Starts armed so an ESP reset
// while OFF keeps the old "controller on -> machine on" behaviour.
static bool          bleWakeArmed   = true;
static unsigned long bleDisarmedAt  = 0;
static bool          blePresentLast = false;  // for seen/lost edge logging

// --- Misc timers ---
static unsigned long bootStart    = 0;
static unsigned long lastHeartbeat = 0;

// Worst-case normalLoop() duration since the last heartbeat, in microseconds.
// The loop has to sample BUTTON_SENSE often enough to catch a ~150ms button tap, so
// anything that blocks — WiFi association, a DNS lookup, a TCP connect to a
// dead broker — shows up here first. Reset every heartbeat so a single stall
// doesn't mask the steady state.
static unsigned long loopMaxUs = 0;

static const char *stateName(PowerState s) {
  switch (s) {
    case STATE_OFF:     return "OFF";
    case STATE_BOOTING: return "BOOTING";
    case STATE_ON:      return "ON";
  }
  return "?";
}

static void setState(PowerState next) {
  if (next == state) return;
  Serial.printf("[STATE] %s -> %s\n", stateName(state), stateName(next));
  state = next;
}

// Raw (pre-time-debounce) board-up level, derived from the TPMS1 voltage with
// hysteresis so a signal hovering near the logic threshold doesn't chatter.
static bool senseLevel = false;

static bool readBoardSense(uint32_t *outMv = nullptr) {
  // Average several reads to reject single-sample ADC/line spikes. Without this,
  // one stray spike past SENSE_HIGH_MV chatters the hysteresis and restarts the
  // board-off debounce, stalling shutdown detection (see SENSE_OVERSAMPLE).
  uint32_t acc = 0;
  for (int i = 0; i < SENSE_OVERSAMPLE; i++) {
    acc += analogReadMilliVolts(BOARD_SENSE);
  }
  uint32_t mv = acc / SENSE_OVERSAMPLE;
  if (outMv) *outMv = mv;
  if (senseLevel) {
    if (mv < SENSE_LOW_MV) senseLevel = false;
  } else {
    if (mv > SENSE_HIGH_MV) senseLevel = true;
  }
  return senseLevel;
}

static void psuOn() {
  digitalWrite(PS_ON_PIN, PS_ON_ASSERT);
  Serial.println("[PSU ] PS_ON# asserted (LOW) -> PSU ON");
}

static void psuOff() {
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  Serial.println("[PSU ] PS_ON# released (high-Z) -> PSU OFF");
}

// Shared power-on path, used by both the button and the BLE wake. Takes the
// loop's `now` rather than calling millis() itself: the BOOTING timeout compares
// against the `now` cached at the top of normalLoop(), and a fresh millis() here
// can land a millisecond past it. Since the comparison is unsigned, bootStart >
// now makes (now - bootStart) underflow to a huge value and trip the timeout on
// the very next loop. Sharing one clock per iteration keeps the math monotonic.
static void powerOn(const char *reason, unsigned long now) {
  Serial.printf("[ACT ] %s -> powering on\n", reason);
  // Stop answering spoofed-identity Classic pages before the board comes up, so
  // the ESP never competes with the real BC250 for the controller once it boots.
  // The per-loop update below keeps it off for the whole non-OFF window.
  btHostAllowIncoming(false);
  psuOn();
  bootStart = now;
  setState(STATE_BOOTING);
}

// Shared power-off path. Starts the BLE-wake cooldown and disarms the wake latch
// so the controller's post-shutdown reconnect burst can't wake us again. Takes
// `now` for the same single-clock-per-loop reason as powerOn().
static void powerOff(const char *reason, unsigned long now) {
  Serial.printf("[ACT ] %s -> powering off\n", reason);
  psuOff();
  bleInhibitUntil = now + BLE_WAKE_COOLDOWN_MS;
  bleWakeArmed = false;
  bleDisarmedAt = now;
  Serial.println("[BLE ] wake disarmed until controller goes quiet");
  setState(STATE_OFF);
}

// Bluetooth callbacks only record sightings; power decisions stay in loop().
static void controllerFound(const char *addr, const char *, int, bool classic) {
  for (uint8_t i = 0; i < config.wakeAddrCount; ++i) {
    if (strcmp(addr, config.wakeAddrs[i].c_str()) != 0) continue;
    if (!classic && (!bleSeenEver || uint32_t(millis() - bleLastSeen) >= BLE_PRESENCE_TIMEOUT_MS))
      Serial.printf("[BLE ] registered controller advertising: %s\n", addr);
    bleLastSeen = millis();
    bleSeenEver = true;
    if (classic) Serial.printf("[BT] registered Classic controller seen: %s\n", addr);
    break;
  }
}

static bool bleScanning = false;

static void startBleScan(bool coexWithWifi) {
  if (config.wakeAddrCount == 0) {
    Serial.println("[BT] no controller bound; wake disabled");
    return;
  }
  bool started = controllerScanBegin(controllerFound, false, coexWithWifi);
  bleScanning = true; // desired state, including automatic startup retries
  Serial.printf("[BT] scan requested for %u controller(s): %s\n",
                config.wakeAddrCount, started ? "started" : "failed; will retry");
}

static void setBleScanActive(bool active) {
  if (config.wakeAddrCount == 0 || active == bleScanning) return;
  if (active) bleScanning = controllerScanStart();
  else if (controllerScanStop()) bleScanning = false;
}

// Persist a setup request and reboot into the WiFi portal.
static void enterSetupMode(const char *reason) {
  Serial.printf("[ACT ] %s -> entering setup mode\n", reason);
  setForceSetup(true);
  delay(50);
  ESP.restart();
}

// Debounce a raw level into a stable level. Returns true if the stable value
// changed this call, writing the new value into *stable.
static bool debounce(bool raw, bool *stable, bool *lastRaw,
                     unsigned long *lastChange, unsigned long now,
                     unsigned long window) {
  if (raw != *lastRaw) {
    *lastRaw = raw;
    *lastChange = now;
  }
  if (raw != *stable && (now - *lastChange) >= window) {
    *stable = raw;
    return true;
  }
  return false;
}

static void normalBegin() {
  Serial.println("=== BC250 PSU controller ===");

  // PS_ON# open-drain, released by default so the PSU stays off at boot.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);

  // Switch: BUTTON_GND is local ground; BUTTON_SENSE is read with pull-up.
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);
  if (BOOT_BUTTON_PIN >= 0) pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // TPMS1 sense: read as ADC over the full 0-3.3V range.
  analogSetPinAttenuation(BOARD_SENSE, ADC_11db);

  // Seed debounced states from the current levels.
  buttonStable = buttonLastRaw = (digitalRead(BUTTON_SENSE) == LOW);
  boardSenseStable = boardSenseLastRaw = readBoardSense();

  // If the board is already up when the ESP (re)boots, adopt the ON state
  // rather than assuming OFF — keeps us in sync after an ESP-only reset.
  if (boardSenseStable) {
    psuOn();  // make sure PS_ON# matches reality
    setState(STATE_ON);
  }

  Serial.printf("[INIT] state=%s board=%s bound=%s\n",
                stateName(state), boardSenseStable ? "UP" : "DOWN",
                (config.wakeAddrCount == 0) ? "(none)" : config.wakeAddr.c_str());

  startBleScan(wifiConfigured());
#if defined(BC250_BLUEPAD32)
  setBleScanActive(state == STATE_OFF);
#endif
  // Report the chosen Bluetooth identity once the radio is up (spoofed vs. the
  // "BC250 Power Setup" fallback, and why).
  btHostLogStatus();
  haBegin();
}

static bool g_setupMode = false;

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "SDIO";
#if ESP_IDF_VERSION_MAJOR >= 5
    case ESP_RST_USB:      return "USB";        // host toggled DTR/RTS
    case ESP_RST_JTAG:     return "JTAG";
    case ESP_RST_EFUSE:    return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";  // supply glitch detector
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
#endif
    default:               return "UNKNOWN";
  }
}

void setup() {
  controllerScanPrepare();
  Serial.begin(115200);
  // Give native USB serial time to enumerate where the target uses it.
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }
  Serial.println();

  // Log why we rebooted; this helps distinguish a power issue from a crash
  // (PANIC) or an intentional restart (SW).
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[INIT] reset reason: %s (%d)\n", resetReasonName(rr), (int)rr);

  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_HIGH ? LOW : HIGH);
  }

  loadConfig();

  // The portal only starts on an explicit request (8s button hold while OFF).
  // Otherwise we ALWAYS run the normal controller so button control works even
  // when no Bluetooth controller has been configured.
  if (config.forceSetup) {
    g_setupMode = true;
    portalBegin();
  } else {
    normalBegin();
  }
}

static void normalLoop() {
  controllerScanLoop();
  unsigned long now = millis();

  // --- Drain any command from Home Assistant ---
  // MQTT callbacks fire inside mqtt.loop() with no access to this iteration's
  // `now`, so they only park a command here. Running it through the same
  // powerOn()/powerOff() the button uses means the boot watchdog and the BLE
  // cooldown arm identically whatever the trigger was — there is no second code
  // path that could bypass them.
  switch (haTakeCommand()) {
    case HA_CMD_POWER_ON:
      if (state == STATE_OFF) powerOn("Home Assistant switch on", now);
      break;
    case HA_CMD_WOL_POWER_ON:
      if (state == STATE_OFF) powerOn("Wake-on-LAN packet", now);
      break;
    case HA_CMD_POWER_OFF:
      if (state != STATE_OFF) powerOff("Home Assistant switch off", now);
      break;
    case HA_CMD_ENTER_SETUP:
      enterSetupMode("Home Assistant setup button");
      break;
    case HA_CMD_RESTART:
      Serial.println("[ACT ] Home Assistant restart button -> restarting");
      delay(50);
      ESP.restart();
      break;
    case HA_CMD_NONE:
    default:
      break;
  }

  // --- Sample & debounce inputs ---
  uint32_t senseMv;
  bool buttonRaw = (digitalRead(BUTTON_SENSE) == LOW);   // pressed == LOW
  bool boardRaw  = readBoardSense(&senseMv);             // board up (hysteresis)

  if (BOOT_BUTTON_PIN >= 0) {
    bool bootRaw = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    if (debounce(bootRaw, &bootButtonStable, &bootButtonLastRaw,
                 &bootButtonLastChange, now, DEBOUNCE_MS)) {
      bootButtonPressStart = now;
      bootButtonArmed = bootButtonStable && state == STATE_OFF;
    }
    // Cancel if the machine starts during the hold; release before retrying.
    if (state != STATE_OFF || boardRaw) bootButtonArmed = false;
    if (bootButtonStable && bootButtonArmed &&
        (now - bootButtonPressStart) >= SETUP_HOLD_MS) {
      bootButtonArmed = false;
      enterSetupMode("BOOT hold (>8s) while OFF");
    }
  }

  if (debounce(buttonRaw, &buttonStable, &buttonLastRaw,
               &buttonLastChange, now, DEBOUNCE_MS)) {
    if (buttonStable) {
      // Press began.
      pressStart = now;
      pressStartedOff = (state == STATE_OFF);
      longPressFired = false;
      setupFired = false;
      Serial.println("[BTN ] pressed");
    } else {
      // Released. A short tap that began while OFF powers on (power-on is on
      // release so that a long hold from OFF can mean "enter setup" instead).
      unsigned long held = now - pressStart;
      Serial.printf("[BTN ] released after %lu ms\n", held);
      if (pressStartedOff && !setupFired && state == STATE_OFF &&
          held < SETUP_HOLD_MS) {
        powerOn("short press while OFF", now);
      }
    }
  }

  // --- Button holds ---
  if (buttonStable && pressStartedOff && !setupFired &&
      (now - pressStart) >= SETUP_HOLD_MS) {
    // Long hold from OFF -> reconfigure (does not power the machine on).
    setupFired = true;
    enterSetupMode("long hold (>8s) while OFF");
  }
  if (buttonStable && !pressStartedOff && !longPressFired && state == STATE_ON &&
      (now - pressStart) >= LONG_PRESS_MS) {
    // Long hold that began while ON -> force off.
    longPressFired = true;
    powerOff("long press (>5s) while ON", now);
  }

  // --- BLE controller wake ("machine follows controller") ---
  // While OFF and armed, the controller being present powers the machine on.
  // A power-off disarms the wake; it re-arms once the controller is quiet, so its
  // reconnect-advertising burst at shutdown can't wake us however long it lasts.
  // "Quiet" is two gates (see below): a short floor from the power-off for a
  // controller that is already off, and a longer silence-since-last-sighting for
  // one still emitting its burst. The fixed cooldown still applies on top.
  //
  // bleLastSeen is written by the scan task and can land after `now` was taken.
  // The unsigned difference then underflows to a huge "silence", which is
  // harmless for presence (one loop of "absent") but would falsely re-arm the
  // latch — so the silence is computed signed.
  unsigned long seen = bleLastSeen;
  int32_t bleSilentMs = (int32_t)(now - seen);
  bool blePresent =
      bleSeenEver && bleSilentMs < (int32_t)BLE_PRESENCE_TIMEOUT_MS;
  bool bleInhibited = (int32_t)(bleInhibitUntil - now) > 0;

  if (blePresent != blePresentLast) {
    blePresentLast = blePresent;
    Serial.printf("[BLE ] controller %s\n", blePresent ? "seen" : "lost");
  }

  // Two gates, so a quiet controller re-arms fast while a noisy one still waits:
  //   - A floor measured from the power-off (BLE_WAKE_COOLDOWN_MS). While ON the
  //     scan is paused, so bleLastSeen is stale at power-off and the silence gate
  //     below is already satisfied; the floor gives the resumed scan time to hear
  //     the reconnect burst before we trust that silence. It equals the cooldown,
  //     during which presence is inhibited anyway, so arming at its end is safe.
  //   - Silence since the last sighting (BLE_REARM_SILENCE_MS). Once the burst
  //     starts, each page refreshes bleLastSeen and holds the latch disarmed.
  // A controller switched off emits no burst, so only the floor applies and it
  // re-arms at power-off + cooldown. One left on waits out the burst gaps.
  if (!bleWakeArmed &&
      (now - bleDisarmedAt) >= BLE_WAKE_COOLDOWN_MS &&
      (!bleSeenEver || bleSilentMs >= (int32_t)BLE_REARM_SILENCE_MS)) {
    bleWakeArmed = true;
    Serial.println("[BLE ] controller quiet -> wake re-armed");
  }

  if (state == STATE_OFF && bleWakeArmed && blePresent && !bleInhibited) {
    powerOn("controller present (BLE)", now);
  }

  bool boardChanged = debounce(boardRaw, &boardSenseStable, &boardSenseLastRaw,
                               &boardSenseChange, now,
                               state == STATE_BOOTING ? DEBOUNCE_MS
                                                      : BOARD_OFF_DEBOUNCE_MS);

  // --- State-driven board-sense handling ---
  switch (state) {
    case STATE_BOOTING:
      if (boardSenseStable) {
        Serial.println("[ACT ] TPMS1 HIGH -> board is up");
        setState(STATE_ON);
      } else if ((now - bootStart) >= BOOT_TIMEOUT_MS) {
        powerOff("boot timed out, board never signalled UP", now);
      }
      break;

    case STATE_ON:
      // Board dropped TPMS1 on its own (OS shutdown / crash) -> follow it down.
      if (boardChanged && !boardSenseStable) {
        powerOff("TPMS1 LOW while ON, board shut down", now);
      }
      break;

    case STATE_OFF:
    default:
      break;
  }

  // Optional: release the radio while the machine isn't OFF. The BLE wake only
  // acts in STATE_OFF, so nothing is lost except during that window.
  if (BLE_SCAN_ONLY_WHEN_OFF
#if defined(BC250_BLUEPAD32)
      || true
#endif
  ) {
    setBleScanActive(state == STATE_OFF);
  }

  // Answer spoofed-identity Classic pages only while fully OFF with the board
  // down. A no-op on non-Bluepad32 builds. boardRaw (not the debounced state) is
  // used so a board coming up disables it immediately.
  btHostAllowIncoming(state == STATE_OFF && !boardRaw);

  // --- Heartbeat ---
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.printf("[HB  ] state=%s board=%s btn=%s ble=%s/%s | sense: %umV (%s) "
                  "| heap=%u loopMax=%luus | wifi=%s rssi=%d mqtt=%s\n",
                  stateName(state),
                  boardSenseStable ? "UP" : "DOWN",
                  buttonStable ? "down" : "up",
                  blePresent ? "present" : "absent",
                  bleWakeArmed ? "armed" : "disarmed",
                  senseMv, boardRaw ? "high" : "low",
                  (unsigned)ESP.getFreeHeap(), loopMaxUs,
                  haWifiConnected() ? "up" : "down", (int)haRssi(),
                  haMqttConnected() ? "up" : "down");
    loopMaxUs = 0;
  }

  // --- Home Assistant ---
  // LAST in the iteration on purpose: HA then observes the state this pass
  // produced, so a switch command and the state it causes are reported in the
  // same iteration and the entity never bounces back to its old value.
  HaState hs;
  hs.stateName  = stateName(state);
  hs.powerOn    = (state != STATE_OFF);   // BOOTING reports ON
  hs.boardUp    = boardSenseStable;
  hs.blePresent = blePresent;
  hs.bleActive  = config.wakeAddrCount > 0;
  hs.senseMv    = senseMv;
  // A blocking connect attempt inside the 10s boot watchdog could stall long
  // enough to trip a spurious "boot timed out", so don't allow one while BOOTING.
  haLoop(now, hs, state != STATE_BOOTING);
}

#if defined(BC250_SERIAL_TEST)
// USB bench testing only. Does not write pairing selection or setup state to NVS.
static void serialTestLoop() {
  static char command[48];
  static size_t length;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (length < sizeof(command) - 1) command[length++] = c;
      else length = 0;
      continue;
    }
    command[length] = 0; length = 0;
    unsigned int b[6]; int consumed = 0;
    if (sscanf(command, "test %2x:%2x:%2x:%2x:%2x:%2x%n",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &consumed) == 6 &&
        command[consumed] == 0) {
      controllerScanStop();
      char addr[18];
      snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
               b[0], b[1], b[2], b[3], b[4], b[5]);
      config.wakeAddr = config.wakeAddrs[0] = addr;
      config.wakeAddrCount = 1;
      bleSeenEver = false;
      if (g_setupMode) { g_setupMode = false; normalBegin(); }
      else startBleScan(wifiConfigured());
      Serial.printf("[TEST] temporary binding %s; restart restores saved config\n", addr);
    }
  }
}
#endif

// Drive the onboard status LED so the current mode is readable at a glance,
// without a serial console. Non-blocking: the pattern is derived from the
// wall clock each loop. Distinct patterns:
//   setup portal   - fast blink (~3 Hz): the setup AP is up, connect to it
//   board ON       - solid on: the BC250 is running
//   board BOOTING  - fast flicker: PSU asserted, waiting for the board
//   OFF, armed     - a brief pulse every 2 s: idle and ready to wake
//   OFF, disarmed  - dark: wake is inhibited (just powered off / cooldown)
static void updateStatusLed(unsigned long now) {
  if (STATUS_LED_PIN < 0) return;
  bool on;
  if (g_setupMode) {
    on = (now % 300) < 150;             // setup portal: fast blink
  } else if (state == STATE_ON) {
    on = true;                          // board up: solid
  } else if (state == STATE_BOOTING) {
    on = (now % 160) < 80;              // powering up: fast flicker
  } else {  // STATE_OFF
    // Disarmed (just powered off, holding until the controller goes quiet and
    // the wake re-arms): slow blink. Armed idle standby: dark.
    on = bleWakeArmed ? false : ((now % 1500) < 750);
  }
  digitalWrite(STATUS_LED_PIN, (on == STATUS_LED_ACTIVE_HIGH) ? HIGH : LOW);
}

void loop() {
#if defined(BC250_SERIAL_TEST)
  serialTestLoop();
#endif
  updateStatusLed(millis());
  if (g_setupMode) {
    portalLoop();
    return;
  }
  // Time the whole iteration so the heartbeat can report the worst case. The
  // span excludes this iteration's own reporting, which is what we want: it
  // measures the input-sampling cadence, not the printf.
  unsigned long t0 = micros();
  normalLoop();
  unsigned long span = micros() - t0;
  if (span > loopMaxUs) loopMaxUs = span;
}
