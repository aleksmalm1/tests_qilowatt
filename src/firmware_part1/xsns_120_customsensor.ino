#ifdef USE_I2C
#ifdef USE_CUSTOMSENSOR

// xsns_120_customsensor.ino — BME280 I2C sensor driver for Tasmota
//
// Reads temperature, humidity, and pressure from a BME280 every 10 seconds
// using forced mode. Raw ADC values are compensated using factory calibration
// data per Bosch datasheet. Outputs JSON via Tasmota telemetry (FUNC_JSON_APPEND).

#define XSNS_120 120

// BME280 I2C addresses
static const uint8_t BME_ADDR0 = 0x76;
static const uint8_t BME_ADDR1 = 0x77;

// BME280 register map
static const uint8_t REG_ID        = 0xD0;
static const uint8_t ID_BME280     = 0x60;
static const uint8_t REG_RESET     = 0xE0;
static const uint8_t VAL_RESET     = 0xB6;
static const uint8_t REG_CTRL_HUM  = 0xF2;
static const uint8_t REG_STATUS    = 0xF3;
static const uint8_t REG_CTRL_MEAS = 0xF4;
static const uint8_t REG_CONFIG    = 0xF5;
static const uint8_t REG_CALIB00   = 0x88; // calibration block 1 (26 bytes)
static const uint8_t REG_CALIB26   = 0xE1; // calibration block 2 (7 bytes)
static const uint8_t REG_DATA      = 0xF7; // raw data burst (8 bytes)

// I2C wrappers around Tasmota's helper API
static bool CS_Read8(uint8_t addr, uint8_t reg, uint8_t *out) {
  return I2cValidRead8(out, addr, reg);
}
static bool CS_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len) {
  return (I2cReadBuffer(addr, reg, buf, len) == 0);
}
static bool CS_Write8(uint8_t addr, uint8_t reg, uint8_t val) {
  return I2cWrite8(addr, reg, val);
}

// factory calibration coefficients read from chip once during init
struct BmeCal {
  uint16_t dig_T1; int16_t dig_T2, dig_T3;
  uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
  uint8_t  dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4, dig_H5; int8_t dig_H6;
};

// driver runtime state
static struct {
  bool inited;
  bool valid;
  uint8_t addr;
  uint8_t fail_count;
  uint32_t last_read;
  const char *err;
  BmeCal cal;
  int32_t t_fine;       // shared between T/P/H compensation
  float t_c, h_rh, p_hpa;
} cs;

// byte-order helpers for calibration parsing (BME280 stores little-endian)
static inline uint16_t U16LE(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline int16_t  S16LE(const uint8_t *p) { return (int16_t)U16LE(p); }
static inline int16_t  SignExtend12(int16_t v) {
  v &= 0x0FFF;
  if (v & 0x0800) v |= 0xF000;
  return v;
}

// scan both possible addresses and verify chip ID
static bool CS_Detect(void) {
  const uint8_t addrs[] = { BME_ADDR0, BME_ADDR1 };
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t id = 0;
    if (CS_Read8(addrs[i], REG_ID, &id) && id == ID_BME280) {
      cs.addr = addrs[i];
      return true;
    }
  }
  return false;
}

// read and parse both calibration blocks from chip
static bool CS_ReadAndParseCal(void) {
  uint8_t c[26];
  uint8_t h[7];
  if (!CS_ReadBuf(cs.addr, REG_CALIB00, c, sizeof(c))) return false;
  if (!CS_ReadBuf(cs.addr, REG_CALIB26, h, sizeof(h))) return false;

  cs.cal.dig_T1 = U16LE(c + 0);
  cs.cal.dig_T2 = S16LE(c + 2);
  cs.cal.dig_T3 = S16LE(c + 4);

  cs.cal.dig_P1 = U16LE(c + 6);
  cs.cal.dig_P2 = S16LE(c + 8);
  cs.cal.dig_P3 = S16LE(c + 10);
  cs.cal.dig_P4 = S16LE(c + 12);
  cs.cal.dig_P5 = S16LE(c + 14);
  cs.cal.dig_P6 = S16LE(c + 16);
  cs.cal.dig_P7 = S16LE(c + 18);
  cs.cal.dig_P8 = S16LE(c + 20);
  cs.cal.dig_P9 = S16LE(c + 22);

  // humidity calibration uses mixed byte packing across both blocks
  cs.cal.dig_H1 = c[25];
  cs.cal.dig_H2 = (int16_t)((uint16_t)h[0] | ((uint16_t)h[1] << 8));
  cs.cal.dig_H3 = h[2];
  int16_t h4 = (int16_t)((((int16_t)h[3]) << 4) | (h[4] & 0x0F));
  int16_t h5 = (int16_t)((((int16_t)h[5]) << 4) | (h[4] >> 4));
  cs.cal.dig_H4 = SignExtend12(h4);
  cs.cal.dig_H5 = SignExtend12(h5);
  cs.cal.dig_H6 = (int8_t)h[6];

  return true;
}

// set oversampling x1 for all channels, sleep mode
// ctrl_hum must be written before ctrl_meas per datasheet
static bool CS_Configure(void) {
  if (!CS_Write8(cs.addr, REG_CTRL_HUM,  0x01)) return false;
  if (!CS_Write8(cs.addr, REG_CONFIG,    0x00)) return false;
  if (!CS_Write8(cs.addr, REG_CTRL_MEAS, 0x24)) return false;
  return true;
}

// full init: detect, reset, read calibration, configure
static bool CS_InitOnce(void) {
  cs.err = nullptr;
  cs.inited = false;

  if (!CS_Detect())          { cs.err = "NotDetected"; return false; }

  (void)CS_Write8(cs.addr, REG_RESET, VAL_RESET);
  delay(5); // BME280 needs ~2ms after soft reset

  if (!CS_ReadAndParseCal()) { cs.err = "CalibReadFail"; return false; }
  if (!CS_Configure())       { cs.err = "ConfigFail";    return false; }

  cs.inited = true;
  cs.valid = false;
  cs.fail_count = 0;

  AddLog(LOG_LEVEL_INFO, PSTR("CS: BME280 init OK at 0x%02X"), cs.addr);
  return true;
}

// trigger a single forced-mode measurement
static bool CS_TriggerForced(void) {
  return CS_Write8(cs.addr, REG_CTRL_MEAS, 0x25);
}

// poll status register until measurement completes (bounded to ~50ms)
static bool CS_WaitReady(void) {
  for (uint8_t i = 0; i < 25; i++) {
    uint8_t st = 0;
    if (!CS_Read8(cs.addr, REG_STATUS, &st)) return false;
    if ((st & 0x08) == 0) return true;
    delay(2);
  }
  return false;
}

// read 8-byte raw burst and unpack 20-bit pressure/temp, 16-bit humidity
static bool CS_ReadRaw(int32_t *adc_t, int32_t *adc_p, int32_t *adc_h) {
  uint8_t d[8];
  if (!CS_ReadBuf(cs.addr, REG_DATA, d, sizeof(d))) return false;

  *adc_p = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | ((int32_t)d[2] >> 4);
  *adc_t = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | ((int32_t)d[5] >> 4);
  *adc_h = ((int32_t)d[6] << 8)  |  (int32_t)d[7];
  return true;
}

// Bosch compensation: temperature (returns °C * 100, sets t_fine)
static int32_t CS_CompT_x100(int32_t adc_T) {
  int32_t var1 = ((((adc_T >> 3) - ((int32_t)cs.cal.dig_T1 << 1))) * ((int32_t)cs.cal.dig_T2)) >> 11;
  int32_t var2 = (((((adc_T >> 4) - ((int32_t)cs.cal.dig_T1)) * ((adc_T >> 4) - ((int32_t)cs.cal.dig_T1))) >> 12) *
                  ((int32_t)cs.cal.dig_T3)) >> 14;
  cs.t_fine = var1 + var2;
  return (cs.t_fine * 5 + 128) >> 8;
}

// Bosch compensation: pressure (returns Pa, requires t_fine)
static uint32_t CS_CompP_Pa(int32_t adc_P) {
  int64_t var1 = (int64_t)cs.t_fine - 128000;
  int64_t var2 = var1 * var1 * (int64_t)cs.cal.dig_P6;
  var2 += (var1 * (int64_t)cs.cal.dig_P5) << 17;
  var2 += ((int64_t)cs.cal.dig_P4) << 35;
  var1 = ((var1 * var1 * (int64_t)cs.cal.dig_P3) >> 8) + ((var1 * (int64_t)cs.cal.dig_P2) << 12);
  var1 = (((((int64_t)1) << 47) + var1) * (int64_t)cs.cal.dig_P1) >> 33;
  if (var1 == 0) return 0;

  int64_t p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = ((int64_t)cs.cal.dig_P9 * (p >> 13) * (p >> 13)) >> 25;
  var2 = ((int64_t)cs.cal.dig_P8 * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (((int64_t)cs.cal.dig_P7) << 4);
  return (uint32_t)(p >> 8);
}

// Bosch compensation: humidity (returns %RH * 1024, requires t_fine)
static uint32_t CS_CompH_x1024(int32_t adc_H) {
  int32_t v = cs.t_fine - 76800;
  v = (((((adc_H << 14) - (((int32_t)cs.cal.dig_H4) << 20) - (((int32_t)cs.cal.dig_H5) * v)) + 16384) >> 15) *
       (((((((v * (int32_t)cs.cal.dig_H6) >> 10) * (((v * (int32_t)cs.cal.dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
           (int32_t)cs.cal.dig_H2 + 8192) >> 14));
  v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)cs.cal.dig_H1) >> 4);
  if (v < 0) v = 0;
  if (v > 419430400) v = 419430400;
  return (uint32_t)(v >> 12);
}

// trigger, wait, read, compensate, validate — one complete measurement cycle
static bool CS_ReadOnce(void) {
  cs.err = nullptr;

  if (!CS_TriggerForced()) { cs.err = "TrigFail"; return false; }
  if (!CS_WaitReady())     { cs.err = "Timeout";  return false; }

  int32_t at, ap, ah;
  if (!CS_ReadRaw(&at, &ap, &ah)) { cs.err = "RawFail"; return false; }

  int32_t t_x100   = CS_CompT_x100(at);
  uint32_t p_pa    = CS_CompP_Pa(ap);
  uint32_t h_x1024 = CS_CompH_x1024(ah);

  if (p_pa == 0) { cs.err = "Press0"; return false; }

  cs.t_c  = (float)t_x100 / 100.0f;
  cs.p_hpa = (float)p_pa / 100.0f;
  cs.h_rh  = (float)h_x1024 / 1024.0f;

  // reject values outside BME280 operating range
  if (cs.t_c < -40.0f || cs.t_c > 85.0f ||
      cs.h_rh < 0.0f || cs.h_rh > 100.0f ||
      cs.p_hpa < 300.0f || cs.p_hpa > 1100.0f) {
    cs.err = "Range";
    return false;
  }

  cs.valid = true;
  cs.err = nullptr;
  return true;
}

// called every second; reads sensor every 10s with up to 3 retries
// after 3 consecutive failed cycles, forces full re-init
static void CS_EverySecond(void) {
  if (!cs.inited) {
    CS_InitOnce();
    return;
  }

  if ((TasmotaGlobal.uptime - cs.last_read) < 10) return;
  cs.last_read = TasmotaGlobal.uptime;

  for (uint8_t i = 0; i < 3; i++) {
    if (CS_ReadOnce()) { cs.fail_count = 0; return; }
    delay(10);
  }

  cs.fail_count++;
  cs.valid = false;
  if (cs.fail_count >= 3) {
    cs.inited = false;
  }
}

// append sensor data to Tasmota telemetry JSON
static void CS_JsonAppend(void) {
  if (!cs.inited || !cs.valid || cs.err) {
    ResponseAppend_P(PSTR(",\"CustomSensor\":{\"Error\":\"%s\",\"FailCount\":%u}"),
      cs.err ? cs.err : (cs.inited ? "NoData" : "NotReady"), cs.fail_count);
    return;
  }

  ResponseAppend_P(PSTR(",\"CustomSensor\":{\"Temperature\":%*_f,\"Humidity\":%*_f,\"Pressure\":%*_f}"),
    Settings->flag2.temperature_resolution, &cs.t_c,
    Settings->flag2.humidity_resolution, &cs.h_rh,
    Settings->flag2.pressure_resolution, &cs.p_hpa);
}

// Tasmota driver entry point
bool Xsns120(uint32_t function) {
  switch (function) {
    case FUNC_INIT:
      memset(&cs, 0, sizeof(cs));
      CS_InitOnce();
      break;
    case FUNC_EVERY_SECOND:
      CS_EverySecond();
      break;
    case FUNC_JSON_APPEND:
      CS_JsonAppend();
      break;
  }
  return false;
}

#endif  // USE_CUSTOMSENSOR
#endif  // USE_I2C
