// ============================================================================
// hardware.h — Warstwa hardware: PbHUB, sensory, aktuatory
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Warstwa hardware — dekompozycja abstrakcji",
//                "PbHUB — sensor bus abstraction",
//                "Wspólny kontrakt driverów", "Czujniki — kontrakty",
//                "Aktywatory — kontrakty", "Odkrycia empiryczne — PbHUB v1.1"
// Architektura:  docs/ARCHITECTURE.md
//
// UWAGA: PbHUB v1.1 I2C: SDA=G9, SCL=G10, 100kHz, @0x61
// ============================================================================
#pragma once

#include <cstdint>
#include "config.h"

// ---------------------------------------------------------------------------
// ReadResult — ujednolicony wynik odczytu (health tracking)
// PLAN.md → "Wspólny kontrakt driverów"
// ---------------------------------------------------------------------------
enum class SensorHealth : uint8_t {
    OK,
    FAIL,        // brak odpowiedzi I2C / timeout
    OUT_OF_RANGE // wartość poza rozsądnym zakresem
};

template<typename T>
struct ReadResult {
    T            value;
    SensorHealth health = SensorHealth::OK;
    uint32_t     readAtMs = 0;

    bool ok() const { return health == SensorHealth::OK; }
};

// ---------------------------------------------------------------------------
// WaterLevelState — stan czujnika poziomu wody (domenowy)
// ---------------------------------------------------------------------------
enum class WaterLevelState : uint8_t {
    OK,          // woda obecna (brak problemu)
    TRIGGERED,   // brak wody / overflow
    UNKNOWN,     // sensor fault
};

// ---------------------------------------------------------------------------
// Sensory — wyniki odczytów
// ---------------------------------------------------------------------------
struct EnvReading {
    float tempC;
    float humidityPct;
};

struct WaterGuards {
    WaterLevelState potMax;        // overflow per-pot
    WaterLevelState reservoirMin;  // reservoir level (wspólny)
};

struct PotSensorSnapshot {
    float    moisturePct;        // znormalizowany 0-100%
    uint16_t moistureRaw;        // surowy ADC
    uint16_t moistureRawFiltered; // przefiltrowany raw do UI/sterowania
    float    moistureEma;        // po EMA filter
    WaterGuards waterGuards;
};

struct EnvSnapshot {
    float    tempC;
    float    humidityPct;
    float    lux;
    float    pressureHpa;
};

struct SensorSnapshot {
    PotSensorSnapshot pots[kMaxPots];
    EnvSnapshot       env;
    uint32_t          timestampMs;
};

// Normalize M5 Watering Unit raw ADC to moisture percent using the current
// installation-specific two-point nonlinear model with per-pot RAWf endpoints
// and per-pot curve exponent.
float normalizeMoistureRaw(uint16_t raw, uint16_t dryRaw, uint16_t wetRaw,
                           float curveExponent);

// ---------------------------------------------------------------------------
// PbHubBus — jedyny punkt dostępu do PbHUB v1.1
// PLAN.md → "PbHUB — sensor bus abstraction"
// ---------------------------------------------------------------------------
class PbHubBus {
public:
    bool     init(uint8_t i2cAddr, uint8_t delayMs);
    bool     probePresent();
    uint8_t  fwVersion();

    // Odczyty — per pbhub_v1.1_firmware_reference.md
    ReadResult<uint16_t> analogRead(uint8_t channel);           // cmd 0x06: ADC 12-bit, pin0 only
    ReadResult<bool>     digitalRead(uint8_t channel, uint8_t pin = 0);  // cmd 0x04/0x05

    // Sterowanie
    bool     digitalWrite(uint8_t channel, uint8_t pin, bool level);

private:
    uint8_t  _addr = 0x61;
    uint8_t  _delayMs = 10;

    // Kody komend PbHUB v1.1 (PLAN.md → "Odkrycia empiryczne")
    static constexpr uint8_t kChBase[6] = {0x40, 0x50, 0x60, 0x70, 0x80, 0xA0};
    uint8_t cmdRead(uint8_t ch, uint8_t pin) const;
    uint8_t cmdWrite(uint8_t ch, uint8_t pin) const;
    bool    i2cWrite(uint8_t cmd, uint8_t data);
    bool    i2cRead16(uint8_t cmd, uint16_t& out);
    bool    i2cRead8(uint8_t cmd, uint8_t& out);
};

// ---------------------------------------------------------------------------
// SoilMoistureSensor — odczyt wilgotności gleby przez PbHUB
// ---------------------------------------------------------------------------
class SoilMoistureSensor {
public:
    void init(PbHubBus* bus, uint8_t channel);
    ReadResult<uint16_t> readRaw(uint32_t nowMs);
    bool isReady() const { return _bus != nullptr; }
    uint8_t channel() const { return _channel; }

private:
    PbHubBus* _bus = nullptr;
    uint8_t   _channel = 0;
    // PbHUB ADC (cmd 0x06) always reads pin0 — no pin selection needed
};

// ---------------------------------------------------------------------------
// WaterLevelSensor — cyfrowy czujnik poziomu (overflow / reservoir)
// ---------------------------------------------------------------------------
class WaterLevelSensor {
public:
    void init(PbHubBus* bus, uint8_t channel, uint8_t pin, bool activeLow);
    ReadResult<WaterLevelState> readState(uint32_t nowMs);

private:
    PbHubBus* _bus = nullptr;
    uint8_t   _channel = 0;
    uint8_t   _pin = 0;
    bool      _activeLow = true;
};

// ---------------------------------------------------------------------------
// EnvSensor — SHT30 (temp + humidity) via I2C
// ---------------------------------------------------------------------------
class EnvSensor {
public:
    bool init(uint8_t addr = 0x44);
    ReadResult<EnvReading> readEnv(uint32_t nowMs);

private:
    uint8_t _addr = 0x44;
};

// ---------------------------------------------------------------------------
// LightSensor — BH1750 via I2C
// ---------------------------------------------------------------------------
class LightSensor {
public:
    bool init(uint8_t addr = 0x23);
    ReadResult<float> readLux(uint32_t nowMs);

private:
    uint8_t _addr = 0x23;
};

// ---------------------------------------------------------------------------
// BarometerSensor — QMP6988 via I2C (pełna implementacja z kalibracją OTP)
// ---------------------------------------------------------------------------
class BarometerSensor {
public:
    bool init(uint8_t addr = 0x70);
    ReadResult<float> readPressureHpa(uint32_t nowMs);

private:
    uint8_t _addr = 0x70;
    bool    _ready = false;

    // QMP6988 OTP calibration coefficients
    float _a0, _a1, _a2;
    float _b00, _bt1, _bt2, _bp1;
    float _b11, _bp2, _b12, _b21, _bp3;

    bool _readOtp();
    bool _writeReg(uint8_t reg, uint8_t val);
    bool _readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
};

// ---------------------------------------------------------------------------
// PumpActuator — sterowanie pompą przez PbHUB
// PLAN.md → "Aktywatory — kontrakty domenowe"
// ---------------------------------------------------------------------------
class PumpActuator {
public:
    void init(PbHubBus* bus, uint8_t channel, uint8_t pin = 1);  // Watering Unit: pin1 = PUMP_EN

    bool on(uint32_t nowMs, uint32_t plannedDurationMs);
    bool off(uint32_t nowMs, const char* reason);
    bool isOn() const;

    uint32_t onSinceMs()  const { return _onSinceMs; }
    uint32_t onDuration(uint32_t nowMs) const;

private:
    PbHubBus* _bus = nullptr;
    uint8_t   _channel = 0;
    uint8_t   _pin = 1;        // Watering Unit: pin1 = PUMP_EN
    bool      _isOn = false;
    uint32_t  _onSinceMs = 0;
    uint32_t  _plannedMs = 0;
};

// ---------------------------------------------------------------------------
// DualButton — odczyt Dual Button na CH5 PbHUB (IN/IN mode)
// ---------------------------------------------------------------------------
struct DualButtonState {
    bool bluePressed = false;      // stabilized press state
    bool redPressed = false;       // stabilized press state
    bool blueRawPressed = false;   // single-sample raw state
    bool redRawPressed = false;    // single-sample raw state
    bool blueStable = false;       // enough consecutive samples collected
    bool redStable = false;        // enough consecutive samples collected
    bool blueOk = false;           // transport-level read success
    bool redOk = false;            // transport-level read success
    bool unstable = false;         // any line is unstable / failed
};

class DualButton {
public:
    void init(PbHubBus* bus, uint8_t channel, uint8_t bluePin = 0, uint8_t redPin = 1);
    DualButtonState read(uint32_t nowMs);

private:
    PbHubBus* _bus = nullptr;
    uint8_t   _channel = 5;
    uint8_t   _bluePin = 0;
    uint8_t   _redPin  = 1;
    bool      _blueLastRawPressed = false;
    bool      _redLastRawPressed = false;
    bool      _blueStablePressed = false;
    bool      _redStablePressed = false;
    uint8_t   _blueStableSamples = 0;
    uint8_t   _redStableSamples = 0;

    static constexpr uint8_t kStableSampleThreshold = 4;
};

// ---------------------------------------------------------------------------
// HardwareManager — inicjalizacja i dostęp do wszystkich driverów
// ---------------------------------------------------------------------------
class HardwareManager {
public:
    bool init(const HardwareConfig& hwCfg, const Config& cfg);
    void initPot(uint8_t potIdx, const HardwareConfig& hw, const Config& cfg);

    // I2C bus recovery & sensor re-init (call after power glitch / persistent failures)
    void i2cBusRecovery();
    void reinitI2cSensors();

    // Dostęp do driverów
    PbHubBus&          pbhub()             { return _pbhub; }
    SoilMoistureSensor& soilSensor(uint8_t potIdx) { return _soilSensors[potIdx]; }
    WaterLevelSensor&  overflowSensor(uint8_t potIdx) { return _overflowSensors[potIdx]; }
    WaterLevelSensor&  reservoirSensor()   { return _reservoirSensor; }
    EnvSensor&         envSensor()         { return _envSensor; }
    LightSensor&       lightSensor()       { return _lightSensor; }
    BarometerSensor&   baroSensor()        { return _baroSensor; }
    PumpActuator&      pump(uint8_t potIdx) { return _pumps[potIdx]; }
    DualButton&        dualButton()        { return _dualBtn; }

    // Odczyt całego snapshotu (wywoływany z ControlTask)
    void readAllSensors(uint32_t nowMs, const Config& cfg, SensorSnapshot& snap);

private:
    const HardwareConfig* _hwCfg = nullptr;  // stored at init for recovery
    PbHubBus           _pbhub;
    SoilMoistureSensor _soilSensors[kMaxPots];
    WaterLevelSensor   _overflowSensors[kMaxPots];
    WaterLevelSensor   _reservoirSensor;
    EnvSensor          _envSensor;
    LightSensor        _lightSensor;
    BarometerSensor    _baroSensor;
    PumpActuator       _pumps[kMaxPots];
    DualButton         _dualBtn;
};
