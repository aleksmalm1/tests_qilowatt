# Part 2: Berry Alarm Monitor Script

## Overview

Berry script that monitors the CustomSensor driver's temperature, humidity, and pressure readings using a **5-sample moving average**. When an average exceeds a threshold, the script publishes an MQTT alert and logs the event to flash storage via Tasmota's `persist` module. Duplicate alerts are suppressed until the condition clears.

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐     ┌────────────┐
│ 1. Poll      │────▶│ 2. Average   │────▶│ 3. Evaluate  │────▶│ 4. Alert   │
│  read_sensors│     │  5-sample    │     │  Threshold   │     │  MQTT pub  │
│  every 10s   │     │  moving avg  │     │  comparison  │     │  + persist │
└──────────────┘     └──────────────┘     └──────────────┘     └────────────┘
```

## Requirements

- **Firmware:** Tasmota build with **Berry scripting** support
- **Driver:** `xsns_120_customsensor` must be active and producing valid `CustomSensor` telemetry
- **MQTT:** Broker connected (for alert publishing)
- **Deployment:** Upload to device filesystem and load via `autoexec.be` or Tasmota Berry console

### Installation

Upload the script to the device and add to `autoexec.be`:

```berry
load("customsensor_berry_script.be")
```

Or load manually from the Berry console:

```berry
load("customsensor_berry_script.be")
```

## Configuration

### Alarm Thresholds

Hardcoded at the top of the script:

| Threshold | Variable | Default | Unit |
|---|---|---|---|
| Temperature high | `TEMP_HI` | `30.0` | °C |
| Humidity high | `HUM_HI` | `80.0` | %RH |
| Pressure high | `PRESS_HI` | `1030.0` | hPa |

### Moving Average Window

| Parameter | Variable | Default |
|---|---|---|
| Window size | `WIN` | `5` samples |

At 10-second polling, a full window covers **50 seconds** of data.

## How Each Stage Works

### Stage 1: Polling (`poll`)

- Registered as a **cron job** running every 10 seconds (`*/10 * * * * *`) with ID `cs10s`
- Reads the current sensor state via `tasmota.read_sensors()` and parses JSON
- Extracts `Temperature`, `Humidity`, and `Pressure` from the `CustomSensor` key
- **Early exits** (no alarm evaluation):
  - `CustomSensor` key is missing
  - `Error` key is present in the sensor object
  - Any of the three values is `nil`

> **Safe reload:** Before registering the cron job, the script removes any existing `cs10s` entry to prevent duplicate timers on script reload.

### Stage 2: Moving Average

- Each metric has a dedicated circular buffer (`buf_t`, `buf_h`, `buf_p`)
- New values are appended; when the buffer exceeds `WIN` (5), the oldest entry is removed
- Arithmetic mean is computed over all samples in the buffer
- **Threshold evaluation is deferred** until all three buffers have at least `WIN` samples (first 50 seconds after boot are silent)

### Stage 3: Threshold Evaluation

Each metric is evaluated independently with **edge-triggered** logic:

| Condition | Action |
|---|---|
| Average **exceeds** threshold AND alarm not active | **Set** alarm, publish alert, log event |
| Average **at or below** threshold AND alarm active | **Clear** alarm, log recovery event |
| Average exceeds threshold AND alarm already active | No action (duplicate suppression) |
| Average at or below threshold AND alarm not active | No action |

### Stage 4: Alerting & Logging

#### MQTT Alerts

Published to `tele/<device_topic>/ALERT`:

**Alarm triggered:**
```json
{"Alert": "TempHigh", "Temperature": 31.2, "TempAvg5": 30.5}
```

```json
{"Alert": "HumHigh", "Humidity": 82.0, "HumAvg5": 81.3}
```

```json
{"Alert": "PressHigh", "Pressure": 1032.0, "PressAvg5": 1031.1}
```

**No MQTT message is published on recovery** — only the log event is recorded.

#### Alert Topic Resolution

- The topic is resolved **lazily** on first use (not at script load time)
- Read from the device via `tasmota.cmd("Topic", true)`
- Constructed as: `tele/<Topic>/ALERT`
- If resolution fails (e.g., MQTT not yet connected), the MQTT publish is skipped but the persist log is still written

#### Flash Logging (`persist`)

Each event is stored in `persist.cs_log` as a list of objects:

```json
{
  "ts": "2026-02-22T14:30:15",
  "event": "TempHigh",
  "metric": "Temp",
  "raw": 31.2,
  "avg5": 30.5
}
```

| Field | Description |
|---|---|
| `ts` | Timestamp from device RTC (`tasmota.time_str`) |
| `event` | Event name (see table below) |
| `metric` | Short metric identifier |
| `raw` | Raw sensor value at the time of the event |
| `avg5` | 5-sample moving average that triggered the event |

**Log rotation:** The log is capped at **100 entries**. Oldest entries are removed when the limit is reached to prevent flash exhaustion.

After every write, `persist.dirty()` and `persist.save()` are called to flush to flash.

## Event Types

| Event | Meaning | MQTT Published |
|---|---|---|
| `TempHigh` | Temperature average exceeded `TEMP_HI` | ✓ |
| `TempNormal` | Temperature average returned to or below `TEMP_HI` | ✗ |
| `HumHigh` | Humidity average exceeded `HUM_HI` | ✓ |
| `HumNormal` | Humidity average returned to or below `HUM_HI` | ✗ |
| `PressHigh` | Pressure average exceeded `PRESS_HI` | ✓ |
| `PressNormal` | Pressure average returned to or below `PRESS_HI` | ✗ |

## Tasmota Log Messages

All log messages use **log level 2** (info) and are prefixed with `CS:`:

| Message | Meaning |
|---|---|
| `CS: TempHigh avg=<value>` | Temperature alarm triggered |
| `CS: TempNormal avg=<value>` | Temperature alarm cleared |
| `CS: HumHigh avg=<value>` | Humidity alarm triggered |
| `CS: HumNormal avg=<value>` | Humidity alarm cleared |
| `CS: PressHigh avg=<value>` | Pressure alarm triggered |
| `CS: PressNormal avg=<value>` | Pressure alarm cleared |
| `CS: topic resolve failed: <msg>` | Could not read device topic |
| `CS: poll error: <type> <msg>` | Unhandled exception during poll |

## Error Handling

| Scenario | Behavior |
|---|---|
| CustomSensor not loaded / no data | `cs` is `nil`, poll exits silently |
| Sensor reports error state | `Error` key detected, poll exits silently |
| Missing individual readings | `nil` check on T/H/P, poll exits silently |
| MQTT not connected | Topic resolution fails, alert skipped, persist log still written |
| Topic resolution exception | Caught, logged at level 2, topic remains `nil` |
| Flash log overflow | Oldest entries rotated out at 100 entries |
| Script reloaded | Existing `cs10s` cron removed before re-adding |
| Any unhandled exception in poll | Caught by top-level `try/except`, logged at level 2 |

## Timing

| Event | Interval |
|---|---|
| Cron poll | Every 10 seconds |
| Full window populated | 50 seconds (5 × 10s) after first valid reading |
| First possible alarm | After 5th valid reading (~50s) |
| Persist flush | Immediately on every alarm/recovery event |