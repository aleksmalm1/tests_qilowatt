# Part 3: Python Testing & Provisioning Tool

## Overview

CLI tool that automates Tasmota device discovery, provisioning, sensor validation, and test reporting. Runs all four stages in a single invocation.

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐     ┌────────────┐
│  1. Discover │────▶│ 2. Configure │────▶│ 3. Validate  │────▶│ 4. Report  │
│  mDNS scan   │     │  HTTP API    │     │  MQTT sub    │     │  JSON file │
└─────────────┘     └──────────────┘     └──────────────┘     └────────────┘
```

## Requirements

```bash
pip install -r requirements.txt
```

Dependencies (`requirements.txt`):
- `zeroconf` — mDNS device discovery
- `requests` — Tasmota HTTP API communication
- `paho-mqtt` — MQTT subscribe & payload validation

## Usage

### Minimal (auto-detect topic from device)

```bash
python3 tasmota_tool.py \
  --web-user admin \
  --web-pass <password> \
  --ssid "MyWiFi" \
  --wifi-pass <wifi_password> \
  --mqtt-host 192.168.0.17
```

### With explicit topic

```bash
python3 tasmota_tool.py \
  --web-user admin \
  --web-pass <password> \
  --ssid "MyWiFi" \
  --wifi-pass <wifi_password> \
  --mqtt-host 192.168.0.17 \
  --mqtt-topic esp_s3
```

### With known device IP (skips mDNS scan)

```bash
python3 tasmota_tool.py \
  --device-ip 192.168.0.13 \
  --web-user admin \
  --web-pass <password> \
  --ssid "MyWiFi" \
  --wifi-pass <wifi_password> \
  --mqtt-host 192.168.0.17 \
  --mqtt-topic esp_s3
```

### All options

```bash
python3 tasmota_tool.py \
  --device-ip 192.168.0.13 \
  --web-user admin \
  --web-pass <password> \
  --ssid "MyWiFi" \
  --wifi-pass <wifi_password> \
  --mqtt-host 192.168.0.17 \
  --mqtt-port 1883 \
  --mqtt-user <mqtt_user> \
  --mqtt-pass <mqtt_password> \
  --mqtt-topic esp_s3 \
  --duration 30 \
  --mdns-timeout 5.0 \
  --output report.json
```

## CLI Arguments

| Argument | Required | Default | Description |
|---|---|---|---|
| `--ssid` | **Yes** | — | WiFi SSID to configure on device |
| `--wifi-pass` | **Yes** | — | WiFi password to configure |
| `--mqtt-host` | **Yes** | — | MQTT broker IP address |
| `--device-ip` | No | mDNS scan | Skip discovery, target this IP directly |
| `--web-user` | No | `None` | Tasmota WebUI username (needed if device has auth enabled) |
| `--web-pass` | No | `None` | Tasmota WebUI password |
| `--mqtt-port` | No | `1883` | MQTT broker port |
| `--mqtt-user` | No | `""` | MQTT broker username |
| `--mqtt-pass` | No | `""` | MQTT broker password |
| `--mqtt-topic` | No | auto-detected | Tasmota device topic — read from device `Status 0` if omitted |
| `--duration` | No | `30` | Seconds to listen for MQTT sensor messages |
| `--mdns-timeout` | No | `5.0` | mDNS discovery timeout in seconds |
| `--output` | No | `report.json` | Output path for the JSON test report |

## How Each Stage Works

### Stage 1: Discovery

- Uses **mDNS** (`_http._tcp.local.`) via the `zeroconf` library
- Listener callback **only collects IPv4 addresses** — no slow HTTP work in the callback
- IPv6 addresses are skipped (Tasmota HTTP API is IPv4-only)
- After the scan completes, each candidate IP is probed with `Status 0`
- A valid Tasmota device must return a JSON dict containing both `StatusNET` and `Status` keys
- The device's current **Topic** is read from the `Status 0` response for auto-detection
- If `--device-ip` is provided, mDNS is skipped entirely

### Stage 2: Configuration

- Sends WiFi and MQTT settings via Tasmota's **HTTP command API** (`/cm?cmnd=...`)
- Commands are sent **individually** (not via `Backlog`) because `Backlog` splits on `;` before parsing — SSIDs or passwords containing `;` would silently break
- Spaces in SSIDs/passwords are handled automatically by URL encoding (`requests` library)
- No quoting applied — Tasmota's HTTP parser treats the entire string after the command name as the value
- Supports **WebUI authentication** (`--web-user` / `--web-pass`) appended as query parameters
- Sets `TelePeriod 10` so sensor data is published every 10 seconds during validation
- After configuration, the tool **polls `Status 0`** every 1 second (up to 30s) instead of using a fixed `sleep()` — handles variable device restart times

Commands sent:
```
SSID1 <ssid>
Password1 <password>
MqttHost <host>
MqttPort <port>
MqttUser <user>          (if provided)
MqttPassword <password>  (if provided)
Topic <topic>            (if provided)
TelePeriod 10
```

### Stage 3: Validation

- Subscribes to `tele/<topic>/SENSOR` via **paho-mqtt**
- Topic is resolved as: `--mqtt-topic` if provided, otherwise read from device's `Status 0` response
- Listens for the configured `--duration` (default 30s)
- Each received message is validated against the expected format:

**Expected payload structure:**
```json
{
  "CustomSensor": {
    "Temperature": 23.5,
    "Humidity": 45.2,
    "Pressure": 1013.25
  }
}
```

**Validation checks:**
| Check | Fail condition |
|---|---|
| `CustomSensor` key exists | Missing entirely |
| `Temperature` present and numeric | Missing or non-numeric |
| `Humidity` present and numeric | Missing or non-numeric |
| `Pressure` present and numeric | Missing or non-numeric |

### Stage 4: Report

Generates a JSON file with discovery, configuration, and validation results.

**Passwords are redacted** (`***`) — the report is safe to share or commit.

**Pass criteria:** At least one message received AND all messages valid.

## Example Output

### Console

```
[*] mDNS scan (5.0s) ...
[*] Found 3 candidate IP(s), probing ...
[✓] 1 device(s) found
[*] Using topic: esp_s3
[*] Configuring 192.168.0.13 ...
[✓] Configuration applied
[*] Waiting for 192.168.0.13 to come back (up to 30s) ...
[✓] Device 192.168.0.13 is back online
[*] Listening on MQTT 192.168.0.17:1883 for 30s ...
[*] Subscribed to tele/esp_s3/SENSOR

Result: PASS ✓  (3/3 valid)
```

### Report (`report.json`)

```json
{
  "generated_at": "2026-02-22T14:30:00+00:00",
  "summary": {
    "devices_found": 1,
    "messages_received": 3,
    "valid": 3,
    "invalid": 0,
    "pass": true
  },
  "devices": [
    {
      "ip": "192.168.0.13",
      "hostname": "esp_s3",
      "topic": "esp_s3"
    }
  ],
  "configuration": {
    "SSID1": {"SSID1": "b3dac0 2.4"},
    "Password1": "***",
    "MqttHost": {"MqttHost": "192.168.0.17"},
    "MqttPort": {"MqttPort": 1883},
    "Topic": {"Topic": "esp_s3"},
    "TelePeriod": {"TelePeriod": 10}
  },
  "validation": [
    {
      "ts": "2026-02-22T14:30:15+00:00",
      "valid": true,
      "errors": [],
      "payload": {
        "CustomSensor": {
          "Temperature": 23.5,
          "Humidity": 45.2,
          "Pressure": 1013.25
        }
      }
    }
  ]
}
```


## Error Handling

| Scenario | Behavior |
|---|---|
| Device unreachable | `probe_tasmota()` returns `None`, reported as "No devices found" |
| WebUI auth required but not provided | Tasmota returns `{"WARNING": "Need user=..."}`, probe fails cleanly |
| Topic cannot be determined | Tool exits with message: "pass --mqtt-topic explicitly" |
| Device doesn't restart in time | `wait_for_device()` times out after 30s, tool proceeds with warning |
| MQTT broker unreachable | Connection error printed, validation returns empty list, report shows 0 messages |
| Malformed MQTT payload | JSON parse error caught, recorded as invalid with error description |
| No messages received during duration | Report `pass` is `false` (requires at least 1 valid message) |