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
        xQueueOverwrite(s_saveQueue, &cfg);
    }
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
    g_budget.reservoirCurrentMl      = g_config.reservoirCapacityMl;  // zakładamy pełny
    g_budget.reservoirLowThresholdMl = g_config.reservoirLowThresholdMl;

    // Tick counters
    uint32_t lastTick10ms  = millis();
    uint32_t lastTick100ms = millis();
    uint32_t lastTick1s    = millis();
    uint32_t lastLogDump   = millis() - 25000;   // first status dump after ~5s
    uint32_t lastEspAliveLog = millis() - 8000;

    SensorSnapshot snap{};

    for (;;) {
        uint32_t now = millis();

        // --- Generuj ticki ---
        if (now - lastTick10ms >= 10) {
            lastTick10ms = now;
            // 10ms: odczyt Dual Button, manual pump
            DualButtonState dualBtn = g_hardware.dualButton().read(now);
            manualPumpTick(now, dualBtn, snap, g_config, g_manual,
                           g_uiState.selectedPot, g_budget, g_hardware);
        }

        if (now - lastTick100ms >= 100) {
            lastTick100ms = now;

            // 100ms: pełny odczyt sensorów
            g_hardware.readAllSensors(now, g_config, snap);

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
                            g_hardware.pump(i).off(now, "mode_switch");
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
                    p.off(now, "HW_SAFETY_TIMEOUT");
                    Serial.printf("[POT%d] HW_SAFETY: pump forced off after %dms\n",
                                  i, p.onDuration(now));
                }
            }

            // Budżet rezerwuaru
            updateWaterBudget(now, snap, g_budget, g_config);
        }

        if (now - lastTick1s >= 1000) {
            lastTick1s = now;

            // Trend analysis per-pot
            for (uint8_t i = 0; i < g_config.numPots; ++i) {
                if (!g_config.pots[i].enabled) continue;
                trendTick(now, snap.pots[i].moistureEma,
                          g_trendStates[i], g_config);
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
            sample.moistureRaw = snap.pots[0].moistureRaw;  // TODO: per-pot history
            sample.tempC_x10  = (int16_t)(snap.env.tempC * 10);
            sample.lux         = (uint16_t)fminf(snap.env.lux, 65535.0f);
            sample.flags       = 0;
            if (g_budget.reservoirLow)          sample.flags |= 0x01;
            if (snap.pots[0].waterGuards.potMax == WaterLevelState::TRIGGERED)
                                                sample.flags |= 0x02;
            for (uint8_t i = 0; i < kMaxPots; ++i) {
                if (g_hardware.pump(i).isOn())  sample.flags |= 0x04;
            }
            historyTick(now, sample, g_history);
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
            ESP_LOGI(kLogTag, "ctrl alive, mode=%s, wifi=%s, heap=%u",
                     g_config.mode == Mode::AUTO ? "AUTO" : "MANUAL",
                     g_netState.wifiConnected ? "up" : "down",
                     (unsigned)ESP.getFreeHeap());
        }

        // --- Obsługa eventów (non-blocking) ---
        Event evt{};
        while (g_eventQueue.pop(evt, 0)) {
            switch (evt.type) {
                case EventType::REQUEST_SET_MODE:
                    g_config.mode = static_cast<Mode>(evt.payload.config.key);
                    requestConfigSave(g_config);
                    Serial.printf("[CTRL] mode=%d\n", (int)g_config.mode);
                    break;

                case EventType::REQUEST_SET_PLANT: {
                    uint8_t pot = evt.payload.config.key;
                    uint8_t prof = evt.payload.config.valueU16;
                    if (pot < kMaxPots && prof < kNumProfiles) {
                        g_config.pots[pot].plantProfileIndex = prof;
                        requestConfigSave(g_config);
                        Serial.printf("[CTRL] pot%d profile=%d\n", pot, prof);
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
                    }
                    break;
                }

                case EventType::REQUEST_REFILL:
                    handleRefill(g_budget, g_config);
                    Serial.println("[CTRL] reservoir refilled");
                    break;

                case EventType::REQUEST_VACATION_TOGGLE:
                    handleVacationToggle(!g_config.vacationMode, g_config);
                    requestConfigSave(g_config);
                    Serial.printf("[CTRL] vacation=%d\n", g_config.vacationMode);
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

static void uiTaskFn(void* /*param*/) {
    Serial.println("[UI] started");
    uiInit();

    for (;;) {
        uint32_t now = millis();
        M5.update();

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
                bool changed = uiHandleBtnBLong(g_uiState, g_config, g_budget);
                if (changed) {
                    g_eventQueue.push(Event{EventType::CONFIG_SAVE_REQUEST});
                }
            }
            g_uiState.needsRedraw = true;
        }

        // BtnB: nawigacja (w Settings: następna opcja, w MAIN: przełącz widok)
        if (M5.BtnB.wasClicked()) {
            uiHandleBtnB(g_uiState, g_config);
            g_uiState.needsRedraw = true;
        }

        // BtnB long press: alternatywna zmiana wartości w Settings
        static bool s_btnBLongHandled = false;
        if (M5.BtnB.pressedFor(800) && !s_btnBLongHandled) {
            s_btnBLongHandled = true;
            if (g_uiState.screen == UiScreen::SETTINGS) {
                bool changed = uiHandleBtnBLong(g_uiState, g_config, g_budget);
                if (changed) {
                    g_eventQueue.push(Event{EventType::CONFIG_SAVE_REQUEST});
                }
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
            memcpy(uSnap.cycles, g_cycles, sizeof(g_cycles));
            uSnap.budget        = g_budget;
            uSnap.config        = g_config;
            uSnap.netConfig     = g_netConfig;
            uSnap.duskPhase     = g_duskDetector.phase;
            uSnap.wifiConnected = g_netState.wifiConnected;

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

        // Heartbeat check
        if (g_netState.wifiConnected && g_netState.telegramEnabled) {
            if (isDailyHeartbeatTime(now, g_solarClock, g_duskDetector,
                                     false /* NTP TODO */, g_netState)) {
                DailyReportData rptData{};
                rptData.sensors  = readSnapshot();
                rptData.budget   = g_budget;
                memcpy(rptData.trends, g_trendStates, sizeof(g_trendStates));
                rptData.config   = g_config;
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

        // Krótki timeout — max 8s, nie blokuj dłużej
        uint32_t wifiDeadline = millis() + 8000;
        while (WiFi.status() != WL_CONNECTED && millis() < wifiDeadline) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            g_netState.wifiConnected = true;
            MDNS.begin("autogarden");
            Serial.printf("[BOOT] WiFi OK: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[BOOT] WiFi not connected — will retry in NetTask");
            WiFi.disconnect();
        }
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
    s_saveQueue = xQueueCreate(1, sizeof(Config));  // overwrite queue (1 slot)

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
