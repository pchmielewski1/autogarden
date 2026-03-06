// ============================================================================
// analysis.cpp — EMA, trend, dusk detector, solar clock, history
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Filtracja odczytów — EMA",
//                "Analiza trendów", "Detektor zmierzchu/świtu",
//                "Estymacja zegara słonecznego", "Historia pomiarów"
// ============================================================================

#include "analysis.h"
#include "events.h"
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <cmath>
#include "log_serial.h"

#define Serial AGSerial

// ===========================================================================
// Globalne instancje
// ===========================================================================
TrendState g_trendStates[kMaxPots];

// ===========================================================================
// EmaFilter
// ===========================================================================
float EmaFilter::update(float sample, uint32_t nowMs) {
    if (!initialized) {
        value = sample;
        initialized = true;
        lastUpdateMs = nowMs;
        return value;
    }

    uint32_t gap = nowMs - lastUpdateMs;
    if (gap > kMaxGapMs) {
        // Sensor był offline >60s → reinicjalizuj
        Serial.printf("[ANL] event=ema_reinit gap_ms=%u\n", gap);
        value = sample;
    } else {
        value = alpha * sample + (1.0f - alpha) * value;
    }
    lastUpdateMs = nowMs;
    return value;
}

void EmaFilter::reset() {
    value = NAN;
    initialized = false;
    lastUpdateMs = 0;
}

// ===========================================================================
// TrendState — analiza tendencji schnięcia gleby
// PLAN.md → "Analiza trendów"
// ===========================================================================

// Helper: median z tablicy float (in-place sort)
static float medianOf(float arr[], uint8_t n) {
    // Bubble sort — max 24 elementów, OK
    for (uint8_t i = 0; i < n; ++i) {
        for (uint8_t j = i + 1; j < n; ++j) {
            if (arr[j] < arr[i]) {
                float t = arr[i]; arr[i] = arr[j]; arr[j] = t;
            }
        }
    }
    if (n % 2 == 0) return (arr[n / 2 - 1] + arr[n / 2]) / 2.0f;
    return arr[n / 2];
}

void trendTick(uint32_t nowMs, float moisturePct, TrendState& ts, const Config& cfg) {
    if (std::isnan(moisturePct)) return;

    // Pierwszy odczyt
    if (ts.lastSampleMs == 0) {
        ts.lastMoisturePct = moisturePct;
        ts.lastSampleMs = nowMs;
        return;
    }

    uint32_t elapsed = nowMs - ts.lastSampleMs;
    if (elapsed < 3600000UL) return;   // co 1h

    // Delta %/h
    float hours = elapsed / 3600000.0f;
    float deltaPerHour = (moisturePct - ts.lastMoisturePct) / hours;

    ts.hourlyDeltas[ts.headIdx] = deltaPerHour;
    ts.headIdx = (ts.headIdx + 1) % TrendState::kHours;
    if (ts.count < TrendState::kHours) ts.count++;
    ts.lastMoisturePct = moisturePct;
    ts.lastSampleMs = nowMs;

    // Wyucz baseline (median ujemnych deltów — naturalne schnięcie)
    if (ts.count >= 6) {
        float negatives[TrendState::kHours];
        uint8_t nNeg = 0;
        for (uint8_t i = 0; i < ts.count && nNeg < TrendState::kHours; ++i) {
            if (ts.hourlyDeltas[i] < 0.0f) {
                negatives[nNeg++] = ts.hourlyDeltas[i];
            }
        }
        if (nNeg >= 3) {
            ts.normalDryingRate = medianOf(negatives, nNeg);
            ts.baselineCalibrated = true;
        }
    }

    // Test anomalii
    if (ts.baselineCalibrated) {
        float threshold = ts.normalDryingRate * cfg.anomalyDryingRateMultiplier;
        if (deltaPerHour < threshold) {
            Serial.printf("[ANL] event=trend_anomaly rate_pct_h=%.1f baseline_pct_h=%.1f\n",
                          deltaPerHour, ts.normalDryingRate);
        }
    } else {
        if (deltaPerHour < -cfg.anomalyDryingRateThreshold) {
            Serial.printf("[ANL] event=trend_anomaly rate_pct_h=%.1f baseline=unlearned\n", deltaPerHour);
        }
    }

    Serial.printf("[ANL] event=trend_sample delta_pct_h=%.2f baseline_pct_h=%.2f calibrated=%s\n",
                  deltaPerHour, ts.normalDryingRate,
                  ts.baselineCalibrated ? "yes" : "no");
}

// Strong definitions — override weak stubs in watering.cpp
bool trendBaselineLearned(uint8_t potIdx) {
    if (potIdx >= kMaxPots) return false;
    return g_trendStates[potIdx].baselineCalibrated;
}

float trendCurrentRate(uint8_t potIdx) {
    if (potIdx >= kMaxPots) return 0.0f;
    TrendState& ts = g_trendStates[potIdx];
    if (ts.count == 0) return 0.0f;
    uint8_t lastIdx = (ts.headIdx == 0) ? (TrendState::kHours - 1) : (ts.headIdx - 1);
    return ts.hourlyDeltas[lastIdx];
}

// ===========================================================================
// DuskDetector — scoring + FSM
// PLAN.md → "Detektor zmierzchu/świtu — fuzja sensorowa"
// ===========================================================================

// Wagi zmierzchu
static constexpr float kDuskW_light     = 0.35f;
static constexpr float kDuskW_lightRate = 0.30f;
static constexpr float kDuskW_temp      = 0.15f;
static constexpr float kDuskW_humidity  = 0.10f;
static constexpr float kDuskW_pressure  = 0.10f;

// Wagi świtu
static constexpr float kDawnW_light     = 0.50f;
static constexpr float kDawnW_lightRate = 0.20f;
static constexpr float kDawnW_temp      = 0.15f;
static constexpr float kDawnW_humidity  = 0.10f;
static constexpr float kDawnW_pressure  = 0.05f;

// Helper: clamp do 0..1
static float clamp01(float x) {
    return (x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x;
}

static float mapRange(float x, float inMin, float inMax, float outMin, float outMax) {
    float t = (x - inMin) / (inMax - inMin);
    return outMin + clamp01(t) * (outMax - outMin);
}

// ---------------------------------------------------------------------------
// computeDerivatives — pochodne z okna ring buffer
// ---------------------------------------------------------------------------
EnvDerivatives computeDerivatives(const EnvSample* window, uint8_t count,
                                  uint8_t head, uint8_t windowSize,
                                  uint8_t lookbackSamples)
{
    EnvDerivatives d{};
    if (count < 2) return d;

    uint8_t n = (lookbackSamples < count) ? lookbackSamples : count;

    // Najnowszy i najstarszy w zakresie
    uint8_t newestIdx = (head == 0) ? (windowSize - 1) : (head - 1);
    uint8_t oldestIdx = newestIdx;
    for (uint8_t i = 0; i < n - 1; ++i) {
        oldestIdx = (oldestIdx == 0) ? (windowSize - 1) : (oldestIdx - 1);
    }

    const EnvSample& newest = window[newestIdx];
    const EnvSample& oldest = window[oldestIdx];

    float dtMin = (newest.ms - oldest.ms) / 60000.0f;
    if (dtMin < 0.1f) return d;

    d.dLux_dt   = (newest.lux     - oldest.lux)     / dtMin;
    d.dTemp_dt  = (newest.tempC   - oldest.tempC)   / dtMin;
    d.dHum_dt   = (newest.humPct  - oldest.humPct)  / dtMin;
    d.dPress_dt = (newest.pressHpa - oldest.pressHpa) / dtMin;

    return d;
}

// ---------------------------------------------------------------------------
// scoreDusk — PLAN.md → scoring zmierzchu
// ---------------------------------------------------------------------------
DuskScores scoreDusk(float lux, const EnvDerivatives& d) {
    DuskScores s{};

    // 1. Light absolute
    if (lux > 10000.0f)      s.light = 0.0f;
    else if (lux > 1000.0f)  s.light = mapRange(lux, 10000.0f, 1000.0f, 0.0f, 0.3f);
    else if (lux > 200.0f)   s.light = mapRange(lux, 1000.0f, 200.0f, 0.3f, 0.7f);
    else if (lux > 10.0f)    s.light = mapRange(lux, 200.0f, 10.0f, 0.7f, 1.0f);
    else                      s.light = 1.0f;

    // 2. Light rate
    float relRate = (lux > 10.0f) ? (d.dLux_dt / lux) : d.dLux_dt;
    if (relRate > -0.005f)       s.lightRate = 0.0f;
    else if (relRate > -0.02f)   s.lightRate = mapRange(relRate, -0.005f, -0.02f, 0.0f, 1.0f);
    else if (relRate > -0.1f)    s.lightRate = 0.8f;
    else                          s.lightRate = 0.2f;

    // 3. Temperature declining
    if (d.dTemp_dt < -0.005f)
        s.temp = clamp01(fabsf(d.dTemp_dt) / 0.05f);
    else
        s.temp = 0.0f;

    // 4. Humidity rising
    if (d.dHum_dt > 0.005f)
        s.humidity = clamp01(d.dHum_dt / 0.05f);
    else
        s.humidity = 0.0f;

    // 5. Pressure stability
    if (fabsf(d.dPress_dt) < 0.02f)      s.pressure = 1.0f;
    else if (d.dPress_dt > 0.0f)          s.pressure = 0.9f;
    else if (d.dPress_dt > -0.03f)        s.pressure = 0.5f;
    else                                   s.pressure = 0.0f;

    return s;
}

// ---------------------------------------------------------------------------
// scoreDawn — PLAN.md → scoring świtu
// ---------------------------------------------------------------------------
DuskScores scoreDawn(float lux, const EnvDerivatives& d) {
    DuskScores s{};

    // Light rising from darkness
    if (lux < 1.0f)          s.light = 0.0f;
    else if (lux < 50.0f)    s.light = 0.3f + 0.4f * (lux / 50.0f);
    else if (lux < 500.0f)   s.light = 0.7f + 0.3f * clamp01(lux / 500.0f);
    else                      s.light = 1.0f;

    // Light rate rising
    if (d.dLux_dt > 0.5f)
        s.lightRate = clamp01(d.dLux_dt / 50.0f);
    else
        s.lightRate = 0.0f;

    // Temp: nie spada → rośnie
    if (d.dTemp_dt > -0.005f)
        s.temp = clamp01((d.dTemp_dt + 0.005f) / 0.03f);
    else
        s.temp = 0.0f;

    // Humidity: nie rośnie → spada
    if (d.dHum_dt < 0.005f)
        s.humidity = clamp01((0.005f - d.dHum_dt) / 0.03f);
    else
        s.humidity = 0.0f;

    // Pressure: neutralne
    s.pressure = 0.5f;

    return s;
}

// Composite score helpers
static float duskComposite(const DuskScores& s) {
    return s.light     * kDuskW_light
         + s.lightRate * kDuskW_lightRate
         + s.temp      * kDuskW_temp
         + s.humidity  * kDuskW_humidity
         + s.pressure  * kDuskW_pressure;
}

static float dawnComposite(const DuskScores& s) {
    return s.light     * kDawnW_light
         + s.lightRate * kDawnW_lightRate
         + s.temp      * kDawnW_temp
         + s.humidity  * kDawnW_humidity
         + s.pressure  * kDawnW_pressure;
}

// ---------------------------------------------------------------------------
// duskDetectorTick — FSM: NIGHT ↔ DAY z transitentami
// ---------------------------------------------------------------------------
void duskDetectorTick(uint32_t nowMs, float lux, float tempC,
                      float humPct, float pressHpa,
                      DuskDetector& det, const Config& cfg)
{
    // --- Dodaj próbkę do okna ---
    bool addSample = false;
    if (det.count == 0) {
        addSample = true;
    } else {
        uint8_t prevIdx = (det.head == 0) ? (DuskDetector::kWindowSize - 1) : (det.head - 1);
        if ((nowMs - det.window[prevIdx].ms) >= DuskDetector::kEnvSampleIntervalMs) {
            addSample = true;
        }
    }

    if (addSample) {
        det.window[det.head] = { nowMs, lux, tempC, humPct, pressHpa };
        det.head = (det.head + 1) % DuskDetector::kWindowSize;
        if (det.count < DuskDetector::kWindowSize) det.count++;
    }

    if (det.count < 10) return;  // za mało danych

    EnvDerivatives deriv = computeDerivatives(
        det.window, det.count, det.head, DuskDetector::kWindowSize, 10);

    switch (det.phase) {

    // ==================== NIGHT ====================
    case DuskPhase::NIGHT: {
        DuskScores dawn = scoreDawn(lux, deriv);
        det.dawnScore = dawnComposite(dawn);

        // Próg wejścia: DAWN_SCORE_ENTER = 0.50
        float enterTh = cfg.duskScoreEnterThreshold - 0.05f;  // dawn threshold ~0.50
        if (det.dawnScore >= enterTh) {
            // Sprawdź min czas nocy
            if (det.lastDuskMs > 0) {
                uint32_t nightSoFar = nowMs - det.lastDuskMs;
                if (nightSoFar < DuskDetector::kMinNightDurationMs) {
                    break;  // za krótka noc
                }
            }
            det.phase = DuskPhase::DAWN_TRANSITION;
            det.transitionStartMs = nowMs;
            Serial.printf("[DUSK] event=dawn_transition_start score=%.2f lux=%.0f\n", det.dawnScore, lux);
        }
        break;
    }

    // ==================== DAWN_TRANSITION ====================
    case DuskPhase::DAWN_TRANSITION: {
        DuskScores dawn = scoreDawn(lux, deriv);
        det.dawnScore = dawnComposite(dawn);
        uint32_t elapsed = nowMs - det.transitionStartMs;

        // Cancel threshold: ~0.25
        float cancelTh = cfg.duskScoreCancelThreshold - 0.05f;
        float confirmTh = cfg.duskScoreConfirmThreshold - 0.05f;  // ~0.60

        if (det.dawnScore < cancelTh) {
            det.phase = DuskPhase::NIGHT;
            Serial.printf("[DUSK] event=dawn_cancel score=%.2f elapsed_min=%u\n",
                          det.dawnScore, elapsed / 60000);
        } else if (det.dawnScore >= confirmTh && elapsed >= cfg.transitionConfirmMs) {
            det.phase = DuskPhase::DAY;
            det.lastDawnMs = nowMs;
            if (det.lastDuskMs > 0) {
                det.nightLengthMs = nowMs - det.lastDuskMs;
                Serial.printf("[DUSK] event=dawn_confirmed night_len_min=%u\n", det.nightLengthMs / 60000);
            } else {
                Serial.println("[DUSK] event=dawn_confirmed first_since_boot=yes");
            }
            duskStateSave(det);
        } else if (elapsed > DuskDetector::kTransitionMaxDurationMs) {
            det.phase = DuskPhase::DAY;
            det.lastDawnMs = nowMs;
            Serial.printf("[DUSK] event=dawn_timeout elapsed_min=%u action=assume_day\n", elapsed / 60000);
            duskStateSave(det);
        }
        break;
    }

    // ==================== DAY ====================
    case DuskPhase::DAY: {
        DuskScores dusk = scoreDusk(lux, deriv);
        det.duskScore = duskComposite(dusk);

        if (det.duskScore >= cfg.duskScoreEnterThreshold) {
            // Sprawdź min czas dnia
            if (det.lastDawnMs > 0) {
                uint32_t daySoFar = nowMs - det.lastDawnMs;
                if (daySoFar < DuskDetector::kMinDayDurationMs) {
                    break;
                }
            }
            det.phase = DuskPhase::DUSK_TRANSITION;
            det.transitionStartMs = nowMs;
            Serial.printf("[DUSK] event=dusk_transition_start score=%.2f lux=%.0f dlux_per_min=%.1f\n",
                          det.duskScore, lux, deriv.dLux_dt);
        }
        break;
    }

    // ==================== DUSK_TRANSITION ====================
    case DuskPhase::DUSK_TRANSITION: {
        DuskScores dusk = scoreDusk(lux, deriv);
        det.duskScore = duskComposite(dusk);
        uint32_t elapsed = nowMs - det.transitionStartMs;

        if (det.duskScore < cfg.duskScoreCancelThreshold) {
            det.phase = DuskPhase::DAY;
            Serial.printf("[DUSK] event=dusk_cancel score=%.2f elapsed_min=%u lux=%.0f\n",
                          det.duskScore, elapsed / 60000, lux);
        } else if (det.duskScore >= cfg.duskScoreConfirmThreshold
                   && elapsed >= cfg.transitionConfirmMs)
        {
            det.phase = DuskPhase::NIGHT;
            det.lastDuskMs = nowMs;
            if (det.lastDawnMs > 0) {
                det.dayLengthMs = nowMs - det.lastDawnMs;
                Serial.printf("[DUSK] event=dusk_confirmed day_len_min=%u\n", det.dayLengthMs / 60000);
            } else {
                Serial.println("[DUSK] event=dusk_confirmed first_since_boot=yes");
            }
            duskStateSave(det);
            // >>> PODLEWANIE OKNO <<<
            // TODO(events): eventQueue.push(Event::tick(EventType::DUSK_DETECTED))
        } else if (elapsed > DuskDetector::kTransitionMaxDurationMs) {
            det.phase = DuskPhase::NIGHT;
            det.lastDuskMs = nowMs;
            Serial.printf("[DUSK] event=dusk_timeout elapsed_min=%u action=assume_night\n", elapsed / 60000);
            duskStateSave(det);
        }
        break;
    }

    } // switch
}

// ===========================================================================
// DuskState — NVS persistence
// ===========================================================================
bool duskStateSave(const DuskDetector& det) {
    Preferences prefs;
    if (!prefs.begin(kNvsDusk, false)) {
        Serial.println("[DUSK] event=nvs_save_failed reason=open_namespace");
        return false;
    }
    DuskState st;
    st.phase         = static_cast<uint8_t>(det.phase);
    st.dayLengthMs   = det.dayLengthMs;
    st.nightLengthMs = det.nightLengthMs;
    size_t written = prefs.putBytes("dusk", &st, sizeof(DuskState));
    prefs.end();
    Serial.printf("[DUSK] event=nvs_saved phase=%d day_min=%u night_min=%u\n",
                  st.phase, st.dayLengthMs / 60000, st.nightLengthMs / 60000);
    return written == sizeof(DuskState);
}

bool duskStateLoad(DuskDetector& det) {
    Preferences prefs;
    if (!prefs.begin(kNvsDusk, true)) return false;
    if (!prefs.isKey("dusk")) { prefs.end(); return false; }
    DuskState st;
    size_t len = prefs.getBytes("dusk", &st, sizeof(DuskState));
    prefs.end();
    if (len != sizeof(DuskState)) return false;
    if (st.phase > static_cast<uint8_t>(DuskPhase::DUSK_TRANSITION)) return false;

    // Restore phase — but only stable phases (DAY/NIGHT).
    // Transitions are volatile, revert to the stable side.
    DuskPhase restored;
    switch (static_cast<DuskPhase>(st.phase)) {
        case DuskPhase::DAY:
        case DuskPhase::DAWN_TRANSITION:  // was becoming day
            restored = DuskPhase::DAY;
            break;
        case DuskPhase::NIGHT:
        case DuskPhase::DUSK_TRANSITION:  // was becoming night
        default:
            restored = DuskPhase::NIGHT;
            break;
    }
    det.phase = restored;
    det.dayLengthMs   = st.dayLengthMs;
    det.nightLengthMs = st.nightLengthMs;
    Serial.printf("[DUSK] event=nvs_restored phase=%s day_min=%u night_min=%u\n",
                  (restored == DuskPhase::DAY) ? "DAY" : "NIGHT",
                  st.dayLengthMs / 60000, st.nightLengthMs / 60000);
    return true;
}

void duskBootstrap(DuskDetector& det, float lux) {
    // Quick heuristic from first sensor read — override only if confident.
    // If NVS already restored a phase, still override if sensor strongly disagrees.
    //   lux > 200 → definitely daytime (indoor artificial light rarely that high)
    //   lux < 5   → definitely night
    //   5..200    → ambiguous, trust NVS phase
    DuskPhase before = det.phase;
    if (lux > 200.0f && det.phase == DuskPhase::NIGHT) {
        det.phase = DuskPhase::DAY;
        Serial.printf("[DUSK] event=bootstrap lux=%.0f from=NIGHT to=DAY\n", lux);
    } else if (lux < 5.0f && det.phase == DuskPhase::DAY) {
        det.phase = DuskPhase::NIGHT;
        Serial.printf("[DUSK] event=bootstrap lux=%.0f from=DAY to=NIGHT\n", lux);
    } else {
        Serial.printf("[DUSK] event=bootstrap lux=%.0f action=keep_nvs phase=%s\n", lux,
                      (det.phase == DuskPhase::DAY) ? "DAY" : "NIGHT");
    }
}

// ===========================================================================
// SolarClock
// ===========================================================================
void updateSolarClock(const DuskDetector& det, SolarClock& clk) {
    if (det.dayLengthMs > 0 && det.nightLengthMs > 0) {
        bool transitionUpdated = (det.lastDawnMs != clk.lastDawnMs) ||
                                 (det.lastDuskMs != clk.lastDuskMs);

        clk.dayLengthMs   = det.dayLengthMs;
        clk.nightLengthMs = det.nightLengthMs;
        clk.estimatedDayMs = clk.dayLengthMs + clk.nightLengthMs;
        if (transitionUpdated) {
            clk.lastDawnMs = det.lastDawnMs;
            clk.lastDuskMs = det.lastDuskMs;
            if (det.lastDawnMs > 0 && det.lastDuskMs > 0) {
                clk.cycleCount++;
            }
            Serial.printf("[SOLAR] event=clock_update day_h=%u day_m=%u night_h=%u night_m=%u cycle=%u\n",
                          clk.dayLengthMs / 3600000, (clk.dayLengthMs / 60000) % 60,
                          clk.nightLengthMs / 3600000, (clk.nightLengthMs / 60000) % 60,
                          clk.cycleCount);
        }
        clk.calibrated = true;
    }
}

uint32_t estimateNextDawn(const DuskDetector& det, const SolarClock& clk, uint32_t nowMs) {
    (void)nowMs;
    if (!clk.calibrated) return 0;  // UNKNOWN
    if (det.phase == DuskPhase::NIGHT || det.phase == DuskPhase::DAWN_TRANSITION) {
        return det.lastDuskMs + clk.nightLengthMs;
    }
    return det.lastDawnMs + clk.dayLengthMs + clk.nightLengthMs;
}

uint32_t estimateNextDusk(const DuskDetector& det, const SolarClock& clk, uint32_t nowMs) {
    (void)nowMs;
    if (!clk.calibrated) return 0;
    if (det.phase == DuskPhase::DAY || det.phase == DuskPhase::DUSK_TRANSITION) {
        return det.lastDawnMs + clk.dayLengthMs;
    }
    uint32_t nextDawn = det.lastDuskMs + clk.nightLengthMs;
    return nextDawn + clk.dayLengthMs;
}

// ===========================================================================
// RingBuffer template implementation
// ===========================================================================
template<typename T, uint16_t MAX_SIZE>
void RingBuffer<T, MAX_SIZE>::push(const T& item) {
    _buf[_head] = item;
    _head = (_head + 1) % MAX_SIZE;
    if (_count < MAX_SIZE) _count++;
}

template<typename T, uint16_t MAX_SIZE>
const T& RingBuffer<T, MAX_SIZE>::at(uint16_t idx) const {
    // idx=0 → oldest
    uint16_t start = (_count < MAX_SIZE) ? 0 : _head;
    return _buf[(start + idx) % MAX_SIZE];
}

template<typename T, uint16_t MAX_SIZE>
void RingBuffer<T, MAX_SIZE>::clear() {
    _head = 0;
    _count = 0;
}

// Explicit instantiations
template class RingBuffer<SensorSample, 180>;
template class RingBuffer<SensorSample, 288>;
template class RingBuffer<SensorSample, 720>;
template class RingBuffer<WateringRecord, 100>;

// ===========================================================================
// SensorHistory — tick + NVS flush
// ===========================================================================
void historyTick(uint32_t nowMs, const SensorSample& sample, SensorHistory& hist) {
    auto averageLevel1Tail = [&](uint16_t samplesToAvg, SensorSample& out) -> bool {
        uint16_t size = hist.level1.size();
        if (size == 0) return false;
        uint16_t n = std::min<uint16_t>(samplesToAvg, size);
        uint32_t sumMoist = 0;
        int32_t sumTemp = 0;
        uint32_t sumLux = 0;
        uint8_t flagsOr = 0;
        uint32_t ts = 0;
        for (uint16_t i = size - n; i < size; ++i) {
            const auto& s = hist.level1.at(i);
            sumMoist += s.moistureRaw;
            sumTemp += s.tempC_x10;
            sumLux += s.lux;
            flagsOr |= s.flags;
            ts = s.timestampMs;
        }
        out.timestampMs = ts;
        out.moistureRaw = static_cast<uint16_t>(sumMoist / n);
        out.tempC_x10 = static_cast<int16_t>(sumTemp / static_cast<int32_t>(n));
        out.lux = static_cast<uint16_t>(sumLux / n);
        out.flags = flagsOr;
        out._pad = 0;
        return true;
    };

    auto averageLevel2Tail = [&](uint16_t samplesToAvg, SensorSample& out) -> bool {
        uint16_t size = hist.level2.size();
        if (size == 0) return false;
        uint16_t n = std::min<uint16_t>(samplesToAvg, size);
        uint32_t sumMoist = 0;
        int32_t sumTemp = 0;
        uint32_t sumLux = 0;
        uint8_t flagsOr = 0;
        uint32_t ts = 0;
        for (uint16_t i = size - n; i < size; ++i) {
            const auto& s = hist.level2.at(i);
            sumMoist += s.moistureRaw;
            sumTemp += s.tempC_x10;
            sumLux += s.lux;
            flagsOr |= s.flags;
            ts = s.timestampMs;
        }
        out.timestampMs = ts;
        out.moistureRaw = static_cast<uint16_t>(sumMoist / n);
        out.tempC_x10 = static_cast<int16_t>(sumTemp / static_cast<int32_t>(n));
        out.lux = static_cast<uint16_t>(sumLux / n);
        out.flags = flagsOr;
        out._pad = 0;
        return true;
    };

    // Level 1: co 10 s
    if ((nowMs - hist.lastLevel1AddMs) >= 10000) {
        hist.level1.push(sample);
        hist.lastLevel1AddMs = nowMs;
    }

    // Level 2: co 5 min — downsample z level1
    if ((nowMs - hist.lastLevel2FlushMs) >= 300000) {
        if (!hist.level1.empty()) {
            SensorSample avg = sample;
            if (averageLevel1Tail(30, avg)) {
                hist.level2.push(avg);
            }
        }
        hist.lastLevel2FlushMs = nowMs;
    }

    // Level 3: co 1h — downsample z level2
    if ((nowMs - hist.lastLevel3FlushMs) >= 3600000) {
        if (!hist.level2.empty()) {
            SensorSample avg = sample;
            if (averageLevel2Tail(12, avg)) {
                hist.level3.push(avg);
            }
        }
        hist.lastLevel3FlushMs = nowMs;
    }
}

void historyAddWatering(SensorHistory& hist, const WateringRecord& rec) {
    hist.wateringLog.push(rec);
}

float historyCalcDailyConsumption(const SensorHistory& hist, uint32_t nowMs) {
    float total = 0.0f;
    uint16_t count = 0;
    for (uint16_t i = 0; i < hist.wateringLog.size(); ++i) {
        const WateringRecord& r = hist.wateringLog.at(i);
        if ((nowMs - r.timestampMs) < 86400000UL) {  // last 24h
            total += r.totalPumpedMl_x10 / 10.0f;
            count++;
        }
    }
    return total;
}
