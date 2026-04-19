// ============================================================================
// ui.h — GUI na StickS3 LCD (135×240), nawigacja, Settings
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Ekran StickS3 — design UI",
//                "Ekran Settings", "Nawigacja ekranów"
// Architektura:  docs/ARCHITECTURE.md
//
// Wyświetlacz: M5.Display (M5Unified) — 135×240 portrait.
// Przyciski: BtnA (front) = Settings toggle, BtnB (bok) = switch view.
// ============================================================================
#pragma once

#include <cstdint>
#include "config.h"
#include "hardware.h"
#include "watering.h"
#include "analysis.h"

// ---------------------------------------------------------------------------
// Ekrany / widoki
// ---------------------------------------------------------------------------
enum class UiScreen : uint8_t {
    MAIN,
    SETTINGS,
};

enum class DualViewMode : uint8_t {
    COMPACT,         // obie doniczki kompaktowo
    DETAIL_POT0,     // pełny widok pot 0
    DETAIL_POT1,     // pełny widok pot 1
};

// ---------------------------------------------------------------------------
// Stan UI (utrzymywany w UiTask)
// ---------------------------------------------------------------------------
struct UiState {
    UiScreen     screen          = UiScreen::MAIN;
    DualViewMode dualViewMode    = DualViewMode::COMPACT;
    uint8_t      settingsIndex   = 0;
    uint8_t      selectedPot     = 0;  // Dual Button steruje tą doniczką
    bool         needsRedraw     = true;

    // Anti‑flicker
    uint32_t lastRedrawMs        = 0;
    static constexpr uint32_t kMinRedrawIntervalMs = 200;
};

// ---------------------------------------------------------------------------
// UiSnap — snapshot danych do renderowania (kopiowany raz na ramkę)
// ---------------------------------------------------------------------------
struct UiSnap {
    SensorSnapshot  sensors;
    WateringCycle   cycles[kMaxPots];
    PumpStopStatus  pumpStop[kMaxPots];
    PumpOwner       currentPumpOwner[kMaxPots];
    WaterBudget     budget;
    Config          config;
    NetConfig       netConfig;
    uint32_t        lastCycleDoneMs[kMaxPots] = {};
    DuskPhase       duskPhase;
    bool            wifiConnected;
};

// ---------------------------------------------------------------------------
// API — inicjalizacja i tick
// ---------------------------------------------------------------------------

// Inicjalizuj UI (M5.Display, ustaw font, kolory)
void uiInit();

// Tick — wywoływany z UiTask co ~100-200ms
// Obsługuje przyciski M5 (BtnA/BtnB) i renderuje.
void uiTick(uint32_t nowMs, UiState& state, const UiSnap& snap);

// ---------------------------------------------------------------------------
// API — rendery poszczególnych ekranów
// ---------------------------------------------------------------------------

void renderSinglePotScreen(uint32_t nowMs, const UiSnap& snap, uint8_t potIdx);
void renderDualPotScreen(uint32_t nowMs, const UiSnap& snap, const UiState& state);
void renderSettingsScreen(const UiSnap& snap, const UiState& state);

// ---------------------------------------------------------------------------
// API — obsługa przycisków (wywoływana z uiTick)
// ---------------------------------------------------------------------------

// BtnA short → toggle MAIN/SETTINGS
void uiHandleBtnA(UiState& state, const Config& cfg);

// BtnB short → switch view (COMPACT→POT0→POT1) lub nawigacja Settings
void uiHandleBtnB(UiState& state, const Config& cfg);

// BtnB long press → zmień wartość w Settings
// Zwraca true jeśli config został zmieniony (propagować do ControlTask)
bool uiHandleBtnBLong(UiState& state, Config& cfg, WaterBudget& budget);

// ---------------------------------------------------------------------------
// Helpers — rysowanie
// ---------------------------------------------------------------------------

// Narysuj pasek postępu (x, y, w, h, pct 0..1, color)
void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h,
                     float pct, uint32_t color);

// Narysuj banner alertu na dole ekranu
void drawAlertBanner(int16_t y, const char* msg, uint32_t color);
