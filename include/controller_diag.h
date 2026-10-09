#pragma once

// Register on the Bluetooth task; poll on the Arduino task.
using ControllerAdvertisement = void (*)(const char *addr, int rssi);
void controllerDiagBegin(ControllerAdvertisement advertisement);
void controllerDiagPoll(bool active);
