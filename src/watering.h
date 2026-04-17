// ============================================================================
// watering.h — FSM podlewania, safety, budżet wody, manual pump
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Algorytm podlewania — pełna specyfikacja",
//                "FSM podlewania", "Polityki bezpieczeństwa",
//                "Algorytm podlewania pulsowego (Pulse-Soak-Measure)",
//                "Harmonogram podlewania (Schedule)",
//                "Przycisk manualny (Dual Button) — bezpieczeństwo",
//                "Rezerwuar wody — budżet i tryb kryzysowy",
//                "Tryb wakacyjny (Vacation Mode)",
//                "Profile roślin (PlantProfile)"
// Architektura:  docs/ARCHITECTURE.md
// ============================================================================
#pragma once

#include <cstdint>
#include "config.h"
#include "hardware.h"
#include "events.h"
#include "analysis.h"

// ---------------------------------------------------------------------------
// Fazy cyklu podlewania (PLAN.md → "Stany cyklu podlewania")
// ---------------------------------------------------------------------------
enum class WateringPhase : uint8_t {
    IDLE,
    EVALUATING,
    PULSE,
    SOAK,
    MEASURING,
    OVERFLOW_WAIT,
    DONE,
    BLOCKED,
};

enum class WateringFeedbackCode : uint8_t {
    NONE = 0,
    CYCLE_START_SCHEDULE,
    CYCLE_START_RESCUE,
    SKIP_ALREADY_WET,
    SKIP_ABOVE_MAX,
    OVERFLOW_DETECTED,
    OVERFLOW_RESUME,
    TARGET_REACHED,
    STOP_MAX_EXCEEDED,
    STOP_MAX_PULSES,
    OVERFLOW_TIMEOUT,
    SAFETY_BLOCK_OVERFLOW_RISK,
    SAFETY_BLOCK_TANK_EMPTY,
    SAFETY_BLOCK_OVERFLOW_SENSOR_UNKNOWN,
    SAFETY_BLOCK_TANK_SENSOR_UNKNOWN,
    SAFETY_BLOCK_RESERVOIR_EMPTY,
    SAFETY_BLOCK_PUMP_CONFIG_INVALID,
    HARD_TIMEOUT,
    SAFETY_UNBLOCK,
    CYCLE_DONE_GENERIC,
};

enum class PumpOwner : uint8_t {
    NONE = 0,
    AUTO,
    MANUAL,
    REMOTE,
};

// ---------------------------------------------------------------------------
// WateringCycle — kontekst jednego cyklu per-pot
// PLAN.md → "Kontekst jednego cyklu podlewania"
// ---------------------------------------------------------------------------
struct WateringCycle {
    WateringPhase phase      = WateringPhase::IDLE;
    uint8_t  potIndex        = 0;
    uint8_t  pulseCount      = 0;
    uint8_t  maxPulses       = 0;
    uint32_t pulseDurationMs = 0;
    uint32_t soakTimeMs      = 0;
    uint32_t phaseStartMs    = 0;
    float    moistureBeforeCycle    = 0.0f;
    float    moistureAfterLastSoak  = 0.0f;
    uint32_t totalPumpedMs   = 0;
    float    totalPumpedMl   = 0.0f;
    PumpOwner source         = PumpOwner::AUTO;

    void reset() { *this = WateringCycle{}; }
};

// ---------------------------------------------------------------------------
// WaterBudget — budżet wody wspólnego rezerwuaru
// PLAN.md → "Rezerwuar wody — budżet i tryb kryzysowy"
// ---------------------------------------------------------------------------
struct WaterBudget {
    float    reservoirCapacityMl    = 10000.0f;  // z Config
    float    reservoirCurrentMl     = 10000.0f;
    float    totalPumpedMl          = 0.0f;
    float    totalPumpedMlPerPot[kMaxPots] = {};
    bool     reservoirLow           = false;
    uint32_t reservoirLowSinceMs    = 0;
    float    reservoirLowThresholdMl = 2000.0f;  // z Config
    float    daysRemaining          = 999.0f;
    uint32_t lastRefillMs            = 0;       // millis() of last refill (0 = boot)
};

// ---------------------------------------------------------------------------
// ManualState — stan sterowania ręcznego (Dual Button)
// PLAN.md → "Przycisk manualny (Dual Button) — bezpieczeństwo"
// ---------------------------------------------------------------------------
struct ManualState {
    uint32_t blueHeldMs       = 0;     // timestamp wciśnięcia (0 = nie wciśnięty)
    bool     blueOwnsPump     = false; // true jeśli manual uruchomił pompę
    uint8_t  activePot        = 0xFF;  // pot controlled by manual button
    bool     locked           = false;
    uint32_t lockUntilMs      = 0;

    // Anti-spam
    static constexpr uint8_t kMaxHistory = 10;
    uint32_t pressHistory[kMaxHistory] = {};
    uint8_t  pressCount       = 0;
};

// ---------------------------------------------------------------------------
// SafetyResult — wynik oceny bezpieczeństwa
// ---------------------------------------------------------------------------
struct SafetyResult {
    bool hardBlock  = false;
    const char* reason = nullptr;

    SafetyResult() = default;
    SafetyResult(bool block, const char* r) : hardBlock(block), reason(r) {}
};

// ---------------------------------------------------------------------------
// ScheduleResult — wynik evaluateSchedule
// PLAN.md → "Harmonogram podlewania (Schedule)"
// ---------------------------------------------------------------------------
enum class ScheduleDecision : uint8_t {
    NO_ACTION,
    START_CYCLE,
    RESCUE_WATER,
};

struct ScheduleResult {
    ScheduleDecision decision = ScheduleDecision::NO_ACTION;
    const char* reason = nullptr;

    ScheduleResult() = default;
    ScheduleResult(ScheduleDecision d, const char* r)
        : decision(d), reason(r) {}
};

// ---------------------------------------------------------------------------
// ActuatorState — śledzenie stanu aktuatorów
// ---------------------------------------------------------------------------
struct ActuatorState {
    uint32_t lastPumpStopAtMs[kMaxPots] = {};
    uint32_t lastCycleDoneMs[kMaxPots]  = {};
    PumpOwner currentPumpOwner[kMaxPots] = {};
    uint32_t lastFeedbackSeq[kMaxPots]  = {};
    WateringFeedbackCode lastFeedbackCode[kMaxPots] = {};
    float    lastFeedbackValue1[kMaxPots] = {};
    float    lastFeedbackValue2[kMaxPots] = {};
    uint8_t  lastFeedbackPulseCount[kMaxPots] = {};
    bool     overflowIncidentLatched[kMaxPots] = {};
    bool     overflowUnknownLatched[kMaxPots] = {};
    bool     reservoirIncidentLatched = false;
    bool     reservoirUnknownLatched = false;
    bool     reservoirClearPending = false;
    WateringFeedbackCode activeSafetyCode[kMaxPots] = {};
};

// ---------------------------------------------------------------------------
// API — główne funkcje domeny podlewania
// ---------------------------------------------------------------------------

// Tick podlewania — wywoływany z ControlTask co ~100ms
// Iteruje po doniczkach, obsługuje FSM per-pot
void wateringTick(uint32_t nowMs,
                  const SensorSnapshot& sensors,
                  const Config& cfg,
                  const DuskDetector& dusk,
                  const SolarClock& solar,
                  WateringCycle cycles[],
                  WaterBudget& budget,
                  ActuatorState& actuator,
                  HardwareManager& hw);

// Ocena harmonogramu — czy teraz podlewać? (per-pot)
ScheduleResult evaluateSchedule(uint32_t nowMs,
                                const PotSensorSnapshot& potSens,
                                const EnvSnapshot& envSens,
                                const Config& cfg,
                                const DuskDetector& dusk,
                                const SolarClock& solar,
                                const ActuatorState& actuator,
                                uint8_t potIdx);

// Polityki bezpieczeństwa
SafetyResult evaluateSafety(uint32_t nowMs,
                            const PotSensorSnapshot& potSens,
                            const Config& cfg,
                            const PotConfig& potCfg,
                            const ActuatorState& actuator,
                            uint8_t potIdx);

SafetyResult evaluateExtendedSafety(uint32_t nowMs,
                                    const PotSensorSnapshot& potSens,
                                    const Config& cfg,
                                    const PotConfig& potCfg,
                                    const WaterBudget& budget,
                                    const ActuatorState& actuator,
                                    uint8_t potIdx);

// Przycisk manualny — wywoływany co tick
void manualPumpTick(uint32_t nowMs,
                    const DualButtonState& btn,
                    const SensorSnapshot& sensors,
                    const Config& cfg,
                    ManualState& manual,
                    ActuatorState& actuator,
                    uint8_t selectedPot,
                    WaterBudget& budget,
                    HardwareManager& hw);

// Budżet wody
void updateWaterBudget(uint32_t nowMs,
                       const SensorSnapshot& sensors,
                       WaterBudget& budget,
                       const Config& cfg);

void handleRefill(WaterBudget& budget, const Config& cfg);

void addPumped(WaterBudget& budget, float ml, uint8_t potIdx);

// Vacation mode
void applyVacationOverrides(const Config& cfg, const PlantProfile& base,
                            PlantProfile& effective);

void handleVacationToggle(bool enable, Config& cfg);
