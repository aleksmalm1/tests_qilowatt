Smart Sensor Module for Tasmota (BME280) – Submission
=======================================================

This repo contains the deliverables for the Smart Sensor Module for Tasmota evaluation task:

- Part 1: Tasmota C/C++ driver (`xsns_120_customsensor.ino`) for BME280 (I2C)
- Part 2: Berry script for threshold alerts + moving average + flash log rotation
- Part 3: Python CLI tool for (optional) provisioning + MQTT validation + JSON test report

Assumption: You already have a working local Tasmota source directory on your machine and you can build/flash firmware from it. This submission only provides the files and instructions for where to place them.


Repo contents
-------------

```
docs/
  Wiring.txt
src/
  firmware_part1/
    xsns_120_customsensor.ino
    xsns_120_customsensor_docs.md
  berry_part2/
    customsensor_ berry_script.be
    berry_script_docs.md
  python_part3/
    tasmota_tool.py
    Python_Tool_docs.md
```


Hardware
--------
- ESP32
- BME280 breakout (I2C)
- Jumper wires


Wiring (I2C)
------------

```
BME280  ->  MCU
----------------
VCC     ->  3V3
GND     ->  GND
SDA     ->  GPIO configured as I2C SDA in Tasmota
SCL     ->  GPIO configured as I2C SCL in Tasmota
```


Prerequisites
-------------

On your machine (for compiling)
- Git (optional, if your Tasmota dir is already present)
- Python 3.10+
- PlatformIO Core (or VSCode + PlatformIO)

Python dependencies (for the CLI tool):
- `zeroconf` — mDNS device discovery
- `requests` — Tasmota HTTP API communication
- `paho-mqtt` — MQTT subscribe & payload validation

Install them with:

```bash
pip install zeroconf requests paho-mqtt
```

Network
- MQTT broker reachable from your device
- Your device on the same LAN as your Python tool (for discovery/provisioning)


Step 1 — Copy the driver file into your Tasmota directory
---------------------------------------------------------

Take the driver file from this repo:

  `src/firmware_part1/xsns_120_customsensor.ino`

Copy it into your local Tasmota source tree:

  `<TASMOTA_DIR>/tasmota/tasmota_xsns_sensor/`
  

Example:

```bash
cp src/firmware_part1/xsns_120_customsensor.ino /path/to/Tasmota/tasmota/tasmota_xsns_sensor/
```


Step 2 — Enable the driver via user_config_override.h 
-------------------------------------------------------------------------------

This submission uses `user_config_override.h` ONLY for compile-time feature flags.
WiFi + MQTT credentials are NOT compiled into firmware and must be configured at runtime
(via Web UI, console, or provisioning tool).

Edit `user_config_override.h` in your Tasmota source tree:

  `<TASMOTA_DIR>/tasmota/user_config_override.h`

The content should look like this (important: keep the `#endif`):

```cpp
#ifndef _USER_CONFIG_OVERRIDE_H_
#define _USER_CONFIG_OVERRIDE_H_

#warning "**** user_config_override.h: Using Settings from this File ****"

#define USE_I2C
#define USE_CUSTOMSENSOR
#undef USE_BME280

#endif  // _USER_CONFIG_OVERRIDE_H_
```


Step 3 — Build and flash
------------------------

Pick the correct PlatformIO environment for your board.

Example (ESP32-S3 DevKitC class boards):

```bash
cd /path/to/Tasmota
platformio run -e tasmota32s3-qio_opi-all
```

If you are not on ESP32-S3, pick an appropriate env from the Tasmota repo configs
(e.g., `tasmota32`, `tasmota32c3`, etc).

After build, your `.bin` will appear under:
  `.pio/build/<env>/firmware.bin` (exact path depends on environment)


Flash (example with esptool):

```bash
python -m esptool --port /dev/ttyACM0 erase_flash
python -m esptool --port /dev/ttyACM0 write_flash 0x0 .pio/build/<env>/firmware.bin
```


Step 4 — Configure Tasmota at runtime (I2C + MQTT)
-------------------------------------------------------------

Configure I2C pins
- Configuration → Configure Module
- Set GPIO for I2C SDA and I2C SCL
- Save (device restarts)

Configure MQTT
- Configuration → Configure MQTT (host/port/user/pass)
- Configuration → Configure Other (ensure MQTT is enabled, depending on build defaults)

(Optional) Adjust telemetry period:

The driver reads the sensor every 10 seconds internally. Tasmota's `TelePeriod` controls
how often the `tele/<topic>/SENSOR` message is published via MQTT. The default is 300 seconds.
If you want sensor data published more frequently, lower it in the Tasmota Console:

```
TelePeriod 10
```

Note: If you use the Python tool to provision settings via HTTP, you can skip manual
MQTT configuration and let the tool set it.


Step 5 — Verify MQTT sensor payload
-----------------------------------

Tasmota publishes sensor data to:
  `tele/<TOPIC>/SENSOR`

To verify, subscribe to the topic using `mosquitto_sub`:

```bash
mosquitto_sub -h <MQTT_BROKER_IP> -t "tele/+/SENSOR" -v
```

Or use the Tasmota Web Console to check sensor output directly:
- Go to **Consoles → Console** in the Web UI
- Sensor readings appear in the periodic telemetry log

Expected content (example):

```json
{
  "Time": "2026-02-22T12:34:56",
  "CustomSensor": {
    "Temperature": 23.4,
    "Humidity": 41.2,
    "Pressure": 1008.6
  }
}
```

Error state (example):

```json
{
  "Time": "2026-02-22T12:34:56",
  "CustomSensor": {
    "Error": "NotDetected",
    "FailCount": 3
  }
}
```


Step 6 — Install and run the Berry automation
---------------------------------------------

Upload the script:
- Tasmota Web UI → Consoles → Manage File system
- Upload: `src/berry_part2/customsensor_ berry_script.be`

Run it (Tasmota Console):

```
Br load("customsensor_ berry_script.be")
```

(Optional) Auto-start on boot:
Create/edit `autoexec.be` and add:

```berry
load("customsensor_ berry_script.be")
```

The Berry script:
- Watches the `CustomSensor` readings from the driver
- Applies a moving average (last 5 readings)
- Triggers MQTT alerts to `tele/<topic>/ALERT` when thresholds are exceeded
- Suppresses duplicate alerts until the condition clears
- Writes events to flash via `persist` with log rotation (max 100 entries)

Default thresholds (adjustable at the top of the script):

| Parameter | Default | Unit |
|---|---|---|
| Temperature | 30.0 | °C |
| Humidity | 80.0 | %RH |
| Pressure | 1030.0 | hPa |


Step 7 — Python validation (and optional provisioning)
------------------------------------------------------

Setup:

```bash
cd src/python_part3
python -m venv .venv
source .venv/bin/activate
pip install zeroconf requests paho-mqtt
```

Run:

```bash
python tasmota_tool.py --help
```

Typical usage examples:

```bash
# Auto-discover device, configure, and validate
python tasmota_tool.py \
  --ssid MyWiFi \
  --wifi-pass secret \
  --mqtt-host 192.168.0.10 \
  --web-user admin

# Skip discovery, target a known device IP
python tasmota_tool.py \
  --device-ip 192.168.0.13 \
  --ssid MyWiFi \
  --wifi-pass secret \
  --mqtt-host 192.168.0.10 \
  --mqtt-topic esp_s3

# Specify output report path
python tasmota_tool.py \
  --device-ip 192.168.0.13 \
  --ssid MyWiFi \
  --wifi-pass secret \
  --mqtt-host 192.168.0.10 \
  --output report.json
```

Outputs:
- Prints discovery/provisioning status
- Subscribes to `tele/<topic>/SENSOR`
- Validates payload format (`CustomSensor` key with `Temperature`, `Humidity`, `Pressure`)
- Writes a JSON test report


Documentation
-------------

Each part has its own detailed documentation:

- **Part 1 (Driver):** `src/firmware_part1/xsns_120_customsensor_docs.md`
- **Part 2 (Berry):** `src/berry_part2/berry_script_docs.md`
- **Part 3 (Python):** `src/python_part3/Python_Tool_docs.md`
