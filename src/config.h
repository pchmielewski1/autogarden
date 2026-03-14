// ============================================================================
// config.h — Struktury konfiguracji, walidacja, NVS persist
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Schemat danych (pseudostruktura)",
//                "Zaktualizowana struktura Config (pełna)",
//                "Walidacja (kontrakt)", "Wersjonowanie i migracje",
//                "Zapis asynchroniczny", "Bezpieczne defaulty"
// Architektura:  docs/ARCHITECTURE.md
// ============================================================================
#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Stałe globalne
// ---------------------------------------------------------------------------
static constexpr uint8_t  kMaxPots      = 2;
static constexpr uint8_t  kNumProfiles  = 6;  // Pomidor..Custom
static constexpr uint16_t kConfigSchema = 2;  // wersja schematu NVS

// NVS namespace names
static constexpr const char* kNvsConfig  = "ag_config";
static constexpr const char* kNvsNet     = "ag_net";
static constexpr const char* kNvsHist    = "ag_hist";
static constexpr const char* kNvsDusk    = "ag_dusk";
static constexpr const char* kNvsRuntime = "ag_runtime";

static constexpr uint16_t kNetConfigSchema = 2;
static constexpr uint16_t kRuntimeSchema = 3;  // bump on struct change

// ---------------------------------------------------------------------------
// Profil rośliny (PlantProfile) — PLAN.md → "Profile roślin"
// ---------------------------------------------------------------------------
struct PlantProfile {
    const char* name;
    const char* icon;
    float  targetMoisturePct;
    float  criticalLowPct;
    float  maxMoisturePct;
    float  hysteresisPct;
    uint32_t soakTimeMs;
    uint16_t pulseWaterMl;
    uint8_t  maxPulsesPerCycle;
};

// Tablica profili — definicja w config.cpp
extern const PlantProfile kProfiles[kNumProfiles];

// ---------------------------------------------------------------------------
// Kalibracja gleby per-pot
// ---------------------------------------------------------------------------
struct SoilCalib {
    uint16_t rawDry  = 3000;   // odczyt ADC w powietrzu (sucho)
    uint16_t rawWet  = 1200;   // odczyt ADC w mokrej ziemi
};

// ---------------------------------------------------------------------------
// PotConfig — per-pot (PLAN.md → "PotConfig")
// ---------------------------------------------------------------------------
struct PotConfig {
    bool     enabled            = false;
    uint8_t  plantProfileIndex  = 0;       // 0=Pomidor, ..., 5=Custom

    // Custom override (gdy plantProfileIndex == 5/CUSTOM)
    float    customTargetPct         = 60.0f;
    float    customCriticalLowPct    = 30.0f;
    float    customMaxMoisturePct    = 80.0f;
    float    customHysteresisPct     = 3.0f;
    uint32_t customSoakTimeMs        = 35000;
    uint16_t customPulseWaterMl      = 40;
    uint8_t  customMaxPulsesPerCycle = 5;

    // Pompa — per-pot
    float    pumpMlPerSec       = 0.0f;    // 0 = nie skalibrowana
    bool     pumpCalibrated     = false;

    // Sensory — per-pot
    SoilCalib soilCalib;
    float    sagFactor          = 1.14f;   // crosstalk compensation
    bool     potMaxActiveLow    = true;    // overflow sensor polaryzacja
    float    moistureEmaAlpha   = 0.1f;    // EMA smoothing

    // Puls — per-pot (GUI-configurable, niezależny od profilu)
    uint16_t pulseWaterMl        = 25;      // ml per puls (10–100)
};

// ---------------------------------------------------------------------------
// Tryb pracy
// ---------------------------------------------------------------------------
enum class Mode : uint8_t {
    AUTO   = 0,
    MANUAL = 1,
};

// ---------------------------------------------------------------------------
// Polityka braku czujnika poziomu wody
// ---------------------------------------------------------------------------
enum class UnknownPolicy : uint8_t {
    BLOCK              = 0,
    ALLOW_WITH_WARNING = 1,
};

// ---------------------------------------------------------------------------
// Config — główna struktura (PLAN.md → "Zaktualizowana struktura Config")
// ---------------------------------------------------------------------------
struct Config {
    uint16_t schemaVersion = kConfigSchema;

    // Tryb
    Mode mode = Mode::AUTO;

    // Multi-pot
    uint8_t   numPots = 1;                     // 1 lub 2
    PotConfig pots[kMaxPots];                  // pots[0] domyślnie enabled

    // Pompa — globalne safety
    uint32_t pumpOnMsMax  = 30000;             // hard timeout per puls
    uint32_t cooldownMs   = 60000;             // min przerwa między cyklami

    // Anti-overflow
    bool     antiOverflowEnabled = true;
    uint32_t overflowMaxWaitMs   = 600000;     // 10 min
    UnknownPolicy waterLevelUnknownPolicy = UnknownPolicy::BLOCK;

    // Warunki pogodowe
    float    heatBlockTempC          = 35.0f;
    float    directSunLuxThreshold   = 40000.0f;
    bool     morningWateringEnabled  = false;

    // Dusk detector
    uint32_t duskWateringWindowMs       = 7200000;   // 2h
    float    duskScoreEnterThreshold    = 0.55f;
    float    duskScoreConfirmThreshold  = 0.65f;
    float    duskScoreCancelThreshold   = 0.30f;
    uint32_t transitionConfirmMs        = 900000;    // 15 min
    uint32_t fallbackIntervalMs         = 6UL * 3600 * 1000;  // 6h

    // Opcjonalnie (WiFi/NTP)
    float    latitude       = 52.23f;
    float    longitude      = 21.01f;
    int8_t   utcOffsetHours = 1;

    // Rezerwuar — WSPÓLNY
    float    reservoirCapacityMl     = 1500.0f;   // 1.5L domyślnie
    float    reservoirLowThresholdMl = 400.0f;    // 0.4L próg LOW

    // Przycisk manualny
    uint32_t manualMaxHoldMs   = 30000;
    uint32_t manualCooldownMs  = 60000;

    // Anomaly detection
    float    anomalyDryingRateThreshold    = 5.0f;   // %/h fallback
    float    anomalyDryingRateMultiplier   = 3.0f;

    // Vacation mode
    bool     vacationMode                  = false;
    float    vacationTargetReductionPct    = 10.0f;
    uint8_t  vacationMaxPulsesOverride     = 2;
    float    vacationCooldownMultiplier    = 2.0f;
};

// ---------------------------------------------------------------------------
// NetConfig — sieć (osobny NVS namespace "ag_net")
// ---------------------------------------------------------------------------
struct NetConfig {
    uint16_t schemaVersion  = kNetConfigSchema;
    bool     provisioned       = false;
    char     wifiSsid[33]      = {};
    char     wifiPass[65]      = {};
    char     telegramBotName[64] = {};
    char     telegramBotToken[64] = {};
    char     telegramChatIds[128] = {};
};

// ---------------------------------------------------------------------------
// HardwareConfig — mapowanie pinów i kanałów PbHUB
// (PLAN.md → "Konfiguracja hardware")
// ---------------------------------------------------------------------------
struct PotChannels {
    uint8_t soilAdcChannel;       // kanał PbHUB: moisture ADC (cmd 0x06, pin0 only)
    uint8_t pumpOutputChannel;    // kanał PbHUB: pompa (digital write pin1)
    uint8_t potMaxLevelChannel;   // kanał PbHUB: overflow sensor
    // M5Stack Watering Unit on PbHUB Grove:
    //   pin0 (IN side)  = AOUT (moisture analog) — read via ADC cmd 0x06
    //   pin1 (OUT side) = PUMP_EN (pump control)  — write via Digital cmd 0x01
    uint8_t pumpOutputPin;        // pin PbHUB: PUMP_EN (default 1)
};

struct HardwareConfig {
    // I2C — StickS3 Port.A: SDA=G9, SCL=G10
    uint8_t  i2cSdaPin  = 9;
    uint8_t  i2cSclPin  = 10;
    uint32_t i2cFreq    = 100000;    // 100kHz

    // Adresy I2C
    uint8_t  addrPbHub  = 0x61;
    uint8_t  addrEnv    = 0x44;      // SHT30
    uint8_t  addrBaro   = 0x70;      // QMP6988
    uint8_t  addrLight  = 0x23;      // BH1750

    // PbHUB channel mapping — per-pot
    PotChannels potChannels[kMaxPots] = {
        { 0, 0, 2, 1 },   // pot 0: ADC=CH0.pin0(AOUT), pump=CH0.pin1(PUMP_EN), overflow=CH2
        { 1, 1, 4, 1 },   // pot 1: ADC=CH1.pin0(AOUT), pump=CH1.pin1(PUMP_EN), overflow=CH4
    };

    // Wspólne kanały
    uint8_t reservoirMinChannel = 3;   // CH3: water level reservoir
    uint8_t dualButtonChannel   = 5;   // CH5: Dual Button (IN/IN!)
    uint8_t dualBtnBluePin      = 0;   // pin A — niebieski
    uint8_t dualBtnRedPin       = 1;   // pin B — czerwony

    // PbHUB timing
    uint8_t  pbhubDelayMs       = 10;
    uint8_t  pbhubWarmupCycles  = 5;
};

// Globalna instancja HW config (constexpr — nie zapisujemy do NVS)
extern const HardwareConfig g_hwConfig;

// ---------------------------------------------------------------------------
// API — config load/save/validate
// ---------------------------------------------------------------------------

// Walidacja — zwraca true jeśli OK, false + loguje powody
bool     configValidate(const Config& cfg);

// ---------------------------------------------------------------------------
// RuntimeState — volatile data persisted to NVS for survival across reboots
// Everything the system LEARNS at runtime: budget, trends, cooldowns.
// Only cleared by factory reset or schema bump.
// ---------------------------------------------------------------------------
struct RuntimeState {
    uint16_t schema = kRuntimeSchema;

    // ── Water Budget ──
    float    reservoirCurrentMl      = 0.0f;
    float    totalPumpedMl           = 0.0f;
    float    totalPumpedMlPerPot[kMaxPots] = {};
    bool     reservoirLow            = false;

    // ── Trend Baselines (per-pot) ──
    float    normalDryingRate[kMaxPots]    = {};   // learned %/h
    bool     baselineCalibrated[kMaxPots]  = {};
    float    hourlyDeltas[kMaxPots][24]    = {};   // full ring buffers
    uint8_t  trendHeadIdx[kMaxPots]        = {};
    uint8_t  trendCount[kMaxPots]          = {};

    // ── Cooldown — seconds since last cycle completed (0 = unknown) ──
    uint32_t secsSinceLastCycleDone[kMaxPots] = {};
    uint32_t lastAutoWaterNightSeq[kMaxPots] = {};

    // ── Refill timestamp (seconds since last refill at save time) ──
    uint32_t secsSinceRefill          = 0;

    // ── Dusk scheduling supplement ──
    uint32_t secsSinceLastDusk        = 0;
    uint32_t secsSinceLastDawn        = 0;
    uint32_t nightSequence            = 0;

    // ── Solar clock supplement ──
    uint8_t  solarCycleCount         = 0;
    bool     solarCalibrated         = false;

    uint8_t  _pad[3] = {};
};

// NVS persist
bool     configLoad(Config& cfg);
bool     configSave(const Config& cfg);
void     configLoadDefaults(Config& cfg);

// NetConfig persist
bool     netConfigLoad(NetConfig& net);
bool     netConfigSave(const NetConfig& net);

// Factory reset — czyści wszystkie namespace'y NVS
void     configFactoryReset();

// RuntimeState NVS persist
bool     runtimeStateSave(const RuntimeState& rs);
bool     runtimeStateLoad(RuntimeState& rs);

// Helper: pobierz aktywny profil dla doniczki
const PlantProfile& getActiveProfile(const Config& cfg, uint8_t potIdx);
