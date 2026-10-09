#pragma once
#include <Arduino.h>

// Advertising callbacks run on the Bluetooth task; WROOM HID callbacks run in update().
// Copy fields before returning.
using ControllerFound = void (*)(const char *addr, const char *name, int rssi,
                                bool classic);
void controllerScanPrepare();
bool controllerScanBegin(ControllerFound callback, bool setup, bool coex);
bool controllerScanStart();
bool controllerScanStop();
void controllerScanLoop();
bool controllerScanRunning();
struct ControllerScanStatus {
  bool classicSupported;
  bool bleRunning;
  bool classicRunning;
  int bleError;
  int classicError;
};
ControllerScanStatus controllerScanStatus();
