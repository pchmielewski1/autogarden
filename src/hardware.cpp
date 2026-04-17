// ============================================================================
// hardware.cpp — Implementacja driverów hardware
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Warstwa hardware — dekompozycja abstrakcji"
// Architektura:  docs/ARCHITECTURE.md
//
// UWAGA AGENTOWI:
//   I2C: SDA=G9, SCL=G10, 100kHz. NIE ZMIENIAJ PINÓW!
//   PbHUB v1.1 komendy (docs/pbhub_v1.1_firmware_reference.md):
//     cmd byte = ch_base | cmd_nibble
//     0x00/0x01 = Digital Write pin0/pin1
//     0x04/0x05 = Digital Read  pin0/pin1
//     0x06      = ADC Read 12-bit (pin0 ONLY)
//   Między Wire.endTransmission() a Wire.requestFrom() → delay(_delayMs)
// ============================================================================

#include "hardware.h"
#include <Wire.h>
#include <Arduino.h>
#include <cmath>
#include <cstring>
#include "log_serial.h"

#define Serial AGSerial

// Out-of-class definition for static constexpr (ODR-used)
constexpr uint8_t PbHubBus::kChBase[6];

namespace {
constexpr uint16_t kMaxPbHubAdcRaw = 4095;
constexpr uint16_t kMoistureFallbackDryRaw = 2230;
constexpr uint8_t kMoistureMedianWindow = 10;

float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

float moistureCurve(float t, float curveExponent) {
    t = clamp01(t);
    if (curveExponent < 0.1f) curveExponent = 0.1f;
    if (curveExponent > 12.0f) curveExponent = 12.0f;
    return powf(t, curveExponent);
}

uint16_t medianOfUint16(const uint16_t* samples, uint8_t count) {
    if (count == 0) return kMoistureFallbackDryRaw;

    uint16_t sorted[kMoistureMedianWindow] = {};
    for (uint8_t i = 0; i < count; ++i) {
        sorted[i] = samples[i];
    }

    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            if (sorted[j] < sorted[i]) {
                uint16_t temp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temp;
            }
        }
    }

    if ((count % 2U) == 0U) {
        return static_cast<uint16_t>((sorted[count / 2 - 1] + sorted[count / 2]) / 2U);
    }
    return sorted[count / 2];
}
}

float normalizeMoistureRaw(uint16_t raw, uint16_t dryRaw, uint16_t wetRaw,
                           float curveExponent) {
    if (wetRaw >= dryRaw) {
        dryRaw = kMoistureFallbackDryRaw;
        wetRaw = 1752;
    }

    if (raw >= dryRaw) return 0.0f;
    if (raw <= wetRaw) return 100.0f;

    // Two-point nonlinear mapping based only on measured dry and wet endpoints.
    // We normalize raw ADC to 0..1 and then apply a convex curve that delays
    // saturation near wetRaw, so values close to the wet endpoint do not jump
    // to 100% too early.
    const float span = static_cast<float>(dryRaw - wetRaw);
    const float normalizedWetness = clamp01((static_cast<float>(dryRaw) - static_cast<float>(raw)) / span);
    return 100.0f * moistureCurve(normalizedWetness, curveExponent);
}

// ===== PbHubBus =============================================================

bool PbHubBus::init(uint8_t i2cAddr, uint8_t delayMs) {
    _addr = i2cAddr;
    _delayMs = delayMs;

    // Wire.begin() wywoływane w main.cpp (lub HardwareManager::init)
    // Tutaj tylko probePresent
    return probePresent();
}

bool PbHubBus::probePresent() {
    Wire.beginTransmission(_addr);
    return Wire.endTransmission() == 0;
}

uint8_t PbHubBus::fwVersion() {
    uint8_t ver = 0;
    Wire.beginTransmission(_addr);
    Wire.write(0xFE);   // firmware version register
    Wire.endTransmission();
    delay(_delayMs);
    Wire.requestFrom(_addr, (uint8_t)1);
    if (Wire.available()) ver = Wire.read();
    return ver;
}

uint8_t PbHubBus::cmdRead(uint8_t ch, uint8_t pin) const {
    // Digital read: base + 0x04 + pin  (per pbhub_v1.1_firmware_reference.md)
    return kChBase[ch] + 0x04 + pin;
}

uint8_t PbHubBus::cmdWrite(uint8_t ch, uint8_t pin) const {
    // Digital write: base + 0x00 + pin  (per pbhub_v1.1_firmware_reference.md)
    return kChBase[ch] + 0x00 + pin;
}

bool PbHubBus::i2cWrite(uint8_t cmd, uint8_t data) {
    Wire.beginTransmission(_addr);
    Wire.write(cmd);
    Wire.write(data);
    return Wire.endTransmission() == 0;
}

bool PbHubBus::i2cRead16(uint8_t cmd, uint16_t& out) {
    Wire.beginTransmission(_addr);
    Wire.write(cmd);
    if (Wire.endTransmission() != 0) return false;
    delay(_delayMs);
    Wire.requestFrom(_addr, (uint8_t)2);
    if (Wire.available() < 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    out = (hi << 8) | lo;
    return true;
}

bool PbHubBus::i2cRead8(uint8_t cmd, uint8_t& out) {
    Wire.beginTransmission(_addr);
    Wire.write(cmd);
    if (Wire.endTransmission() != 0) return false;
    delay(_delayMs);
    Wire.requestFrom(_addr, (uint8_t)1);
    if (!Wire.available()) return false;
    out = Wire.read();
    return true;
}

ReadResult<uint16_t> PbHubBus::analogRead(uint8_t channel) {
    // ADC Read 12-bit: base + 0x06 (pin0 only, per pbhub_v1.1_firmware_reference.md)
    // PbHUB reconfigures pin0 to GPIO_MODE_ANALOG, reads STM32 ADC channel = ch num
    ReadResult<uint16_t> r;
    r.readAtMs = millis();
    uint8_t cmd = kChBase[channel] + 0x06;
    if (!i2cRead16(cmd, r.value)) {
        r.health = SensorHealth::FAIL;
        r.value = 0;
    }
    return r;
}

ReadResult<bool> PbHubBus::digitalRead(uint8_t channel, uint8_t pin) {
    ReadResult<bool> r;
    r.readAtMs = millis();
    uint8_t cmd = kChBase[channel] + 0x04 + pin;
    uint8_t val = 0;
    if (!i2cRead8(cmd, val)) {
        r.health = SensorHealth::FAIL;
        r.value = false;
    } else {
        r.value = (val != 0);
    }
    return r;
}

bool PbHubBus::digitalWrite(uint8_t channel, uint8_t pin, bool level) {
    // Digital write: base + 0x00 + pin (per pbhub_v1.1_firmware_reference.md)
    // cmd 0x00 = pin0, cmd 0x01 = pin1. Data: 0x00=LOW, 0x01=HIGH
    uint8_t cmd = kChBase[channel] + 0x00 + pin;
    return i2cWrite(cmd, level ? 0x01 : 0x00);
}

// ===== SoilMoistureSensor ===================================================

void SoilMoistureSensor::init(PbHubBus* bus, uint8_t channel) {
    _bus = bus;
    _channel = channel;
}

ReadResult<uint16_t> SoilMoistureSensor::readRaw(uint32_t nowMs) {
    // PbHUB ADC cmd 0x06 always reads pin0 — the IN-side pin of the channel.
    // Watering Unit: pin0 = AOUT (moisture), so this is correct.
    ReadResult<uint16_t> r;
    r.readAtMs = nowMs;
    if (!_bus) { r.health = SensorHealth::FAIL; r.value = 0; return r; }
    r = _bus->analogRead(_channel);
    r.readAtMs = nowMs;
    return r;
}

// ===== WaterLevelSensor =====================================================

void WaterLevelSensor::init(PbHubBus* bus, uint8_t channel, uint8_t pin, bool activeLow) {
    _bus = bus;
    _channel = channel;
    _pin = pin;
    _activeLow = activeLow;
}

ReadResult<bool> WaterLevelSensor::readRawLevel(uint32_t nowMs) {
    ReadResult<bool> r;
    r.readAtMs = nowMs;

    if (!_bus) {
        r.health = SensorHealth::FAIL;
        r.value = false;
        return r;
    }

    r = _bus->digitalRead(_channel, _pin);
    r.readAtMs = nowMs;
    return r;
}

WaterLevelState WaterLevelSensor::normalizePhysicalLevel(bool physicalHigh) const {
    if (_activeLow) {
        return physicalHigh ? WaterLevelState::OK : WaterLevelState::TRIGGERED;
    }
    return physicalHigh ? WaterLevelState::TRIGGERED : WaterLevelState::OK;
}

ReadResult<WaterLevelState> WaterLevelSensor::readState(uint32_t nowMs) {
    ReadResult<WaterLevelState> r;
    r.readAtMs = nowMs;

    auto raw = readRawLevel(nowMs);
    if (!raw.ok()) {
        r.health = SensorHealth::FAIL;
        r.value = WaterLevelState::UNKNOWN;
        return r;
    }

    r.value = normalizePhysicalLevel(raw.value);

    return r;
}

// ===== EnvSensor (SHT30) ====================================================

bool EnvSensor::init(uint8_t addr) {
    _addr = addr;
    Wire.beginTransmission(_addr);
    return Wire.endTransmission() == 0;
}

ReadResult<EnvReading> EnvSensor::readEnv(uint32_t nowMs) {
    ReadResult<EnvReading> r;
    r.readAtMs = nowMs;

    // SHT30: single-shot high repeatability: cmd 0x2400
    Wire.beginTransmission(_addr);
    Wire.write(0x24);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
        r.health = SensorHealth::FAIL;
        return r;
    }

    delay(20);  // SHT30 measurement time ~15ms

    Wire.requestFrom(_addr, (uint8_t)6);
    if (Wire.available() < 6) {
        r.health = SensorHealth::FAIL;
        return r;
    }

    uint8_t data[6];
    for (int i = 0; i < 6; i++) data[i] = Wire.read();

    // Temp: bytes 0,1 (skip CRC byte 2)
    uint16_t rawTemp = (data[0] << 8) | data[1];
    r.value.tempC = -45.0f + 175.0f * (float)rawTemp / 65535.0f;

    // Humidity: bytes 3,4 (skip CRC byte 5)
    uint16_t rawHum = (data[3] << 8) | data[4];
    r.value.humidityPct = 100.0f * (float)rawHum / 65535.0f;

    return r;
}

// ===== LightSensor (BH1750) =================================================

namespace {
static constexpr uint8_t kLightReadAttempts = 2;
static constexpr uint32_t kLuxStaleHoldMs = 120000;
static constexpr uint16_t kLuxQuickReinitFailThreshold = 5;
static constexpr uint32_t kLuxQuickReinitCooldownMs = 5000;

struct WaterLevelFilterState {
    bool initialized = false;
    WaterLevelState rawState = WaterLevelState::UNKNOWN;
    WaterLevelState filteredState = WaterLevelState::UNKNOWN;
    bool rawHigh = false;
    bool rawValid = false;
    bool pendingTrip = false;
    bool pendingClear = false;
    uint32_t stableSinceMs = 0;
    uint32_t pendingSinceMs = 0;
};

static WaterLevelStatus buildWaterLevelStatus(const WaterLevelFilterState& state,
                                              bool activeLow) {
    WaterLevelStatus status;
    status.rawState = state.rawState;
    status.filteredState = state.filteredState;
    status.rawHigh = state.rawHigh;
    status.rawValid = state.rawValid;
    status.activeLow = activeLow;
    status.pendingTrip = state.pendingTrip;
    status.pendingClear = state.pendingClear;
    status.unstable = state.pendingTrip || state.pendingClear || !state.rawValid;
    status.stableSinceMs = state.stableSinceMs;
    status.pendingSinceMs = state.pendingSinceMs;
    return status;
}

static WaterLevelStatus updateWaterLevelFilter(WaterLevelFilterState& state,
                                               const ReadResult<bool>& rawLevel,
                                               const WaterLevelSensor& sensor,
                                               uint32_t tripDebounceMs,
                                               uint32_t clearDebounceMs,
                                               uint32_t nowMs) {
    if (!rawLevel.ok()) {
        state.initialized = true;
        state.rawValid = false;
        state.rawState = WaterLevelState::UNKNOWN;
        state.pendingTrip = false;
        state.pendingClear = false;
        state.pendingSinceMs = 0;
        if (state.filteredState != WaterLevelState::UNKNOWN) {
            state.filteredState = WaterLevelState::UNKNOWN;
            state.stableSinceMs = nowMs;
        }
        return buildWaterLevelStatus(state, sensor.activeLow());
    }

    WaterLevelState desiredState = sensor.normalizePhysicalLevel(rawLevel.value);
    state.rawHigh = rawLevel.value;
    state.rawValid = true;
    state.rawState = desiredState;

    if (!state.initialized || state.filteredState == WaterLevelState::UNKNOWN) {
        state.initialized = true;
        state.filteredState = desiredState;
        state.pendingTrip = false;
        state.pendingClear = false;
        state.pendingSinceMs = 0;
        state.stableSinceMs = nowMs;
        return buildWaterLevelStatus(state, sensor.activeLow());
    }

    if (desiredState == state.filteredState) {
        state.pendingTrip = false;
        state.pendingClear = false;
        state.pendingSinceMs = 0;
        return buildWaterLevelStatus(state, sensor.activeLow());
    }

    if (desiredState == WaterLevelState::TRIGGERED) {
        if (!state.pendingTrip) {
            state.pendingTrip = true;
            state.pendingClear = false;
            state.pendingSinceMs = nowMs;
        } else if ((nowMs - state.pendingSinceMs) >= tripDebounceMs) {
            state.filteredState = WaterLevelState::TRIGGERED;
            state.pendingTrip = false;
            state.pendingSinceMs = 0;
            state.stableSinceMs = nowMs;
        }
        return buildWaterLevelStatus(state, sensor.activeLow());
    }

    if (!state.pendingClear) {
        state.pendingClear = true;
        state.pendingTrip = false;
        state.pendingSinceMs = nowMs;
    } else if ((nowMs - state.pendingSinceMs) >= clearDebounceMs) {
        state.filteredState = WaterLevelState::OK;
        state.pendingClear = false;
        state.pendingSinceMs = 0;
        state.stableSinceMs = nowMs;
    }

    return buildWaterLevelStatus(state, sensor.activeLow());
}
}

bool LightSensor::init(uint8_t addr) {
    _addr = addr;
    // Power On
    Wire.beginTransmission(_addr);
    Wire.write(0x01);
    if (Wire.endTransmission() != 0) return false;

    // Continuous High Resolution Mode
    Wire.beginTransmission(_addr);
    Wire.write(0x10);
    return Wire.endTransmission() == 0;
}

ReadResult<float> LightSensor::readLux(uint32_t nowMs) {
    ReadResult<float> r;
    r.readAtMs = nowMs;

    for (uint8_t attempt = 0; attempt < kLightReadAttempts; ++attempt) {
        Wire.requestFrom(_addr, (uint8_t)2);
        if (Wire.available() >= 2) {
            uint8_t hi = Wire.read();
            uint8_t lo = Wire.read();
            uint16_t raw = (hi << 8) | lo;
            r.value = (float)raw / 1.2f;  // BH1750 formula
            return r;
        }

        while (Wire.available() > 0) {
            (void)Wire.read();
        }
        delayMicroseconds(150);
    }

    r.health = SensorHealth::FAIL;
    r.value = 0.0f;
    return r;
}

// ===== BarometerSensor (QMP6988) ============================================
// Datasheet: QMP6988 register map & compensation formulas
// Regs: 0xD1 = chip ID (should be 0x5C), 0xA0-0xB8 = OTP calibration, 
//       0xF1 = IIR filter, 0xF4 = ctrl_meas (osrs_t, osrs_p, power mode)
//       0xF7-0xF9 = pressure raw (24-bit), 0xFA-0xFC = temperature raw (24-bit)

// QMP6988 register defines
static constexpr uint8_t QMP_REG_CHIP_ID    = 0xD1;
static constexpr uint8_t QMP_REG_OTP_START  = 0xA0;
static constexpr uint8_t QMP_REG_CTRL_MEAS  = 0xF4;
static constexpr uint8_t QMP_REG_IIR        = 0xF1;
static constexpr uint8_t QMP_REG_DATA       = 0xF7;  // 6 bytes: P[23:0] T[23:0]
static constexpr uint8_t QMP_CHIP_ID_VAL    = 0x5C;

bool BarometerSensor::_writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool BarometerSensor::_readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(_addr, len);
    if (Wire.available() < len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool BarometerSensor::_readOtp() {
    // Read 25 bytes of OTP calibration from 0xA0..0xB8
    uint8_t otp[25];
    if (!_readRegs(QMP_REG_OTP_START, otp, 25)) return false;

    // Parse calibration integers (big-endian signed/unsigned from datasheet)
    // b00: otp[0..1] (16-bit signed) combined with otp[24] bits 3:0 → 20-bit
    int32_t b00_raw  = (int16_t)((otp[0] << 8) | otp[1]);
    int32_t bt1_raw  = (int16_t)((otp[2] << 8) | otp[3]);
    int32_t bt2_raw  = (int16_t)((otp[4] << 8) | otp[5]);
    int32_t bp1_raw  = (int16_t)((otp[6] << 8) | otp[7]);
    int32_t b11_raw  = (int16_t)((otp[8] << 8) | otp[9]);
    int32_t bp2_raw  = (int16_t)((otp[10] << 8) | otp[11]);
    int32_t b12_raw  = (int16_t)((otp[12] << 8) | otp[13]);
    int32_t b21_raw  = (int16_t)((otp[14] << 8) | otp[15]);
    int32_t bp3_raw  = (int16_t)((otp[16] << 8) | otp[17]);
    int32_t a0_raw   = (int16_t)((otp[18] << 8) | otp[19]);
    int32_t a1_raw   = (int16_t)((otp[20] << 8) | otp[21]);
    int32_t a2_raw   = (int16_t)((otp[22] << 8) | otp[23]);

    // Apply scaling factors from QMP6988 datasheet (Table in compensation section)
    _a0  = a0_raw  / 16.0f;
    _a1  = -6.30e-03f + 4.30e-04f * a1_raw / 32767.0f;
    _a2  = -1.90e-11f + 1.20e-10f * a2_raw / 32767.0f;

    // b00 extended with high nibble of otp[24]
    int32_t b00_ext = (b00_raw << 4) | ((otp[24] >> 4) & 0x0F);
    _b00 = b00_ext / 16.0f;   // effectively b00_raw + fractional from otp[24]

    _bt1 = 1.00e-01f + 9.10e-02f * bt1_raw / 32767.0f;
    _bt2 = 1.20e-08f + 1.20e-06f * bt2_raw / 32767.0f;
    _bp1 = 3.30e-02f + 1.90e-02f * bp1_raw / 32767.0f;
    _b11 = 2.10e-07f + 1.40e-07f * b11_raw / 32767.0f;
    _bp2 = -6.30e-10f + 3.50e-10f * bp2_raw / 32767.0f;
    _b12 = 2.90e-13f + 7.60e-13f * b12_raw / 32767.0f;
    _b21 = 2.10e-15f + 1.20e-14f * b21_raw / 32767.0f;
    _bp3 = 1.30e-16f + 7.90e-17f * bp3_raw / 32767.0f;

    return true;
}

bool BarometerSensor::init(uint8_t addr) {
    _addr = addr;
    _ready = false;

    // Verify chip ID
    uint8_t id = 0;
    if (!_readRegs(QMP_REG_CHIP_ID, &id, 1) || id != QMP_CHIP_ID_VAL) {
        Serial.printf("[BARO] QMP6988 ID mismatch: got 0x%02X expected 0x%02X\n", id, QMP_CHIP_ID_VAL);
        // Try alternate address 0x56
        if (_addr == 0x70) {
            _addr = 0x56;
            if (!_readRegs(QMP_REG_CHIP_ID, &id, 1) || id != QMP_CHIP_ID_VAL) {
                Serial.printf("[BARO] QMP6988 not found @0x70 or @0x56\n");
                _addr = addr;  // restore
                return false;
            }
            Serial.printf("[BARO] QMP6988 found @0x56 (not 0x70)\n");
        } else {
            return false;
        }
    }

    // Read OTP calibration coefficients
    if (!_readOtp()) {
        Serial.println("[BARO] OTP read failed");
        return false;
    }

    // IIR filter coefficient = 2 (register value 0x01)
    _writeReg(QMP_REG_IIR, 0x01);

    // ctrl_meas: osrs_t=010 (×2), osrs_p=100 (×16), mode=11 (normal)
    // = 0b010_100_11 = 0x53
    _writeReg(QMP_REG_CTRL_MEAS, 0x53);

    _ready = true;
    Serial.printf("[BARO] QMP6988 OK @0x%02X, OTP loaded\n", _addr);
    return true;
}

ReadResult<float> BarometerSensor::readPressureHpa(uint32_t nowMs) {
    ReadResult<float> r;
    r.readAtMs = nowMs;
    r.value = 0.0f;

    if (!_ready) {
        r.health = SensorHealth::FAIL;
        return r;
    }

    // Read 6 bytes: pressure (3) + temperature (3)
    uint8_t data[6];
    if (!_readRegs(QMP_REG_DATA, data, 6)) {
        r.health = SensorHealth::FAIL;
        return r;
    }

    // 24-bit unsigned raw values
    int32_t rawP = ((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | data[2];
    int32_t rawT = ((int32_t)data[3] << 16) | ((int32_t)data[4] << 8) | data[5];

    // Raw P=0 + T=0 means the sensor lost its register config (power glitch)
    // and is in sleep/standby mode.  Mark not-ready so reinit() can fix it.
    if (rawP == 0 && rawT == 0) {
        _ready = false;
        r.health = SensorHealth::FAIL;
        static uint32_t s_lastZeroLog = 0;
        if (nowMs - s_lastZeroLog >= 10000) {
            Serial.println("[BARO] raw P=0 T=0 — sensor in sleep mode, needs re-init");
            s_lastZeroLog = nowMs;
        }
        return r;
    }

    // Convert to signed: datasheet says subtract 2^23
    float Dt = (float)rawT - 8388608.0f;   // 2^23
    float Dp = (float)rawP - 8388608.0f;

    // Temperature compensation
    float Tr = _a0 + _a1 * Dt + _a2 * Dt * Dt;

    // Pressure compensation (using compensated temperature Tr)
    float Pa = _b00 + _bt1 * Tr + _bp1 * Dp
             + _b11 * Tr * Dp + _bt2 * Tr * Tr
             + _bp2 * Dp * Dp + _b12 * Dp * Tr * Tr
             + _b21 * Dp * Dp * Tr + _bp3 * Dp * Dp * Dp;

    // Pa is in Pa, convert to hPa
    r.value = Pa / 100.0f;

    // Sanity check: valid atmospheric pressure range 300-1100 hPa
    if (r.value < 300.0f || r.value > 1100.0f) {
        // Rate-limit: log max once per 10s to avoid serial spam
        static uint32_t s_lastOutOfRangeLog = 0;
        static uint16_t s_outOfRangeCount = 0;
        s_outOfRangeCount++;
        if (nowMs - s_lastOutOfRangeLog >= 10000) {
            Serial.printf("[BARO] out of range: %.1f hPa (raw P=%d T=%d) x%d in 10s\n",
                           r.value, rawP, rawT, s_outOfRangeCount);
            s_lastOutOfRangeLog = nowMs;
            s_outOfRangeCount = 0;
        }
        r.health = SensorHealth::OUT_OF_RANGE;
    }

    return r;
}

// ===== PumpActuator =========================================================

void PumpActuator::init(PbHubBus* bus, uint8_t channel, uint8_t pin) {
    _bus = bus;
    _channel = channel;
    _pin = pin;
    _isOn = false;
    // Upewnij się że pompa jest OFF na starcie
    if (_bus) _bus->digitalWrite(_channel, _pin, false);
}

bool PumpActuator::on(uint32_t nowMs, uint32_t plannedDurationMs) {
    if (_isOn) return true;
    if (!_bus) { Serial.println("[PUMP] _bus null!"); return false; }
    if (!_bus->digitalWrite(_channel, _pin, true)) {
        Serial.printf("[PUMP] CH%d pin%d ON failed!\n", _channel, _pin);
        return false;
    }
    _isOn = true;
    _onSinceMs = nowMs;
    _plannedMs = plannedDurationMs;
    Serial.printf("[PUMP] CH%d.pin%d ON planned=%dms\n", _channel, _pin, plannedDurationMs);
    return true;
}

bool PumpActuator::off(uint32_t nowMs, const char* reason) {
    if (!_isOn && !reason) return true;  // już OFF
    if (_bus) {
        if (!_bus->digitalWrite(_channel, _pin, false)) {
            Serial.printf("[PUMP] CH%d.pin%d OFF failed reason=%s\n",
                          _channel, _pin, reason ? reason : "none");
            return false;
        }
    }
    uint32_t dur = _isOn ? (nowMs - _onSinceMs) : 0;
    _isOn = false;
    if (!_isOn && reason && strcmp(reason, "FAILSAFE_IDLE_OFF") == 0 && dur == 0) {
        return true;
    }
    Serial.printf("[PUMP] CH%d.pin%d OFF reason=%s duration=%dms\n",
                  _channel, _pin, reason ? reason : "none", dur);
    return true;
}

bool PumpActuator::isOn() const { return _isOn; }

uint32_t PumpActuator::onDuration(uint32_t nowMs) const {
    if (!_isOn) return 0;
    return nowMs - _onSinceMs;
}

// ===== DualButton ===========================================================

void DualButton::init(PbHubBus* bus, uint8_t channel, uint8_t bluePin, uint8_t redPin) {
    _bus = bus;
    _channel = channel;
    _bluePin = bluePin;
    _redPin  = redPin;
}

DualButtonState DualButton::read(uint32_t nowMs) {
    DualButtonState s;
    if (!_bus) return s;

    auto blue = _bus->digitalRead(_channel, _bluePin);
    auto red  = _bus->digitalRead(_channel, _redPin);
    s.blueOk = blue.ok();
    s.redOk = red.ok();
    s.blueRawPressed = blue.ok() && !blue.value;
    s.redRawPressed = red.ok() && !red.value;

    auto updateStableState = [](bool ok,
                                bool rawPressed,
                                bool& lastRawPressed,
                                bool& stablePressed,
                                uint8_t& stableSamples,
                                bool& stable) {
        if (!ok) {
            stableSamples = 0;
            stablePressed = false;
            stable = false;
            return;
        }

        if (rawPressed == lastRawPressed) {
            if (stableSamples < DualButton::kStableSampleThreshold) {
                stableSamples++;
            }
        } else {
            lastRawPressed = rawPressed;
            stableSamples = 1;
        }

        stable = stableSamples >= DualButton::kStableSampleThreshold;
        if (stable) {
            stablePressed = rawPressed;
        }
    };

    updateStableState(s.blueOk, s.blueRawPressed,
                      _blueLastRawPressed, _blueStablePressed,
                      _blueStableSamples, s.blueStable);
    updateStableState(s.redOk, s.redRawPressed,
                      _redLastRawPressed, _redStablePressed,
                      _redStableSamples, s.redStable);

    s.bluePressed = s.blueStable ? _blueStablePressed : false;
    s.redPressed = s.redStable ? _redStablePressed : false;
    s.unstable = !s.blueOk || !s.redOk
              || (s.blueRawPressed != s.bluePressed)
              || (s.redRawPressed != s.redPressed);

    // Diagnostyka co 2s (nie spamuj co 10ms)
    static uint32_t lastDbg = 0;
    if (nowMs - lastDbg > 2000
        && (s.bluePressed || s.redPressed || s.unstable || !blue.ok() || !red.ok())) {
        lastDbg = nowMs;
        Serial.printf("[DBTN] CH%d blue: ok=%d raw=%d stable=%d pressed=%d | red: ok=%d raw=%d stable=%d pressed=%d unstable=%d\n",
                      _channel,
                      s.blueOk, s.blueRawPressed, s.blueStable, s.bluePressed,
                      s.redOk, s.redRawPressed, s.redStable, s.redPressed,
                      s.unstable);
    }
    return s;
}

// ===== HardwareManager ======================================================

// ===== I2C Bus Recovery =====================================================
// After a power glitch on Grove peripherals, a slave may hold SDA low
// (mid-transaction state). Clock SCL manually until SDA releases, then
// issue STOP, then re-init Wire.
void HardwareManager::i2cBusRecovery() {
    if (!_hwCfg) return;
    Serial.println("[HW] I2C bus recovery — clocking SCL to unstick SDA");

    Wire.end();

    pinMode(_hwCfg->i2cSdaPin, INPUT_PULLUP);
    pinMode(_hwCfg->i2cSclPin, OUTPUT);

    // Clock up to 16 pulses on SCL to free a stuck slave
    for (int i = 0; i < 16; i++) {
        digitalWrite(_hwCfg->i2cSclPin, LOW);
        delayMicroseconds(5);
        digitalWrite(_hwCfg->i2cSclPin, HIGH);
        delayMicroseconds(5);
        if (digitalRead(_hwCfg->i2cSdaPin)) {
            Serial.printf("[HW] SDA released after %d clocks\n", i + 1);
            break;
        }
    }

    // Generate STOP condition: SDA low → SCL high → SDA high
    pinMode(_hwCfg->i2cSdaPin, OUTPUT);
    digitalWrite(_hwCfg->i2cSdaPin, LOW);
    delayMicroseconds(5);
    digitalWrite(_hwCfg->i2cSclPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(_hwCfg->i2cSdaPin, HIGH);
    delayMicroseconds(5);

    Wire.begin(_hwCfg->i2cSdaPin, _hwCfg->i2cSclPin, _hwCfg->i2cFreq);
    Serial.println("[HW] I2C bus recovery complete, Wire re-initialized");
}

void HardwareManager::reinitI2cSensors() {
    if (!_hwCfg) return;
    Serial.println("[HW] Re-initializing I2C sensors after recovery");

    // PbHUB
    if (_pbhub.init(_hwCfg->addrPbHub, _hwCfg->pbhubDelayMs)) {
        Serial.printf("[HW] PbHUB re-init OK @0x%02X\n", _hwCfg->addrPbHub);
    } else {
        Serial.printf("[HW] PbHUB re-init FAILED @0x%02X\n", _hwCfg->addrPbHub);
    }

    // ENV (SHT30)
    if (_envSensor.init(_hwCfg->addrEnv)) {
        Serial.printf("[HW] SHT30 re-init OK @0x%02X\n", _hwCfg->addrEnv);
    } else {
        Serial.printf("[HW] SHT30 re-init FAILED @0x%02X\n", _hwCfg->addrEnv);
    }

    // Barometer (QMP6988) — needs OTP + register config
    if (_baroSensor.init(_hwCfg->addrBaro)) {
        Serial.printf("[HW] QMP6988 re-init OK @0x%02X\n", _hwCfg->addrBaro);
    } else {
        Serial.printf("[HW] QMP6988 re-init FAILED @0x%02X\n", _hwCfg->addrBaro);
    }

    // Light (BH1750) — needs power-on + mode register
    if (_lightSensor.init(_hwCfg->addrLight)) {
        Serial.printf("[HW] BH1750 re-init OK @0x%02X\n", _hwCfg->addrLight);
    } else {
        Serial.printf("[HW] BH1750 re-init FAILED @0x%02X\n", _hwCfg->addrLight);
    }
}

bool HardwareManager::init(const HardwareConfig& hw, const Config& cfg) {
    Serial.println("[HW] Initializing...");
    _hwCfg = &hw;   // store for recovery

    // I2C — SDA=G9, SCL=G10
    Wire.begin(hw.i2cSdaPin, hw.i2cSclPin, hw.i2cFreq);
    Serial.printf("[HW] I2C: SDA=G%d, SCL=G%d, %dHz\n",
                  hw.i2cSdaPin, hw.i2cSclPin, hw.i2cFreq);

    // PbHUB
    if (!_pbhub.init(hw.addrPbHub, hw.pbhubDelayMs)) {
        Serial.println("[HW] WARNING: PbHUB not responding!");
    } else {
        Serial.printf("[HW] PbHUB OK @0x%02X fw=%d\n", hw.addrPbHub, _pbhub.fwVersion());
    }

    // Warmup — odrzuć pierwsze odczyty
    for (uint8_t i = 0; i < hw.pbhubWarmupCycles; i++) {
        for (uint8_t ch = 0; ch < 6; ch++) _pbhub.analogRead(ch);
        delay(10);
    }

    // Per-pot sensory i pompy
    for (uint8_t i = 0; i < cfg.numPots; i++) {
        if (!cfg.pots[i].enabled) continue;
        const auto& ch = hw.potChannels[i];

        _soilSensors[i].init(&_pbhub, ch.soilAdcChannel);
        _overflowSensors[i].init(&_pbhub, ch.potMaxLevelChannel, 0, cfg.pots[i].potMaxActiveLow);
        _pumps[i].init(&_pbhub, ch.pumpOutputChannel, ch.pumpOutputPin);

        Serial.printf("[HW] Pot%d: ADC=CH%d.pin0(AOUT) pump=CH%d.pin%d(PUMP_EN) overflow=CH%d\n",
                      i, ch.soilAdcChannel,
                      ch.pumpOutputChannel, ch.pumpOutputPin,
                      ch.potMaxLevelChannel);
    }

    // Startup ADC diagnostic — read ADC (cmd 0x06, pin0 only) on each pot channel
    for (uint8_t i = 0; i < cfg.numPots; i++) {
        if (!cfg.pots[i].enabled) continue;
        const auto& ch = hw.potChannels[i];
        auto adc = _pbhub.analogRead(ch.soilAdcChannel);
        Serial.printf("[HW] DIAG Pot%d CH%d ADC(0x06)=%d%s\n",
                      i, ch.soilAdcChannel,
                      adc.value, adc.ok() ? "" : " FAIL");
        if (adc.ok() && adc.value > 100) {
            Serial.printf("[HW] DIAG Pot%d: moisture ADC looks valid (%d)\n",
                          i, adc.value);
        } else if (adc.ok()) {
            Serial.printf("[HW] DIAG Pot%d: ADC=%d (low/dry or sensor disconnected)\n",
                          i, adc.value);
        } else {
            Serial.printf("[HW] DIAG Pot%d: ADC read FAILED — PbHUB I2C error\n", i);
        }
    }

    // Reservoir sensor
    _reservoirSensor.init(&_pbhub, hw.reservoirMinChannel, 0, false);
    Serial.printf("[HW] Reservoir: CH%d\n", hw.reservoirMinChannel);

    // Dual Button
    _dualBtn.init(&_pbhub, hw.dualButtonChannel, hw.dualBtnBluePin, hw.dualBtnRedPin);
    Serial.printf("[HW] DualButton: CH%d (blue=pin%d, red=pin%d)\n",
                  hw.dualButtonChannel, hw.dualBtnBluePin, hw.dualBtnRedPin);

    // ENV III (SHT30 + QMP6988)
    if (_envSensor.init(hw.addrEnv)) {
        Serial.printf("[HW] SHT30 OK @0x%02X\n", hw.addrEnv);
    } else {
        Serial.printf("[HW] WARNING: SHT30 not responding @0x%02X\n", hw.addrEnv);
    }

    if (_baroSensor.init(hw.addrBaro)) {
        Serial.printf("[HW] QMP6988 OK @0x%02X\n", hw.addrBaro);
    } else {
        Serial.printf("[HW] WARNING: QMP6988 not responding @0x%02X\n", hw.addrBaro);
    }

    // BH1750
    if (_lightSensor.init(hw.addrLight)) {
        Serial.printf("[HW] BH1750 OK @0x%02X\n", hw.addrLight);
    } else {
        Serial.printf("[HW] WARNING: BH1750 not responding @0x%02X\n", hw.addrLight);
    }

    Serial.println("[HW] Init complete");
    return true;
}

void HardwareManager::initPot(uint8_t potIdx, const HardwareConfig& hw, const Config& cfg) {
    if (potIdx >= kMaxPots) return;
    if (!cfg.pots[potIdx].enabled) return;
    const auto& ch = hw.potChannels[potIdx];
    _soilSensors[potIdx].init(&_pbhub, ch.soilAdcChannel);
    _overflowSensors[potIdx].init(&_pbhub, ch.potMaxLevelChannel, 0, cfg.pots[potIdx].potMaxActiveLow);
    _pumps[potIdx].init(&_pbhub, ch.pumpOutputChannel, ch.pumpOutputPin);
    Serial.printf("[HW] initPot(%d): ADC=CH%d.pin0 pump=CH%d.pin%d overflow=CH%d\n",
                  potIdx, ch.soilAdcChannel,
                  ch.pumpOutputChannel, ch.pumpOutputPin,
                  ch.potMaxLevelChannel);
}

void HardwareManager::readAllSensors(uint32_t nowMs, const Config& cfg, SensorSnapshot& snap) {
    snap.timestampMs = nowMs;

    // Reservoir — wspólny
    auto resRawLevel = _reservoirSensor.readRawLevel(nowMs);

    // Throttled I2C failure logging (max every 10s)
    static uint32_t s_lastFailLogMs = 0;
    static uint16_t s_failCountSoil[kMaxPots] = {};
    static uint16_t s_invalidCountSoil[kMaxPots] = {};
    static uint16_t s_zeroCountSoil[kMaxPots] = {};
    static uint16_t s_lastValidSoilRaw[kMaxPots] = {};
    static bool s_hasValidSoilRaw[kMaxPots] = {};
    static uint16_t s_failCountOverflow[kMaxPots] = {};
    static uint16_t s_failCountEnv = 0;
    static uint16_t s_failCountLux = 0;
    static uint16_t s_failCountBaro = 0;
    static uint16_t s_failCountRes = 0;
    static WaterLevelFilterState s_overflowFilter[kMaxPots] = {};
    static WaterLevelFilterState s_reservoirFilter = {};
    static float s_lastValidLux = 0.0f;
    static bool s_hasValidLux = false;
    static uint32_t s_lastValidLuxMs = 0;
    static uint16_t s_consecLuxFails = 0;
    static uint32_t s_lastLuxReinitMs = 0;
    static bool s_luxRecovering = false;
    static uint32_t s_lastSoilWarnMs[kMaxPots] = {};
    static uint16_t s_soilMedianSamples[kMaxPots][kMoistureMedianWindow] = {};
    static uint8_t s_soilMedianCount[kMaxPots] = {};
    static uint8_t s_soilMedianHead[kMaxPots] = {};

    if (!resRawLevel.ok()) s_failCountRes++;
    WaterLevelStatus reservoirStatus = updateWaterLevelFilter(
        s_reservoirFilter,
        resRawLevel,
        _reservoirSensor,
        kWaterLevelTripDebounceMs,
        kReservoirClearDebounceMs,
        nowMs);

    // Per-pot
    for (uint8_t i = 0; i < cfg.numPots; i++) {
        if (!cfg.pots[i].enabled) continue;

        // Overflow sensor
        auto potMaxRawLevel = _overflowSensors[i].readRawLevel(nowMs);
        WaterLevelStatus potMaxStatus = updateWaterLevelFilter(
            s_overflowFilter[i],
            potMaxRawLevel,
            _overflowSensors[i],
            kWaterLevelTripDebounceMs,
            kPotOverflowClearDebounceMs,
            nowMs);
        snap.pots[i].waterGuards.potMax = potMaxStatus.filteredState;
        snap.pots[i].waterGuards.potMaxStatus = potMaxStatus;
        if (!potMaxRawLevel.ok()) s_failCountOverflow[i]++;
        snap.pots[i].waterGuards.reservoirMin = reservoirStatus.filteredState;
        snap.pots[i].waterGuards.reservoirMinStatus = reservoirStatus;

        // Moisture ADC
        auto rawResult = _soilSensors[i].readRaw(nowMs);
        if (rawResult.ok() && rawResult.value > 0 && rawResult.value <= kMaxPbHubAdcRaw) {
            snap.pots[i].moistureRaw = rawResult.value;
            s_lastValidSoilRaw[i] = rawResult.value;
            s_hasValidSoilRaw[i] = true;
        } else if (rawResult.ok() && rawResult.value > kMaxPbHubAdcRaw) {
            s_invalidCountSoil[i]++;
            snap.pots[i].moistureRaw = s_hasValidSoilRaw[i]
                ? s_lastValidSoilRaw[i]
                : kMoistureFallbackDryRaw;
            if ((nowMs - s_lastSoilWarnMs[i]) >= 10000) {
                s_lastSoilWarnMs[i] = nowMs;
                Serial.printf("[POT%d] MOISTURE_RAW_INVALID raw=%u fallback_raw=%d\n",
                              i, rawResult.value, snap.pots[i].moistureRaw);
            }
        } else if (!rawResult.ok()) {
            // Preserve last valid value to avoid false 100% from raw=0 fallback
            s_failCountSoil[i]++;
            if ((nowMs - s_lastSoilWarnMs[i]) >= 10000) {
                s_lastSoilWarnMs[i] = nowMs;
                Serial.printf("[POT%d] MOISTURE_READ_FAIL using last raw=%d\n",
                              i, snap.pots[i].moistureRaw);
            }
        } else {
            // raw==0 with successful I2C read is usually disconnected/wrong channel.
            // Keep last good sample if available, otherwise assume dry baseline.
            s_zeroCountSoil[i]++;
            if (s_hasValidSoilRaw[i]) {
                snap.pots[i].moistureRaw = s_lastValidSoilRaw[i];
            } else {
                snap.pots[i].moistureRaw = kMoistureFallbackDryRaw;
            }
            if ((nowMs - s_lastSoilWarnMs[i]) >= 10000) {
                s_lastSoilWarnMs[i] = nowMs;
                Serial.printf("[POT%d] MOISTURE_RAW_ZERO (wire/channel?) fallback_raw=%d\n",
                              i, snap.pots[i].moistureRaw);
            }
        }

        // Safety net: never allow zero raw into normalization path
        // (raw=0 produced false 100% moisture with dry/wet defaults).
        if (snap.pots[i].moistureRaw == 0) {
            snap.pots[i].moistureRaw = s_hasValidSoilRaw[i]
                ? s_lastValidSoilRaw[i]
                : kMoistureFallbackDryRaw;
        }

        s_soilMedianSamples[i][s_soilMedianHead[i]] = snap.pots[i].moistureRaw;
        s_soilMedianHead[i] = (s_soilMedianHead[i] + 1) % kMoistureMedianWindow;
        if (s_soilMedianCount[i] < kMoistureMedianWindow) {
            s_soilMedianCount[i]++;
        }

        uint16_t filteredMoistureRaw = medianOfUint16(s_soilMedianSamples[i], s_soilMedianCount[i]);
        snap.pots[i].moistureRawFiltered = filteredMoistureRaw;

        // Median on raw ADC rejects short spikes; both fast control and the later
        // EMA path stay aligned because percent is still derived from RAWf.
        snap.pots[i].moisturePct = normalizeMoistureRaw(filteredMoistureRaw,
                                cfg.pots[i].moistureDryRaw,
                                cfg.pots[i].moistureWetRaw,
                                cfg.pots[i].moistureCurveExponent);

        // Mapping diagnostic for Watering Unit on PbHUB channel:
        // M5Stack Watering Unit U101: pin0=AOUT, pin1=PUMP_EN
        // If raw stays at fallback for long, print a clear hint.
        static uint16_t s_rawFallbackCount[kMaxPots] = {};
        if (!rawResult.ok() || rawResult.value == 0 || rawResult.value > kMaxPbHubAdcRaw) {
            s_rawFallbackCount[i]++;
        } else {
            s_rawFallbackCount[i] = 0;
        }
        if (s_rawFallbackCount[i] == 50) {  // ~5s at 100ms tick
            Serial.printf("[POT%d] MOISTURE_STUCK_FALLBACK: no valid ADC — check Grove cable on PbHUB CH%d\n",
                          i, _soilSensors[i].channel());
        }
    }

    // ENV
    auto env = _envSensor.readEnv(nowMs);
    if (env.ok()) {
        snap.env.tempC = env.value.tempC;
        snap.env.humidityPct = env.value.humidityPct;
    } else {
        s_failCountEnv++;
    }

    auto lux = _lightSensor.readLux(nowMs);
    if (lux.ok()) {
        if (s_luxRecovering || s_consecLuxFails > 0) {
            Serial.printf("[HW] event=light_recovered lux=%.0f fail_streak=%u\n",
                          lux.value, s_consecLuxFails);
        }
        snap.env.lux = lux.value;
        snap.env.lightState = LightSignalState::VALID;
        snap.env.luxAgeMs = 0;
        s_lastValidLux = lux.value;
        s_lastValidLuxMs = nowMs;
        s_hasValidLux = true;
        s_consecLuxFails = 0;
        s_luxRecovering = false;
    } else {
        s_failCountLux++;
        s_consecLuxFails++;

        uint32_t luxAgeMs = (s_hasValidLux && nowMs >= s_lastValidLuxMs)
            ? (nowMs - s_lastValidLuxMs)
            : 0;
        bool reuseLastLux = s_hasValidLux && luxAgeMs <= kLuxStaleHoldMs;

        snap.env.lux = reuseLastLux ? s_lastValidLux : 0.0f;
        snap.env.luxAgeMs = reuseLastLux ? luxAgeMs : 0;
        if (s_luxRecovering) {
            snap.env.lightState = LightSignalState::RECOVERING;
        } else if (reuseLastLux) {
            snap.env.lightState = LightSignalState::STALE;
        } else {
            snap.env.lightState = LightSignalState::UNKNOWN;
        }

        if (_hwCfg
            && s_consecLuxFails >= kLuxQuickReinitFailThreshold
            && (nowMs - s_lastLuxReinitMs) >= kLuxQuickReinitCooldownMs) {
            bool reinitOk = _lightSensor.init(_hwCfg->addrLight);
            s_lastLuxReinitMs = nowMs;
            s_luxRecovering = true;
            snap.env.lightState = LightSignalState::RECOVERING;
            Serial.printf("[HW] event=light_reinit result=%s fail_streak=%u\n",
                          reinitOk ? "ok" : "fail",
                          s_consecLuxFails);
        }
    }

    auto baro = _baroSensor.readPressureHpa(nowMs);
    if (baro.ok()) snap.env.pressureHpa = baro.value;
    else s_failCountBaro++;

    // --- Auto I2C recovery on sustained failures ---
    // Track consecutive failures for direct-I2C sensors (env, baro, lux).
    // After 30 consecutive failures (~3s at 100ms tick) attempt bus recovery
    // + sensor re-init. Cooldown 30s between attempts.
    static uint16_t s_consecI2cFails = 0;
    static uint32_t s_lastRecoveryMs = 0;
    bool anyDirectI2cFail = !env.ok() || !baro.ok() || !lux.ok();
    bool allDirectI2cFail = !env.ok() && !baro.ok() && !lux.ok();

    if (anyDirectI2cFail) {
        s_consecI2cFails++;
    } else {
        s_consecI2cFails = 0;
    }

    if (s_consecI2cFails >= 30 && (nowMs - s_lastRecoveryMs >= 30000)) {
        Serial.printf("[HW] event=i2c_recovery_trigger consec_fails=%u\n",
                  s_consecI2cFails);
        i2cBusRecovery();
        reinitI2cSensors();
        s_lastRecoveryMs = nowMs;
        s_consecI2cFails = 0;
    }
    // Also reinit individual sensors that are failing while others work
    // (e.g. only baro stuck after power glitch, env+lux OK)
    else if (s_consecI2cFails >= 20 && !allDirectI2cFail
             && (nowMs - s_lastRecoveryMs >= 30000)) {
        Serial.println("[HW] event=i2c_partial_failure action=reinit_sensors");
        reinitI2cSensors();
        s_lastRecoveryMs = nowMs;
        s_consecI2cFails = 0;
    }

    // --- Periodic I2C fail summary (every 10s) ---
    if (nowMs - s_lastFailLogMs >= 10000) {
        bool anyFail = s_failCountRes > 0 || s_failCountEnv > 0
                    || s_failCountLux > 0 || s_failCountBaro > 0;
        for (uint8_t fi = 0; fi < kMaxPots && !anyFail; ++fi) {
            if (s_failCountSoil[fi] > 0 || s_failCountOverflow[fi] > 0)
                anyFail = true;
        }
        if (anyFail) {
            char summary[256];
            int pos = snprintf(summary, sizeof(summary),
                               "[HW] event=i2c_fail_summary window_s=10 env=%u lux=%u baro=%u res=%u",
                               s_failCountEnv, s_failCountLux, s_failCountBaro, s_failCountRes);
            for (uint8_t fi = 0; fi < cfg.numPots; ++fi) {
                if (pos > 0 && pos < static_cast<int>(sizeof(summary))) {
                    pos += snprintf(summary + pos, sizeof(summary) - pos,
                                    " soil%u=%u soil%u_invalid=%u soil%u_zero=%u ovf%u=%u",
                                    fi, s_failCountSoil[fi], fi, s_invalidCountSoil[fi], fi, s_zeroCountSoil[fi],
                                    fi, s_failCountOverflow[fi]);
                }
            }
            Serial.printf("%s\n", summary);
        }
        s_failCountRes = s_failCountEnv = s_failCountLux = s_failCountBaro = 0;
        for (uint8_t fi = 0; fi < kMaxPots; ++fi) {
            s_failCountSoil[fi] = s_failCountOverflow[fi] = 0;
            s_invalidCountSoil[fi] = 0;
            s_zeroCountSoil[fi] = 0;
        }
        s_lastFailLogMs = nowMs;
    }
}
