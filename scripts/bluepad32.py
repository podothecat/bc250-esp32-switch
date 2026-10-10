# Bluepad32's official Arduino package adds SDK components to Arduino 2.
Import("env")
from pathlib import Path
sdk = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")) / "tools/sdk/esp32"
for component in ("bluepad32", "bluepad32_arduino", "btstack"):
    env.Append(CPPPATH=[str(sdk / "include" / component / "include")])
env.Append(LIBS=["bluepad32_arduino", "bluepad32", "btstack", "bluepad32_arduino_bootstrap"])

# The pinned 4.1.0 archive dispatches BLE advertising from uni_bt.c to
# uni_bt_le.c. Interpose only that handler so normal wake can observe packets
# without Bluepad32 stopping the scan to initiate HID pairing. Setup delegates
# to the original handler. Do not patch the shared framework package.
env.Append(LINKFLAGS=["-Wl,--wrap=uni_bt_le_on_gap_event_advertising_report"])
project_flags = env.GetProjectOption("build_flags", "")
if "BC250_NAME_PROBE" in (project_flags if isinstance(project_flags, str) else " ".join(project_flags)):
    for symbol in ("uni_hid_device_on_device_discovered", "uni_bt_bredr_setup",
                   "uni_bt_bredr_on_hci_remote_name_request_complete"):
        env.Append(LINKFLAGS=["-Wl,--wrap=" + symbol])
else:
    for symbol in ("btstack_init", "esp_bt_controller_enable", "uni_bt_bredr_setup",
                   "uni_bt_bredr_on_hci_connection_request"):
        env.Append(LINKFLAGS=["-Wl,--wrap=" + symbol])
