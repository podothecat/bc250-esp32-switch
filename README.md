# BC250 ESP32 Power Switch

An ESP32 power controller for an AMD **BC250** board running as a desktop. The
BC250 is fed from a PCI-E connector and has no ATX power button, so this firmware
drives the SFX PSU's `PS_ON#` line and senses board power, giving you a real power
button — plus optional "turn on when I pick up my controller" via Bluetooth.

## Features

- **Push-button power**: tap to turn on; hold 5 s while running to force off.
- **Follows the board**: if the OS shuts the board down, the PSU is cut automatically.
- **Boot watchdog**: if the board doesn't come up within 10 s, the PSU is released.
- **BLE controller wake** (optional): when a bound controller (e.g. an 8BitDo) powers
  on, the machine powers on with it.
- **Wake-on-LAN** (optional): listen for WoL magic packets over WiFi and power on
  the machine through the same guarded power-on path as the physical button.
- **Home Assistant** (optional): joins your WiFi and appears in HA over MQTT with
  auto-discovery as a switch plus power/state/signal sensors.
- **WiFi setup portal**: configure the bound controller from a phone — no reflashing.

## What this fork adds

Upstream ([Thunkar/bc250-esp32-switch](https://github.com/Thunkar/bc250-esp32-switch)) provides everything above except Home Assistant. If you don’t need network/smarthome control, use the original repo. This fork adds:
- Joining an existing WiFi network
- MQTT with auto-discovery
- Additional screens in the setup portal to enter wifi and MQTT credentials

MQTT enables the BC-250 to appear as a device in Home Assistant, which allows you to power on and off the PSU and see current power status. With some Home Assistant automation, this allows you to do things like power on the BC-250 from a voice assistant, or perform smart home automations when it powers on or off.

**Every new field is optional and independently skippable.** A device with no SSID
stored never brings up WiFi at all — it logs `station mode disabled` and runs
button-and-BLE only, exactly as it did before. Configuring WiFi but not MQTT joins the
network without appearing in Home Assistant. The button remains the primary control and
works standalone in every configuration.

Be aware that the ESP32 cannot perform a *soft* power off. It drives the PSU's `PS_ON#` line directly and has no channel into the running OS, so Power On energises the rail and the board boots because it now has power, while Power Off cuts that rail outright — the equivalent of holding an ATX power button for 5 seconds. Neither one asks the OS to do anything.

Shutting down from inside the OS is the clean path, and it is handled properly: the ESP sees `TPMS1` drop and releases the PSU on its own. Reserve the HA switch and the 5 second button hold for a machine that is already wedged.

## Wiring

The ESP is permanently powered from the ATX connector's **5 V standby**, so it runs
whether the machine is on or off. Share a common ground between the ESP, the PSU, and
the board.

| Signal | ESP32-C3 DevKitM-1 | ESP32-WROOM-32 Dev Module | Connects to | Notes |
|--------|-------------------|-----------------------------|-------------|-------|
| Button sense | GPIO5 | GPIO18 | Momentary switch, terminal A | Read with internal pull-up |
| Button ground | GPIO6 | GPIO19 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| PSU control | GPIO4 | GPIO23 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW |
| Board sense | GPIO3 | GPIO34 | BC250 `TPMS1` (pin 9) | ~3.3 V when the board is up, 0 when off |
| Power / ground | 5V / GND | Dev board 5V/VIN / GND | PSU standby + common ground | Permanent power for the ESP |

Use the board-specific pins in the table; do not wire both columns at once. GPIO34 on
the WROOM32 is input-only and is used for the ADC sense signal. `PS_ON#` idles at ~5 V
(pulled up inside the PSU). The selected GPIO is driven open-drain so the 3.3 V part
never fights the 5 V rail — it only ever sinks to ground to switch the PSU on.
These assignments target development boards; verify that the listed GPIOs are exposed
on your carrier. Do not feed 5 V directly into a bare WROOM32 module.

`TPMS1` is a higher-impedance signal that hovers near the logic threshold, so it's read
as an analog voltage with hysteresis rather than a digital pin.

### Connector pinouts

**ATX 24-pin main connector** — tap three pins:

```
               +3.3V ─┤  1 │ 13 ├─ +3.3V
               +3.3V ─┤  2 │ 14 ├─ −12V
                 GND ─┤  3 │ 15 ├─ GND
                 +5V ─┤  4 │ 16 ├─ PS_ON#   ◄── PSU control GPIO (green, open-drain, active LOW)
                 GND ─┤  5 │ 17 ├─ GND      ◄── ESP GND (any GND pin works)
                 +5V ─┤  6 │ 18 ├─ GND
                 GND ─┤  7 │ 19 ├─ GND
              PWR_OK ─┤  8 │ 20 ├─ (RSVD)
ESP 5V/VIN ◄── +5VSB ─┤  9 │ 21 ├─ +5V
                +12V ─┤ 10 │ 22 ├─ +5V
                +12V ─┤ 11 │ 23 ├─ +5V
               +3.3V ─┤ 12 │ 24 ├─ GND
```

**TPMS1 header** — single pin for board-power sense:

```
   PCICLK ─┤  1   2 ├─ GND
    FRAME ─┤  3   4 ├─ SMB_CLK_MAIN
  PCIRST# ─┤  5   6 ├─ SMB_DATA_MAIN
     LAD3 ─┤  7   8 ├─ LAD2
       3V ─┤  9  10 ├─ LAD1      ◄── pin 9 (3V) = board-on sense ──► Board-sense GPIO
     LAD0 ─┤ 11  12 ├─ GND
          ─┤     14 ├─ S_PWRDWN#
     3VSB ─┤ 15  16 ├─ SERIRQ#
      GND ─┤ 17  18 ├─ GND
```

Pin 9 is the only TPMS1 pin used: it reads ~3.3 V when the board is powered and 0 V when
off. No ground wire is needed from this header — the ESP already shares ground with the
board through the ATX connector.

## Button controls

| Action | Result |
|--------|--------|
| Tap while **off** | Power on |
| Hold ≥ 5 s while **on** | Force power off |
| Hold ≥ 8 s while **off** | Enter WiFi setup portal |
| WROOM32 onboard **BOOT**: hold ≥ 8 s while **off** | Enter WiFi setup portal (GPIO0) |

The button is the primary control and always works, even with no controller configured.

On WROOM32, the BOOT shortcut only enters setup; a short press does nothing.
Use it after the ESP32 has booted, with the BC250 off. Release BOOT once setup
starts. Holding BOOT during power-on or while pressing RST enters the chip's
download mode instead.

## Bluetooth controller wake

When a controller is bound (via the portal), the machine **follows the controller**:
turn the controller on and the machine powers up. After a power-off the wake is
disarmed until the controller has been silent for 10 s, so its reconnect burst (an Xbox
controller keeps advertising for a while after losing its host) can't switch the
machine back on. Once the controller has given up and gone quiet, pressing its power
button wakes the machine again.

## Setup portal

Hold the button ≥ 8 s while off (or on first use) to start the portal:

1. Connect to the open WiFi network **`BC250 Switch Setup`** and open `http://192.168.4.1`.
2. Create a password.
3. **WiFi** — pick your network from the list captured at startup, or type one in.
   You can also enable WoL detection here; it requires a configured WiFi network.
4. **MQTT** — your broker's host, port and credentials.
5. **Controller** — pick it from the live BLE scan, or enter its MAC.
6. **Finish** — name the device, review, and reboot into normal operation.

Only the password is required. **Every later step can be skipped**, and each is
independent: no WiFi means the device runs exactly as it always has, on the button
alone. Skipping just MQTT joins the network without appearing in Home Assistant.

To clear something later, blank the field and save — an empty SSID disables WiFi, an
empty broker host disables MQTT, and an empty MAC un-binds the controller. Password
fields left blank keep whatever is already stored; clear one and save to remove it.
Clearing the WiFi network also disables WoL detection.

When enabled, WoL listens for UDP packets on port 9 containing the standard magic
packet pattern (six `FF` bytes followed by the target MAC repeated 16 times). The
WiFi setup screen displays the ESP32's WiFi MAC when WoL is enabled; use that as the
target MAC in your WoL app. The ESP32 accepts valid WoL packets regardless of the
embedded target MAC, so this address is used to address the packet, not as a filter.
The sending device must broadcast the packet onto the network the ESP32 has joined;
router/VLAN broadcast isolation can prevent delivery.

The WiFi list is scanned once at startup, before the setup AP exists — a live rescan
would take the AP off-channel and drop your phone's connection. To refresh it, re-enter
setup mode.

## Home Assistant

With WiFi and a broker configured, the device publishes MQTT discovery and shows up
automatically. Entities:

| Entity | Type | Meaning |
|---|---|---|
| Power | switch | On whenever the PSU is asserted (booting counts as on) |
| Board power | binary sensor | `TPMS1` — whether the board itself is actually up |
| State | sensor | `OFF` / `BOOTING` / `ON` |
| Controller present | binary sensor | Bound controller is advertising (only if one is bound) |
| Sense voltage | sensor | Raw `TPMS1` reading in mV |
| WiFi signal, Free heap, Uptime | sensors | Diagnostics, published every 30 s |
| Restart, Setup mode | buttons | Reboot, or reboot into the setup portal |

**The switch reports reality, not intent.** Commands from HA run through exactly the
same power path as the physical button, including the boot watchdog. So if you turn the
switch on and the board never asserts `TPMS1`, the 10 s watchdog cuts the PSU and **the
switch flips itself back off**. That is correct behaviour — the machine genuinely did
not come up — but it looks like a bug if you aren't expecting it.

If the broker is unreachable, the device keeps working normally on the button; it just
retries the broker every 20 s in the background.

## Build & flash

PlatformIO (pioarduino). Select the environment for your board. Two steps — firmware
and the portal's web UI (a single `app/index.html` packed into SPIFFS):

```bash
pio run -e esp32-c3-devkitm-1 -t upload  # C3 firmware
pio run -e esp32-c3-devkitm-1 -t uploadfs

pio run -e esp32-wroom32 -t upload       # WROOM32 firmware
pio run -e esp32-wroom32 -t uploadfs
```

`esp32-wroom32` targets the generic `esp32dev` board profile. The WROOM32 pin map is
defined separately from the C3 map in [include/board.h](include/board.h); ensure the
physical wiring follows the matching column above.

## Notes

- **WiFi TX power**: the default `WIFI_POWER_8_5dBm` is intentionally conservative.
  These ESP32-C3 *mini* boards have an RF/power quirk
  ([arduino-esp32 #6551](https://github.com/espressif/arduino-esp32/issues/6551)) where
  the radio is unusable at full power. Both the SoftAP (`AP_TX_POWER`) and station
  mode (`STA_TX_POWER`) therefore run at `WIFI_POWER_8_5dBm`
  (in [include/board.h](include/board.h)). At full power the symptom is *not* a
  brownout — there are no resets at all — it is a silent failure to work: the portal
  is invisible, and station mode never completes association, logging
  `disconnected, reason=2` (`AUTH_EXPIRE`) forever. Severity varies between physical
  boards, so one unit working at 19.5 dBm says nothing about the next. See
  [docs/baseline.md](docs/baseline.md) for the measurements.
- **Radio coexistence**: WiFi and BLE share one radio. Once an SSID is configured the
  BLE scan drops from ~50 % to ~15 % duty (`BLE_SCAN_*_COEX_MS` in
  [include/board.h](include/board.h)) so it doesn't starve the WiFi MAC — controller
  wake still works, it just takes a little longer to notice. With no SSID configured the
  original ~50 % duty is used unchanged.
- Serial debug runs at **115200** baud. C3 native USB-CDC is enabled by its build
  environment; WROOM32 boards use their USB-UART bridge.
- Pin assignments and all timing constants live in [include/board.h](include/board.h).
- **Every "off" is a hard rail cut.** The controller drives `PS_ON#` directly and has
  no channel into the running OS, so the HA switch and the 5 s button hold yank power
  rather than requesting a shutdown. A shutdown started *inside* the OS is handled
  cleanly — the ESP sees TPMS1 drop and follows the board down. Plans for a graceful
  path are in [docs/future-work.md](docs/future-work.md).

### Multiple controllers

The setup portal supports up to four controllers. Tap scanned devices to toggle
selection, or enter up to four comma-separated BLE MAC addresses. Save to replace
the registered list; clear the field and save to disable BLE wake. Existing
single-controller settings are retained when upgrading. Any registered controller
can wake the BC250. After power-off, all registered controllers must stop
advertising for the existing silence period before BLE wake re-arms.

Upload both firmware and the portal filesystem when updating this feature:
Run `pio run -e esp32-wroom32 -t upload`, then
`pio run -e esp32-wroom32 -t uploadfs` (use the C3 environment for C3).

### BLE discovery in the setup portal

Bluetooth scanning starts only when a connected phone opens the authenticated
controller picker, which refreshes every two seconds. Before that, setup runs
Wi-Fi only so Bluetooth does not compete with phone association/DHCP. Scanning
pauses when all AP clients disconnect or picker polling stops for 12 seconds. The
list holds the 48 most recently observed addresses, expires devices after 30
seconds without sightings, and replaces the oldest entry when full.
**Scan again** clears only the discovery list and restarts BLE scanning; it keeps
the saved controller configuration and current selections. The picker shows
scanner state, last reception time, cache occupancy, and replacement count.
A stopped or failed scan is retried at three-second intervals. C3 disables
NimBLE's internal result storage; WROOM copies Bluepad32 connection data directly
into the bounded cache without retaining a second results list.

### PlayStation controllers on WROOM32

WROOM32 uses the official Bluepad32 4.1.0 Arduino package (Arduino 2.0.17)
with PlatformIO espressif32 6.9.0. C3 retains its Arduino 3 / NimBLE build.
WROOM reports ready gamepad connections rather than raw inquiry results.

Open the controller picker, then hold **PS + Create** on DualSense or
**PS + SHARE** on DualShock 4 until the light bar flashes. Select the gamepad
and save; up to four addresses are supported. Existing address registrations
are retained, but a controller must also pair with this board to reconnect.
Bluetooth keys are preserved across reboots and are not cleared by rescan.

In normal mode, a BLE advertisement matching a registered address can trigger
power-on without completing HID pairing with the ESP32. This allows BLE pads
to wake the PC even when their current Bluetooth bond is with the PC. Registered
Classic gamepad connections also trigger power-on. Normal mode suppresses
Bluepad32's BLE advertising-to-HID connection handler, so BLE scanning continues
without initiating pairing with the pad. Setup mode retains BLE HID discovery.
The boot log identifies normal mode as `advertisements only (no BLE connections)`.
Address changes or advertisements the ESP32 cannot receive still prevent wake.
The existing cooldown and silence-based re-arm apply to both detection paths.
`[BLE ] registered controller advertising` identifies this path; `[BTDIAG]`
reports radio counters every five seconds while detection is requested.
The gamepad is immediately disconnected, and discovery is paused while the PC
is starting or on. Connections arriving while detection is paused are released
without waking the PC. Setup discovery remains limited to an active picker
with a connected Wi-Fi client. RSSI is unavailable for HID connection events.
PS-button wake and subsequent PC reconnection require testing with the actual
controller; disconnecting from the board does not guarantee automatic PC pairing.

Upload both WROOM firmware and the filesystem image for this update.
