// ============================================================================
// config.cpp — Implementacja konfiguracji, NVS, walidacja
// ============================================================================
// Źródło prawdy: docs/PLAN.md → odpowiednie sekcje Config
// Architektura:  docs/ARCHITECTURE.md
// ============================================================================

#include "config.h"
#include <Preferences.h>
#include <Arduino.h>
#include "log_serial.h"

#define Serial AGSerial

namespace {
constexpr uint16_t kMoistureRawMinGap = 32;
constexpr float kMoistureCurveExponentMin = 0.1f;
constexpr float kMoistureCurveExponentMax = 12.0f;
constexpr float kFixedPumpMlPerSec = 5.17f;

uint16_t defaultPotDryRaw(uint8_t potIdx) {
    switch (potIdx) {
        case 0: return 2228;
        case 1: return 2229;
        default: return 2230;
    }
}

uint16_t defaultPotWetRaw(uint8_t potIdx) {
    switch (potIdx) {
        case 0: return 1766;
        case 1: return 1786;
        default: return 1752;
    }
}

float defaultPotCurveExponent(uint8_t potIdx) {
    switch (potIdx) {
        case 0: return 0.63f;
        case 1: return 0.65f;
        default: return 5.0f;
    }
}

void applyPotMoistureDefaults(PotConfig& pot, uint8_t potIdx) {
    pot.moistureDryRaw = defaultPotDryRaw(potIdx);
    pot.moistureWetRaw = defaultPotWetRaw(potIdx);
    pot.moistureCurveExponent = defaultPotCurveExponent(potIdx);
}

void applyFixedPumpParameters(Config& cfg) {
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        cfg.pots[i].pumpMlPerSec = kFixedPumpMlPerSec;
        cfg.pots[i].pumpCalibrated = true;
    }
}

bool normalizeFixedPumpParameters(Config& cfg) {
    bool changed = false;

    for (uint8_t i = 0; i < kMaxPots; ++i) {
        if (!cfg.pots[i].pumpCalibrated ||
            cfg.pots[i].pumpMlPerSec < (kFixedPumpMlPerSec - 0.01f) ||
            cfg.pots[i].pumpMlPerSec > (kFixedPumpMlPerSec + 0.01f)) {
            cfg.pots[i].pumpMlPerSec = kFixedPumpMlPerSec;
            cfg.pots[i].pumpCalibrated = true;
            changed = true;
        }
    }

    return changed;
}

bool moistureEndpointsValid(uint16_t dryRaw, uint16_t wetRaw) {
    if (dryRaw > 4095 || wetRaw > 4095) {
        return false;
    }
    if (wetRaw >= dryRaw) {
        return false;
    }
    return (dryRaw - wetRaw) >= kMoistureRawMinGap;
}

bool moistureCurveExponentValid(float exponent) {
    return exponent >= kMoistureCurveExponentMin
        && exponent <= kMoistureCurveExponentMax;
}

struct LegacyPotConfigV4 {
    bool     enabled = false;
    uint8_t  plantProfileIndex = 0;
    float    customTargetPct = 60.0f;
    float    customCriticalLowPct = 30.0f;
    float    customMaxMoisturePct = 80.0f;
    float    customHysteresisPct = 3.0f;
    uint32_t customSoakTimeMs = 35000;
    uint16_t customPulseWaterMl = 40;
    uint8_t  customMaxPulsesPerCycle = 5;
    float    pumpMlPerSec = 0.0f;
    bool     pumpCalibrated = false;
    bool     potMaxActiveLow = true;
    float    moistureEmaAlpha = 0.1f;
    uint16_t pulseWaterMl = 25;
};

struct LegacyConfigV4 {
    uint16_t schemaVersion = 4;
    Mode mode = Mode::AUTO;
    uint8_t numPots = 1;
    LegacyPotConfigV4 pots[kMaxPots];
    uint32_t pumpOnMsMax = 30000;
    uint32_t cooldownMs = 60000;
    bool antiOverflowEnabled = true;
    uint32_t overflowMaxWaitMs = 600000;
    UnknownPolicy waterLevelUnknownPolicy = UnknownPolicy::BLOCK;
    float heatBlockTempC = 35.0f;
    float directSunLuxThreshold = 40000.0f;
    bool morningWateringEnabled = false;
    uint32_t duskWateringWindowMs = 7200000;
    float duskScoreEnterThreshold = 0.55f;
    float duskScoreConfirmThreshold = 0.65f;
    float duskScoreCancelThreshold = 0.30f;
    uint32_t transitionConfirmMs = 900000;
    uint32_t fallbackIntervalMs = 6UL * 3600 * 1000;
    float latitude = 52.23f;
    float longitude = 21.01f;
    int8_t utcOffsetHours = 1;
    float reservoirCapacityMl = 1500.0f;
    float reservoirLowThresholdMl = 400.0f;
    uint32_t manualMaxHoldMs = 30000;
    uint32_t manualCooldownMs = 60000;
    float anomalyDryingRateThreshold = 5.0f;
    float anomalyDryingRateMultiplier = 3.0f;
    bool vacationMode = false;
    float vacationTargetReductionPct = 10.0f;
    uint8_t vacationMaxPulsesOverride = 2;
    float vacationCooldownMultiplier = 2.0f;
};

struct LegacyPotConfigV5 {
    bool     enabled = false;
    uint8_t  plantProfileIndex = 0;
    float    customTargetPct = 60.0f;
    float    customCriticalLowPct = 30.0f;
    float    customMaxMoisturePct = 80.0f;
    float    customHysteresisPct = 3.0f;
    uint32_t customSoakTimeMs = 35000;
    uint16_t customPulseWaterMl = 40;
    uint8_t  customMaxPulsesPerCycle = 5;
    float    pumpMlPerSec = 0.0f;
    bool     pumpCalibrated = false;
    bool     potMaxActiveLow = true;
    float    moistureEmaAlpha = 0.1f;
    uint16_t moistureDryRaw = 2230;
    uint16_t moistureWetRaw = 1752;
    uint16_t pulseWaterMl = 25;
};

struct LegacyConfigV5 {
    uint16_t schemaVersion = 5;
    Mode mode = Mode::AUTO;
    uint8_t numPots = 1;
    LegacyPotConfigV5 pots[kMaxPots];
    uint32_t pumpOnMsMax = 30000;
    uint32_t cooldownMs = 60000;
    bool antiOverflowEnabled = true;
    uint32_t overflowMaxWaitMs = 600000;
    UnknownPolicy waterLevelUnknownPolicy = UnknownPolicy::BLOCK;
    float heatBlockTempC = 35.0f;
    float directSunLuxThreshold = 40000.0f;
    bool morningWateringEnabled = false;
    uint32_t duskWateringWindowMs = 7200000;
    float duskScoreEnterThreshold = 0.55f;
    float duskScoreConfirmThreshold = 0.65f;
    float duskScoreCancelThreshold = 0.30f;
    uint32_t transitionConfirmMs = 900000;
    uint32_t fallbackIntervalMs = 6UL * 3600 * 1000;
    float latitude = 52.23f;
    float longitude = 21.01f;
    int8_t utcOffsetHours = 1;
    float reservoirCapacityMl = 1500.0f;
    float reservoirLowThresholdMl = 400.0f;
    uint32_t manualMaxHoldMs = 30000;
    uint32_t manualCooldownMs = 60000;
    float anomalyDryingRateThreshold = 5.0f;
    float anomalyDryingRateMultiplier = 3.0f;
    bool vacationMode = false;
    float vacationTargetReductionPct = 10.0f;
    uint8_t vacationMaxPulsesOverride = 2;
    float vacationCooldownMultiplier = 2.0f;
};

void migrateConfigV4ToV5(const LegacyConfigV4& legacy, Config& cfg) {
    cfg = Config{};
    cfg.mode = legacy.mode;
    cfg.numPots = legacy.numPots;
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        cfg.pots[i].enabled = legacy.pots[i].enabled;
        cfg.pots[i].plantProfileIndex = legacy.pots[i].plantProfileIndex;
        cfg.pots[i].customTargetPct = legacy.pots[i].customTargetPct;
        cfg.pots[i].customCriticalLowPct = legacy.pots[i].customCriticalLowPct;
        cfg.pots[i].customMaxMoisturePct = legacy.pots[i].customMaxMoisturePct;
        cfg.pots[i].customHysteresisPct = legacy.pots[i].customHysteresisPct;
        cfg.pots[i].customSoakTimeMs = legacy.pots[i].customSoakTimeMs;
        cfg.pots[i].customPulseWaterMl = legacy.pots[i].customPulseWaterMl;
        cfg.pots[i].customMaxPulsesPerCycle = legacy.pots[i].customMaxPulsesPerCycle;
        cfg.pots[i].pumpMlPerSec = legacy.pots[i].pumpMlPerSec;
        cfg.pots[i].pumpCalibrated = legacy.pots[i].pumpCalibrated;
        cfg.pots[i].potMaxActiveLow = legacy.pots[i].potMaxActiveLow;
        cfg.pots[i].moistureEmaAlpha = legacy.pots[i].moistureEmaAlpha;
        cfg.pots[i].pulseWaterMl = legacy.pots[i].pulseWaterMl;
        applyPotMoistureDefaults(cfg.pots[i], i);
    }
    cfg.pumpOnMsMax = legacy.pumpOnMsMax;
    cfg.cooldownMs = legacy.cooldownMs;
    cfg.antiOverflowEnabled = legacy.antiOverflowEnabled;
    cfg.overflowMaxWaitMs = legacy.overflowMaxWaitMs;
    cfg.waterLevelUnknownPolicy = legacy.waterLevelUnknownPolicy;
    cfg.heatBlockTempC = legacy.heatBlockTempC;
    cfg.directSunLuxThreshold = legacy.directSunLuxThreshold;
    cfg.morningWateringEnabled = legacy.morningWateringEnabled;
    cfg.duskWateringWindowMs = legacy.duskWateringWindowMs;
    cfg.duskScoreEnterThreshold = legacy.duskScoreEnterThreshold;
    cfg.duskScoreConfirmThreshold = legacy.duskScoreConfirmThreshold;
    cfg.duskScoreCancelThreshold = legacy.duskScoreCancelThreshold;
    cfg.transitionConfirmMs = legacy.transitionConfirmMs;
    cfg.fallbackIntervalMs = legacy.fallbackIntervalMs;
    cfg.latitude = legacy.latitude;
    cfg.longitude = legacy.longitude;
    cfg.utcOffsetHours = legacy.utcOffsetHours;
    cfg.reservoirCapacityMl = legacy.reservoirCapacityMl;
    cfg.reservoirLowThresholdMl = legacy.reservoirLowThresholdMl;
    cfg.manualMaxHoldMs = legacy.manualMaxHoldMs;
    cfg.manualCooldownMs = legacy.manualCooldownMs;
    cfg.anomalyDryingRateThreshold = legacy.anomalyDryingRateThreshold;
    cfg.anomalyDryingRateMultiplier = legacy.anomalyDryingRateMultiplier;
    cfg.vacationMode = legacy.vacationMode;
    cfg.vacationTargetReductionPct = legacy.vacationTargetReductionPct;
    cfg.vacationMaxPulsesOverride = legacy.vacationMaxPulsesOverride;
    cfg.vacationCooldownMultiplier = legacy.vacationCooldownMultiplier;
    cfg.schemaVersion = kConfigSchema;
}

void migrateConfigV5ToV6(const LegacyConfigV5& legacy, Config& cfg) {
    cfg = Config{};
    cfg.mode = legacy.mode;
    cfg.numPots = legacy.numPots;
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        cfg.pots[i].enabled = legacy.pots[i].enabled;
        cfg.pots[i].plantProfileIndex = legacy.pots[i].plantProfileIndex;
        cfg.pots[i].customTargetPct = legacy.pots[i].customTargetPct;
        cfg.pots[i].customCriticalLowPct = legacy.pots[i].customCriticalLowPct;
        cfg.pots[i].customMaxMoisturePct = legacy.pots[i].customMaxMoisturePct;
        cfg.pots[i].customHysteresisPct = legacy.pots[i].customHysteresisPct;
        cfg.pots[i].customSoakTimeMs = legacy.pots[i].customSoakTimeMs;
        cfg.pots[i].customPulseWaterMl = legacy.pots[i].customPulseWaterMl;
        cfg.pots[i].customMaxPulsesPerCycle = legacy.pots[i].customMaxPulsesPerCycle;
        cfg.pots[i].pumpMlPerSec = legacy.pots[i].pumpMlPerSec;
        cfg.pots[i].pumpCalibrated = legacy.pots[i].pumpCalibrated;
        cfg.pots[i].potMaxActiveLow = legacy.pots[i].potMaxActiveLow;
        cfg.pots[i].moistureEmaAlpha = legacy.pots[i].moistureEmaAlpha;
        cfg.pots[i].moistureDryRaw = legacy.pots[i].moistureDryRaw;
        cfg.pots[i].moistureWetRaw = legacy.pots[i].moistureWetRaw;
        cfg.pots[i].pulseWaterMl = legacy.pots[i].pulseWaterMl;
        cfg.pots[i].moistureCurveExponent = defaultPotCurveExponent(i);
    }
    cfg.pumpOnMsMax = legacy.pumpOnMsMax;
    cfg.cooldownMs = legacy.cooldownMs;
    cfg.antiOverflowEnabled = legacy.antiOverflowEnabled;
    cfg.overflowMaxWaitMs = legacy.overflowMaxWaitMs;
    cfg.waterLevelUnknownPolicy = legacy.waterLevelUnknownPolicy;
    cfg.heatBlockTempC = legacy.heatBlockTempC;
    cfg.directSunLuxThreshold = legacy.directSunLuxThreshold;
    cfg.morningWateringEnabled = legacy.morningWateringEnabled;
    cfg.duskWateringWindowMs = legacy.duskWateringWindowMs;
    cfg.duskScoreEnterThreshold = legacy.duskScoreEnterThreshold;
    cfg.duskScoreConfirmThreshold = legacy.duskScoreConfirmThreshold;
    cfg.duskScoreCancelThreshold = legacy.duskScoreCancelThreshold;
    cfg.transitionConfirmMs = legacy.transitionConfirmMs;
    cfg.fallbackIntervalMs = legacy.fallbackIntervalMs;
    cfg.latitude = legacy.latitude;
    cfg.longitude = legacy.longitude;
    cfg.utcOffsetHours = legacy.utcOffsetHours;
    cfg.reservoirCapacityMl = legacy.reservoirCapacityMl;
    cfg.reservoirLowThresholdMl = legacy.reservoirLowThresholdMl;
    cfg.manualMaxHoldMs = legacy.manualMaxHoldMs;
    cfg.manualCooldownMs = legacy.manualCooldownMs;
    cfg.anomalyDryingRateThreshold = legacy.anomalyDryingRateThreshold;
    cfg.anomalyDryingRateMultiplier = legacy.anomalyDryingRateMultiplier;
    cfg.vacationMode = legacy.vacationMode;
    cfg.vacationTargetReductionPct = legacy.vacationTargetReductionPct;
    cfg.vacationMaxPulsesOverride = legacy.vacationMaxPulsesOverride;
    cfg.vacationCooldownMultiplier = legacy.vacationCooldownMultiplier;
    cfg.schemaVersion = kConfigSchema;
}
}

// ---------------------------------------------------------------------------
// Profile roślin — stała tablica (PLAN.md → "Profile roślin")
// ---------------------------------------------------------------------------
//                             name        icon  target crit  max   hyst  soak    pulse maxP
const PlantProfile kProfiles[kNumProfiles] = {
    { "Pomidor",    "\xF0\x9F\x8D\x85", 65.0f, 40.0f, 80.0f, 3.0f, 35000, 25, 6 },
    { "Papryka",    "\xF0\x9F\x8C\xB6", 62.0f, 35.0f, 75.0f, 3.0f, 35000, 25, 5 },
    { "Bazylia",    "\xF0\x9F\x8C\xBF", 60.0f, 30.0f, 75.0f, 3.0f, 30000, 20, 4 },
    { "Truskawka",  "\xF0\x9F\x8D\x93", 70.0f, 40.0f, 85.0f, 3.0f, 40000, 25, 6 },
    { "Chili",      "\xF0\x9F\x8C\xB6", 55.0f, 30.0f, 70.0f, 3.0f, 35000, 20, 5 },
    { "Custom",     "\xE2\x9A\x99",      0.0f,  0.0f,  0.0f, 3.0f, 35000, 25, 5 },
};

// ---------------------------------------------------------------------------
// HardwareConfig — globalna instancja
// ---------------------------------------------------------------------------
const HardwareConfig g_hwConfig = {};  // domyślne wartości z struct

// ---------------------------------------------------------------------------
// Walidacja (PLAN.md → "Walidacja (kontrakt)")
// ---------------------------------------------------------------------------
bool configValidate(const Config& cfg) {
    bool ok = true;

    if (cfg.numPots < 1 || cfg.numPots > kMaxPots) {
        Serial.printf("[CONFIG] FAIL: numPots=%d out of range\n", cfg.numPots);
        ok = false;
    }

    for (uint8_t i = 0; i < cfg.numPots; i++) {
        const auto& pot = cfg.pots[i];
        if (!pot.enabled) {
            Serial.printf("[CONFIG] FAIL: pots[%d] not enabled but numPots=%d\n", i, cfg.numPots);
            ok = false;
        }
        if (pot.plantProfileIndex >= kNumProfiles) {
            Serial.printf("[CONFIG] FAIL: pots[%d].plantProfileIndex=%d\n", i, pot.plantProfileIndex);
            ok = false;
        }
        if (pot.pumpMlPerSec < 0.0f) {
            Serial.printf("[CONFIG] FAIL: pots[%d].pumpMlPerSec=%.2f\n", i, pot.pumpMlPerSec);
            ok = false;
        }
        if (pot.moistureEmaAlpha <= 0.0f || pot.moistureEmaAlpha > 1.0f) {
            Serial.printf("[CONFIG] FAIL: pots[%d].moistureEmaAlpha=%.2f\n", i, pot.moistureEmaAlpha);
            ok = false;
        }
        if (!moistureEndpointsValid(pot.moistureDryRaw, pot.moistureWetRaw)) {
            Serial.printf("[CONFIG] FAIL: pots[%d].moistureDryRaw=%u moistureWetRaw=%u\n",
                          i,
                          static_cast<unsigned>(pot.moistureDryRaw),
                          static_cast<unsigned>(pot.moistureWetRaw));
            ok = false;
        }
        if (!moistureCurveExponentValid(pot.moistureCurveExponent)) {
            Serial.printf("[CONFIG] FAIL: pots[%d].moistureCurveExponent=%.2f\n",
                          i, pot.moistureCurveExponent);
            ok = false;
        }
    }

    if (cfg.pumpOnMsMax == 0) { Serial.println("[CONFIG] FAIL: pumpOnMsMax=0"); ok = false; }
    if (cfg.reservoirCapacityMl <= 0.0f) { Serial.println("[CONFIG] FAIL: reservoirCapacityMl<=0"); ok = false; }
    if (cfg.reservoirLowThresholdMl >= cfg.reservoirCapacityMl) {
        Serial.println("[CONFIG] FAIL: reservoirLowThresholdMl >= capacity");
        ok = false;
    }
    if (cfg.heatBlockTempC <= 0.0f || cfg.heatBlockTempC >= 60.0f) {
        Serial.printf("[CONFIG] FAIL: heatBlockTempC=%.1f\n", cfg.heatBlockTempC);
        ok = false;
    }
    if (cfg.anomalyDryingRateMultiplier <= 1.0f || cfg.anomalyDryingRateMultiplier > 10.0f) {
        Serial.printf("[CONFIG] FAIL: anomalyDryingRateMultiplier=%.1f\n", cfg.anomalyDryingRateMultiplier);
        ok = false;
    }
    if (cfg.vacationTargetReductionPct < 0.0f || cfg.vacationTargetReductionPct > 50.0f) {
        Serial.printf("[CONFIG] FAIL: vacationTargetReductionPct=%.1f\n", cfg.vacationTargetReductionPct);
        ok = false;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
void configLoadDefaults(Config& cfg) {
    cfg = Config{};                  // struct defaults
    cfg.pots[0].enabled = true;      // pot 0 zawsze aktywna
    applyFixedPumpParameters(cfg);
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        applyPotMoistureDefaults(cfg.pots[i], i);
    }
}

// ---------------------------------------------------------------------------
// NVS Load / Save — Config
// (PLAN.md → "Zapis asynchroniczny")
// ---------------------------------------------------------------------------
bool configLoad(Config& cfg) {
    Preferences prefs;
    if (!prefs.begin(kNvsConfig, true)) {   // read-only
        // Namespace nie istnieje jeszcze (pierwszy boot) — utwórz z defaultami
        Serial.println("[CONFIG] NVS namespace not found — first boot, creating with defaults");
        configLoadDefaults(cfg);
        // Utwórz namespace zapisując defaults
        if (prefs.begin(kNvsConfig, false)) {
            prefs.putUShort("schema", kConfigSchema);
            prefs.putBytes("cfg", &cfg, sizeof(Config));
            prefs.end();
            Serial.println("[CONFIG] NVS namespace created with defaults");
        }
        return false;
    }

    // Check if keys exist (may be missing after factory reset)
    if (!prefs.isKey("schema") || !prefs.isKey("cfg")) {
        Serial.println("[CONFIG] NVS keys missing (post-reset?) — creating defaults");
        prefs.end();
        configLoadDefaults(cfg);
        if (prefs.begin(kNvsConfig, false)) {
            prefs.putUShort("schema", kConfigSchema);
            prefs.putBytes("cfg", &cfg, sizeof(Config));
            prefs.end();
        }
        return false;
    }

    uint16_t schema = prefs.getUShort("schema", 0);
    size_t storedLen = prefs.getBytesLength("cfg");

    if (schema == kConfigSchema && storedLen == sizeof(Config)) {
        size_t len = prefs.getBytes("cfg", &cfg, sizeof(Config));
        prefs.end();

        if (len != sizeof(Config)) {
            Serial.printf("[CONFIG] NVS read size mismatch (%d vs %d) — defaults\n",
                          (int)len, (int)sizeof(Config));
            configLoadDefaults(cfg);
            return false;
        }
    } else if (schema == 4 && storedLen == sizeof(LegacyConfigV4)) {
        LegacyConfigV4 legacy{};
        size_t len = prefs.getBytes("cfg", &legacy, sizeof(legacy));
        prefs.end();
        if (len != sizeof(legacy)) {
            Serial.printf("[CONFIG] Legacy NVS read size mismatch (%d vs %d) — defaults\n",
                          (int)len, (int)sizeof(legacy));
            configLoadDefaults(cfg);
            return false;
        }
        migrateConfigV4ToV5(legacy, cfg);
        if (!configValidate(cfg)) {
            Serial.println("[CONFIG] Migrated config failed validation — using defaults");
            configLoadDefaults(cfg);
            return false;
        }
        configSave(cfg);
        Serial.println("[CONFIG] Migrated config v4 -> v5");
        return true;
    } else if (schema == 5 && storedLen == sizeof(LegacyConfigV5)) {
        LegacyConfigV5 legacy{};
        size_t len = prefs.getBytes("cfg", &legacy, sizeof(legacy));
        prefs.end();
        if (len != sizeof(legacy)) {
            Serial.printf("[CONFIG] Legacy v5 NVS read size mismatch (%d vs %d) — defaults\n",
                          (int)len, (int)sizeof(legacy));
            configLoadDefaults(cfg);
            return false;
        }
        migrateConfigV5ToV6(legacy, cfg);
        if (!configValidate(cfg)) {
            Serial.println("[CONFIG] Migrated v5 config failed validation — using defaults");
            configLoadDefaults(cfg);
            return false;
        }
        configSave(cfg);
        Serial.println("[CONFIG] Migrated config v5 -> v6");
        return true;
    } else {
        Serial.printf("[CONFIG] Schema mismatch (nvs=%d len=%d, expected=%d len=%d) — using defaults\n",
                      schema, (int)storedLen, kConfigSchema, (int)sizeof(Config));
        prefs.end();
        configLoadDefaults(cfg);
        return false;
    }

    bool fixedPumpParamsChanged = normalizeFixedPumpParameters(cfg);

    if (!configValidate(cfg)) {
        Serial.println("[CONFIG] Loaded config failed validation — using defaults");
        configLoadDefaults(cfg);
        return false;
    }

    if (fixedPumpParamsChanged) {
        configSave(cfg);
        Serial.println("[CONFIG] Applied fixed M5 Watering pump parameters");
    }

    Serial.printf("[CONFIG] Loaded OK: numPots=%d, mode=%s, reservoir=%.0fml\n",
                  cfg.numPots,
                  cfg.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                  cfg.reservoirCapacityMl);
    return true;
}

bool configSave(const Config& cfg) {
    Preferences prefs;
    if (!prefs.begin(kNvsConfig, false)) {  // read-write
        Serial.println("[CONFIG] NVS open for write failed!");
        return false;
    }

    prefs.putUShort("schema", kConfigSchema);
    size_t written = prefs.putBytes("cfg", &cfg, sizeof(Config));
    prefs.end();

    if (written != sizeof(Config)) {
        Serial.printf("[CONFIG] NVS write failed (%d vs %d)\n",
                      (int)written, (int)sizeof(Config));
        return false;
    }

    Serial.println("[CONFIG] Saved OK");
    return true;
}

// ---------------------------------------------------------------------------
// NVS Load / Save — NetConfig
// ---------------------------------------------------------------------------
namespace {
struct LegacyNetConfigV1 {
    bool  provisioned       = false;
    char  wifiSsid[33]      = {};
    char  wifiPass[65]      = {};
    char  telegramBotToken[64] = {};
    char  telegramChatId[16]   = {};
};
}

bool netConfigLoad(NetConfig& net) {
    Preferences prefs;
    net = NetConfig{};  // always zero-init first

    if (!prefs.begin(kNvsNet, true)) {
        // Namespace doesn't exist (fresh NVS) — create with defaults
        Serial.println("[NET] NVS namespace not found — first boot");
        if (prefs.begin(kNvsNet, false)) {
            prefs.putBytes("net", &net, sizeof(NetConfig));
            prefs.end();
        }
        return false;
    }

    // Namespace exists but key may be missing (e.g. after factory reset)
    if (!prefs.isKey("net")) {
        Serial.println("[NET] NVS key 'net' missing — writing defaults");
        prefs.end();
        if (prefs.begin(kNvsNet, false)) {
            prefs.putBytes("net", &net, sizeof(NetConfig));
            prefs.end();
        }
        return false;
    }

    size_t storedLen = prefs.getBytesLength("net");
    size_t len = prefs.getBytes("net", &net, storedLen <= sizeof(NetConfig) ? storedLen : sizeof(NetConfig));
    prefs.end();

    if (len == sizeof(NetConfig) && net.schemaVersion == kNetConfigSchema) {
        return true;
    }

    if (storedLen == sizeof(LegacyNetConfigV1)) {
        LegacyNetConfigV1 legacy{};
        prefs.begin(kNvsNet, true);
        size_t legacyLen = prefs.getBytes("net", &legacy, sizeof(legacy));
        prefs.end();
        if (legacyLen == sizeof(legacy)) {
            net = NetConfig{};
            net.provisioned = legacy.provisioned;
            strncpy(net.wifiSsid, legacy.wifiSsid, sizeof(net.wifiSsid) - 1);
            strncpy(net.wifiPass, legacy.wifiPass, sizeof(net.wifiPass) - 1);
            strncpy(net.telegramBotToken, legacy.telegramBotToken, sizeof(net.telegramBotToken) - 1);
            strncpy(net.telegramChatIds, legacy.telegramChatId, sizeof(net.telegramChatIds) - 1);
            netConfigSave(net);
            Serial.println("[NET] event=config_migrated from=v1 to=v2");
            return true;
        }
    }

    net = NetConfig{};
    netConfigSave(net);
    Serial.println("[NET] event=config_invalid action=defaults_written");
    return false;
}

bool netConfigSave(const NetConfig& net) {
    Preferences prefs;
    if (!prefs.begin(kNvsNet, false)) return false;
    size_t written = prefs.putBytes("net", &net, sizeof(NetConfig));
    prefs.end();
    return written == sizeof(NetConfig);
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------
void configFactoryReset() {
    Serial.println("[CONFIG] FACTORY RESET — clearing all NVS namespaces");
    {
        Preferences prefs;
        prefs.begin(kNvsConfig, false);
        prefs.clear();
        prefs.end();
    }
    {
        Preferences prefs;
        prefs.begin(kNvsNet, false);
        prefs.clear();
        prefs.end();
    }
    {
        Preferences prefs;
        prefs.begin(kNvsHist, false);
        prefs.clear();
        prefs.end();
    }
    {
        Preferences prefs;
        prefs.begin(kNvsDusk, false);
        prefs.clear();
        prefs.end();
    }
    {
        Preferences prefs;
        prefs.begin(kNvsRuntime, false);
        prefs.clear();
        prefs.end();
    }
    Serial.println("[CONFIG] All NVS cleared. Restarting...");
}

// ---------------------------------------------------------------------------
// RuntimeState — NVS persist
// ---------------------------------------------------------------------------
bool runtimeStateSave(const RuntimeState& rs) {
    Preferences prefs;
    if (!prefs.begin(kNvsRuntime, false)) {
        Serial.println("[RUNTIME] NVS save: can't open namespace");
        return false;
    }
    prefs.putUShort("schema", kRuntimeSchema);
    size_t written = prefs.putBytes("state", &rs, sizeof(RuntimeState));
    prefs.end();
    return written == sizeof(RuntimeState);
}

bool runtimeStateLoad(RuntimeState& rs) {
    Preferences prefs;
    rs = RuntimeState{};
    if (!prefs.begin(kNvsRuntime, true)) return false;
    if (!prefs.isKey("schema") || !prefs.isKey("state")) { prefs.end(); return false; }

    uint16_t schema = prefs.getUShort("schema", 0);
    if (schema != kRuntimeSchema) {
        Serial.printf("[RUNTIME] schema mismatch (nvs=%d, expected=%d) — discarding\n",
                      schema, kRuntimeSchema);
        prefs.end();
        return false;
    }

    size_t len = prefs.getBytes("state", &rs, sizeof(RuntimeState));
    prefs.end();

    if (len != sizeof(RuntimeState)) {
        Serial.printf("[RUNTIME] NVS size mismatch (%d vs %d)\n", (int)len, (int)sizeof(RuntimeState));
        rs = RuntimeState{};
        return false;
    }

    Serial.printf("[RUNTIME] Loaded: reservoir=%.0fml pumped=%.1fml low=%s\n",
                  rs.reservoirCurrentMl, rs.totalPumpedMl,
                  rs.reservoirLow ? "YES" : "no");
    return true;
}

// ---------------------------------------------------------------------------
// Helper: aktywny profil per-pot
// ---------------------------------------------------------------------------
const PlantProfile& getActiveProfile(const Config& cfg, uint8_t potIdx) {
    const PotConfig& pot = cfg.pots[potIdx];
    uint8_t idx = pot.plantProfileIndex;
    if (idx >= kNumProfiles) idx = 0;

    // Custom profile runtime materialization (index 5)
    if (idx == (kNumProfiles - 1)) {
        static PlantProfile s_custom[kMaxPots];
        PlantProfile custom = kProfiles[idx];
        custom.targetMoisturePct = pot.customTargetPct;
        custom.criticalLowPct = pot.customCriticalLowPct;
        custom.maxMoisturePct = pot.customMaxMoisturePct;
        custom.hysteresisPct = pot.customHysteresisPct;
        custom.soakTimeMs = pot.customSoakTimeMs;
        custom.pulseWaterMl = pot.customPulseWaterMl;
        custom.maxPulsesPerCycle = pot.customMaxPulsesPerCycle;
        s_custom[potIdx] = custom;
        return s_custom[potIdx];
    }

    return kProfiles[idx];
}
