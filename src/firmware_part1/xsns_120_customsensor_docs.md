# Part 1: Custom Sensor Firmware Driver

## Overview

Tasmota sensor driver (`xsns_120`) that reads temperature, humidity, and pressure from a **BME280** over I2C. Uses forced mode with factory calibration compensation per the Bosch datasheet. Outputs data via Tasmota's telemetry system (`FUNC_JSON_APPEND`).

```
┌───────────┐     ┌───────────┐     ┌──────────────┐     ┌──────────────┐
│  1. Init  │────▶│ 2. Read   │────▶│ 3. Compensate│────▶│ 4. Telemetry │
│  Detect   │     │  Trigger  │     │  Bosch algo   │     │  JSON append │
│  + Calib  │     │  + Raw ADC│     │  T / H / P    │     │  MQTT publish│
└───────────┘     └───────────┘     └──────────────┘     └──────────────┘
```

## Requirements

- **Hardware:** BME280 sensor connected via I2C (address `0x76` or `0x77`)
- **Firmware:** Tasmota build with `USE_I2C` and `USE_CUSTOMSENSOR` defined
- **Slot:** Registered as `XSNS_120`

### Build Flags

Add to `user_config_override.h`:

```cpp
#define USE_I2C
#define USE_CUSTOMSENSOR
```

## How Each Stage Works

### Stage 1: Initialization (`FUNC_INIT`)

1. **Detect** — Scans I2C addresses `0x76` and `0x77`, reads register `0xD0` and checks for chip ID `0x60` (BME280)
2. **Reset** — Sends soft reset command (`0xB6` to register `0xE0`), waits 5 ms
3. **Read Calibration** — Reads two calibration blocks from the chip:
   - Block 1: 26 bytes from register `0x88` (temperature & pressure coefficients)
   - Block 2: 7 bytes from register `0xE1` (humidity coefficients)
4. **Configure** — Sets oversampling ×1 for all channels, sleep mode:
   - `ctrl_hum` (`0xF2`) = `0x01` — humidity oversampling ×1
   - `config` (`0xF5`) = `0x00` — no filter, no standby
   - `ctrl_meas` (`0xF4`) = `0x24` — temp/press oversampling ×1, sleep mode

> **Note:** `ctrl_hum` must be written before `ctrl_meas` per the BME280 datasheet.

### Stage 2: Periodic Reading (`FUNC_EVERY_SECOND`)

- Called every second by Tasmota's scheduler
- Actual measurement is performed **every 10 seconds** (based on uptime delta)
- Each measurement cycle:
  1. **Trigger** — Writes `0x25` to `ctrl_meas` (forced mode, single measurement)
  2. **Wait** — Polls status register `0xF3` (bit 3) up to 25 times with 2 ms delays (~50 ms max)
  3. **Read** — Burst-reads 8 bytes from register `0xF7`:
     - Bytes 0–2: 20-bit raw pressure
     - Bytes 3–5: 20-bit raw temperature
     - Bytes 6–7: 16-bit raw humidity
- **Retry logic:** Up to 3 attempts per cycle
- **Re-init logic:** After 3 consecutive failed cycles, driver resets to uninitialized state and re-runs full init

### Stage 3: Compensation

Applies Bosch's integer compensation algorithms to convert raw ADC values:

| Measurement | Raw Bits | Compensation Output | Final Conversion |
|---|---|---|---|
| Temperature | 20-bit | `t_x100` (°C × 100) | `float` °C |
| Pressure | 20-bit | `p_pa` (Pascals) | `float` hPa |
| Humidity | 16-bit | `h_x1024` (%RH × 1024) | `float` %RH |

- Temperature compensation also produces `t_fine`, a shared intermediate value required by pressure and humidity compensation
- Compensation order is always: **Temperature → Pressure → Humidity**

#### Range Validation

After compensation, values are checked against BME280 operating limits:

| Parameter | Valid Range |
|---|---|
| Temperature | −40 °C to +85 °C |
| Humidity | 0 %RH to 100 %RH |
| Pressure | 300 hPa to 1100 hPa |

Out-of-range readings are rejected with error `"Range"`.

### Stage 4: Telemetry Output (`FUNC_JSON_APPEND`)

Appends sensor data to Tasmota's telemetry JSON, published via MQTT on `tele/<topic>/SENSOR`.

**Successful reading:**
```json
{
  "CustomSensor": {
    "Temperature": 23.5,
    "Humidity": 45.2,
    "Pressure": 1013.25
  }
}
```

**Error state:**
```json
{
  "CustomSensor": {
    "Error": "NotDetected",
    "FailCount": 3
  }
}
```

Decimal precision follows Tasmota's global resolution settings:
- `Settings->flag2.temperature_resolution`
- `Settings->flag2.humidity_resolution`
- `Settings->flag2.pressure_resolution`

## Register Map

| Register | Address | Purpose |
|---|---|---|
| `REG_ID` | `0xD0` | Chip ID (expect `0x60`) |
| `REG_RESET` | `0xE0` | Soft reset (write `0xB6`) |
| `REG_CTRL_HUM` | `0xF2` | Humidity oversampling |
| `REG_STATUS` | `0xF3` | Measurement status |
| `REG_CTRL_MEAS` | `0xF4` | Temp/press oversampling + mode |
| `REG_CONFIG` | `0xF5` | Filter & standby config |
| `REG_CALIB00` | `0x88` | Calibration block 1 (26 bytes) |
| `REG_CALIB26` | `0xE1` | Calibration block 2 (7 bytes) |
| `REG_DATA` | `0xF7` | Raw data burst (8 bytes) |

## Calibration Coefficients

### Temperature & Pressure (from block 1)

| Coefficient | Type | Offset |
|---|---|---|
| `dig_T1` | `uint16` | 0 |
| `dig_T2` | `int16` | 2 |
| `dig_T3` | `int16` | 4 |
| `dig_P1` | `uint16` | 6 |
| `dig_P2`–`dig_P9` | `int16` | 8–22 |

### Humidity (mixed packing across both blocks)

| Coefficient | Type | Source |
|---|---|---|
| `dig_H1` | `uint8` | Block 1, byte 25 |
| `dig_H2` | `int16` | Block 2, bytes 0–1 |
| `dig_H3` | `uint8` | Block 2, byte 2 |
| `dig_H4` | `int16` (12-bit) | Block 2, bytes 3–4 (low nibble) |
| `dig_H5` | `int16` (12-bit) | Block 2, bytes 4 (high nibble)–5 |
| `dig_H6` | `int8` | Block 2, byte 6 |

## Error Handling

| Error String | Cause |
|---|---|
| `NotDetected` | No BME280 found at `0x76` or `0x77` |
| `CalibReadFail` | Failed to read calibration registers |
| `ConfigFail` | Failed to write config registers |
| `TrigFail` | Failed to trigger forced measurement |
| `Timeout` | Measurement did not complete within ~50 ms |
| `RawFail` | Failed to read raw data burst |
| `Press0` | Pressure compensation returned 0 (division guard) |
| `Range` | Compensated value outside BME280 operating range |
| `NoData` | Initialized but no valid reading yet |
| `NotReady` | Driver not initialized |

## Timing

| Event | Interval |
|---|---|
| `FUNC_EVERY_SECOND` callback | 1 second |
| Actual sensor read | Every 10 seconds |
| Measurement wait timeout | ~50 ms (25 × 2 ms polls) |
| Post-reset delay | 5 ms |
| Retry delay between attempts | 10 ms |
| Max retries per cycle | 3 |
| Failed cycles before re-init | 3 |