#pragma once

// Register on the Bluetooth task; poll on the Arduino task. The advertisement
// callback reports every sighting with the advertised name (empty until a
// scan response carrying it arrives) and whether the device looks like a game
// controller (HID service or gamepad/joystick appearance). The caller decides
// what to do with non-gamepad sightings.
using ControllerAdvertisement = void (*)(const char *addr, const char *name,
                                         int rssi, bool gamepad);
void controllerDiagBegin(ControllerAdvertisement advertisement);
void controllerDiagPoll(bool active);
