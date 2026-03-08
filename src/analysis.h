// ============================================================================
// analysis.h — EMA, trend, dusk detector, solar clock, sensor history
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Filtracja odczytów — EMA",
//                "Analiza trendów", "Detektor zmierzchu/świtu — fuzja sensorowa",
//                "Estymacja zegara słonecznego", "Historia pomiarów"
// Architektura:  docs/ARCHITECTURE.md
// ============================================================================
#pragma once

#include <cstdint>
#include <cmath>
#include "config.h"

// ---------------------------------------------------------------------------
// EmaFilter — Exponential Moving Average
// PLAN.md → "Filtracja odczytów — EMA"
// ---------------------------------------------------------------------------
struct EmaFilter {
    float    alpha         = 0.1f;   // 0..1, mniejsze = stabilniejsze
    float    value         = NAN;
    bool     initialized  = false;
    uint32_t lastUpdateMs = 0;

    // Reinicjalizuj jeśli gap > maxGapMs
    static constexpr uint32_t kMaxGapMs = 60000;

    float update(float sample, uint32_t nowMs);
    void  reset();
};

// ---------------------------------------------------------------------------
// TrendState — analiza schoodowości gleby per-pot (ring buffer godzinowy)
// PLAN.md → "Analiza trendów"
// ---------------------------------------------------------------------------
struct TrendState {
    static constexpr uint8_t kHours = 24;

    float    hourlyDeltas[kHours] = {};
    uint8_t  headIdx              = 0;
    uint8_t  count                = 0;       // 0..24
    float    lastMoisturePct      = NAN;
    uint32_t lastSampleMs         = 0;

    float    normalDryingRate     = NAN;     // wyuczona bazowa (%/h, ujemna)
    bool     baselineCalibrated   = false;
};

// TrendState per-pot — globalne tablice
extern TrendState g_trendStates[kMaxPots];

// tick — wywoływać co 1 s (wewnętrznie odpala co 1h)
void trendTick(uint32_t nowMs, float moisturePct, TrendState& ts, const Config& cfg);

// Weak overrides wywoływane z watering.cpp
bool  trendBaselineLearned(uint8_t potIdx);
float trendCurrentRate(uint8_t potIdx);

// ---------------------------------------------------------------------------
// DuskDetector — fuzja sensorowa (BH1750 + SHT30 + QMP6988)
// PLAN.md → "Detektor zmierzchu/świtu"
// ---------------------------------------------------------------------------

// Fazy detektora
enum class DuskPhase : uint8_t {
    NIGHT,
    DAWN_TRANSITION,
    DAY,
    DUSK_TRANSITION,
};

// Próbka okna pomiarowego
struct EnvSample {
    uint32_t ms;
    float    lux;
    float    tempC;
    float    humPct;
    float    pressHpa;
};

// Pochodne czasowe
struct EnvDerivatives {
    float dLux_dt;     // lux / min
    float dTemp_dt;    // °C / min
    float dHum_dt;     // %RH / min
    float dPress_dt;   // hPa / min
};

// Scoring components
struct DuskScores {
    float light;
    float lightRate;
    float temp;
    float humidity;
    float pressure;
};

// Stan głównego detektora
struct DuskDetector {
    DuskPhase phase = DuskPhase::NIGHT;   // bezpieczny default

    // Okno pomiarowe (ring buffer)
    static constexpr uint8_t kWindowSize = 60;  // 60 próbek × 30s = 30 min
    EnvSample window[kWindowSize]  = {};
    uint8_t   head                 = 0;
    uint8_t   count                = 0;

    // Timestampy
    uint32_t transitionStartMs     = 0;
    uint32_t lastDawnMs            = 0;
    uint32_t lastDuskMs            = 0;
    uint32_t dayLengthMs           = 0;
    uint32_t nightLengthMs         = 0;

    // Scoring (do logowania / UI)
    float    duskScore             = 0.0f;
    float    dawnScore             = 0.0f;

    // Stałe — mapowane z Config w runtime
    static constexpr uint32_t kEnvSampleIntervalMs      = 30000;  // 30 s
    static constexpr uint32_t kMinDayDurationMs          = 4UL * 3600 * 1000;
    static constexpr uint32_t kMinNightDurationMs        = 3UL * 3600 * 1000;
    static constexpr uint32_t kTransitionMaxDurationMs   = 120UL * 60 * 1000;
};

// Tick — wywoływany co ~1s z ControlTask
void duskDetectorTick(uint32_t nowMs, float lux, float tempC,
                      float humPct, float pressHpa,
                      DuskDetector& det, const Config& cfg);

// ---------------------------------------------------------------------------
// DuskState — persisted to NVS across reboots
// ---------------------------------------------------------------------------
struct DuskState {
    uint8_t  phase          = 0;   // DuskPhase as uint8
    uint32_t dayLengthMs    = 0;   // estimated day length from SolarClock
    uint32_t nightLengthMs  = 0;   // estimated night length
    uint8_t  _pad[3]        = {};  // alignment
};

bool duskStateSave(const DuskDetector& det);    // save current phase + estimates to NVS
bool duskStateLoad(DuskDetector& det);           // restore phase + estimates from NVS
void duskBootstrap(DuskDetector& det, float lux); // instant phase from first lux reading

// Helpery
DuskScores scoreDusk(float lux, const EnvDerivatives& d);
DuskScores scoreDawn(float lux, const EnvDerivatives& d);
EnvDerivatives computeDerivatives(const EnvSample* window, uint8_t count,
                                  uint8_t head, uint8_t windowSize,
                                  uint8_t lookbackSamples = 10);

// ---------------------------------------------------------------------------
// SolarClock — estymowany zegar (bez RTC/NTP)
// PLAN.md → "Estymacja zegara słonecznego"
// ---------------------------------------------------------------------------
struct SolarClock {
    uint32_t lastDawnMs       = 0;
    uint32_t lastDuskMs       = 0;
    uint32_t dayLengthMs      = 0;
    uint32_t nightLengthMs    = 0;
    uint32_t estimatedDayMs   = 0;
    bool     calibrated       = false;
    uint8_t  cycleCount       = 0;
};

void updateSolarClock(const DuskDetector& det, SolarClock& clk);
uint32_t estimateNextDawn(const DuskDetector& det, const SolarClock& clk, uint32_t nowMs);
uint32_t estimateNextDusk(const DuskDetector& det, const SolarClock& clk, uint32_t nowMs);

// ---------------------------------------------------------------------------
// SensorHistory — ring buffers (RAM + NVS)
// PLAN.md → "Historia pomiarów — ring buffer z kompresją"
// ---------------------------------------------------------------------------
struct SensorSample {
    uint32_t timestampMs;
    uint16_t moistureRaw;
    int16_t  tempC_x10;       // np. 23.5°C = 235
    uint16_t lux;             // clamp do 65535
    uint8_t  flags;           // bit0=reservoirLow, bit1=overflow, bit2=pumpOn, ...
    uint8_t  _pad;
};  // 12 bytes

struct WateringRecord {
    uint32_t timestampMs;
    uint8_t  potIndex;
    uint8_t  pulseCount;
    uint16_t totalPumpedMl_x10;   // ml × 10
    uint16_t moistureBefore_x10;
    uint16_t moistureAfter_x10;
    uint8_t  reason;              // WateringFeedbackCode enum
    uint8_t  _pad;
};  // 12 bytes

// Ring buffer template
template<typename T, uint16_t MAX_SIZE>
class RingBuffer {
public:
    void     push(const T& item);
    const T& at(uint16_t idx) const;   // 0 = oldest
    uint16_t size() const { return _count; }
    bool     empty() const { return _count == 0; }
    void     clear();

private:
    T        _buf[MAX_SIZE];
    uint16_t _head  = 0;
    uint16_t _count = 0;
};

// History store singleton
struct SensorHistory {
    // Level 1: RAM — co 10s, ostatnie 30 min (180 rekordów)
    RingBuffer<SensorSample, 180>  level1;

    // Level 2: RAM/NVS — co 5 min, ostatnie 24h (288 rekordów)
    RingBuffer<SensorSample, 288>  level2;

    // Level 3: NVS — co 1h, ostatnie 30 dni (720 rekordów)
    RingBuffer<SensorSample, 720>  level3;

    // Watering records
    RingBuffer<WateringRecord, 100> wateringLog;

    // Timestamps ostatnich flush
    uint32_t lastLevel1AddMs  = 0;
    uint32_t lastLevel2FlushMs = 0;
    uint32_t lastLevel3FlushMs = 0;
};

// Tick — dodaj sample i wykonaj downsampling / NVS flush
void historyTick(uint32_t nowMs, const SensorSample& sample, SensorHistory& hist);

// Dodaj rekord podlewania
void historyAddWatering(SensorHistory& hist, const WateringRecord& rec);

// Analiza dziennego zużycia (sum all pots, last 24h)
float historyCalcDailyConsumption(const SensorHistory& hist, uint32_t nowMs);
