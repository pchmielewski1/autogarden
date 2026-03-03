// ============================================================================
// main.cpp — Entry point: boot sequence + FreeRTOS tasks
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Sekwencja boot", "Scheduler ticków",
//                "Docelowa architektura wykonania (FreeRTOS)"
// Architektura:  docs/ARCHITECTURE.md
//
// Port.A I2C: SDA=G9, SCL=G10
// PbHUB v1.1: @0x61
//
// Taski:
//   ControlTask (priorytet 5) — sensory, FSM podlewania, safety, trend, dusk
//   UiTask      (priorytet 3) — ekran, przyciski M5, factory reset
//   NetTask     (priorytet 2) — WiFi reconnect, Telegram, heartbeat
//   ConfigTask  (priorytet 1) — debounced NVS save
// ============================================================================

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_log.h>

#include "events.h"
#include "config.h"
#include "hardware.h"
#include "watering.h"
#include "analysis.h"
#include "ui.h"
#include "network.h"

// ============================================================================
// Globalne instancje (zadeklarowane extern w headerach, zdefiniowane tutaj)
// ============================================================================

EventQueue      g_eventQueue;

// Config
Config          g_config;
NetConfig       g_netConfig;

static const char* kLogTag = "AUTOGARDEN";

// Hardware
HardwareManager g_hardware;

// Watering domain
WateringCycle   g_cycles[kMaxPots];
WaterBudget     g_budget;
ActuatorState   g_actuator;
ManualState     g_manual;

// Analysis
EmaFilter       g_emaFilters[kMaxPots];
// g_trendStates defined in analysis.cpp
DuskDetector    g_duskDetector;
SolarClock      g_solarClock;
SensorHistory   g_history;

// UI
UiState         g_uiState;

// Network
NetworkState    g_netState;

// Snapshot — ControlTask pisze, UiTask/NetTask czyta (atomic swap)
static SemaphoreHandle_t s_snapMutex = nullptr;
static SensorSnapshot    s_latestSnap;

struct SharedState {
    Config        config;
    NetConfig     netConfig;
    WaterBudget   budget;
    WateringCycle cycles[kMaxPots];
    TrendState    trends[kMaxPots];
    DuskPhase     duskPhase = DuskPhase::NIGHT;
    bool          wifiConnected = false;
    uint8_t       selectedPot = 0;
};

static SemaphoreHandle_t s_stateMutex = nullptr;
static SharedState       s_sharedState;

// Config save request — debounced
static QueueHandle_t     s_saveQueue = nullptr;

// ============================================================================
// Helpers — snapshot (thread-safe r/w)
// ============================================================================

static void publishSnapshot(const SensorSnapshot& snap) {
    if (xSemaphoreTake(s_snapMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_latestSnap = snap;
        xSemaphoreGive(s_snapMutex);
    }
}

static SensorSnapshot readSnapshot() {
    SensorSnapshot snap{};
    if (xSemaphoreTake(s_snapMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = s_latestSnap;
        xSemaphoreGive(s_snapMutex);
    }
    return snap;
}

static void publishSharedStateFromControl() {
    if (xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_sharedState.config = g_config;
        s_sharedState.netConfig = g_netConfig;
        s_sharedState.budget = g_budget;
        memcpy(s_sharedState.cycles, g_cycles, sizeof(g_cycles));
        memcpy(s_sharedState.trends, g_trendStates, sizeof(g_trendStates));
        s_sharedState.duskPhase = g_duskDetector.phase;
        xSemaphoreGive(s_stateMutex);
    }
}

static void publishSharedNetStatus(bool wifiConnected) {
    if (xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_sharedState.netConfig = g_netConfig;
        s_sharedState.wifiConnected = wifiConnected;
        xSemaphoreGive(s_stateMutex);
    }
}

static SharedState readSharedState() {
    SharedState state{};
    if (xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        state = s_sharedState;
        xSemaphoreGive(s_stateMutex);
    }
    return state;
}

static void setSelectedPotFromUi(uint8_t selectedPot) {
    if (xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_sharedState.selectedPot = selectedPot;
        xSemaphoreGive(s_stateMutex);
    }
}

// ============================================================================
// ConfigTask — debounced NVS save (niski priorytet)
// PLAN.md → "Zapis asynchroniczny (nie blokować ControlTask)"
// ============================================================================

static void configTaskFn(void* /*param*/) {
    Config pending;
    bool   hasPending   = false;
    uint32_t lastReqMs  = 0;
    constexpr uint32_t kDebounceMs = 500;

    for (;;) {
        Config incoming;
        // Czekaj na request z max 100ms timeout (poll)
        if (xQueueReceive(s_saveQueue, &incoming, pdMS_TO_TICKS(100)) == pdTRUE) {
            pending    = incoming;
            hasPending = true;
            lastReqMs  = millis();
        }

        if (hasPending && (millis() - lastReqMs) >= kDebounceMs) {
            bool ok = configSave(pending);
            Serial.printf("[CFG] save %s\n", ok ? "OK" : "FAIL");
            hasPending = false;
        }
    }
}

// Helper: kolejkuj zapis configu (nie blokuje)
static void requestConfigSave(const Config& cfg) {
    if (s_saveQueue) {
        if (xQueueSend(s_saveQueue, &cfg, 0) != pdTRUE) {
            xQueueReset(s_saveQueue);
            xQueueSend(s_saveQueue, &cfg, 0);
        }
    }
}

// ============================================================================
// RuntimeState — snapshot builder + save helper
// ============================================================================
static void buildRuntimeState(RuntimeState& rs) {
    rs.schema = kRuntimeSchema;
    uint32_t now = millis();

    // Budget
    rs.reservoirCurrentMl = g_budget.reservoirCurrentMl;
    rs.totalPumpedMl      = g_budget.totalPumpedMl;
    for (uint8_t i = 0; i < kMaxPots; ++i)
        rs.totalPumpedMlPerPot[i] = g_budget.totalPumpedMlPerPot[i];
    rs.reservoirLow       = g_budget.reservoirLow;

    // Refill timestamp — convert millis to seconds-ago
    if (g_budget.lastRefillMs > 0) {
        rs.secsSinceRefill = (now - g_budget.lastRefillMs) / 1000;
    } else {
        rs.secsSinceRefill = 0;
    }

    // Trend baselines
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        rs.normalDryingRate[i]    = g_trendStates[i].normalDryingRate;
        rs.baselineCalibrated[i]  = g_trendStates[i].baselineCalibrated;
        rs.trendHeadIdx[i]        = g_trendStates[i].headIdx;
        rs.trendCount[i]          = g_trendStates[i].count;
        for (uint8_t h = 0; h < TrendState::kHours; ++h)
            rs.hourlyDeltas[i][h] = g_trendStates[i].hourlyDeltas[h];
    }

    // Cooldowns — convert millis timestamp to seconds-ago
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        if (g_actuator.lastCycleDoneMs[i] > 0) {
            rs.secsSinceLastCycleDone[i] = (now - g_actuator.lastCycleDoneMs[i]) / 1000;
        } else {
            rs.secsSinceLastCycleDone[i] = 0;
        }
    }

    // Solar clock
    rs.solarCycleCount = g_solarClock.cycleCount;
    rs.solarCalibrated = g_solarClock.calibrated;
}

static bool s_runtimeDirty = false;   // set true when state changes

static void saveRuntimeIfDirty(uint32_t nowMs, uint32_t& lastSaveMs) {
    if (!s_runtimeDirty) return;
    if (nowMs - lastSaveMs < 60000) return;  // max once per 60s

    RuntimeState rs;
    buildRuntimeState(rs);
    if (runtimeStateSave(rs)) {
        Serial.printf("[RUNTIME] saved: reservoir=%.0fml pumped=%.1fml\n",
                      rs.reservoirCurrentMl, rs.totalPumpedMl);
    }
    s_runtimeDirty = false;
    lastSaveMs = nowMs;
}

// Force immediate save (e.g. after pump event)
static void saveRuntimeNow(uint32_t& lastSaveMs) {
    RuntimeState rs;
    buildRuntimeState(rs);
    if (runtimeStateSave(rs)) {
        Serial.printf("[RUNTIME] saved (event): reservoir=%.0fml pumped=%.1fml\n",
                      rs.reservoirCurrentMl, rs.totalPumpedMl);
    }
    s_runtimeDirty = false;
    lastSaveMs = millis();
}

static bool forcePumpOffWithRetry(uint8_t potIdx, uint32_t nowMs,
                                  const char* reason,
                                  uint8_t retries = 3) {
    PumpActuator& pump = g_hardware.pump(potIdx);
    for (uint8_t attempt = 0; attempt < retries; ++attempt) {
        if (pump.off(nowMs, reason)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    Serial.printf("[POT%d] SAFETY_BLOCK reason=pump_off_failed src=%s\n",
                  potIdx, reason ? reason : "unknown");
    return false;
}

// ============================================================================
// ControlTask — odczyty sensorów, FSM podlewania, safety, analiza
// PLAN.md → "ControlTask"
// ============================================================================

static void controlTaskFn(void* /*param*/) {
    Serial.println("[CTRL] started");

    // Inicjalizacja EMA per-pot
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        g_emaFilters[i].alpha = g_config.pots[i].moistureEmaAlpha;
        g_emaFilters[i].reset();
    }

    // Inicjalizacja budżetu rezerwuaru z configu
    g_budget.reservoirCapacityMl     = g_config.reservoirCapacityMl;
    g_budget.reservoirLowThresholdMl = g_config.reservoirLowThresholdMl;

    // ── Restore RuntimeState from NVS (budget, trends, cooldowns, solar) ──
    RuntimeState rs;
    if (runtimeStateLoad(rs)) {
        // Budget
        g_budget.reservoirCurrentMl      = rs.reservoirCurrentMl;
        g_budget.totalPumpedMl           = rs.totalPumpedMl;
        for (uint8_t i = 0; i < kMaxPots; ++i)
            g_budget.totalPumpedMlPerPot[i] = rs.totalPumpedMlPerPot[i];
        g_budget.reservoirLow            = rs.reservoirLow;

        // Refill timestamp — convert seconds-ago to millis
        if (rs.secsSinceRefill > 0) {
            uint32_t bootMs = millis();
            uint32_t elapsedMs = rs.secsSinceRefill * 1000UL;
            g_budget.lastRefillMs = (elapsedMs < bootMs) ? (bootMs - elapsedMs) : 1;
            Serial.printf("[CTRL] Refill was %ds ago\n", rs.secsSinceRefill);
        } else {
            g_budget.lastRefillMs = millis();  // first boot — treat as fresh refill
        }

        // Sanity: if capacity changed in config, clamp
        if (g_budget.reservoirCurrentMl > g_config.reservoirCapacityMl)
            g_budget.reservoirCurrentMl = g_config.reservoirCapacityMl;

        Serial.printf("[CTRL] Budget restored: current=%.0fml pumped=%.1fml low=%s\n",
                      g_budget.reservoirCurrentMl, g_budget.totalPumpedMl,
                      g_budget.reservoirLow ? "YES" : "no");

        // Trend baselines (per-pot)
        for (uint8_t i = 0; i < kMaxPots; ++i) {
            g_trendStates[i].normalDryingRate    = rs.normalDryingRate[i];
            g_trendStates[i].baselineCalibrated  = rs.baselineCalibrated[i];
            g_trendStates[i].headIdx             = rs.trendHeadIdx[i];
            g_trendStates[i].count               = rs.trendCount[i];
            for (uint8_t h = 0; h < TrendState::kHours; ++h)
                g_trendStates[i].hourlyDeltas[h] = rs.hourlyDeltas[i][h];
            if (rs.baselineCalibrated[i]) {
                Serial.printf("[CTRL] Trend[%d] restored: baseline=%.2f%%/h samples=%d\n",
                              i, rs.normalDryingRate[i], rs.trendCount[i]);
            }
        }

        // Cooldowns — convert seconds-since-last-cycle to millis timestamp
        uint32_t bootMs = millis();
        for (uint8_t i = 0; i < kMaxPots; ++i) {
            if (rs.secsSinceLastCycleDone[i] > 0) {
                // Reconstruct as if the cycle ended (now - elapsed) ago
                uint32_t elapsedMs = rs.secsSinceLastCycleDone[i] * 1000UL;
                if (elapsedMs < bootMs) {
                    g_actuator.lastCycleDoneMs[i] = bootMs - elapsedMs;
                } else {
                    g_actuator.lastCycleDoneMs[i] = 1;  // long ago, but not zero (zero = never)
                }
                Serial.printf("[CTRL] Cooldown[%d] restored: %ds ago\n",
                              i, rs.secsSinceLastCycleDone[i]);
            }
        }

        // Solar clock
        g_solarClock.cycleCount = rs.solarCycleCount;
        g_solarClock.calibrated = rs.solarCalibrated;
        g_solarClock.dayLengthMs = g_duskDetector.dayLengthMs;
        g_solarClock.nightLengthMs = g_duskDetector.nightLengthMs;
    } else {
        // No saved state — assume full reservoir (first boot or post-reset)
        g_budget.reservoirCurrentMl = g_config.reservoirCapacityMl;
        g_budget.lastRefillMs = millis();  // treat first boot as fresh refill
        Serial.println("[CTRL] No saved runtime state — budget=full, trends=empty");
    }

    // Restore dusk detector phase from NVS (persisted across reboots)
    if (!duskStateLoad(g_duskDetector)) {
        Serial.println("[CTRL] No saved dusk state — starting as NIGHT");
    }

    // Tick counters
    uint32_t lastTick10ms  = millis();
    uint32_t lastTick100ms = millis();
    uint32_t lastTick1s    = millis();
    uint32_t lastLogDump   = millis() - 25000;   // first status dump after ~5s
    uint32_t lastEspAliveLog = millis() - 8000;
    uint32_t lastRuntimeSave = millis();
    float    prevTotalPumped = g_budget.totalPumpedMl;  // track pump events

    SensorSnapshot snap{};
    bool duskBootstrapped = false;  // one-shot lux-based phase check

    for (;;) {
        uint32_t now = millis();

        // --- Generuj ticki ---
        if (now - lastTick10ms >= 10) {
            lastTick10ms = now;
            // 10ms: odczyt Dual Button, manual pump
            DualButtonState dualBtn = g_hardware.dualButton().read(now);
            SharedState shared = readSharedState();
            manualPumpTick(now, dualBtn, snap, g_config, g_manual,
                           shared.selectedPot, g_budget, g_hardware);
        }

        if (now - lastTick100ms >= 100) {
            lastTick100ms = now;

            // 100ms: pełny odczyt sensorów
            g_hardware.readAllSensors(now, g_config, snap);

            // One-shot: bootstrap dusk phase from first valid lux reading
            if (!duskBootstrapped && snap.env.lux > 0.0f) {
                duskBootstrap(g_duskDetector, snap.env.lux);
                duskBootstrapped = true;
            }

            // EMA filtracja moisture per-pot
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                if (!g_config.pots[i].enabled) continue;
                snap.pots[i].moistureEma =
                    g_emaFilters[i].update(snap.pots[i].moisturePct, now);
            }

            // Publikuj snapshot (thread-safe)
            publishSnapshot(snap);

            // FSM podlewania (AUTO mode)
            if (g_config.mode == Mode::AUTO) {
                wateringTick(now, snap, g_config, g_cycles, g_budget,
                             g_actuator, g_hardware);
            } else {
                // Mode != AUTO → abort any active watering cycles
                for (uint8_t i = 0; i < g_config.numPots; ++i) {
                    if (g_cycles[i].phase != WateringPhase::IDLE) {
                        if (g_cycles[i].phase == WateringPhase::PULSE &&
                            g_hardware.pump(i).isOn()) {
                            forcePumpOffWithRetry(i, now, "mode_switch");
                        }
                        Serial.printf("[POT%d] CYCLE_ABORT reason=mode_switch phase=%d\n",
                                      i, static_cast<int>(g_cycles[i].phase));
                        g_cycles[i].reset();
                    }
                }
            }

            // Hardware-level pump safety — niezależnie od trybu/FSM
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                PumpActuator& p = g_hardware.pump(i);
                if (p.isOn() && p.onDuration(now) > g_config.pumpOnMsMax) {
                    forcePumpOffWithRetry(i, now, "HW_SAFETY_TIMEOUT");
                    Serial.printf("[POT%d] HW_SAFETY: pump forced off after %dms\n",
                                  i, p.onDuration(now));
                }
            }

            // Budżet rezerwuaru
            updateWaterBudget(now, snap, g_budget, g_config);

            publishSharedStateFromControl();

            // ── Runtime state save triggers ──
            // Immediate save when water was pumped (budget changed materially)
            if (g_budget.totalPumpedMl != prevTotalPumped) {
                prevTotalPumped = g_budget.totalPumpedMl;
                s_runtimeDirty = true;
                // Save immediately after pump event (critical data)
                saveRuntimeNow(lastRuntimeSave);
            }
        }

        // Periodic runtime state save (every 60s if anything changed)
        if (now - lastTick1s < 100) {  // only check at ~1Hz to avoid churn
            saveRuntimeIfDirty(now, lastRuntimeSave);
        }

        if (now - lastTick1s >= 1000) {
            lastTick1s = now;

            // Trend analysis per-pot
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                if (!g_config.pots[i].enabled) continue;
                bool wasCal = g_trendStates[i].baselineCalibrated;
                uint8_t prevCount = g_trendStates[i].count;
                trendTick(now, snap.pots[i].moistureEma,
                          g_trendStates[i], g_config);
                // Mark dirty if trend data changed (new hourly delta or baseline learned)
                if (g_trendStates[i].count != prevCount ||
                    g_trendStates[i].baselineCalibrated != wasCal) {
                    s_runtimeDirty = true;
                }
            }

            // Dusk detector
            duskDetectorTick(now,
                             snap.env.lux,
                             snap.env.tempC,
                             snap.env.humidityPct,
                             snap.env.pressureHpa,
                             g_duskDetector, g_config);
            updateSolarClock(g_duskDetector, g_solarClock);

            // Sensor history
            SensorSample sample{};
            sample.timestampMs = now;
            uint32_t rawSum = 0;
            uint8_t rawCount = 0;
            bool anyOverflow = false;
            for (uint8_t pi = 0; pi < g_config.numPots; ++pi) {
                if (!g_config.pots[pi].enabled) continue;
                rawSum += snap.pots[pi].moistureRaw;
                rawCount++;
                if (snap.pots[pi].waterGuards.potMax == WaterLevelState::TRIGGERED) {
                    anyOverflow = true;
                }
            }
            sample.moistureRaw = (rawCount > 0)
                ? static_cast<uint16_t>(rawSum / rawCount)
                : 0;
            sample.tempC_x10  = (int16_t)(snap.env.tempC * 10);
            sample.lux         = (uint16_t)fminf(snap.env.lux, 65535.0f);
            sample.flags       = 0;
            if (g_budget.reservoirLow)          sample.flags |= 0x01;
            if (anyOverflow)                    sample.flags |= 0x02;
            for (uint8_t i = 0; i < kMaxPots; ++i) {
                if (g_hardware.pump(i).isOn())  sample.flags |= 0x04;
            }
            historyTick(now, sample, g_history);

            publishSharedStateFromControl();
        }

        // ==================== 30s PERIODIC STATUS DUMP ====================
        if (now - lastLogDump >= 30000) {
            lastLogDump = now;
            uint32_t upSec = now / 1000;

            Serial.println("----- STATUS -----");
            Serial.printf("[SYS] uptime=%dm%ds heap=%d freeMin=%d\n",
                          upSec / 60, upSec % 60,
                          (int)ESP.getFreeHeap(), (int)ESP.getMinFreeHeap());
            Serial.printf("[SYS] mode=%s numPots=%d vacation=%s\n",
                          g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                          g_config.numPots,
                          g_config.vacationMode ? "ON" : "OFF");

            // Env sensors
            Serial.printf("[ENV] temp=%.1fC hum=%.1f%% lux=%.0f press=%.1fhPa\n",
                          snap.env.tempC, snap.env.humidityPct,
                          snap.env.lux, snap.env.pressureHpa);

            // Per-pot
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                if (!g_config.pots[i].enabled) continue;
                const auto& ps = snap.pots[i];
                const auto& cyc = g_cycles[i];
                const char* phaseStr = "?";
                switch (cyc.phase) {
                    case WateringPhase::IDLE:          phaseStr = "IDLE"; break;
                    case WateringPhase::EVALUATING:    phaseStr = "EVAL"; break;
                    case WateringPhase::PULSE:         phaseStr = "PULSE"; break;
                    case WateringPhase::SOAK:          phaseStr = "SOAK"; break;
                    case WateringPhase::MEASURING:     phaseStr = "MEAS"; break;
                    case WateringPhase::OVERFLOW_WAIT: phaseStr = "OFLOW"; break;
                    case WateringPhase::DONE:          phaseStr = "DONE"; break;
                    case WateringPhase::BLOCKED:       phaseStr = "BLOCK"; break;
                }
                Serial.printf("[POT%d] raw=%d comp=%.0f pct=%.1f%% ema=%.1f%% phase=%s\n",
                              i, ps.moistureRaw, ps.moistureComp,
                              ps.moisturePct, ps.moistureEma, phaseStr);

                // When IDLE in AUTO mode — show WHY not watering
                if (cyc.phase == WateringPhase::IDLE && g_config.mode == Mode::AUTO) {
                    const PlantProfile& pr = getActiveProfile(g_config, i);
                    float effTarget = pr.targetMoisturePct;
                    if (g_config.vacationMode) {
                        effTarget -= g_config.vacationTargetReductionPct;
                        if (effTarget < 5.0f) effTarget = 5.0f;
                    }
                    float trigPct = effTarget - pr.hysteresisPct;

                    // Cooldown?
                    uint32_t effCd = g_config.cooldownMs;
                    if (g_config.vacationMode)
                        effCd = static_cast<uint32_t>(effCd * g_config.vacationCooldownMultiplier);
                    bool cooling = (g_actuator.lastCycleDoneMs[i] != 0) &&
                                   (now - g_actuator.lastCycleDoneMs[i] < effCd);

                    const char* idleReason = "moisture_ok";
                    if (cooling)
                        idleReason = "COOLDOWN";
                    else if (snap.env.tempC > g_config.heatBlockTempC)
                        idleReason = "HEAT_BLOCK";
                    else if (snap.env.lux > g_config.directSunLuxThreshold)
                        idleReason = "DIRECT_SUN";
                    else if (ps.moisturePct >= trigPct)
                        idleReason = "moisture_ok";
                    else
                        idleReason = "SHOULD_START";   // something wrong if we see this

                    Serial.printf("[POT%d] idle: target=%.0f%% trigger=%.0f%% profile=%s puls=%dml reason=%s\n",
                                  i, effTarget, trigPct, pr.name,
                                  g_config.pots[i].pulseWaterMl, idleReason);
                    if (cooling) {
                        uint32_t cdLeft = effCd - (now - g_actuator.lastCycleDoneMs[i]);
                        Serial.printf("[POT%d] cooldown_remaining=%ds\n", i, cdLeft / 1000);
                    }
                }

                Serial.printf("[POT%d] overflow=%s reservoir=%s crosstalk=%s\n",
                              i,
                              ps.waterGuards.potMax == WaterLevelState::OK ? "OK" :
                              ps.waterGuards.potMax == WaterLevelState::TRIGGERED ? "TRIG" : "UNK",
                              ps.waterGuards.reservoirMin == WaterLevelState::OK ? "OK" :
                              ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED ? "TRIG" : "UNK",
                              ps.crosstalkUplift ? "yes" : "no");
                if (cyc.phase != WateringPhase::IDLE && cyc.phase != WateringPhase::DONE) {
                    Serial.printf("[POT%d] pulse=%d/%d pumped=%.1fml phaseSince=%ds\n",
                                  i, cyc.pulseCount, cyc.maxPulses,
                                  cyc.totalPumpedMl,
                                  (int)((now - cyc.phaseStartMs) / 1000));
                }
                // Trend
                const auto& tr = g_trendStates[i];
                if (tr.count > 0) {
                    Serial.printf("[POT%d] trend rate=%.2f%%/h baseline=%.2f cal=%s samples=%d\n",
                                  i, trendCurrentRate(i), tr.normalDryingRate,
                                  tr.baselineCalibrated ? "yes" : "no", tr.count);
                }
            }

            // Budget
            Serial.printf("[BUDGET] current=%.0fml total_pumped=%.1fml capacity=%.0fml low=%s\n",
                          g_budget.reservoirCurrentMl, g_budget.totalPumpedMl,
                          g_budget.reservoirCapacityMl,
                          g_budget.reservoirLow ? "YES" : "no");

            // Dusk detector
            const char* duskStr = "?";
            switch (g_duskDetector.phase) {
                case DuskPhase::NIGHT:           duskStr = "NIGHT"; break;
                case DuskPhase::DAWN_TRANSITION: duskStr = "DAWN_TR"; break;
                case DuskPhase::DAY:             duskStr = "DAY"; break;
                case DuskPhase::DUSK_TRANSITION: duskStr = "DUSK_TR"; break;
            }
            Serial.printf("[DUSK] phase=%s dawnScore=%.2f duskScore=%.2f samples=%d\n",
                          duskStr, g_duskDetector.dawnScore, g_duskDetector.duskScore,
                          g_duskDetector.count);
            if (g_solarClock.calibrated) {
                Serial.printf("[SOLAR] day=%dh%dm night=%dh%dm cycles=%d\n",
                              g_solarClock.dayLengthMs / 3600000,
                              (g_solarClock.dayLengthMs / 60000) % 60,
                              g_solarClock.nightLengthMs / 3600000,
                              (g_solarClock.nightLengthMs / 60000) % 60,
                              g_solarClock.cycleCount);
            }

            Serial.println("------------------");
            Serial.flush();  // ensure STATUS dump is sent
        }

        // Independent heartbeat via ESP logger (same path as WiFiGeneric logs)
        if (now - lastEspAliveLog >= 10000) {
            lastEspAliveLog = now;
            SharedState shared = readSharedState();
            ESP_LOGI(kLogTag, "ctrl alive, mode=%s, wifi=%s, heap=%u",
                     g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                     shared.wifiConnected ? "up" : "down",
                     (unsigned)ESP.getFreeHeap());
        }

        // --- Obsługa eventów (non-blocking) ---
        Event evt{};
        while (g_eventQueue.pop(evt, 0)) {
            switch (evt.type) {
                case EventType::REQUEST_SET_MODE:
                    g_config.mode = static_cast<Mode>(evt.payload.config.valueU16);
                    requestConfigSave(g_config);
                    Serial.printf("[CTRL] mode=%d\n", (int)g_config.mode);
                    publishSharedStateFromControl();
                    break;

                case EventType::REQUEST_SET_PLANT: {
                    uint8_t pot = evt.payload.config.key;
                    uint8_t prof = evt.payload.config.valueU16;
                    if (pot < kMaxPots && prof < kNumProfiles) {
                        g_config.pots[pot].plantProfileIndex = prof;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pot%d profile=%d\n", pot, prof);
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_NUM_POTS: {
                    uint8_t n = evt.payload.config.valueU16;
                    if (n >= 1 && n <= kMaxPots) {
                        g_config.numPots = n;
                        for (uint8_t i = 0; i < n; ++i)
                            g_config.pots[i].enabled = true;
                        for (uint8_t i = n; i < kMaxPots; ++i)
                            g_config.pots[i].enabled = false;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] numPots=%d\n", n);
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_RESERVOIR: {
                    float cap = evt.payload.config.valueF;
                    if (cap >= 1000.0f && cap <= 50000.0f) {
                        g_config.reservoirCapacityMl = cap;
                        g_budget.reservoirCapacityMl = cap;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] reservoir=%.0f ml\n", cap);
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_RESERVOIR_LOW: {
                    float low = evt.payload.config.valueF;
                    if (low >= 100.0f && low < g_config.reservoirCapacityMl) {
                        g_config.reservoirLowThresholdMl = low;
                        g_budget.reservoirLowThresholdMl = low;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] reservoir_low=%.0f ml\n", low);
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_PULSE_ML: {
                    uint16_t pulseMl = evt.payload.config.valueU16;
                    if (pulseMl >= 10 && pulseMl <= 100) {
                        for (uint8_t pi = 0; pi < kMaxPots; ++pi) {
                            g_config.pots[pi].pulseWaterMl = pulseMl;
                        }
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pulse=%uml\n", pulseMl);
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_VACATION: {
                    bool enable = evt.payload.config.valueU16 != 0;
                    handleVacationToggle(enable, g_config);
                    requestConfigSave(g_config);
                    Serial.printf("[CTRL] vacation=%d\n", g_config.vacationMode);
                    publishSharedStateFromControl();
                    break;
                }

                case EventType::REQUEST_REFILL:
                    handleRefill(g_budget, g_config);
                    s_runtimeDirty = true;
                    saveRuntimeNow(lastRuntimeSave);
                    Serial.println("[CTRL] reservoir refilled + runtime saved");
                    publishSharedStateFromControl();
                    break;

                case EventType::REQUEST_VACATION_TOGGLE:
                    handleVacationToggle(!g_config.vacationMode, g_config);
                    requestConfigSave(g_config);
                    Serial.printf("[CTRL] vacation=%d\n", g_config.vacationMode);
                    publishSharedStateFromControl();
                    break;

                case EventType::REQUEST_START_WIFI_SETUP:
                    if (!g_netState.apActive) {
                        startApNonBlocking(g_netConfig, g_netState);
                        publishSharedNetStatus(g_netState.wifiConnected);
                    }
                    break;

                case EventType::SYSTEM_FACTORY_RESET:
                    Serial.println("[CTRL] FACTORY RESET");
                    configFactoryReset();
                    delay(500);
                    ESP.restart();
                    break;

                case EventType::CONFIG_SAVE_REQUEST:
                    // Jeśli numPots wzrosło, reinicjalizuj hardware nowych potów
                    for (uint8_t pi = 0; pi < g_config.numPots; ++pi) {
                        if (g_config.pots[pi].enabled && !g_hardware.soilSensor(pi).isReady()) {
                            g_hardware.initPot(pi, g_hwConfig, g_config);
                            g_emaFilters[pi].alpha = g_config.pots[pi].moistureEmaAlpha;
                            g_emaFilters[pi].reset();
                        }
                    }
                    requestConfigSave(g_config);
                    publishSharedStateFromControl();
                    break;

                default:
                    break;
            }
        }

        // Yield — krótki sleep, scheduler rozdziela czas
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ============================================================================
// UiTask — ekran, przyciski M5, factory reset
// PLAN.md → "UiTask"
// ============================================================================

static uint32_t s_factoryResetHoldStartMs = 0;

static void checkFactoryReset(uint32_t nowMs) {
    // BtnA + BtnB jednocześnie przez 5 sekund → factory reset
    bool btnA = M5.BtnA.isPressed();
    bool btnB = M5.BtnB.isPressed();

    if (btnA && btnB) {
        if (s_factoryResetHoldStartMs == 0) {
            s_factoryResetHoldStartMs = nowMs;
        } else if ((nowMs - s_factoryResetHoldStartMs) >= 5000) {
            // Wyświetl pytanie na ekranie
            M5.Display.fillScreen(0x0000);
            M5.Display.setTextColor(0xF800);  // RED
            M5.Display.setCursor(10, 50);
            M5.Display.println("FACTORY RESET?");
            M5.Display.setTextColor(0xFFFF);
            M5.Display.setCursor(10, 80);
            M5.Display.println("BtnA = Confirm");
            M5.Display.setCursor(10, 100);
            M5.Display.println("Wait = Cancel");

            // Czekaj na puszczenie
            while (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
                M5.update();
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            // Czekaj na potwierdzenie (10s timeout)
            uint32_t deadline = millis() + 10000;
            while (millis() < deadline) {
                M5.update();
                if (M5.BtnA.wasClicked()) {
                    Serial.println("[UI] Factory reset confirmed");
                    g_eventQueue.push(Event{EventType::SYSTEM_FACTORY_RESET});
                    // Wyświetl komunikat i czekaj na restart z ControlTask
                    M5.Display.fillScreen(0x0000);
                    M5.Display.setCursor(10, 60);
                    M5.Display.setTextColor(0x07E0);
                    M5.Display.println("Reset done!");
                    M5.Display.println("Restarting...");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            // Timeout — cancelled
            M5.Display.fillScreen(0x0000);
            M5.Display.setCursor(10, 60);
            M5.Display.println("Reset cancelled.");
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_factoryResetHoldStartMs = 0;
        }
    } else {
        s_factoryResetHoldStartMs = 0;
    }
}

static void dispatchUiSettingsChanges(const Config& beforeCfg,
                                      const Config& afterCfg,
                                      const WaterBudget& beforeBudget,
                                      const WaterBudget& afterBudget,
                                      bool changed) {
    bool anyRequest = false;
    Event evt{};

    if (beforeCfg.numPots != afterCfg.numPots) {
        evt.type = EventType::REQUEST_SET_NUM_POTS;
        evt.payload.config.valueU16 = afterCfg.numPots;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    for (uint8_t i = 0; i < kMaxPots; ++i) {
        if (beforeCfg.pots[i].plantProfileIndex != afterCfg.pots[i].plantProfileIndex) {
            evt = Event{};
            evt.type = EventType::REQUEST_SET_PLANT;
            evt.payload.config.key = i;
            evt.payload.config.valueU16 = afterCfg.pots[i].plantProfileIndex;
            g_eventQueue.push(evt);
            anyRequest = true;
        }
    }

    if (beforeCfg.reservoirCapacityMl != afterCfg.reservoirCapacityMl) {
        evt = Event{};
        evt.type = EventType::REQUEST_SET_RESERVOIR;
        evt.payload.config.valueF = afterCfg.reservoirCapacityMl;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (beforeCfg.reservoirLowThresholdMl != afterCfg.reservoirLowThresholdMl) {
        evt = Event{};
        evt.type = EventType::REQUEST_SET_RESERVOIR_LOW;
        evt.payload.config.valueF = afterCfg.reservoirLowThresholdMl;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (beforeCfg.pots[0].pulseWaterMl != afterCfg.pots[0].pulseWaterMl) {
        evt = Event{};
        evt.type = EventType::REQUEST_SET_PULSE_ML;
        evt.payload.config.valueU16 = afterCfg.pots[0].pulseWaterMl;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (beforeCfg.mode != afterCfg.mode) {
        evt = Event{};
        evt.type = EventType::REQUEST_SET_MODE;
        evt.payload.config.valueU16 = static_cast<uint16_t>(afterCfg.mode);
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (beforeCfg.vacationMode != afterCfg.vacationMode) {
        evt = Event{};
        evt.type = EventType::REQUEST_SET_VACATION;
        evt.payload.config.valueU16 = afterCfg.vacationMode ? 1 : 0;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (beforeBudget.totalPumpedMl != afterBudget.totalPumpedMl &&
        afterBudget.totalPumpedMl == 0.0f) {
        evt = Event{};
        evt.type = EventType::REQUEST_REFILL;
        g_eventQueue.push(evt);
        anyRequest = true;
    }

    if (changed && !anyRequest) {
        evt = Event{};
        evt.type = EventType::REQUEST_START_WIFI_SETUP;
        g_eventQueue.push(evt);
    }
}

static void uiTaskFn(void* /*param*/) {
    Serial.println("[UI] started");
    uiInit();
    setSelectedPotFromUi(g_uiState.selectedPot);

    for (;;) {
        uint32_t now = millis();
        M5.update();
        SharedState shared = readSharedState();

        // --- Factory reset check ---
        checkFactoryReset(now);

        // --- Obsługa przycisków M5 (BtnA / BtnB) ---
        // BtnA: w MAIN → wejdź do Settings
        //       w SETTINGS → zmień wartosć wybranej opcji
        //       przytrzymanie w SETTINGS → wróć do MAIN
        static bool s_btnALongHandled = false;

        if (M5.BtnA.pressedFor(600) && !s_btnALongHandled) {
            s_btnALongHandled = true;
            if (g_uiState.screen == UiScreen::SETTINGS) {
                g_uiState.screen = UiScreen::MAIN;
                g_uiState.needsRedraw = true;
            }
        }
        if (!M5.BtnA.isPressed()) {
            s_btnALongHandled = false;
        }

        if (M5.BtnA.wasClicked()) {
            if (g_uiState.screen == UiScreen::MAIN) {
                g_uiState.screen = UiScreen::SETTINGS;
                g_uiState.settingsIndex = 0;
            } else {
                // W Settings: zmień wartość (select)
                Config editedCfg = shared.config;
                WaterBudget editedBudget = shared.budget;
                bool changed = uiHandleBtnBLong(g_uiState, editedCfg, editedBudget);
                dispatchUiSettingsChanges(shared.config, editedCfg,
                                          shared.budget, editedBudget, changed);
            }
            g_uiState.needsRedraw = true;
        }

        // BtnB: nawigacja (w Settings: następna opcja, w MAIN: przełącz widok)
        if (M5.BtnB.wasClicked()) {
            uiHandleBtnB(g_uiState, shared.config);
            setSelectedPotFromUi(g_uiState.selectedPot);
            g_uiState.needsRedraw = true;
        }

        // BtnB long press: alternatywna zmiana wartości w Settings
        static bool s_btnBLongHandled = false;
        if (M5.BtnB.pressedFor(800) && !s_btnBLongHandled) {
            s_btnBLongHandled = true;
            if (g_uiState.screen == UiScreen::SETTINGS) {
                Config editedCfg = shared.config;
                WaterBudget editedBudget = shared.budget;
                bool changed = uiHandleBtnBLong(g_uiState, editedCfg, editedBudget);
                dispatchUiSettingsChanges(shared.config, editedCfg,
                                          shared.budget, editedBudget, changed);
                g_uiState.needsRedraw = true;
            }
        }
        if (!M5.BtnB.isPressed()) {
            s_btnBLongHandled = false;
        }

        // --- Render (rate-limited) ---
        if (g_uiState.needsRedraw ||
            (now - g_uiState.lastRedrawMs) >= UiState::kMinRedrawIntervalMs) {

            // Zbuduj UiSnap
            UiSnap uSnap{};
            uSnap.sensors       = readSnapshot();
            memcpy(uSnap.cycles, shared.cycles, sizeof(shared.cycles));
            uSnap.budget        = shared.budget;
            uSnap.config        = shared.config;
            uSnap.netConfig     = shared.netConfig;
            uSnap.duskPhase     = shared.duskPhase;
            uSnap.wifiConnected = shared.wifiConnected;

            uiTick(now, g_uiState, uSnap);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// NetTask — WiFi reconnect, Telegram, heartbeat
// PLAN.md → "NetTask"
// ============================================================================

static void netTaskFn(void* /*param*/) {
    Serial.println("[NET] started");
    netTaskInit(g_netConfig, g_netState);

    for (;;) {
        uint32_t now = millis();

        netTaskTick(now, g_netState, g_netConfig);
        publishSharedNetStatus(g_netState.wifiConnected);

        // Heartbeat check
        if (g_netState.wifiConnected && g_netState.telegramEnabled) {
            SharedState shared = readSharedState();
            if (isDailyHeartbeatTime(now, g_solarClock, g_duskDetector,
                                     false /* NTP TODO */, g_netState)) {
                DailyReportData rptData{};
                rptData.sensors  = readSnapshot();
                rptData.budget   = shared.budget;
                memcpy(rptData.trends, shared.trends, sizeof(shared.trends));
                rptData.config   = shared.config;
                rptData.uptimeMs = now;

                char buf[512];
                formatDailyReport(rptData, buf, sizeof(buf));
                telegramSend(buf, g_netConfig);
                g_netState.heartbeatSentToday = true;
                Serial.println("[NET] heartbeat sent");
            }

            telegramPollCommands(now, g_netConfig);
        }

        // AP mode wymaga szybszego ticka (~100ms); normalny tryb co 1s
        vTaskDelay(pdMS_TO_TICKS(g_netState.apActive ? 100 : 1000));
    }
}

// ============================================================================
// Task stack sizes and priorities
// ============================================================================

static constexpr uint32_t kControlStackSize = 12288;
static constexpr uint32_t kUiStackSize      = 8192;
static constexpr uint32_t kNetStackSize     = 8192;
static constexpr uint32_t kConfigStackSize  = 4096;

static constexpr UBaseType_t kControlPriority = 5;
static constexpr UBaseType_t kUiPriority      = 3;
static constexpr UBaseType_t kNetPriority     = 2;
static constexpr UBaseType_t kConfigPriority  = 1;

// ============================================================================
// setup() — sekwencja boot (PLAN.md → "Sekwencja boot")
// ============================================================================

void setup() {
    // --- M5 init ---
    auto m5cfg = M5.config();
    M5.begin(m5cfg);
    M5.Power.setExtOutput(true);   // Grove 5V ON — wymagane dla PbHUB

    // USB CDC/Serial can be not-ready right after reset.
    // Force init and wait a short time so monitor can attach.
    Serial.begin(115200);
    uint32_t serialDeadline = millis() + 3000;
    while (!Serial && millis() < serialDeadline) {
        delay(10);
    }
    Serial.setDebugOutput(true);
    ESP_LOGI(kLogTag, "boot start, serial_ready=%d", (int)Serial);

    Serial.println("=== autogarden ===");
    Serial.println("[BOOT] M5 initialized, EXT 5V ON");
    Serial.flush();  // force output before any blocking operation

    // --- Boot splash — natychmiast po M5.begin(), przed blokowaniem WiFi/HW ---
    M5.Display.setRotation(0);
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0x07E0);  // green
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.drawString("autogarden", 67, 90);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7BEF);  // dim
    M5.Display.drawString("initializing...", 67, 120);
    M5.Display.setTextDatum(textdatum_t::top_left);
    Serial.println("[BOOT] splash shown");

    // --- Załaduj konfigurację domenową ---
    if (!configLoad(g_config)) {
        Serial.println("[BOOT] config load FAIL — using defaults");
        configLoadDefaults(g_config);
    }
    if (!configValidate(g_config)) {
        Serial.println("[BOOT] config invalid — falling back to defaults");
        configLoadDefaults(g_config);
    }
    Serial.printf("[BOOT] config OK: mode=%s numPots=%d\n",
                  g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                  g_config.numPots);

    // --- Załaduj konfigurację sieci ---
    if (!netConfigLoad(g_netConfig)) {
        Serial.println("[BOOT] netCfg load FAIL — provisioning needed");
    }

    // --- WiFi (OPCJONALNE — nie blokuje boota) ---
    // autogarden działa offline-first. WiFi próbowane w tle przez NetTask.
    if (g_netConfig.provisioned && strlen(g_netConfig.wifiSsid) > 0) {
        Serial.println("[BOOT] WiFi provisioned — trying quick connect...");
        WiFi.mode(WIFI_STA);
        WiFi.begin(g_netConfig.wifiSsid, g_netConfig.wifiPass);
        Serial.println("[BOOT] WiFi connect started — continue boot (NetTask handles retries)");
    } else {
        Serial.println("[BOOT] WiFi not provisioned — offline mode");
        Serial.println("[BOOT] Configure WiFi from Settings > WiFi");
    }

    // --- Inicjalizacja hardware ---
    if (!g_hardware.init(g_hwConfig, g_config)) {
        Serial.println("[BOOT] hardware init FAIL — continuing with degraded mode");
    } else {
        Serial.println("[BOOT] hardware OK");
    }

    // --- Inicjalizacja eventów i synchronizacji ---
    g_eventQueue.init();
    s_snapMutex = xSemaphoreCreateMutex();
    s_stateMutex = xSemaphoreCreateMutex();
    s_saveQueue = xQueueCreate(8, sizeof(Config));

    // Initial shared snapshot
    publishSharedStateFromControl();
    publishSharedNetStatus(false);

    Serial.println("[BOOT] creating FreeRTOS tasks...");

    // --- Tworzenie tasków (bez pinowania do rdzeni) ---
    xTaskCreate(controlTaskFn, "control", kControlStackSize, nullptr,
                kControlPriority, nullptr);

    xTaskCreate(uiTaskFn, "ui", kUiStackSize, nullptr,
                kUiPriority, nullptr);

    xTaskCreate(netTaskFn, "net", kNetStackSize, nullptr,
                kNetPriority, nullptr);

    xTaskCreate(configTaskFn, "config", kConfigStackSize, nullptr,
                kConfigPriority, nullptr);

    Serial.println("[BOOT] all tasks created — scheduler running");
    Serial.flush();

    // Wymuś event SYSTEM_BOOT
    g_eventQueue.push(Event{EventType::SYSTEM_BOOT});
}

// ============================================================================
// loop() — pusty, logika w taskach FreeRTOS
// ============================================================================

void loop() {
    vTaskDelay(portMAX_DELAY);
}
