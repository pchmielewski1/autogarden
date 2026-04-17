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
#include <cstdarg>
#include <cmath>

#include "events.h"
#include "config.h"
#include "hardware.h"
#include "watering.h"
#include "analysis.h"
#include "ui.h"
#include "network.h"
#include "log_serial.h"

#define Serial AGSerial

// ============================================================================
// Globalne instancje (zadeklarowane extern w headerach, zdefiniowane tutaj)
// ============================================================================

EventQueue      g_eventQueue;

// Config
Config          g_config;
NetConfig       g_netConfig;

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
    uint32_t      lastCycleDoneMs[kMaxPots] = {};
    float         pumped24hMl = 0.0f;
    float         pumped24hMlPerPot[kMaxPots] = {};
    uint16_t      wateringEvents24h = 0;
    uint16_t      wateringEvents24hPerPot[kMaxPots] = {};
    uint32_t      lastFeedbackSeq[kMaxPots] = {};
    WateringFeedbackCode lastFeedbackCode[kMaxPots] = {};
    float         lastFeedbackValue1[kMaxPots] = {};
    float         lastFeedbackValue2[kMaxPots] = {};
    uint8_t       lastFeedbackPulseCount[kMaxPots] = {};
    TrendState    trends[kMaxPots];
    DuskPhase     duskPhase = DuskPhase::NIGHT;
    bool          wifiConnected = false;
    uint8_t       selectedPot = 0;
};

static SemaphoreHandle_t s_stateMutex = nullptr;
static SharedState       s_sharedState;

// Config save request — debounced
static QueueHandle_t     s_saveQueue = nullptr;

struct TelegramFeedbackMessage {
    char text[160];
};

static QueueHandle_t     s_tgFeedbackQueue = nullptr;
static constexpr UBaseType_t kTelegramFeedbackDrainPerTick = 2;

struct WateringHistory24hSummary {
    float pumpedMl = 0.0f;
    float pumpedMlPerPot[kMaxPots] = {};
    uint16_t events = 0;
    uint16_t eventsPerPot[kMaxPots] = {};
};

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

static WateringHistory24hSummary summarizeWateringHistory24h(uint32_t nowMs) {
    WateringHistory24hSummary summary{};
    for (uint16_t i = 0; i < g_history.wateringLog.size(); ++i) {
        const WateringRecord& rec = g_history.wateringLog.at(i);
        if (rec.timestampMs == 0 || rec.timestampMs > nowMs) {
            continue;
        }
        if ((nowMs - rec.timestampMs) > 86400000UL) {
            continue;
        }

        float pumpedMl = rec.totalPumpedMl_x10 / 10.0f;
        summary.pumpedMl += pumpedMl;
        if (summary.events < 0xFFFFu) {
            summary.events++;
        }

        if (rec.potIndex < kMaxPots) {
            summary.pumpedMlPerPot[rec.potIndex] += pumpedMl;
            if (summary.eventsPerPot[rec.potIndex] < 0xFFFFu) {
                summary.eventsPerPot[rec.potIndex]++;
            }
        }
    }
    return summary;
}

static void publishSharedStateFromControl() {
    if (xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        WateringHistory24hSummary history24 = summarizeWateringHistory24h(millis());
        s_sharedState.config = g_config;
        s_sharedState.netConfig = g_netConfig;
        s_sharedState.budget = g_budget;
        memcpy(s_sharedState.cycles, g_cycles, sizeof(g_cycles));
        memcpy(s_sharedState.lastCycleDoneMs, g_actuator.lastCycleDoneMs, sizeof(g_actuator.lastCycleDoneMs));
        s_sharedState.pumped24hMl = history24.pumpedMl;
        memcpy(s_sharedState.pumped24hMlPerPot, history24.pumpedMlPerPot, sizeof(history24.pumpedMlPerPot));
        s_sharedState.wateringEvents24h = history24.events;
        memcpy(s_sharedState.wateringEvents24hPerPot, history24.eventsPerPot, sizeof(history24.eventsPerPot));
        memcpy(s_sharedState.lastFeedbackSeq, g_actuator.lastFeedbackSeq, sizeof(g_actuator.lastFeedbackSeq));
        memcpy(s_sharedState.lastFeedbackCode, g_actuator.lastFeedbackCode, sizeof(g_actuator.lastFeedbackCode));
        memcpy(s_sharedState.lastFeedbackValue1, g_actuator.lastFeedbackValue1, sizeof(g_actuator.lastFeedbackValue1));
        memcpy(s_sharedState.lastFeedbackValue2, g_actuator.lastFeedbackValue2, sizeof(g_actuator.lastFeedbackValue2));
        memcpy(s_sharedState.lastFeedbackPulseCount, g_actuator.lastFeedbackPulseCount, sizeof(g_actuator.lastFeedbackPulseCount));
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

static void queueTelegramFeedbackFmt(const char* fmt, ...) {
    if (!s_tgFeedbackQueue || !fmt) {
        return;
    }

    TelegramFeedbackMessage msg{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg.text, sizeof(msg.text), fmt, args);
    va_end(args);
    msg.text[sizeof(msg.text) - 1] = '\0';

    if (msg.text[0] == '\0') {
        return;
    }

    if (xQueueSend(s_tgFeedbackQueue, &msg, 0) != pdTRUE) {
        Serial.println("[TG] event=feedback_drop reason=queue_full");
    }
}

static const char* pumpOwnerName(PumpOwner owner) {
    switch (owner) {
        case PumpOwner::AUTO: return "AUTO";
        case PumpOwner::MANUAL: return "MANUAL";
        case PumpOwner::REMOTE: return "REMOTE";
        case PumpOwner::NONE:
        default:
            return "NONE";
    }
}

static const char* lightStateName(LightSignalState state) {
    switch (state) {
        case LightSignalState::VALID: return "VALID";
        case LightSignalState::STALE: return "STALE";
        case LightSignalState::RECOVERING: return "RECOVERING";
        case LightSignalState::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

static const char* waterLevelStateName(WaterLevelState state) {
    switch (state) {
        case WaterLevelState::OK: return "OK";
        case WaterLevelState::TRIGGERED: return "TRIG";
        case WaterLevelState::UNKNOWN:
        default:
            return "UNK";
    }
}

static const char* waterLevelPendingName(const WaterLevelStatus& status) {
    if (status.pendingTrip) return "TRIP";
    if (status.pendingClear) return "CLEAR";
    return "-";
}

static float effectiveTargetForPot(const Config& cfg, uint8_t potIdx) {
    const PlantProfile& prof = getActiveProfile(cfg, potIdx);
    float target = prof.targetMoisturePct;
    if (cfg.vacationMode) {
        target -= cfg.vacationTargetReductionPct;
        if (target < 5.0f) {
            target = 5.0f;
        }
    }
    return target;
}

static float effectiveMaxForPot(const Config& cfg, uint8_t potIdx) {
    const PlantProfile& prof = getActiveProfile(cfg, potIdx);
    return prof.maxMoisturePct;
}

static const char* remoteWaterRejectMessage(const char* reason) {
    if (!reason) return "water request was rejected by control.";
    if (strcmp(reason, "pot_disabled") == 0) return "water request blocked: pot is disabled.";
    if (strcmp(reason, "mode_manual") == 0) return "water request blocked: mode is MANUAL.";
    if (strcmp(reason, "cycle_active") == 0) return "water request blocked: watering is already active.";
    if (strcmp(reason, "cooldown") == 0) return "water request blocked: cooldown is still active.";
    if (strcmp(reason, "PUMP_CONFIG_INVALID") == 0) return "water request blocked: pump parameters are invalid.";
    if (strcmp(reason, "OVERFLOW_RISK") == 0) return "water request blocked: overflow sensor is triggered.";
    if (strcmp(reason, "TANK_EMPTY") == 0 || strcmp(reason, "RESERVOIR_EMPTY") == 0) return "water request blocked: reservoir is empty.";
    if (strcmp(reason, "OVERFLOW_SENSOR_UNKNOWN") == 0) return "water request blocked: overflow sensor state is unknown.";
    if (strcmp(reason, "TANK_SENSOR_UNKNOWN") == 0) return "water request blocked: reservoir sensor state is unknown.";
    return "water request was rejected by control.";
}

static void publishWateringFeedback(const uint32_t* prevFeedbackSeq) {
    for (uint8_t i = 0; i < g_config.numPots; ++i) {
        if (g_actuator.lastFeedbackSeq[i] == prevFeedbackSeq[i]) {
            continue;
        }

        switch (g_actuator.lastFeedbackCode[i]) {
            case WateringFeedbackCode::CYCLE_START_SCHEDULE:
                queueTelegramFeedbackFmt("Pot %u: automatic cycle started at %.1f%% moisture.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i]);
                break;
            case WateringFeedbackCode::CYCLE_START_RESCUE:
                queueTelegramFeedbackFmt("Pot %u: rescue cycle started at critical moisture %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i]);
                break;
            case WateringFeedbackCode::SKIP_ALREADY_WET:
                queueTelegramFeedbackFmt("Pot %u: skipped, wet enough %.1f%% >= %.1f%% target.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i],
                                         g_actuator.lastFeedbackValue2[i]);
                break;
            case WateringFeedbackCode::SKIP_ABOVE_MAX:
                queueTelegramFeedbackFmt("Pot %u: skipped, above max %.1f%% >= %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i],
                                         g_actuator.lastFeedbackValue2[i]);
                break;
            case WateringFeedbackCode::OVERFLOW_DETECTED:
                queueTelegramFeedbackFmt("Pot %u: overflow detected after pulse %u, waiting before resume.",
                                         static_cast<unsigned>(i + 1),
                                         static_cast<unsigned>(g_actuator.lastFeedbackPulseCount[i]));
                break;
            case WateringFeedbackCode::OVERFLOW_RESUME:
                queueTelegramFeedbackFmt("Pot %u: overflow cleared, resuming below target %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue2[i]);
                break;
            case WateringFeedbackCode::TARGET_REACHED:
                queueTelegramFeedbackFmt("Pot %u: target reached %.1f%% after %u pulse(s).",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i],
                                         static_cast<unsigned>(g_actuator.lastFeedbackPulseCount[i]));
                break;
            case WateringFeedbackCode::STOP_MAX_EXCEEDED:
                queueTelegramFeedbackFmt("Pot %u: stopped, max moisture reached %.1f%% >= %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i],
                                         g_actuator.lastFeedbackValue2[i]);
                break;
            case WateringFeedbackCode::STOP_MAX_PULSES:
                queueTelegramFeedbackFmt("Pot %u: stopped after max pulses %u, moisture %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         static_cast<unsigned>(g_actuator.lastFeedbackPulseCount[i]),
                                         g_actuator.lastFeedbackValue1[i]);
                break;
            case WateringFeedbackCode::OVERFLOW_TIMEOUT:
                queueTelegramFeedbackFmt("Pot %u: stopped after overflow wait timeout (%.0fs / %.0fs).",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i],
                                         g_actuator.lastFeedbackValue2[i]);
                break;
            case WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_RISK:
                queueTelegramFeedbackFmt("Pot %u: blocked by safety, overflow risk.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::SAFETY_BLOCK_TANK_EMPTY:
            case WateringFeedbackCode::SAFETY_BLOCK_RESERVOIR_EMPTY:
                queueTelegramFeedbackFmt("Pot %u: blocked by safety, reservoir empty.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_SENSOR_UNKNOWN:
                queueTelegramFeedbackFmt("Pot %u: blocked by safety, overflow sensor state unknown.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::SAFETY_BLOCK_TANK_SENSOR_UNKNOWN:
                queueTelegramFeedbackFmt("Pot %u: blocked by safety, reservoir sensor state unknown.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::SAFETY_BLOCK_PUMP_CONFIG_INVALID:
                queueTelegramFeedbackFmt("Pot %u: blocked by safety, fixed pump parameters are invalid.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::HARD_TIMEOUT:
                queueTelegramFeedbackFmt("Pot %u: hard timeout, pump forced OFF after %.0fms.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i]);
                break;
            case WateringFeedbackCode::SAFETY_UNBLOCK:
                queueTelegramFeedbackFmt("Pot %u: safety block cleared.",
                                         static_cast<unsigned>(i + 1));
                break;
            case WateringFeedbackCode::CYCLE_DONE_GENERIC:
                queueTelegramFeedbackFmt("Pot %u: cycle finished, moisture now %.1f%%.",
                                         static_cast<unsigned>(i + 1),
                                         g_actuator.lastFeedbackValue1[i]);
                break;
            case WateringFeedbackCode::NONE:
            default:
                break;
        }
    }
}

static uint32_t hashMix(uint32_t hash, uint32_t v) {
    hash ^= v;
    hash *= 16777619u;
    return hash;
}

static uint32_t quantizeX10(float v) {
    return static_cast<uint32_t>(static_cast<int32_t>(v * 10.0f));
}

static uint32_t bucket(float v, float step) {
    if (step <= 0.0f) return 0;
    return static_cast<uint32_t>(static_cast<int32_t>(v / step));
}

static uint32_t statusDigest(const SensorSnapshot& snap) {
    uint32_t h = 2166136261u;
    h = hashMix(h, bucket(snap.env.tempC, 1.0f));
    h = hashMix(h, bucket(snap.env.humidityPct, 3.0f));
    h = hashMix(h, bucket(snap.env.lux, 500.0f));
    h = hashMix(h, bucket(snap.env.pressureHpa, 1.0f));
    h = hashMix(h, static_cast<uint32_t>(snap.env.lightState));
    h = hashMix(h, snap.env.luxAgeMs / 30000u);
    h = hashMix(h, bucket(g_budget.reservoirCurrentMl, 25.0f));
    h = hashMix(h, bucket(g_budget.totalPumpedMl, 5.0f));

    for (uint8_t i = 0; i < g_config.numPots; ++i) {
        if (!g_config.pots[i].enabled) continue;
        const auto& ps = snap.pots[i];
        h = hashMix(h, i);
        h = hashMix(h, bucket(ps.moisturePct, 3.0f));
        h = hashMix(h, bucket(ps.moistureEma, 3.0f));
        h = hashMix(h, static_cast<uint32_t>(ps.waterGuards.potMax));
        h = hashMix(h, static_cast<uint32_t>(ps.waterGuards.reservoirMin));
        h = hashMix(h, ps.waterGuards.potMaxStatus.pendingTrip ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.potMaxStatus.pendingClear ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.reservoirMinStatus.pendingTrip ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.reservoirMinStatus.pendingClear ? 1u : 0u);
    }
    return h;
}

static uint32_t statusCriticalDigest(const SensorSnapshot& snap) {
    uint32_t h = 2166136261u;
    h = hashMix(h, static_cast<uint32_t>(g_config.mode));
    h = hashMix(h, g_config.numPots);
    h = hashMix(h, g_config.vacationMode ? 1u : 0u);
    h = hashMix(h, g_budget.reservoirLow ? 1u : 0u);
    h = hashMix(h, static_cast<uint32_t>(g_duskDetector.phase));
    h = hashMix(h, static_cast<uint32_t>(snap.env.lightState));

    for (uint8_t i = 0; i < g_config.numPots; ++i) {
        if (!g_config.pots[i].enabled) continue;
        const auto& ps = snap.pots[i];
        const auto& cyc = g_cycles[i];
        h = hashMix(h, i);
        h = hashMix(h, static_cast<uint32_t>(cyc.phase));
        h = hashMix(h, cyc.pulseCount);
        h = hashMix(h, ps.waterGuards.potMax == WaterLevelState::TRIGGERED ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.potMaxStatus.pendingTrip ? 1u : 0u);
        h = hashMix(h, ps.waterGuards.reservoirMinStatus.pendingTrip ? 1u : 0u);
    }
    return h;
}

static uint32_t ctrlAliveDigest(Mode mode, bool wifiConnected, uint32_t freeHeap) {
    uint32_t h = 2166136261u;
    h = hashMix(h, static_cast<uint32_t>(mode));
    h = hashMix(h, wifiConnected ? 1u : 0u);
    h = hashMix(h, freeHeap / 8192u);
    return h;
}

// ============================================================================
// ConfigTask — debounced NVS save (niski priorytet)
// PLAN.md → "Zapis asynchroniczny (nie blokować ControlTask)"
// ============================================================================

static void configTaskFn(void* /*param*/) {
    static Config pending{};
    static Config incoming{};
    bool   hasPending   = false;
    uint32_t lastReqMs  = 0;
    constexpr uint32_t kDebounceMs = 500;

    for (;;) {
        // Czekaj na request z max 100ms timeout (poll)
        if (xQueueReceive(s_saveQueue, &incoming, pdMS_TO_TICKS(100)) == pdTRUE) {
            pending    = incoming;
            hasPending = true;
            lastReqMs  = millis();
        }

        if (hasPending && (millis() - lastReqMs) >= kDebounceMs) {
            bool ok = configSave(pending);
            Serial.printf("[CFG] event=save result=%s\n", ok ? "ok" : "fail");
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

    if (g_duskDetector.lastDuskMs > 0) {
        rs.secsSinceLastDusk = (now - g_duskDetector.lastDuskMs) / 1000;
    } else {
        rs.secsSinceLastDusk = 0;
    }

    if (g_duskDetector.lastDawnMs > 0) {
        rs.secsSinceLastDawn = (now - g_duskDetector.lastDawnMs) / 1000;
    } else {
        rs.secsSinceLastDawn = 0;
    }

    rs.nightSequence = g_duskDetector.nightSequence;

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
            g_actuator.currentPumpOwner[potIdx] = PumpOwner::NONE;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    Serial.printf("[POT%d] SAFETY_BLOCK reason=pump_off_failed src=%s\n",
                  potIdx, reason ? reason : "unknown");
    return false;
}

static bool startRemoteWateringPulse(uint32_t nowMs,
                                     uint8_t potIdx,
                                     const SensorSnapshot& snap,
                                     const char** rejectReason = nullptr) {
    if (rejectReason) {
        *rejectReason = nullptr;
    }
    if (potIdx >= g_config.numPots || !g_config.pots[potIdx].enabled) {
        if (rejectReason) *rejectReason = "pot_disabled";
        return false;
    }
    if (g_config.mode != Mode::AUTO) {
        Serial.printf("[POT%d] REMOTE_WATER_BLOCK reason=mode_manual\n", potIdx);
        if (rejectReason) *rejectReason = "mode_manual";
        return false;
    }
    if (g_cycles[potIdx].phase != WateringPhase::IDLE) {
        Serial.printf("[POT%d] REMOTE_WATER_BLOCK reason=cycle_active phase=%d\n",
                      potIdx, static_cast<int>(g_cycles[potIdx].phase));
        if (rejectReason) *rejectReason = "cycle_active";
        return false;
    }

    const PotConfig& potCfg = g_config.pots[potIdx];
    SafetyResult safety = evaluateExtendedSafety(nowMs, snap.pots[potIdx], g_config,
                                                 potCfg, g_budget, g_actuator, potIdx);
    if (safety.hardBlock) {
        Serial.printf("[POT%d] REMOTE_WATER_BLOCK reason=%s\n",
                      potIdx, safety.reason ? safety.reason : "safety");
        if (rejectReason) *rejectReason = safety.reason ? safety.reason : "safety";
        return false;
    }

    uint32_t effCooldown = g_config.cooldownMs;
    if (g_config.vacationMode) {
        effCooldown = static_cast<uint32_t>(effCooldown * g_config.vacationCooldownMultiplier);
    }
    if (g_actuator.lastCycleDoneMs[potIdx] != 0 &&
        (nowMs - g_actuator.lastCycleDoneMs[potIdx]) < effCooldown) {
        Serial.printf("[POT%d] REMOTE_WATER_BLOCK reason=cooldown\n", potIdx);
        if (rejectReason) *rejectReason = "cooldown";
        return false;
    }
    if (potCfg.pumpMlPerSec <= 0.0f) {
        Serial.printf("[POT%d] REMOTE_WATER_BLOCK reason=pump_config_invalid\n", potIdx);
        if (rejectReason) *rejectReason = "PUMP_CONFIG_INVALID";
        return false;
    }

    const PlantProfile& prof = getActiveProfile(g_config, potIdx);
    WateringCycle cycle{};
    cycle.potIndex = potIdx;
    cycle.phase = WateringPhase::EVALUATING;
    cycle.source = PumpOwner::REMOTE;
    cycle.maxPulses = 1;
    cycle.pulseDurationMs = static_cast<uint32_t>((potCfg.pulseWaterMl / potCfg.pumpMlPerSec) * 1000.0f);
    if (cycle.pulseDurationMs > g_config.pumpOnMsMax) {
        cycle.pulseDurationMs = g_config.pumpOnMsMax;
    }
    cycle.soakTimeMs = prof.soakTimeMs;
    cycle.phaseStartMs = nowMs;
    cycle.moistureBeforeCycle = snap.pots[potIdx].moisturePct;
    g_cycles[potIdx] = cycle;
    g_actuator.lastFeedbackCode[potIdx] = WateringFeedbackCode::NONE;
    g_actuator.lastFeedbackValue1[potIdx] = 0.0f;
    g_actuator.lastFeedbackValue2[potIdx] = 0.0f;
    g_actuator.lastFeedbackPulseCount[potIdx] = 0;

    Serial.printf("[POT%d] event=remote_water_start source=telegram pulse_ml=%u\n",
                  potIdx, potCfg.pulseWaterMl);
    return true;
}

// ============================================================================
// ControlTask — odczyty sensorów, FSM podlewania, safety, analiza
// PLAN.md → "ControlTask"
// ============================================================================

static void controlTaskFn(void* /*param*/) {
    Serial.println("[CTRL] event=task_started");

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

        if (rs.secsSinceLastDusk > 0) {
            uint32_t elapsedMs = rs.secsSinceLastDusk * 1000UL;
            g_duskDetector.lastDuskMs = (elapsedMs < bootMs) ? (bootMs - elapsedMs) : 1;
        }
        if (rs.secsSinceLastDawn > 0) {
            uint32_t elapsedMs = rs.secsSinceLastDawn * 1000UL;
            g_duskDetector.lastDawnMs = (elapsedMs < bootMs) ? (bootMs - elapsedMs) : 1;
        }
        g_duskDetector.nightSequence = rs.nightSequence;
        Serial.printf("[CTRL] Dusk runtime restored: last_dusk=%us last_dawn=%us night_seq=%lu\n",
                      rs.secsSinceLastDusk,
                      rs.secsSinceLastDawn,
                      static_cast<unsigned long>(g_duskDetector.nightSequence));

        // Solar clock
        g_solarClock.cycleCount = rs.solarCycleCount;
        g_solarClock.calibrated = rs.solarCalibrated;
        g_solarClock.dayLengthMs = g_duskDetector.dayLengthMs;
        g_solarClock.nightLengthMs = g_duskDetector.nightLengthMs;
    } else {
        // No saved state — assume full reservoir (first boot or post-reset)
        g_budget.reservoirCurrentMl = g_config.reservoirCapacityMl;
        g_budget.lastRefillMs = millis();  // treat first boot as fresh refill
        Serial.println("[CTRL] event=runtime_restore result=missing action=defaults");
    }

    if (!historyStateLoad(millis(), g_history)) {
        Serial.println("[CTRL] event=history_restore result=missing action=ram_only");
    }

    // Restore dusk detector phase from NVS (persisted across reboots)
    if (!duskStateLoad(g_duskDetector)) {
        Serial.println("[CTRL] event=dusk_restore result=missing phase=NIGHT");
    }

    // Tick counters
    uint32_t lastTick10ms  = millis();
    uint32_t lastTick100ms = millis();
    uint32_t lastTick1s    = millis();
    uint32_t lastStatusEvalMs = 0;
    uint32_t lastCtrlAliveEvalMs = millis() - 8000;
    uint32_t lastCtrlAliveEmitMs = 0;
    uint32_t lastRuntimeSave = millis();
    uint32_t lastStatusDigest = 0;
    uint32_t lastCriticalStatusDigest = 0;
    uint32_t lastCtrlAliveDigest = 0;
    uint32_t lastFullStatusMs = 0;
    bool statusDigestInit = false;
    bool ctrlAliveDigestInit = false;
    float    prevTotalPumped = g_budget.totalPumpedMl;  // track pump events

    SensorSnapshot snap{};
    bool duskBootstrapped = false;  // one-shot lux-based phase check
    uint32_t lastFailsafeOffMs[kMaxPots] = {};
    uint32_t lastUnexpectedPumpLogMs[kMaxPots] = {};

    for (;;) {
        uint32_t now = millis();

        // --- Generuj ticki ---
        if (now - lastTick10ms >= 10) {
            lastTick10ms = now;
            // 10ms: odczyt Dual Button, manual pump
            DualButtonState dualBtn = g_hardware.dualButton().read(now);
            SharedState shared = readSharedState();
            manualPumpTick(now, dualBtn, snap, g_config, g_manual, g_actuator,
                           shared.selectedPot, g_budget, g_hardware);
        }

        if (now - lastTick100ms >= 100) {
            lastTick100ms = now;

            // 100ms: pełny odczyt sensorów
            g_hardware.readAllSensors(now, g_config, snap);

            // One-shot: bootstrap dusk phase from first valid lux reading
            if (!duskBootstrapped && snap.env.lightState == LightSignalState::VALID) {
                duskBootstrap(g_duskDetector, snap.env.lux, snap.env.lightState);
                duskBootstrapped = true;
            }

            // EMA filtracja moisture per-pot
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                if (!g_config.pots[i].enabled) continue;
                float rawEma = g_emaFilters[i].update(static_cast<float>(snap.pots[i].moistureRawFiltered), now);
                uint16_t rawEmaRounded = static_cast<uint16_t>(std::lround(rawEma));
                snap.pots[i].moistureEma = normalizeMoistureRaw(rawEmaRounded,
                                                                g_config.pots[i].moistureDryRaw,
                                                                g_config.pots[i].moistureWetRaw,
                                                                g_config.pots[i].moistureCurveExponent);
            }

            // Publikuj snapshot (thread-safe)
            publishSnapshot(snap);

            uint32_t prevFeedbackSeq[kMaxPots];
            memcpy(prevFeedbackSeq, g_actuator.lastFeedbackSeq, sizeof(prevFeedbackSeq));

            // FSM podlewania (AUTO mode)
            if (g_config.mode == Mode::AUTO) {
                wateringTick(now, snap, g_config, g_duskDetector, g_solarClock,
                             g_cycles, g_budget,
                             g_actuator, g_hardware);
                publishWateringFeedback(prevFeedbackSeq);
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
                        queueTelegramFeedbackFmt("Pot %u: cycle aborted because mode switched to MANUAL.",
                                                 static_cast<unsigned>(i + 1));
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

                bool cycleOwnsPump = g_cycles[i].phase == WateringPhase::PULSE;
                bool manualOwnsPump = g_manual.blueOwnsPump && g_manual.activePot == i;
                bool legalContext = cycleOwnsPump || manualOwnsPump;

                if (!legalContext && (now - lastFailsafeOffMs[i]) >= 1000) {
                    lastFailsafeOffMs[i] = now;
                    forcePumpOffWithRetry(i, now, "FAILSAFE_IDLE_OFF");
                }

                if (!legalContext && p.isOn() && (now - lastUnexpectedPumpLogMs[i]) >= 2000) {
                    lastUnexpectedPumpLogMs[i] = now;
                    Serial.printf("[POT%d] PUMP_UNEXPECTED_ON_CONTEXT owner=%s phase=%d manual=%d\n",
                                  i,
                                  pumpOwnerName(g_actuator.currentPumpOwner[i]),
                                  static_cast<int>(g_cycles[i].phase),
                                  manualOwnsPump ? 1 : 0);
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
            uint32_t prevLastDuskMs = g_duskDetector.lastDuskMs;
            uint32_t prevLastDawnMs = g_duskDetector.lastDawnMs;
            uint32_t prevNightSequence = g_duskDetector.nightSequence;
            duskDetectorTick(now,
                             snap.env.lux,
                             snap.env.lightState,
                             snap.env.luxAgeMs,
                             snap.env.tempC,
                             snap.env.humidityPct,
                             snap.env.pressureHpa,
                             g_duskDetector, g_config);
            updateSolarClock(g_duskDetector, g_solarClock);
            if (g_duskDetector.lastDuskMs != prevLastDuskMs ||
                g_duskDetector.lastDawnMs != prevLastDawnMs ||
                g_duskDetector.nightSequence != prevNightSequence) {
                s_runtimeDirty = true;
                saveRuntimeNow(lastRuntimeSave);
            }

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
            if (snap.env.lightState != LightSignalState::VALID) {
                sample.flags |= 0x08;
            }
            if (snap.env.lightState == LightSignalState::RECOVERING) {
                sample.flags |= 0x10;
            }
            if (historyTick(now, sample, g_history)) {
                historyStateSave(now, g_history);
            }

            publishSharedStateFromControl();
        }

        // ==================== STATUS DUMP (change-driven + keepalive) ====================
        if (now - lastStatusEvalMs >= 1000) {
            lastStatusEvalMs = now;
            uint32_t upSec = now / 1000;
            uint32_t digest = statusDigest(snap);
            uint32_t criticalDigest = statusCriticalDigest(snap);
            bool firstEmit = !statusDigestInit;
            bool criticalChanged = firstEmit || (criticalDigest != lastCriticalStatusDigest);
            bool telemetryChanged = firstEmit || (digest != lastStatusDigest);
            bool keepaliveDue = (lastFullStatusMs == 0) || ((now - lastFullStatusMs) >= 900000);
            bool telemetryWindowOpen = (lastFullStatusMs == 0) || ((now - lastFullStatusMs) >= 120000);

            if (criticalChanged || keepaliveDue || (telemetryChanged && telemetryWindowOpen)) {
                lastStatusDigest = digest;
                lastCriticalStatusDigest = criticalDigest;
                statusDigestInit = true;
                lastFullStatusMs = now;

                Serial.println("[STATUS] event=begin");
                Serial.printf("[SYS] uptime=%dm%ds heap=%d freeMin=%d\n",
                              upSec / 60, upSec % 60,
                              (int)ESP.getFreeHeap(), (int)ESP.getMinFreeHeap());
                Serial.printf("[SYS] mode=%s numPots=%d vacation=%s\n",
                              g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                              g_config.numPots,
                              g_config.vacationMode ? "ON" : "OFF");

                if (snap.env.lightState == LightSignalState::VALID) {
                    Serial.printf("[ENV] temp=%.1fC hum=%.1f%% lux=%.0f press=%.1fhPa\n",
                                  snap.env.tempC, snap.env.humidityPct,
                                  snap.env.lux, snap.env.pressureHpa);
                } else {
                    Serial.printf("[ENV] temp=%.1fC hum=%.1f%% lux=%.0f state=%s age=%lus press=%.1fhPa\n",
                                  snap.env.tempC, snap.env.humidityPct,
                                  snap.env.lux,
                                  lightStateName(snap.env.lightState),
                                  static_cast<unsigned long>(snap.env.luxAgeMs / 1000UL),
                                  snap.env.pressureHpa);
                }

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
                    Serial.printf("[POT%d] raw_filt=%d raw_adc=%d pct=%.1f%% ema=%.1f%% phase=%s\n",
                                  i, ps.moistureRawFiltered, ps.moistureRaw,
                                  ps.moisturePct, ps.moistureEma, phaseStr);
                    Serial.printf("[POT%d] pump_owner=%s manual_lock=%s\n",
                                  i,
                                  pumpOwnerName(g_actuator.currentPumpOwner[i]),
                                  g_manual.locked ? "YES" : "no");

                    if (cyc.phase == WateringPhase::IDLE && g_config.mode == Mode::AUTO) {
                        const PlantProfile& pr = getActiveProfile(g_config, i);
                        float effTarget = pr.targetMoisturePct;
                        if (g_config.vacationMode) {
                            effTarget -= g_config.vacationTargetReductionPct;
                            if (effTarget < 5.0f) effTarget = 5.0f;
                        }
                        float trigPct = effTarget - pr.hysteresisPct;

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
                            idleReason = "SHOULD_START";

                        Serial.printf("[POT%d] idle: target=%.0f%% trigger=%.0f%% profile=%s puls=%dml reason=%s\n",
                                      i, effTarget, trigPct, pr.name,
                                      g_config.pots[i].pulseWaterMl, idleReason);
                        if (cooling) {
                            uint32_t cdLeft = effCd - (now - g_actuator.lastCycleDoneMs[i]);
                            Serial.printf("[POT%d] cooldown_remaining=%ds\n", i, cdLeft / 1000);
                        }
                    }

                    Serial.printf("[POT%d] overflow=%s raw=%s pend=%s reservoir=%s raw=%s pend=%s\n",
                                  i,
                                  waterLevelStateName(ps.waterGuards.potMax),
                                  waterLevelStateName(ps.waterGuards.potMaxStatus.rawState),
                                  waterLevelPendingName(ps.waterGuards.potMaxStatus),
                                  waterLevelStateName(ps.waterGuards.reservoirMin),
                                  waterLevelStateName(ps.waterGuards.reservoirMinStatus.rawState),
                                  waterLevelPendingName(ps.waterGuards.reservoirMinStatus));
                    if (cyc.phase != WateringPhase::IDLE && cyc.phase != WateringPhase::DONE) {
                        Serial.printf("[POT%d] pulse=%d/%d pumped=%.1fml phaseSince=%ds\n",
                                      i, cyc.pulseCount, cyc.maxPulses,
                                      cyc.totalPumpedMl,
                                      (int)((now - cyc.phaseStartMs) / 1000));
                    }

                    const auto& tr = g_trendStates[i];
                    if (tr.count > 0) {
                        Serial.printf("[POT%d] trend rate=%.2f%%/h baseline=%.2f cal=%s samples=%d\n",
                                      i, trendCurrentRate(i), tr.normalDryingRate,
                                      tr.baselineCalibrated ? "yes" : "no", tr.count);
                    }
                }

                Serial.printf("[BUDGET] current=%.0fml total_pumped=%.1fml capacity=%.0fml low=%s\n",
                              g_budget.reservoirCurrentMl, g_budget.totalPumpedMl,
                              g_budget.reservoirCapacityMl,
                              g_budget.reservoirLow ? "YES" : "no");

                const char* duskStr = "?";
                switch (g_duskDetector.phase) {
                    case DuskPhase::NIGHT:           duskStr = "NIGHT"; break;
                    case DuskPhase::DAWN_TRANSITION: duskStr = "DAWN_TR"; break;
                    case DuskPhase::DAY:             duskStr = "DAY"; break;
                    case DuskPhase::DUSK_TRANSITION: duskStr = "DUSK_TR"; break;
                }
                Serial.printf("[DUSK] phase=%s dawnScore=%.2f duskScore=%.2f frozen=%s samples=%d\n",
                              duskStr, g_duskDetector.dawnScore, g_duskDetector.duskScore,
                              g_duskDetector.learningFrozen ? "yes" : "no",
                              g_duskDetector.count);
                if (g_solarClock.calibrated) {
                    Serial.printf("[SOLAR] day=%dh%dm night=%dh%dm cycles=%d\n",
                                  g_solarClock.dayLengthMs / 3600000,
                                  (g_solarClock.dayLengthMs / 60000) % 60,
                                  g_solarClock.nightLengthMs / 3600000,
                                  (g_solarClock.nightLengthMs / 60000) % 60,
                                  g_solarClock.cycleCount);
                }

                Serial.println("[STATUS] event=end");
                Serial.flush();
            }
        }

        // Independent heartbeat (change-driven + sparse keepalive)
        if (now - lastCtrlAliveEvalMs >= 10000) {
            lastCtrlAliveEvalMs = now;
            SharedState shared = readSharedState();
            uint32_t freeHeap = ESP.getFreeHeap();
            uint32_t digest = ctrlAliveDigest(g_config.mode, shared.wifiConnected, freeHeap);
            bool changed = (!ctrlAliveDigestInit) || (digest != lastCtrlAliveDigest);
            bool keepaliveDue = (lastCtrlAliveEmitMs == 0) || ((now - lastCtrlAliveEmitMs) >= 300000);
            if (changed || keepaliveDue) {
                ctrlAliveDigestInit = true;
                lastCtrlAliveDigest = digest;
                lastCtrlAliveEmitMs = now;
                Serial.printf("[CTRL] event=alive mode=%s wifi=%s heap=%u\n",
                              g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                              shared.wifiConnected ? "up" : "down",
                              (unsigned)freeHeap);
            }
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
                        uint8_t prevNumPots = g_config.numPots;
                        g_config.numPots = n;
                        for (uint8_t i = 0; i < n; ++i)
                            g_config.pots[i].enabled = true;
                        for (uint8_t i = n; i < kMaxPots; ++i)
                            g_config.pots[i].enabled = false;

                        // If the operator enables additional pots at runtime, initialize
                        // their hardware immediately; waiting for CONFIG_SAVE_REQUEST is
                        // ineffective because that event is not emitted by the UI path.
                        if (n > prevNumPots) {
                            for (uint8_t i = prevNumPots; i < n; ++i) {
                                g_hardware.initPot(i, g_hwConfig, g_config);
                                g_emaFilters[i].alpha = g_config.pots[i].moistureEmaAlpha;
                                g_emaFilters[i].reset();
                            }
                        }

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

                case EventType::REQUEST_SET_MOISTURE_DRY_RAW: {
                    uint8_t pot = evt.payload.config.key;
                    uint16_t dryRaw = evt.payload.config.valueU16;
                    if (pot < kMaxPots && dryRaw <= 4095
                        && dryRaw > g_config.pots[pot].moistureWetRaw + 31) {
                        g_config.pots[pot].moistureDryRaw = dryRaw;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pot%u moisture_dry_raw=%u\n",
                                      static_cast<unsigned>(pot),
                                      static_cast<unsigned>(dryRaw));
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_MOISTURE_WET_RAW: {
                    uint8_t pot = evt.payload.config.key;
                    uint16_t wetRaw = evt.payload.config.valueU16;
                    if (pot < kMaxPots && wetRaw <= 4095
                        && wetRaw + 31 < g_config.pots[pot].moistureDryRaw) {
                        g_config.pots[pot].moistureWetRaw = wetRaw;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pot%u moisture_wet_raw=%u\n",
                                      static_cast<unsigned>(pot),
                                      static_cast<unsigned>(wetRaw));
                        publishSharedStateFromControl();
                    }
                    break;
                }

                case EventType::REQUEST_SET_MOISTURE_CURVE_EXPONENT: {
                    uint8_t pot = evt.payload.config.key;
                    float exponent = evt.payload.config.valueF;
                    if (pot < kMaxPots && exponent >= 0.1f && exponent <= 12.0f) {
                        g_config.pots[pot].moistureCurveExponent = exponent;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pot%u moisture_curve_exp=%.2f\n",
                                      static_cast<unsigned>(pot), exponent);
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
                    Serial.println("[CTRL] event=reservoir_refill runtime_saved=yes");
                    publishSharedStateFromControl();
                    break;

                case EventType::REQUEST_MANUAL_WATER: {
                    uint8_t potIdx = evt.payload.config.key;
                    const char* rejectReason = nullptr;
                    bool ok = startRemoteWateringPulse(now, potIdx, snap, &rejectReason);
                    Serial.printf("[CTRL] event=remote_water pot=%u result=%s\n",
                                  static_cast<unsigned>(potIdx), ok ? "ok" : "blocked");
                    if (!ok && potIdx < g_config.numPots) {
                        queueTelegramFeedbackFmt("Pot %u: %s",
                                                 static_cast<unsigned>(potIdx + 1),
                                                 remoteWaterRejectMessage(rejectReason));
                    }
                    publishSharedStateFromControl();
                    break;
                }

                case EventType::REQUEST_PUMP_OFF: {
                    bool any = false;
                    bool budgetChanged = false;
                    for (uint8_t i = 0; i < g_config.numPots; ++i) {
                        if (g_hardware.pump(i).isOn()) {
                            any = true;
                            uint32_t onMs = g_hardware.pump(i).onDuration(now);
                            if (g_config.pots[i].pumpMlPerSec > 0.0f && onMs > 0) {
                                float pumpedMl = (onMs / 1000.0f) * g_config.pots[i].pumpMlPerSec;
                                addPumped(g_budget, pumpedMl, i);
                                g_cycles[i].totalPumpedMs += onMs;
                                g_cycles[i].totalPumpedMl += pumpedMl;
                                budgetChanged = true;
                            }
                            forcePumpOffWithRetry(i, now, "REMOTE_STOP");
                        }
                        if (g_cycles[i].phase != WateringPhase::IDLE) {
                            any = true;
                            g_cycles[i].reset();
                            g_actuator.lastCycleDoneMs[i] = now;
                        }
                    }
                    g_manual.blueHeldMs = 0;
                    g_manual.blueOwnsPump = false;
                    g_manual.locked = true;
                    g_manual.lockUntilMs = now + 5000;
                    if (budgetChanged) {
                        s_runtimeDirty = true;
                        saveRuntimeNow(lastRuntimeSave);
                    }
                    Serial.printf("[CTRL] event=remote_stop any=%s\n", any ? "yes" : "no");
                    publishSharedStateFromControl();
                    break;
                }

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
                    Serial.println("[CTRL] event=factory_reset");
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
                    Serial.println("[UI] event=factory_reset_confirmed");
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

    for (uint8_t i = 0; i < kMaxPots; ++i) {
        if (beforeCfg.pots[i].moistureDryRaw != afterCfg.pots[i].moistureDryRaw) {
            evt = Event{};
            evt.type = EventType::REQUEST_SET_MOISTURE_DRY_RAW;
            evt.payload.config.key = i;
            evt.payload.config.valueU16 = afterCfg.pots[i].moistureDryRaw;
            g_eventQueue.push(evt);
            anyRequest = true;
        }
        if (beforeCfg.pots[i].moistureWetRaw != afterCfg.pots[i].moistureWetRaw) {
            evt = Event{};
            evt.type = EventType::REQUEST_SET_MOISTURE_WET_RAW;
            evt.payload.config.key = i;
            evt.payload.config.valueU16 = afterCfg.pots[i].moistureWetRaw;
            g_eventQueue.push(evt);
            anyRequest = true;
        }
        if (beforeCfg.pots[i].moistureCurveExponent != afterCfg.pots[i].moistureCurveExponent) {
            evt = Event{};
            evt.type = EventType::REQUEST_SET_MOISTURE_CURVE_EXPONENT;
            evt.payload.config.key = i;
            evt.payload.config.valueF = afterCfg.pots[i].moistureCurveExponent;
            g_eventQueue.push(evt);
            anyRequest = true;
        }
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
            memcpy(uSnap.lastCycleDoneMs, shared.lastCycleDoneMs, sizeof(shared.lastCycleDoneMs));
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
    Serial.println("[NET] event=task_started");
    netTaskInit(g_netConfig, g_netState);

    static SharedState shared{};
    static SensorSnapshot latestSnap{};
    static DailyReportData rptData{};
    static TelegramStatusData tgData{};
    static char heartbeatBuf[1024];
    static uint32_t s_lastFeedbackBacklogLogMs = 0;
    static uint32_t s_lastFeedbackDeferredLogMs = 0;
    static constexpr uint32_t kStartupHeartbeatDelayMs = 5UL * 60 * 1000;
    static constexpr uint32_t kStartupHeartbeatRetryMs = 5UL * 60 * 1000;

    for (;;) {
        uint32_t now = millis();

        netTaskTick(now, g_netState, g_netConfig);
        publishSharedNetStatus(g_netState.wifiConnected);

        // Heartbeat check
        if (g_netState.wifiConnected && g_netState.telegramEnabled) {
            shared = readSharedState();
            latestSnap = readSnapshot();

            tgData.sensors = latestSnap;
            tgData.budget = shared.budget;
            memcpy(tgData.cycles, shared.cycles, sizeof(shared.cycles));
            memcpy(tgData.lastCycleDoneMs, shared.lastCycleDoneMs, sizeof(shared.lastCycleDoneMs));
            memcpy(tgData.lastFeedbackSeq, shared.lastFeedbackSeq, sizeof(shared.lastFeedbackSeq));
            memcpy(tgData.lastFeedbackCode, shared.lastFeedbackCode, sizeof(shared.lastFeedbackCode));
            memcpy(tgData.lastFeedbackValue1, shared.lastFeedbackValue1, sizeof(shared.lastFeedbackValue1));
            memcpy(tgData.lastFeedbackValue2, shared.lastFeedbackValue2, sizeof(shared.lastFeedbackValue2));
            memcpy(tgData.lastFeedbackPulseCount, shared.lastFeedbackPulseCount, sizeof(shared.lastFeedbackPulseCount));
            memcpy(tgData.trends, shared.trends, sizeof(shared.trends));
            tgData.config = shared.config;
            tgData.duskPhase = shared.duskPhase;
            tgData.wifiConnected = shared.wifiConnected;
            tgData.apActive = g_netState.apActive;
            tgData.selectedPot = shared.selectedPot;
            tgData.uptimeMs = now;

            // Najpierw odbierz i obsłuż komendy/callbacki użytkownika,
            // żeby backlog feedbacku nie opóźniał odpowiedzi inline.
            telegramPollCommands(now, g_netConfig, tgData);

            auto sendHeartbeatReport = [&](DailyReportData::Kind kind, const char* typeLabel) {
                rptData.sensors  = latestSnap;
                rptData.budget   = shared.budget;
                memcpy(rptData.trends, shared.trends, sizeof(shared.trends));
                memcpy(rptData.lastCycleDoneMs, shared.lastCycleDoneMs, sizeof(shared.lastCycleDoneMs));
                rptData.pumped24hMl = shared.pumped24hMl;
                memcpy(rptData.pumped24hMlPerPot, shared.pumped24hMlPerPot, sizeof(shared.pumped24hMlPerPot));
                rptData.wateringEvents24h = shared.wateringEvents24h;
                memcpy(rptData.wateringEvents24hPerPot, shared.wateringEvents24hPerPot,
                       sizeof(shared.wateringEvents24hPerPot));
                rptData.config   = shared.config;
                rptData.duskPhase = shared.duskPhase;
                rptData.solarCalibrated = g_solarClock.calibrated;
                rptData.kind = kind;
                rptData.uptimeMs = now;

                formatDailyReport(rptData, heartbeatBuf, sizeof(heartbeatBuf));
                bool sent = telegramSendInlineMessage(heartbeatBuf, tgData, g_netConfig);
                if (sent) {
                    Serial.printf("[NET] event=heartbeat_sent type=%s mode=inline\n", typeLabel);
                } else {
                    Serial.printf("[NET] event=heartbeat_deferred type=%s reason=inline_send_failed\n",
                                  typeLabel);
                }
                return sent;
            };

            if (!g_netState.startupHeartbeatSent
                && now >= kStartupHeartbeatDelayMs
                && (g_netState.lastStartupHeartbeatAttemptMs == 0
                    || (now - g_netState.lastStartupHeartbeatAttemptMs) >= kStartupHeartbeatRetryMs)) {
                g_netState.lastStartupHeartbeatAttemptMs = now;
                if (sendHeartbeatReport(DailyReportData::Kind::STARTUP_CHECK, "startup")) {
                    g_netState.startupHeartbeatSent = true;
                }
            }

            uint32_t prevLastHeartbeatMs = g_netState.lastHeartbeatMs;
            uint32_t prevLastDailyAnchorMs = g_netState.lastDailyAnchorMs;
            if (isDailyHeartbeatTime(now, g_solarClock, g_duskDetector,
                                     g_netState)) {
                bool sent = sendHeartbeatReport(DailyReportData::Kind::DAILY, "daily");

                if (sent) {
                } else {
                    g_netState.lastHeartbeatMs = prevLastHeartbeatMs;
                    g_netState.lastDailyAnchorMs = prevLastDailyAnchorMs;
                }
            }

            if (s_tgFeedbackQueue) {
                UBaseType_t queueBefore = uxQueueMessagesWaiting(s_tgFeedbackQueue);
                UBaseType_t processedCount = 0;
                TelegramFeedbackMessage note{};
                while (processedCount < kTelegramFeedbackDrainPerTick &&
                       xQueuePeek(s_tgFeedbackQueue, &note, 0) == pdTRUE) {
                    if (!telegramHasActivePanel()) {
                        if ((now - s_lastFeedbackDeferredLogMs) >= 5000) {
                            s_lastFeedbackDeferredLogMs = now;
                            Serial.printf("[TG] event=feedback_deferred reason=no_active_panel waiting=%u\n",
                                          static_cast<unsigned>(queueBefore));
                        }
                        break;
                    }

                    bool sent = telegramSendToActivePanel(note.text, tgData, g_netConfig);
                    if (!sent) {
                        if ((now - s_lastFeedbackDeferredLogMs) >= 5000) {
                            s_lastFeedbackDeferredLogMs = now;
                            Serial.printf("[TG] event=feedback_deferred reason=panel_send_failed waiting=%u\n",
                                          static_cast<unsigned>(queueBefore));
                        }
                        break;
                    }

                    xQueueReceive(s_tgFeedbackQueue, &note, 0);
                    ++processedCount;
                    Serial.printf("[TG] event=feedback_sent idx=%u len=%u mode=inline_panel ok=yes\n",
                                  static_cast<unsigned>(processedCount),
                                  static_cast<unsigned>(strlen(note.text)));
                }

                UBaseType_t queueAfter = uxQueueMessagesWaiting(s_tgFeedbackQueue);
                if (queueAfter > 0 && (now - s_lastFeedbackBacklogLogMs) >= 5000) {
                    s_lastFeedbackBacklogLogMs = now;
                    Serial.printf("[TG] event=feedback_backlog before=%u drained=%u after=%u\n",
                                  static_cast<unsigned>(queueBefore),
                                  static_cast<unsigned>(processedCount),
                                  static_cast<unsigned>(queueAfter));
                }
            }
        }

        // AP mode i aktywny Telegram korzystają z krótszego ticka,
        // żeby adaptacyjny polling/callbacki były responsywne bez wpływu na automatykę.
        const uint32_t netDelayMs = g_netState.apActive
            ? 100
            : ((g_netState.wifiConnected && g_netState.telegramEnabled) ? 100 : 1000);
        vTaskDelay(pdMS_TO_TICKS(netDelayMs));
    }
}

// ============================================================================
// Task stack sizes and priorities
// ============================================================================

static constexpr uint32_t kControlStackSize = 12288;
static constexpr uint32_t kUiStackSize      = 8192;
static constexpr uint32_t kNetStackSize     = 12288;
static constexpr uint32_t kConfigStackSize  = 8192;

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
    Serial.setDebugOutput(false);
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("wifi_init", ESP_LOG_NONE);
    esp_log_level_set("phy_init", ESP_LOG_NONE);
    Serial.printf("[BOOT] start serial_ready=%d\n", (int)Serial);

    Serial.println("[BOOT] event=banner name=autogarden");
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
        Serial.println("[BOOT] event=config_load result=fail action=defaults");
        configLoadDefaults(g_config);
    }
    if (!configValidate(g_config)) {
        Serial.println("[BOOT] event=config_validate result=fail action=defaults");
        configLoadDefaults(g_config);
    }
    Serial.printf("[BOOT] config OK: mode=%s numPots=%d\n",
                  g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                  g_config.numPots);

    // --- Załaduj konfigurację sieci ---
    if (!netConfigLoad(g_netConfig)) {
        Serial.println("[BOOT] event=netcfg_load result=fail provisioning=needed");
    }
    applyLocalTelegramConfig(g_netConfig);
    if (strlen(g_netConfig.telegramBotToken) > 0 && g_netConfig.telegramChatIds[0] != '\0') {
        Serial.printf("[BOOT] event=telegram_config source=%s bot=%s targets=%u\n",
                      "nvs_or_local",
                      telegramConfiguredBotName()[0] ? telegramConfiguredBotName() : "configured",
                      static_cast<unsigned>(telegramConfiguredTargetCount(g_netConfig)));
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
        Serial.println("[BOOT] event=hardware_init result=fail mode=degraded");
    } else {
        Serial.println("[BOOT] event=hardware_init result=ok");
    }

    // --- Inicjalizacja eventów i synchronizacji ---
    g_eventQueue.init();
    s_snapMutex = xSemaphoreCreateMutex();
    s_stateMutex = xSemaphoreCreateMutex();
    s_saveQueue = xQueueCreate(8, sizeof(Config));
    s_tgFeedbackQueue = xQueueCreate(12, sizeof(TelegramFeedbackMessage));

    // Initial shared snapshot
    publishSharedStateFromControl();
    publishSharedNetStatus(false);

    Serial.println("[BOOT] event=tasks_create_start");

    // --- Tworzenie tasków (bez pinowania do rdzeni) ---
    xTaskCreate(controlTaskFn, "control", kControlStackSize, nullptr,
                kControlPriority, nullptr);

    xTaskCreate(uiTaskFn, "ui", kUiStackSize, nullptr,
                kUiPriority, nullptr);

    xTaskCreate(netTaskFn, "net", kNetStackSize, nullptr,
                kNetPriority, nullptr);

    xTaskCreate(configTaskFn, "config", kConfigStackSize, nullptr,
                kConfigPriority, nullptr);

    Serial.println("[BOOT] event=tasks_create_done scheduler=running");
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
