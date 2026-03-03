// ============================================================================
// config.cpp — Implementacja konfiguracji, NVS, walidacja
// ============================================================================
// Źródło prawdy: docs/PLAN.md → odpowiednie sekcje Config
// Architektura:  docs/ARCHITECTURE.md
// ============================================================================

#include "config.h"
#include <Preferences.h>
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Profile roślin — stała tablica (PLAN.md → "Profile roślin")
// ---------------------------------------------------------------------------
//                             name        icon  target crit  max   hyst  soak    pulse maxP
const PlantProfile kProfiles[kNumProfiles] = {
    { "Pomidor",    "\xF0\x9F\x8D\x85", 65.0f, 40.0f, 80.0f, 5.0f, 35000, 25, 6 },
    { "Papryka",    "\xF0\x9F\x8C\xB6", 62.0f, 35.0f, 75.0f, 5.0f, 35000, 25, 5 },
    { "Bazylia",    "\xF0\x9F\x8C\xBF", 60.0f, 30.0f, 75.0f, 5.0f, 30000, 20, 4 },
    { "Truskawka",  "\xF0\x9F\x8D\x93", 70.0f, 40.0f, 85.0f, 5.0f, 40000, 25, 6 },
    { "Chili",      "\xF0\x9F\x8C\xB6", 55.0f, 30.0f, 70.0f, 5.0f, 35000, 20, 5 },
    { "Custom",     "\xE2\x9A\x99",      0.0f,  0.0f,  0.0f, 5.0f, 35000, 25, 5 },
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
        if (pot.soilCalib.rawDry == pot.soilCalib.rawWet) {
            Serial.printf("[CONFIG] FAIL: pots[%d].soilCalib dry==wet\n", i);
            ok = false;
        }
        if (pot.moistureEmaAlpha <= 0.0f || pot.moistureEmaAlpha > 1.0f) {
            Serial.printf("[CONFIG] FAIL: pots[%d].moistureEmaAlpha=%.2f\n", i, pot.moistureEmaAlpha);
            ok = false;
        }
        if (pot.sagFactor <= 0.5f || pot.sagFactor >= 2.0f) {
            Serial.printf("[CONFIG] FAIL: pots[%d].sagFactor=%.2f\n", i, pot.sagFactor);
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
    cfg.pots[0].pumpMlPerSec = 5.17f; // zmierzone 2026-03-01
    cfg.pots[0].pumpCalibrated = true;
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
    if (schema != kConfigSchema) {
        Serial.printf("[CONFIG] Schema mismatch (nvs=%d, expected=%d) — using defaults\n",
                      schema, kConfigSchema);
        prefs.end();
        configLoadDefaults(cfg);
        return false;
    }

    size_t len = prefs.getBytes("cfg", &cfg, sizeof(Config));
    prefs.end();

    if (len != sizeof(Config)) {
        Serial.printf("[CONFIG] NVS read size mismatch (%d vs %d) — defaults\n",
                      (int)len, (int)sizeof(Config));
        configLoadDefaults(cfg);
        return false;
    }

    if (!configValidate(cfg)) {
        Serial.println("[CONFIG] Loaded config failed validation — using defaults");
        configLoadDefaults(cfg);
        return false;
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

    size_t len = prefs.getBytes("net", &net, sizeof(NetConfig));
    prefs.end();
    return len == sizeof(NetConfig);
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
