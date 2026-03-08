// ============================================================================
// watering.cpp — FSM podlewania, safety, budżet wody, manual pump
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Algorytm podlewania pulsowego",
//                "FSM podlewania", "Polityki bezpieczeństwa",
//                "Harmonogram podlewania (Schedule)",
//                "Przycisk manualny (Dual Button)",
//                "Rezerwuar wody — budżet i tryb kryzysowy",
//                "Tryb wakacyjny (Vacation Mode)"
// ============================================================================

#include "watering.h"
#include <Arduino.h>   // Serial, millis
#include <cstring>
#include <algorithm>

#include "analysis.h"
#include "log_serial.h"

#define Serial AGSerial

extern SensorHistory g_history;

// ===========================================================================
// Helpers — lokalne
// ===========================================================================

static WateringCycle newCycle(const PlantProfile& prof, const PotConfig& potCfg, uint8_t potIdx) {
    WateringCycle c{};
    c.potIndex        = potIdx;
    c.phase           = WateringPhase::EVALUATING;
    c.maxPulses       = prof.maxPulsesPerCycle;
    c.pulseDurationMs = static_cast<uint32_t>((potCfg.pulseWaterMl / potCfg.pumpMlPerSec) * 1000.0f);
    c.soakTimeMs      = prof.soakTimeMs;
    return c;
}

static bool inCooldown(uint32_t nowMs, uint32_t cooldownMs, const ActuatorState& act, uint8_t potIdx) {
    if (act.lastCycleDoneMs[potIdx] == 0) return false;
    return (nowMs - act.lastCycleDoneMs[potIdx]) < cooldownMs;
}

static WateringFeedbackCode feedbackCodeFromSafetyReason(const char* reason) {
    if (!reason) return WateringFeedbackCode::NONE;
    if (strcmp(reason, "OVERFLOW_RISK") == 0) return WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_RISK;
    if (strcmp(reason, "TANK_EMPTY") == 0) return WateringFeedbackCode::SAFETY_BLOCK_TANK_EMPTY;
    if (strcmp(reason, "OVERFLOW_SENSOR_UNKNOWN") == 0) return WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_SENSOR_UNKNOWN;
    if (strcmp(reason, "TANK_SENSOR_UNKNOWN") == 0) return WateringFeedbackCode::SAFETY_BLOCK_TANK_SENSOR_UNKNOWN;
    if (strcmp(reason, "RESERVOIR_EMPTY") == 0) return WateringFeedbackCode::SAFETY_BLOCK_RESERVOIR_EMPTY;
    if (strcmp(reason, "PUMP_NOT_CALIBRATED") == 0) return WateringFeedbackCode::SAFETY_BLOCK_PUMP_NOT_CALIBRATED;
    return WateringFeedbackCode::NONE;
}

static void publishFeedback(ActuatorState& actuator,
                            uint8_t potIdx,
                            WateringFeedbackCode code,
                            float value1 = 0.0f,
                            float value2 = 0.0f,
                            uint8_t pulseCount = 0) {
    actuator.lastFeedbackSeq[potIdx]++;
    actuator.lastFeedbackCode[potIdx] = code;
    actuator.lastFeedbackValue1[potIdx] = value1;
    actuator.lastFeedbackValue2[potIdx] = value2;
    actuator.lastFeedbackPulseCount[potIdx] = pulseCount;
}

// ===========================================================================
// evaluateSafety — PLAN.md → "Polityki bezpieczeństwa (gates)"
// ===========================================================================
SafetyResult evaluateSafety(uint32_t nowMs,
                            const PotSensorSnapshot& potSens,
                            const Config& cfg,
                            const PotConfig& potCfg,
                            const ActuatorState& actuator,
                            uint8_t potIdx)
{
    (void)actuator;

    // Sensor fail — tylko log, NIE blokuj (sonda może być odłączona)
    // Hard block tylko gdyboth raw=0 I moistureEma=0 I EMA miała czas na ustabilizowanie
    // W MANUAL mode pompa i tak musi działać niezależnie od sondy
    if (potSens.moistureRaw == 0 && potSens.moisturePct <= 0.0f) {
        static uint32_t lastSensorWarn = 0;
        if (nowMs - lastSensorWarn > 10000) {
            lastSensorWarn = nowMs;
            Serial.printf("[POT%d] WARNING: sensor reads 0 (disconnected?)\n", potIdx);
        }
        // NIE blokuj — pozwól na podlewanie z niską wilgotnością
        // return { true, "SENSOR_FAIL" };
    }

    if (cfg.antiOverflowEnabled) {
        // Overflow w doniczce
        if (potSens.waterGuards.potMax == WaterLevelState::TRIGGERED) {
            return { true, "OVERFLOW_RISK" };
        }
        // Rezerwuar → ok, ale rezerwuar jest wspólny, sprawdzamy w extended
        if (potSens.waterGuards.reservoirMin == WaterLevelState::TRIGGERED) {
            return { true, "TANK_EMPTY" };
        }

        // Unknown policy
        if (cfg.waterLevelUnknownPolicy == UnknownPolicy::BLOCK) {
            if (potSens.waterGuards.potMax == WaterLevelState::UNKNOWN) {
                return { true, "OVERFLOW_SENSOR_UNKNOWN" };
            }
            if (potSens.waterGuards.reservoirMin == WaterLevelState::UNKNOWN) {
                return { true, "TANK_SENSOR_UNKNOWN" };
            }
        }
    }

    return { false, nullptr };
}

// ===========================================================================
// evaluateExtendedSafety — dodaje budżet i cooldown
// ===========================================================================
SafetyResult evaluateExtendedSafety(uint32_t nowMs,
                                    const PotSensorSnapshot& potSens,
                                    const Config& cfg,
                                    const PotConfig& potCfg,
                                    const WaterBudget& budget,
                                    const ActuatorState& actuator,
                                    uint8_t potIdx)
{
    // Najpierw basic safety
    SafetyResult basic = evaluateSafety(nowMs, potSens, cfg, potCfg, actuator, potIdx);
    if (basic.hardBlock) return basic;

    // Rezerwuar empty → estymowany 0 ml
    if (budget.reservoirLow && budget.reservoirCurrentMl <= 0.0f) {
        return { true, "RESERVOIR_EMPTY" };
    }

    // Pompa nie skalibrowana → blokuj AUTO
    if (potCfg.pumpMlPerSec <= 0.0f) {
        return { true, "PUMP_NOT_CALIBRATED" };
    }

    return { false, nullptr };
}

// ===========================================================================
// evaluateSchedule — PLAN.md → "Harmonogram podlewania (Schedule)"
// ===========================================================================
ScheduleResult evaluateSchedule(uint32_t nowMs,
                                const PotSensorSnapshot& potSens,
                                const EnvSnapshot& envSens,
                                const Config& cfg,
                                uint8_t potIdx)
{
    static uint32_t s_lastSchedBlockLogMs[kMaxPots] = {};
    static char s_lastSchedBlockReason[kMaxPots][24] = {};

    auto throttledSchedBlockLog = [&](const char* reason, const char* fmt, float val) {
        bool reasonChanged = (strncmp(s_lastSchedBlockReason[potIdx], reason,
                                      sizeof(s_lastSchedBlockReason[potIdx]) - 1) != 0);
        bool timeElapsed = (nowMs - s_lastSchedBlockLogMs[potIdx]) >= 10000;
        if (reasonChanged || timeElapsed) {
            Serial.printf(fmt, potIdx, val);
            strncpy(s_lastSchedBlockReason[potIdx], reason,
                    sizeof(s_lastSchedBlockReason[potIdx]) - 1);
            s_lastSchedBlockReason[potIdx][sizeof(s_lastSchedBlockReason[potIdx]) - 1] = '\0';
            s_lastSchedBlockLogMs[potIdx] = nowMs;
        }
    };

    const PotConfig& potCfg = cfg.pots[potIdx];
    const PlantProfile& prof = getActiveProfile(cfg, potIdx);
    float moisture = potSens.moisturePct;

    // == 0. VACATION ANOMALY BLOCK ==
    if (cfg.vacationMode) {
        if (trendBaselineLearned(potIdx)) {
            float rate = trendCurrentRate(potIdx);
            // TODO(analysis): normalDryingRate dostępny z TrendState
            // Na razie: absolutny fallback threshold
            if (rate < -(cfg.anomalyDryingRateThreshold * cfg.anomalyDryingRateMultiplier)) {
                Serial.printf("[POT%d] SCHEDULE_BLOCK reason=vacation_anomaly rate=%.2f%%/h\n",
                              potIdx, rate);
                return { ScheduleDecision::NO_ACTION, "VACATION_ANOMALY_BLOCK" };
            }
        }
    }

    // == 1. RESCUE — krytycznie sucha gleba ==
    if (moisture < prof.criticalLowPct) {
        Serial.printf("[POT%d] SCHEDULE_RESCUE moisture=%.1f%% critical=%.1f%%\n",
                      potIdx, moisture, prof.criticalLowPct);
        return { ScheduleDecision::RESCUE_WATER, "RESCUE_CRITICAL_LOW" };
    }

    // == 2. COOLDOWN ==
    // Vacation mode: mnożnik cooldownu
    uint32_t effCooldown = cfg.cooldownMs;
    if (cfg.vacationMode) {
        effCooldown = static_cast<uint32_t>(effCooldown * cfg.vacationCooldownMultiplier);
    }
    // potIdx-specific cooldown: używamy ActuatorState (przekazany pośrednio
    // przez wateringTick; tu porównujemy czas z zewnątrz)
    // NOTE: W evaluateSchedule nie mamy dostępu do ActuatorState —
    // cooldown sprawdzany w wateringTick przed wywołaniem schedule.

    // == 3. POGODA ==
    if (envSens.tempC > cfg.heatBlockTempC) {
        throttledSchedBlockLog("too_hot", "[POT%d] SCHEDULE_BLOCK reason=too_hot temp=%.1f\n",
                               envSens.tempC);
        return { ScheduleDecision::NO_ACTION, "HEAT_BLOCK" };
    }
    if (envSens.lux > cfg.directSunLuxThreshold) {
        throttledSchedBlockLog("direct_sun", "[POT%d] SCHEDULE_BLOCK reason=direct_sun lux=%.0f\n",
                               envSens.lux);
        return { ScheduleDecision::NO_ACTION, "DIRECT_SUN" };
    }

    // == 4. DUSK WINDOW ==
    // TODO(dusk): Integracja z DuskDetector (analysis.h)
    // Na razie: fallback — co fallbackIntervalMs podlewaj jeśli sucho

    // == 5. Wilgotność poniżej target − hysteresis → czas podlać ==
    float effectiveTarget = prof.targetMoisturePct;
    if (cfg.vacationMode) {
        effectiveTarget -= cfg.vacationTargetReductionPct;
        if (effectiveTarget < 5.0f) effectiveTarget = 5.0f;
    }

    float triggerPct = effectiveTarget - prof.hysteresisPct;
    if (moisture < triggerPct) {
        return { ScheduleDecision::START_CYCLE, "MOISTURE_LOW" };
    }

    return { ScheduleDecision::NO_ACTION, nullptr };
}

// ===========================================================================
// wateringTick — główna pętla FSM per-pot
// PLAN.md → "Pseudokod głównej pętli podlewania"
// Wywoływany z ControlTask co ~100ms
// ===========================================================================
void wateringTick(uint32_t nowMs,
                  const SensorSnapshot& sensors,
                  const Config& cfg,
                  WateringCycle cycles[],
                  WaterBudget& budget,
                  ActuatorState& actuator,
                  HardwareManager& hw)
{
    static uint32_t s_lastSafetyLogMs[kMaxPots] = {};
    static char s_lastSafetyReason[kMaxPots][32] = {};

    for (uint8_t potIdx = 0; potIdx < cfg.numPots; ++potIdx) {
        if (!cfg.pots[potIdx].enabled) continue;

        const PotConfig& potCfg = cfg.pots[potIdx];
        WateringCycle& cycle = cycles[potIdx];
        const PotSensorSnapshot& potSens = sensors.pots[potIdx];
        const PlantProfile& prof = getActiveProfile(cfg, potIdx);
        PumpActuator& pump = hw.pump(potIdx);

        // --- EXTENDED SAFETY (per-pot + global) ---
        SafetyResult safety = evaluateExtendedSafety(
            nowMs, potSens, cfg, potCfg, budget, actuator, potIdx);

        if (safety.hardBlock) {
            WateringPhase prevPhase = cycle.phase;
            if (cycle.phase == WateringPhase::PULSE && pump.isOn()) {
                pump.off(nowMs, safety.reason);
            }
            cycle.phase = WateringPhase::BLOCKED;

            const char* reason = safety.reason ? safety.reason : "?";
            bool reasonChanged = (strncmp(s_lastSafetyReason[potIdx], reason,
                                          sizeof(s_lastSafetyReason[potIdx]) - 1) != 0);
            bool timeElapsed = (nowMs - s_lastSafetyLogMs[potIdx]) >= 5000;

            if (reasonChanged || timeElapsed) {
                Serial.printf("[POT%d] SAFETY_BLOCK reason=%s\n", potIdx, reason);
                strncpy(s_lastSafetyReason[potIdx], reason,
                        sizeof(s_lastSafetyReason[potIdx]) - 1);
                s_lastSafetyReason[potIdx][sizeof(s_lastSafetyReason[potIdx]) - 1] = '\0';
                s_lastSafetyLogMs[potIdx] = nowMs;
            }
            if (reasonChanged || prevPhase != WateringPhase::BLOCKED) {
                WateringFeedbackCode code = feedbackCodeFromSafetyReason(reason);
                if (code != WateringFeedbackCode::NONE) {
                    publishFeedback(actuator, potIdx, code);
                }
            }
            continue;
        }

        switch (cycle.phase) {

        // ==================== IDLE ====================
        case WateringPhase::IDLE: {
            // Cooldown check
            uint32_t effCooldown = cfg.cooldownMs;
            if (cfg.vacationMode) {
                effCooldown = static_cast<uint32_t>(effCooldown * cfg.vacationCooldownMultiplier);
            }
            if (inCooldown(nowMs, effCooldown, actuator, potIdx)) {
                break;
            }

            ScheduleResult sched = evaluateSchedule(
                nowMs, potSens, sensors.env, cfg, potIdx);

            if (sched.decision == ScheduleDecision::START_CYCLE ||
                sched.decision == ScheduleDecision::RESCUE_WATER)
            {
                cycle = newCycle(prof, potCfg, potIdx);
                cycle.moistureBeforeCycle = potSens.moisturePct;
                cycle.phaseStartMs = nowMs;
                actuator.lastFeedbackCode[potIdx] = WateringFeedbackCode::NONE;
                actuator.lastFeedbackValue1[potIdx] = 0.0f;
                actuator.lastFeedbackValue2[potIdx] = 0.0f;
                actuator.lastFeedbackPulseCount[potIdx] = 0;

                Serial.printf("[POT%d] WATERING_CYCLE_START moisture=%.1f%% reason=%s\n",
                              potIdx, potSens.moisturePct,
                              sched.reason ? sched.reason : "?");
                publishFeedback(actuator, potIdx,
                                sched.decision == ScheduleDecision::RESCUE_WATER
                                    ? WateringFeedbackCode::CYCLE_START_RESCUE
                                    : WateringFeedbackCode::CYCLE_START_SCHEDULE,
                                potSens.moisturePct);
            }
            break;
        }

        // ==================== EVALUATING ====================
        case WateringPhase::EVALUATING: {
            float effectiveTarget = prof.targetMoisturePct;
            float effectiveMax    = prof.maxMoisturePct;
            if (cfg.vacationMode) {
                effectiveTarget -= cfg.vacationTargetReductionPct;
                if (effectiveTarget < 5.0f) effectiveTarget = 5.0f;
            }

            if (potSens.moisturePct >= effectiveTarget) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] WATERING_SKIP reason=already_wet moisture=%.1f%%\n",
                              potIdx, potSens.moisturePct);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::SKIP_ALREADY_WET,
                                potSens.moisturePct, effectiveTarget, cycle.pulseCount);
                break;
            }
            if (potSens.moisturePct >= effectiveMax) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] WATERING_SKIP reason=above_max moisture=%.1f%%\n",
                              potIdx, potSens.moisturePct);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::SKIP_ABOVE_MAX,
                                potSens.moisturePct, effectiveMax, cycle.pulseCount);
                break;
            }

            // Czas pulsu (per-pot configurable)
            cycle.pulseDurationMs = static_cast<uint32_t>(
                (potCfg.pulseWaterMl / potCfg.pumpMlPerSec) * 1000.0f);

            // Hard timeout cap
            if (cycle.pulseDurationMs > cfg.pumpOnMsMax) {
                cycle.pulseDurationMs = cfg.pumpOnMsMax;
            }

            // Crisis mode (reservoir LOW)
            if (budget.reservoirLow) {
                cycle.pulseDurationMs /= 3;
                cycle.maxPulses = (cycle.maxPulses > 2) ? 2 : cycle.maxPulses;
                Serial.printf("[WATER] event=crisis_mode reservoir_remaining_ml=%.0f\n",
                              budget.reservoirCurrentMl);

                if (budget.reservoirCurrentMl <= 0.0f) {
                    cycle.phase = WateringPhase::BLOCKED;
                    Serial.println("[WATER] event=reservoir_empty action=stop");
                    publishFeedback(actuator, potIdx, WateringFeedbackCode::SAFETY_BLOCK_RESERVOIR_EMPTY);
                    break;
                }
            }

            // Vacation max pulses override
            if (cfg.vacationMode) {
                uint8_t vacMax = cfg.vacationMaxPulsesOverride;
                if (vacMax < cycle.maxPulses) {
                    cycle.maxPulses = vacMax;
                }
            }

            cycle.phase = WateringPhase::PULSE;
            cycle.phaseStartMs = nowMs;
            break;
        }

        // ==================== PULSE ====================
        case WateringPhase::PULSE: {
            if (!pump.isOn()) {
                pump.on(nowMs, cycle.pulseDurationMs);
                Serial.printf("[POT%d] PULSE_START n=%d/%d duration=%dms\n",
                              potIdx, cycle.pulseCount + 1, cycle.maxPulses,
                              cycle.pulseDurationMs);
            }

            if ((nowMs - cycle.phaseStartMs) >= cycle.pulseDurationMs) {
                pump.off(nowMs, "pulse_done");
                cycle.pulseCount++;
                cycle.totalPumpedMs += cycle.pulseDurationMs;

                float pulsedMl = (cycle.pulseDurationMs / 1000.0f) * potCfg.pumpMlPerSec;
                cycle.totalPumpedMl += pulsedMl;
                addPumped(budget, pulsedMl, potIdx);

                Serial.printf("[POT%d] PULSE_END n=%d pumped_ml=%.1f total_ml=%.1f\n",
                              potIdx, cycle.pulseCount, pulsedMl, cycle.totalPumpedMl);

                // Overflow check
                if (potSens.waterGuards.potMax == WaterLevelState::TRIGGERED) {
                    cycle.phase = WateringPhase::OVERFLOW_WAIT;
                    cycle.phaseStartMs = nowMs;
                    Serial.printf("[POT%d] OVERFLOW_DETECTED after pulse %d\n",
                                  potIdx, cycle.pulseCount);
                    publishFeedback(actuator, potIdx, WateringFeedbackCode::OVERFLOW_DETECTED,
                                    potSens.moisturePct, 0.0f, cycle.pulseCount);
                } else {
                    cycle.phase = WateringPhase::SOAK;
                    cycle.phaseStartMs = nowMs;
                }
            }

            // Hard timeout safety — pompa nie powinna być ON dłużej niż pumpOnMsMax
            if (pump.isOn() && pump.onDuration(nowMs) > cfg.pumpOnMsMax) {
                pump.off(nowMs, "HARD_TIMEOUT");
                cycle.phase = WateringPhase::BLOCKED;
                Serial.printf("[POT%d] HARD_TIMEOUT pump forced off\n", potIdx);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::HARD_TIMEOUT,
                                pump.onDuration(nowMs), cfg.pumpOnMsMax, cycle.pulseCount);
            }
            break;
        }

        // ==================== SOAK ====================
        case WateringPhase::SOAK: {
            if ((nowMs - cycle.phaseStartMs) >= cycle.soakTimeMs) {
                cycle.phase = WateringPhase::MEASURING;
            }
            break;
        }

        // ==================== MEASURING ====================
        case WateringPhase::MEASURING: {
            float moistureNow = potSens.moisturePct;
            cycle.moistureAfterLastSoak = moistureNow;

            float effectiveTarget = prof.targetMoisturePct;
            if (cfg.vacationMode) {
                effectiveTarget -= cfg.vacationTargetReductionPct;
                if (effectiveTarget < 5.0f) effectiveTarget = 5.0f;
            }

            Serial.printf("[POT%d] SOAK_MEASURE moisture=%.1f%% target=%.1f%% pulse=%d/%d\n",
                          potIdx, moistureNow, effectiveTarget,
                          cycle.pulseCount, cycle.maxPulses);

            if (moistureNow >= effectiveTarget) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] WATERING_TARGET_REACHED moisture=%.1f%%\n",
                              potIdx, moistureNow);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::TARGET_REACHED,
                                moistureNow, effectiveTarget, cycle.pulseCount);
            } else if (moistureNow >= prof.maxMoisturePct) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] WATERING_STOP reason=max_exceeded moisture=%.1f%%\n",
                              potIdx, moistureNow);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::STOP_MAX_EXCEEDED,
                                moistureNow, prof.maxMoisturePct, cycle.pulseCount);
            } else if (cycle.pulseCount >= cycle.maxPulses) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] WATERING_STOP reason=max_pulses moisture=%.1f%% pulses=%d\n",
                              potIdx, moistureNow, cycle.pulseCount);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::STOP_MAX_PULSES,
                                moistureNow, cycle.maxPulses, cycle.pulseCount);
            } else if (potSens.waterGuards.potMax == WaterLevelState::TRIGGERED) {
                cycle.phase = WateringPhase::OVERFLOW_WAIT;
                cycle.phaseStartMs = nowMs;
                publishFeedback(actuator, potIdx, WateringFeedbackCode::OVERFLOW_DETECTED,
                                moistureNow, 0.0f, cycle.pulseCount);
            } else {
                // Kolejny puls
                cycle.phase = WateringPhase::PULSE;
                cycle.phaseStartMs = nowMs;
            }
            break;
        }

        // ==================== OVERFLOW_WAIT ====================
        case WateringPhase::OVERFLOW_WAIT: {
            uint32_t elapsed = nowMs - cycle.phaseStartMs;

            if (potSens.waterGuards.potMax != WaterLevelState::TRIGGERED) {
                Serial.printf("[POT%d] OVERFLOW_CLEARED after %ds\n", potIdx, elapsed / 1000);

                float effectiveTarget = prof.targetMoisturePct;
                if (cfg.vacationMode) {
                    effectiveTarget -= cfg.vacationTargetReductionPct;
                    if (effectiveTarget < 5.0f) effectiveTarget = 5.0f;
                }

                if (potSens.moisturePct < effectiveTarget) {
                    // Kontynuuj ze zmniejszonym pulsem
                    cycle.pulseDurationMs /= 3;
                    if (cycle.pulseDurationMs < 500) cycle.pulseDurationMs = 500;
                    cycle.phase = WateringPhase::PULSE;
                    cycle.phaseStartMs = nowMs;
                    Serial.printf("[POT%d] OVERFLOW_RESUME reduced_pulse=%dms\n",
                                  potIdx, cycle.pulseDurationMs);
                    publishFeedback(actuator, potIdx, WateringFeedbackCode::OVERFLOW_RESUME,
                                    potSens.moisturePct, effectiveTarget, cycle.pulseCount);
                } else {
                    cycle.phase = WateringPhase::DONE;
                }
            } else if (elapsed > cfg.overflowMaxWaitMs) {
                cycle.phase = WateringPhase::DONE;
                Serial.printf("[POT%d] OVERFLOW_TIMEOUT — ending cycle\n", potIdx);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::OVERFLOW_TIMEOUT,
                                elapsed / 1000.0f, cfg.overflowMaxWaitMs / 1000.0f, cycle.pulseCount);
            }
            // else: nadal czekamy
            break;
        }

        // ==================== DONE ====================
        case WateringPhase::DONE: {
            Serial.printf("[POT%d] WATERING_CYCLE_DONE pulses=%d total_ml=%.1f "
                          "before=%.1f%% after=%.1f%%\n",
                          potIdx, cycle.pulseCount, cycle.totalPumpedMl,
                          cycle.moistureBeforeCycle, cycle.moistureAfterLastSoak);

            WateringRecord rec{};
            rec.timestampMs = nowMs;
            rec.potIndex = potIdx;
            rec.pulseCount = cycle.pulseCount;
            rec.totalPumpedMl_x10 = static_cast<uint16_t>(
                std::min(65535.0f, cycle.totalPumpedMl * 10.0f));
            rec.moistureBefore_x10 = static_cast<uint16_t>(
                std::min(65535.0f, cycle.moistureBeforeCycle * 10.0f));
            rec.moistureAfter_x10 = static_cast<uint16_t>(
                std::min(65535.0f, cycle.moistureAfterLastSoak * 10.0f));
            rec.reason = static_cast<uint8_t>(actuator.lastFeedbackCode[potIdx]);
            historyAddWatering(g_history, rec);

            actuator.lastCycleDoneMs[potIdx] = nowMs;
            if (actuator.lastFeedbackCode[potIdx] == WateringFeedbackCode::NONE) {
                publishFeedback(actuator, potIdx, WateringFeedbackCode::CYCLE_DONE_GENERIC,
                                cycle.moistureAfterLastSoak, 0.0f, cycle.pulseCount);
            }
            cycle.reset();
            // cycle.phase = IDLE (po reset)
            break;
        }

        // ==================== BLOCKED ====================
        case WateringPhase::BLOCKED: {
            // Sprawdź czy blokada się skończyła
            SafetyResult recheck = evaluateExtendedSafety(
                nowMs, potSens, cfg, potCfg, budget, actuator, potIdx);
            if (!recheck.hardBlock) {
                cycle.phase = WateringPhase::IDLE;
                Serial.printf("[POT%d] SAFETY_UNBLOCK\n", potIdx);
                publishFeedback(actuator, potIdx, WateringFeedbackCode::SAFETY_UNBLOCK);
            }
            break;
        }

        } // switch
    } // for potIdx
}

// ===========================================================================
// manualPumpTick — Dual Button control
// PLAN.md → "Przycisk manualny (Dual Button) — bezpieczeństwo"
// ===========================================================================
void manualPumpTick(uint32_t nowMs,
                    const DualButtonState& btn,
                    const SensorSnapshot& sensors,
                    const Config& cfg,
                    ManualState& manual,
                    uint8_t selectedPot,
                    WaterBudget& budget,
                    HardwareManager& hw)
{
    // === RED: EMERGENCY STOP (wszystkie pompy) ===
    if (btn.redPressed) {
        for (uint8_t i = 0; i < cfg.numPots; ++i) {
            if (hw.pump(i).isOn()) {
                hw.pump(i).off(nowMs, "MANUAL_STOP");
                Serial.printf("[POT%d] MANUAL_STOP red_button\n", i);
            }
        }
        manual.blueOwnsPump = false;
        manual.locked = true;
        manual.lockUntilMs = nowMs + 5000;
        return;
    }

    // === Lock aktywny (po emergency stop / anti-spam) ===
    static uint32_t s_lastManualLockLogMs = 0;
    if (manual.locked && nowMs < manual.lockUntilMs) {
        if ((nowMs - s_lastManualLockLogMs) >= 2000) {
            s_lastManualLockLogMs = nowMs;
            Serial.printf("[MANUAL] event=lock_active remaining_s=%u\n",
                          static_cast<unsigned>((manual.lockUntilMs - nowMs) / 1000));
        }
        return;
    }
    manual.locked = false;

    if (selectedPot >= cfg.numPots) return;
    const PotSensorSnapshot& potSens = sensors.pots[selectedPot];

    // === BLUE: trzymaj = pompuj wybraną doniczkę ===
    if (btn.bluePressed) {
        // Nowe naciśnięcie — rejestruj timestamp
        if (manual.blueHeldMs == 0) {
            manual.blueHeldMs = nowMs;
            manual.blueOwnsPump = false;
            Serial.printf("[POT%d] event=manual_blue_pressed action=start_pump\n", selectedPot);

            // Anti-spam: rejestracja w historii
            if (manual.pressCount < ManualState::kMaxHistory) {
                manual.pressHistory[manual.pressCount++] = nowMs;
            } else {
                // Saturated history still counts as spam pressure
                manual.pressCount = ManualState::kMaxHistory;
            }
        }

        uint32_t heldDuration = nowMs - manual.blueHeldMs;

        // Max hold time
        if (heldDuration > cfg.manualMaxHoldMs) {
            if (manual.blueOwnsPump && hw.pump(selectedPot).isOn()) {
                hw.pump(selectedPot).off(nowMs, "MANUAL_MAX_HOLD");
            }
            Serial.printf("[POT%d] MANUAL_BLOCK reason=hold_too_long duration=%d\n",
                          selectedPot, heldDuration);
            manual.locked = true;
            manual.lockUntilMs = nowMs + cfg.manualCooldownMs;
            manual.blueHeldMs = 0;
            manual.blueOwnsPump = false;
            return;
        }

        // Overflow check per-pot (only if anti-overflow is enabled)
        if (cfg.antiOverflowEnabled && potSens.waterGuards.potMax == WaterLevelState::TRIGGERED) {
            if (manual.blueOwnsPump && hw.pump(selectedPot).isOn()) {
                hw.pump(selectedPot).off(nowMs, "MANUAL_OVERFLOW");
            }
            Serial.printf("[POT%d] MANUAL_BLOCK reason=overflow_sensor\n", selectedPot);
            manual.locked = true;
            manual.lockUntilMs = nowMs + 10000;
            manual.blueHeldMs = 0;
            manual.blueOwnsPump = false;
            return;
        }

        // Pompuj
        if (!hw.pump(selectedPot).isOn()) {
            if (hw.pump(selectedPot).on(nowMs, cfg.manualMaxHoldMs)) {
                manual.blueOwnsPump = true;
                Serial.printf("[POT%d] MANUAL_PUMP_ON\n", selectedPot);
            } else {
                manual.blueOwnsPump = false;
                Serial.printf("[POT%d] MANUAL_PUMP_ON_FAIL\n", selectedPot);
            }
        }
    } else {
        // Przycisk puszczony
        if (manual.blueOwnsPump && hw.pump(selectedPot).isOn() && manual.blueHeldMs > 0) {
            uint32_t heldDuration = nowMs - manual.blueHeldMs;
            float pumpedMl = (heldDuration / 1000.0f) * cfg.pots[selectedPot].pumpMlPerSec;
            hw.pump(selectedPot).off(nowMs, "MANUAL_RELEASE");
            addPumped(budget, pumpedMl, selectedPot);
            Serial.printf("[MANUAL] event=pump_off held_ms=%u pumped_ml=%.1f\n",
                          static_cast<unsigned>(heldDuration), pumpedMl);
        }
        manual.blueHeldMs = 0;
        manual.blueOwnsPump = false;
    }

    // === ANTI-SPAM: rate limit ===
    // Wyczyść stare wpisy (starsze niż 30s)
    uint8_t newCount = 0;
    for (uint8_t i = 0; i < manual.pressCount; ++i) {
        if ((nowMs - manual.pressHistory[i]) < 30000) {
            manual.pressHistory[newCount++] = manual.pressHistory[i];
        }
    }
    manual.pressCount = newCount;

    constexpr uint8_t kSpamThreshold = 5;
    if (manual.pressCount >= kSpamThreshold) {
        manual.locked = true;
        manual.lockUntilMs = nowMs + 20000;
        manual.blueHeldMs = 0;
        if (manual.blueOwnsPump && hw.pump(selectedPot).isOn()) {
            hw.pump(selectedPot).off(nowMs, "BUTTON_SPAM");
        }
        manual.blueOwnsPump = false;
        Serial.printf("[MANUAL] event=block reason=button_spam count=%u\n", manual.pressCount);
    }
}

// ===========================================================================
// Water budget functions
// ===========================================================================

void updateWaterBudget(uint32_t nowMs,
                       const SensorSnapshot& sensors,
                       WaterBudget& budget,
                       const Config& cfg)
{

    // Rezerwuar uses shared sensor (index irrelevant — stored in pots[0] waterGuards
    // or directly in SensorSnapshot)
    // NOTE: Reservoir sensor state is propagated to all pots[].waterGuards.reservoirMin
    // by readAllSensors(). We check pots[0] since it's always present.
    WaterLevelState resState = sensors.pots[0].waterGuards.reservoirMin;

    if (resState == WaterLevelState::OK) {
        budget.reservoirLow = false;
        budget.reservoirLowSinceMs = 0;
    } else if (resState == WaterLevelState::TRIGGERED) {
        if (!budget.reservoirLow) {
            budget.reservoirLow = true;
            budget.reservoirLowSinceMs = nowMs;
            budget.reservoirCurrentMl = budget.reservoirLowThresholdMl;
            Serial.printf("[WATER] event=reservoir_low remaining_est_ml=%.0f\n", budget.reservoirCurrentMl);
        }
    }

    // Estymacja bieżącego poziomu
    float estimated = budget.reservoirCapacityMl - budget.totalPumpedMl;
    if (estimated < 0.0f) estimated = 0.0f;

    // Jeśli sensor LOW i estymata > threshold → korekta
    if (budget.reservoirLow && estimated > budget.reservoirLowThresholdMl) {
        estimated = budget.reservoirLowThresholdMl;
    }
    budget.reservoirCurrentMl = estimated;

    // daysRemaining — average daily consumption since last refill
    // Uses persisted totalPumpedMl / elapsed days (survives reboots).
    // Needs at least 12h of data and >1ml pumped to be meaningful.
    if (budget.lastRefillMs > 0 && nowMs > budget.lastRefillMs) {
        float daysSinceRefill = (nowMs - budget.lastRefillMs) / 86400000.0f;
        if (daysSinceRefill >= 0.5f && budget.totalPumpedMl > 1.0f) {
            float avgDailyMl = budget.totalPumpedMl / daysSinceRefill;
            budget.daysRemaining = budget.reservoirCurrentMl / avgDailyMl;
        } else {
            budget.daysRemaining = 999.0f;   // not enough data yet
        }
    } else {
        budget.daysRemaining = 999.0f;
    }
}

void handleRefill(WaterBudget& budget, const Config& cfg) {
    budget.reservoirCurrentMl = cfg.reservoirCapacityMl;
    budget.reservoirCapacityMl = cfg.reservoirCapacityMl;
    budget.reservoirLowThresholdMl = cfg.reservoirLowThresholdMl;
    budget.totalPumpedMl = 0.0f;
    for (uint8_t i = 0; i < kMaxPots; ++i) {
        budget.totalPumpedMlPerPot[i] = 0.0f;
    }
    budget.reservoirLow = false;
    budget.reservoirLowSinceMs = 0;
    budget.daysRemaining = 999.0f;
    budget.lastRefillMs = millis();
    Serial.printf("[WATER] event=reservoir_refill capacity_ml=%.0f\n", budget.reservoirCapacityMl);
}

void addPumped(WaterBudget& budget, float ml, uint8_t potIdx) {
    budget.totalPumpedMl += ml;
    if (potIdx < kMaxPots) {
        budget.totalPumpedMlPerPot[potIdx] += ml;
    }
    budget.reservoirCurrentMl -= ml;
    if (budget.reservoirCurrentMl < 0.0f) {
        budget.reservoirCurrentMl = 0.0f;
    }
}

// ===========================================================================
// Vacation mode
// ===========================================================================

void applyVacationOverrides(const Config& cfg, const PlantProfile& base,
                            PlantProfile& effective)
{
    // Kopiuj base do effective
    effective = base;

    if (!cfg.vacationMode) return;

    // Obniż target
    effective.targetMoisturePct -= cfg.vacationTargetReductionPct;
    if (effective.targetMoisturePct < 5.0f) {
        effective.targetMoisturePct = 5.0f;
    }

    // Ogranicz pulsy
    if (cfg.vacationMaxPulsesOverride < effective.maxPulsesPerCycle) {
        effective.maxPulsesPerCycle = cfg.vacationMaxPulsesOverride;
    }

    // Soak time i cooldown handled inline w wateringTick

    Serial.printf("[VACATION] event=apply_overrides target_from_pct=%.1f target_to_pct=%.1f max_pulses_from=%u max_pulses_to=%u\n",
                  base.targetMoisturePct, effective.targetMoisturePct,
                  base.maxPulsesPerCycle, effective.maxPulsesPerCycle);
}

void handleVacationToggle(bool enable, Config& cfg) {
    bool prev = cfg.vacationMode;
    cfg.vacationMode = enable;
    if (prev != enable) {
        Serial.printf("[VACATION] event=toggle state=%s\n", enable ? "ON" : "OFF");
        // TODO(config): configSave(cfg) — powinno iść przez PersistQueue
    }
}

// ===========================================================================
// Weak stubs for analysis dependency — overridden when analysis.cpp is linked
// ===========================================================================
__attribute__((weak)) bool trendBaselineLearned(uint8_t potIdx) {
    (void)potIdx;
    return false;  // brak danych trendu — nie blokuj
}

__attribute__((weak)) float trendCurrentRate(uint8_t potIdx) {
    (void)potIdx;
    return 0.0f;
}
