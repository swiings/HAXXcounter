# HAXXcounter

<img width="768" height="1024" alt="437A1B17-C389-4947-9CC7-85FDB370E0C0_1_105_c" src="https://github.com/user-attachments/assets/1f36220a-08e6-448f-bf8a-0c639025c490" />


HAXXcounter is a handheld nearby-device counter for the Waveshare ESP32-S3-Touch-AMOLED-1.8. It passively observes Bluetooth Low Energy (BLE) advertisements and Wi-Fi management traffic, shows a deduplicated count on the AMOLED display, and can alert when the count changes.

This is an experimental proximity signal counter, not a people-counting system. A device may represent one person, several devices may belong to one person, and some nearby devices will not advertise or probe while they are idle.

## Features

- Counts nearby Wi-Fi and BLE devices in a rolling 60-second window.
- Deduplicates repeated sightings and mitigates BLE private-address rotation using manufacturer data where available.
- Supports **All devices** and **Personal devices** modes.
- Lets you adjust the RSSI threshold from -100 dBm (farther signals) to -50 dBm (closer signals).
- Uses Wi-Fi channel hopping plus periodic active scans to find nearby access points.
- Includes visual and audible count-change alerts.
- Displays battery state, current mode, and range threshold on the built-in AMOLED screen.
- Provides serial diagnostics at 115200 baud.

## Hardware

The firmware is configured for:

- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm)
- ESP32-S3R8 at 240 MHz
- 16 MB QIO flash and 8 MB OPI PSRAM
- SH8601 368 x 448 AMOLED display
- FT3168 touch controller
- AXP2101 power-management IC
- ES8311 audio codec

The default PlatformIO board definition is `esp32-s3-devkitc-1`, with the board-specific memory settings supplied in [platformio.ini](platformio.ini).

## How It Works

HAXXcounter maintains an in-memory table of recently observed device identities. An identity remains live for 60 seconds after its last sighting. Wi-Fi observations use a MAC address; BLE observations use manufacturer data when available to reduce duplicate counts from devices that rotate their random MAC address.

The count shown depends on the selected mode:

- **All devices**: every unique device identity in the active window.
- **Personal devices**: identities classified as likely personal devices, based on Apple, Samsung, Google/Fast Pair data and randomized BLE addresses.

Signals below the selected RSSI threshold are ignored. RSSI is an approximate radio-strength measurement: walls, orientation, and radio activity can significantly affect it.

## Controls

| Input | Action |
| --- | --- |
| Swipe up | Tighten the range by increasing the RSSI threshold. |
| Swipe down | Loosen the range by decreasing the RSSI threshold. |
| Touch and hold | Cycle AMOLED brightness presets. |
| Boot button, short press | Toggle between All devices and Personal devices modes. |
| Boot button, hold for 2 seconds | Lock or unlock touchscreen input. |
| Power button, short press | Cycle alerts: Off, Visual, Sound Low, Sound High. |
| Power button, hold | Print the current device table to the serial monitor. |

## Build and Upload

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) or the PlatformIO IDE extension for VS Code
- A USB data cable connected to the ESP32-S3 board

PlatformIO automatically installs the project dependencies:

- Arduino GFX Library
- LVGL 8
- NimBLE-Arduino
- XPowersLib

### Commands

From the repository root:

```sh
platformio run --environment haxxcounter
platformio run --target upload --environment haxxcounter
platformio device monitor --baud 115200
```

Each build automatically increments the local firmware build number through `scripts/bump_build_number.py`.

## Serial Diagnostics

The serial monitor runs at `115200` baud. The firmware periodically reports the live count, new Wi-Fi and BLE discoveries, filtered sightings, current channel, battery level, alert mode, and RSSI threshold. Holding the power key prints the device table and the age of each entry.

## Privacy and Responsible Use

HAXXcounter is designed to show an aggregate count on-device. Nearby wireless identifiers can be personal data in some jurisdictions. Use it only where you have permission, avoid recording or sharing identifiers, and comply with local laws and venue policies.

Do not use this project for safety-critical occupancy measurement, security decisions, or individual tracking.

## Project Layout

```text
src/
  main.cpp       Application loop, battery state, and physical controls
  counter.cpp    Wi-Fi/BLE discovery, deduplication, and counting
  display.cpp    AMOLED display and touch driver
  ui.cpp         LVGL user interface
  alert.cpp      Visual and audio alert behavior
  audio.cpp      ES8311 audio output
include/
  lv_conf.h      LVGL configuration
scripts/         PlatformIO build helpers
```

## License

No license is currently declared for this repository. Add a license file before distributing or accepting external contributions.
