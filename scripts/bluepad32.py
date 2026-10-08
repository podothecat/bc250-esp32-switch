# Bluepad32's official Arduino package adds SDK components to Arduino 2.
Import("env")
from pathlib import Path
sdk = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32")) / "tools/sdk/esp32"
for component in ("bluepad32", "bluepad32_arduino", "btstack"):
    env.Append(CPPPATH=[str(sdk / "include" / component / "include")])
env.Append(LIBS=["bluepad32_arduino", "bluepad32", "btstack", "bluepad32_arduino_bootstrap"])
