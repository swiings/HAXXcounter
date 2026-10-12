# HAXXcounter

<img width="768" height="1024" alt="437A1B17-C389-4947-9CC7-85FDB370E0C0_1_105_c" src="https://github.com/user-attachments/assets/1f36220a-08e6-448f-bf8a-0c639025c490" />


HAXXcounter is a handheld nearby-device counter for the Waveshare ESP32-S3-Touch-AMOLED-1.8. It passively observes Bluetooth Low Energy (BLE) advertisements and Wi-Fi management traffic, shows a deduplicated count on the AMOLED display, and can alert when the count changes.

This is an experimental proximity signal counter. It also includes an experimental **people estimate** that groups devices which appear to travel together, but it is not a reliable people-counting system. A device may represent one person, several devices may belong to one person, and some nearby devices will not advertise or probe while they are idle.

## Features

- Counts nearby Wi-Fi and BLE devices in a rolling 60-second window.
- Deduplicates repeated sightings and mitigates BLE private-address rotation using manufacturer data where available.
- Classifies BLE devices (phone, watch, earbuds, tracker, laptop, and others) from Apple, Samsung, Google/Fast Pair, Microsoft, Garmin, and Tile advertisement data.
- Supports **All devices**, **Personal devices**, **Phones**, and **Estimated people** modes.
- Estimates people by grouping BLE devices whose signal strength rises and falls together over time.
- Lets you adjust the RSSI threshold from -100 dBm (farther signals) to -50 dBm (closer signals).
- Uses Wi-Fi channel hopping plus periodic active scans to find nearby access points.
- Includes visual and audible count-change alerts.
- Displays battery state, current mode, and range threshold on the built-in AMOLED screen.
- Provides serial diagnostics at 115200 baud, including timestamped device-table dumps for field notes.

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

HAXXcounter maintains an in-memory table of recently observed device identities. An identity remains live for 60 seconds after its last sighting. Wi-Fi observations use a MAC address; BLE observations use manufacturer data when available to reduce duplicate counts from devices that rotate their random MAC address. The BLE scanner reports every advertisement (not just the first per scan), so each device's RSSI is sampled continuously.

The count shown depends on the selected mode:

- **All devices**: every unique device identity in the active window.
- **Personal devices**: identities classified as likely personal devices (phone, watch, earbuds, tracker, laptop, or generic personal).
- **Phones**: identities classified as phones.
- **Est. people**: the experimental people estimate described below.

Signals below the selected RSSI threshold are ignored. RSSI is an approximate radio-strength measurement: walls, orientation, and radio activity can significantly affect it.

### Apple device classification

Apple devices each send more than one kind of advertisement, often from different random addresses:

- **Nearby Info** (`0x10`): its payload length was observed to separate device types — 5 bytes Apple Watch, 6 bytes MacBook, 7 bytes iPhone. This is based on a small set of devices and may vary by OS version; other lengths are classed as generic personal.
- **Find My** (`0x12`): classed as a tracker. iPhones, Watches, and Macs send a short 2-byte Find My advertisement alongside Nearby Info, so one Apple device usually appears as two table entries. A 25-byte Find My payload indicates a device separated from its owner.

### People estimate

The estimate follows [Docs/person-counting-design.md](Docs/person-counting-design.md). Weights and thresholds are initial values and have not been calibrated against labeled data.

1. **Sampling:** each personal BLE device's RSSI is averaged into 5-second windows, keeping 2 minutes of history.
2. **Pair scoring:** device pairs with at least 6 shared windows are scored from co-occurrence (30%), RSSI trajectory correlation (30%), device-class compatibility (20%), ecosystem match (15%), and first/last-heard overlap (5%).
3. **Movement required:** if either device's RSSI barely changes, trajectory evidence is treated as unknown and the pair cannot associate. Stationary devices near each other are left separate rather than guessed together.
4. **Association:** a pair associates after scoring at least 0.80 on 3 consecutive evaluations, stays associated while above 0.60, and separates below that. Scores in between are reported as unresolved.
5. **Grouping:** devices join a group only if every member is associated with every other member (maximum 4 devices), so A~B and B~C does not imply A~C. Groups are rebuilt every window, so associations are reversible.
6. **Address rotation:** when a new address appears as another goes silent (within ±2 windows, same vendor, compatible class, RSSI within 8 dB) and the match is unique, the new address inherits the old one's history and associations.
7. **Counting:** a device silent for 15 seconds stops counting. Only groups containing a phone, watch, or generic personal device count as a person; trackers, laptops, and earbuds count only when grouped with one.

In practice, a person sitting still with a phone and a watch reads as 2 people, and drops to 1 after walking around for a minute or two.

## Controls

| Input | Action |
| --- | --- |
| Swipe up | Tighten the range by increasing the RSSI threshold. |
| Swipe down | Loosen the range by decreasing the RSSI threshold. |
| Touch and hold | Cycle AMOLED brightness presets. |
| Boot button, short press | Cycle modes: All devices, Personal, Phones, Est. people. |
| Boot button, hold for 2 seconds | Lock or unlock touchscreen input. |
| Power button, short press | Cycle alerts: Off, Visual, Sound Low, Sound High. |
| Power button, hold | Print the device table to the serial monitor and show its log timestamp on screen for 4 seconds. |

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

### Host Tests

The classifier and people estimator have no hardware dependencies and can be tested on the host:

```sh
c++ -std=c++17 -Isrc tests/ble_classify_test.cpp src/ble_classify.cpp -o /tmp/ble_test && /tmp/ble_test
c++ -std=c++17 -Isrc tests/person_estimator_test.cpp src/person_estimator.cpp -o /tmp/pe_test && /tmp/pe_test
```

## Serial Diagnostics

The serial monitor runs at `115200` baud. Every line starts with a timestamp in hundredths of a second since boot (`millis()/10`). Useful lines:

| Code | Meaning |
| --- | --- |
| `S3` | Status every 15 s: count, new/live Wi-Fi and BLE, phones, `people~N/M` (people / personal devices), mode, channel, battery. |
| `B1` | New BLE device with class, confidence, evidence bits, RSSI, and raw Apple manufacturer bytes (`apple=`). |
| `C8` / `C9` | Device table dump (power-button hold). The dump's ID is its timestamp, shown on screen as `DUMP nnnnnn`; each row includes `dev=` (session device ID) and `person=` (group ID). |
| `P1` | People estimate changed: devices, people, associated pairs, unresolved pairs. |
| `P2 obs` | Per-window average RSSI for each personal device (`dev:rssi`), usable for offline replay and tuning. |
| `P2 associate` / `separate` / `rotate` | Pair association events with score, and address-rotation handovers. |

Per-advertisement BLE duplicate lines are disabled by `SHOW_BLE_DUPS` in [src/counter.h](src/counter.h) because continuous scanning makes them very noisy.

## Privacy and Responsible Use

HAXXcounter is designed to show an aggregate count on-device. Nearby wireless identifiers can be personal data in some jurisdictions. Use it only where you have permission, avoid recording or sharing identifiers, and comply with local laws and venue policies.

Do not use this project for safety-critical occupancy measurement, security decisions, or individual tracking. The people estimate groups anonymous device records; it does not identify anyone.

## Project Layout

```text
src/
  main.cpp              Application loop, battery state, and physical controls
  counter.cpp           Wi-Fi/BLE discovery, deduplication, and counting
  ble_classify.cpp      BLE advertisement parsing and device classification
  person_estimator.cpp  Experimental people estimate (device grouping)
  display.cpp           AMOLED display and touch driver
  ui.cpp                LVGL user interface
  alert.cpp             Visual and audio alert behavior
  audio.cpp             ES8311 audio output
tests/                  Host-side unit tests
Docs/                   Design notes, including person-counting-design.md
include/
  lv_conf.h             LVGL configuration
scripts/                PlatformIO build helpers
```

