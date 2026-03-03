// ============================================================================
// ui.cpp — GUI na StickS3 LCD (135×240), nawigacja, Settings
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Ekran StickS3 — design UI",
//                "Ekran Settings", "Nawigacja ekranów"
//
// Anti-flicker: M5Canvas (sprite) jako off-screen buffer.
// Cały frame rysowany na sprite, potem pushSprite() — zero migotania.
// ============================================================================

#include "ui.h"
#include "events.h"
#include "network.h"
#include <M5Unified.h>

// ---------------------------------------------------------------------------
// Off-screen sprite buffer (anti-flicker)
// ---------------------------------------------------------------------------
static M5Canvas _canvas(&M5.Display);
static bool     _canvasReady = false;

static constexpr int16_t SCR_W = 135;
static constexpr int16_t SCR_H = 240;

// Shortcut — all drawing goes through _canvas
#define C _canvas

// ---------------------------------------------------------------------------
// Kolory (RGB565)
// ---------------------------------------------------------------------------
static constexpr uint16_t COL_BG       = 0x0000;   // black
static constexpr uint16_t COL_TEXT     = 0xFFFF;   // white
static constexpr uint16_t COL_DIM      = 0x7BEF;   // light gray
static constexpr uint16_t COL_GREEN    = 0x07E0;
static constexpr uint16_t COL_YELLOW   = 0xFFE0;
static constexpr uint16_t COL_ORANGE   = 0xFBE0;
static constexpr uint16_t COL_RED      = 0xF800;
static constexpr uint16_t COL_CYAN     = 0x07FF;
static constexpr uint16_t COL_BLUE     = 0x001F;
static constexpr uint16_t COL_DKBLUE   = 0x000B;
static constexpr uint16_t COL_DKGREEN  = 0x03E0;
static constexpr uint16_t COL_GRAY     = 0x4208;
static constexpr uint16_t COL_HILITE   = 0x07FF;   // cyan
static constexpr uint16_t COL_PANEL    = 0x10A2;   // dark panel bg
static constexpr uint16_t COL_SEP      = 0x31A6;   // separator line

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h,
                     float pct, uint32_t color)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    int16_t fillW = static_cast<int16_t>((w - 2) * pct);

    C.drawRoundRect(x, y, w, h, 2, COL_GRAY);
    if (fillW > 0)
        C.fillRoundRect(x + 1, y + 1, fillW, h - 2, 1, (uint16_t)color);
    if (fillW < w - 2)
        C.fillRect(x + 1 + fillW, y + 1, w - 2 - fillW, h - 2, COL_BG);
}

void drawAlertBanner(int16_t y, const char* msg, uint32_t color) {
    C.fillRoundRect(2, y, SCR_W - 4, 22, 3, (uint16_t)color);
    C.setTextColor(COL_TEXT);
    C.setTextSize(1);
    C.setTextDatum(textdatum_t::middle_center);
    C.drawString(msg, SCR_W / 2, y + 11);
    C.setTextDatum(textdatum_t::top_left);
}

static uint16_t moistureColor(float pct, const PlantProfile& prof) {
    if (pct >= prof.targetMoisturePct)  return COL_GREEN;
    if (pct >= prof.criticalLowPct)     return COL_YELLOW;
    return COL_RED;
}

static const char* phaseStr(WateringPhase ph) {
    switch (ph) {
        case WateringPhase::IDLE:           return "IDLE";
        case WateringPhase::EVALUATING:     return "EVAL";
        case WateringPhase::PULSE:          return "PULSE";
        case WateringPhase::SOAK:           return "SOAK";
        case WateringPhase::MEASURING:      return "MEAS";
        case WateringPhase::OVERFLOW_WAIT:  return "OVFL";
        case WateringPhase::DONE:           return "DONE";
        case WateringPhase::BLOCKED:        return "BLOCK";
        default: return "?";
    }
}

static void drawSep(int16_t y) {
    C.drawFastHLine(4, y, SCR_W - 8, COL_SEP);
}

// ---------------------------------------------------------------------------
// Pixel-art icons (12×12 px) — rysowane prymitywami na M5Canvas
// Każda ikona rysuje się w kwadracie 12×12 od (x,y).
// ---------------------------------------------------------------------------

static constexpr int16_t ICO = 12;  // icon bounding box
static constexpr int16_t TXI = 20;  // text x after left icon (4 + 12 + 4)

// 🌱 Seedling — zielona łodyga + 2 duże listki
static void iconSeedling(int16_t x, int16_t y, uint16_t col = COL_GREEN) {
    C.fillRect(x + 5, y + 5, 2, 7, col);                                 // stem
    C.fillTriangle(x + 5, y + 5, x + 0, y + 0, x + 5, y + 0, col);      // left leaf
    C.fillTriangle(x + 6, y + 5, x + 11, y + 0, x + 6, y + 0, col);     // right leaf
}

// 💧 Water drop — zawsze niebieska (COL_CYAN)
static void iconDrop(int16_t x, int16_t y) {
    C.fillCircle(x + 5, y + 8, 4, COL_CYAN);
    C.fillTriangle(x + 1, y + 7, x + 5, y + 0, x + 9, y + 7, COL_CYAN);
}

// 🪴 Pot — zawsze brązowa doniczka (COL_ORANGE)
static void iconPot(int16_t x, int16_t y) {
    C.fillRect(x + 0, y + 0, 12, 3, COL_ORANGE);                         // rim
    C.fillTriangle(x + 1, y + 3, x + 10, y + 3, x + 3, y + 11, COL_ORANGE);
    C.fillTriangle(x + 10, y + 3, x + 8, y + 11, x + 3, y + 11, COL_ORANGE);
}

// 🌡 Thermometer — rurka + bańka
static void iconThermo(int16_t x, int16_t y) {
    C.fillRect(x + 4, y + 0, 4, 7, COL_TEXT);                            // tube
    C.fillRect(x + 5, y + 1, 2, 5, COL_RED);                             // mercury
    C.fillCircle(x + 5, y + 9, 3, COL_RED);                              // bulb
    C.drawCircle(x + 5, y + 9, 3, COL_TEXT);                              // outline
}

// ☀ Sun — żółte słońce z promieniami
static void iconSun(int16_t x, int16_t y, uint16_t col = COL_YELLOW) {
    C.fillCircle(x + 5, y + 5, 3, col);
    C.drawFastVLine(x + 5, y + 0, 2, col);
    C.drawFastVLine(x + 5, y + 10, 2, col);
    C.drawFastHLine(x + 0, y + 5, 2, col);
    C.drawFastHLine(x + 10, y + 5, 2, col);
    C.fillRect(x + 1, y + 1, 2, 2, col);
    C.fillRect(x + 9, y + 1, 2, 2, col);
    C.fillRect(x + 1, y + 9, 2, 2, col);
    C.fillRect(x + 9, y + 9, 2, 2, col);
}

// ⚙ Gear — koło z zębami
static void iconGear(int16_t x, int16_t y, uint16_t col = COL_TEXT) {
    C.fillCircle(x + 5, y + 5, 4, col);
    C.fillCircle(x + 5, y + 5, 2, COL_BG);                               // hollow center
    C.fillRect(x + 3, y + 0, 5, 2, col);                                 // top tooth
    C.fillRect(x + 3, y + 10, 5, 2, col);                                // bottom tooth
    C.fillRect(x + 0, y + 3, 2, 5, col);                                 // left tooth
    C.fillRect(x + 10, y + 3, 2, 5, col);                                // right tooth
}

// 📶 WiFi bars (4 słupki)
static void iconWifi(int16_t x, int16_t y, uint16_t col = COL_GREEN) {
    C.fillRect(x + 0,  y + 9, 3, 3, col);
    C.fillRect(x + 3,  y + 6, 3, 6, col);
    C.fillRect(x + 6,  y + 3, 3, 9, col);
    C.fillRect(x + 9,  y + 0, 3, 12, col);
}

// ⏰ Clock — tarcza z wskazówkami
static void iconClock(int16_t x, int16_t y, uint16_t col = COL_DIM) {
    C.drawCircle(x + 5, y + 5, 5, col);
    C.drawFastVLine(x + 5, y + 2, 4, col);                               // hour hand
    C.drawFastHLine(x + 5, y + 5, 3, col);                               // minute hand
}

// ⬆ Target arrow
static void iconTarget(int16_t x, int16_t y, uint16_t col = COL_DIM) {
    C.fillTriangle(x + 5, y + 0, x + 0, y + 6, x + 10, y + 6, col);
    C.fillRect(x + 3, y + 6, 5, 6, col);
}

// 🤖 Telegram — paper-plane
static void iconTelegram(int16_t x, int16_t y, uint16_t col = COL_CYAN) {
    C.fillTriangle(x + 0, y + 5, x + 11, y + 0, x + 11, y + 5, col);
    C.fillTriangle(x + 0, y + 5, x + 11, y + 5, x + 6, y + 11, col);
}

// ---------------------------------------------------------------------------
// Ikony warzyw/ziół (12×12 px) — po jednej na profil rośliny
// ---------------------------------------------------------------------------

// 🍅 Pomidor
static void iconTomato(int16_t x, int16_t y) {
    C.fillCircle(x + 5, y + 7, 4, COL_RED);                              // owoc
    C.fillRect(x + 3, y + 0, 5, 3, COL_GREEN);                           // łodyżka
    C.drawPixel(x + 2, y + 2, COL_GREEN);                                // listek L
    C.drawPixel(x + 8, y + 2, COL_GREEN);                                // listek R
    C.drawPixel(x + 3, y + 6, 0xFBE0);                                   // highlight
}

// 🌶 Papryka
static void iconPepper(int16_t x, int16_t y, uint16_t col = COL_RED) {
    C.fillRect(x + 4, y + 0, 3, 3, COL_GREEN);                           // stem
    C.fillRoundRect(x + 2, y + 3, 7, 5, 2, col);                         // body
    C.fillTriangle(x + 3, y + 8, x + 7, y + 8, x + 5, y + 11, col);     // tip
}

// 🌿 Bazylia
static void iconBasil(int16_t x, int16_t y) {
    C.fillRect(x + 5, y + 7, 2, 5, COL_DKGREEN);                         // stem
    C.fillCircle(x + 3, y + 4, 3, COL_GREEN);                            // left leaf
    C.fillCircle(x + 8, y + 4, 3, COL_GREEN);                            // right leaf
    C.fillCircle(x + 5, y + 2, 3, COL_GREEN);                            // top leaf
}

// 🍓 Truskawka
static void iconStrawberry(int16_t x, int16_t y) {
    C.fillRect(x + 2, y + 0, 7, 3, COL_GREEN);                           // liście
    C.fillTriangle(x + 1, y + 3, x + 9, y + 3, x + 5, y + 11, COL_RED); // owoc
    C.drawPixel(x + 3, y + 5, COL_YELLOW);                               // pestka
    C.drawPixel(x + 6, y + 5, COL_YELLOW);
    C.drawPixel(x + 4, y + 7, COL_YELLOW);
    C.drawPixel(x + 6, y + 8, COL_YELLOW);
}

// 🌶 Chili — pomarańczowa
static void iconChili(int16_t x, int16_t y) {
    iconPepper(x, y, COL_ORANGE);
}

// Dispatcher — rysuj ikonę profilu po indeksie
static void drawPlantIcon(int16_t x, int16_t y, uint8_t profileIdx) {
    switch (profileIdx) {
        case 0: iconTomato(x, y);      break;
        case 1: iconPepper(x, y);      break;
        case 2: iconBasil(x, y);       break;
        case 3: iconStrawberry(x, y);  break;
        case 4: iconChili(x, y);       break;
        case 5: iconGear(x, y, COL_DIM); break;
        default: iconSeedling(x, y);   break;
    }
}

// ---------------------------------------------------------------------------
// uiInit — M5.Display + sprite setup
// ---------------------------------------------------------------------------
void uiInit() {
    M5.Display.setRotation(0);   // portrait 135x240
    M5.Display.fillScreen(COL_BG);
    M5.Display.setTextColor(COL_TEXT);
    M5.Display.setTextWrap(false);

    _canvasReady = _canvas.createSprite(SCR_W, SCR_H);
    if (_canvasReady) {
        C.setTextWrap(false);
        C.setTextColor(COL_TEXT);
    }
    Serial.printf("[UI] sprite %s (%dx%d)\n",
                  _canvasReady ? "OK" : "FAIL", SCR_W, SCR_H);
}

// ---------------------------------------------------------------------------
// uiTick — main render dispatch
// ---------------------------------------------------------------------------
void uiTick(uint32_t nowMs, UiState& state, const UiSnap& snap) {
    if (!state.needsRedraw &&
        (nowMs - state.lastRedrawMs) < UiState::kMinRedrawIntervalMs)
        return;

    if (!_canvasReady) return;

    // Clear sprite
    C.fillSprite(COL_BG);
    C.setTextColor(COL_TEXT);
    C.setTextSize(1);

    switch (state.screen) {
    case UiScreen::SETTINGS:
        renderSettingsScreen(snap, state);
        break;
    case UiScreen::MAIN:
    default:
        if (snap.config.numPots == 1) {
            renderSinglePotScreen(nowMs, snap, 0);
        } else {
            switch (state.dualViewMode) {
            case DualViewMode::DETAIL_POT0:
                renderSinglePotScreen(nowMs, snap, 0);
                break;
            case DualViewMode::DETAIL_POT1:
                renderSinglePotScreen(nowMs, snap, 1);
                break;
            case DualViewMode::COMPACT:
            default:
                renderDualPotScreen(nowMs, snap, state);
                break;
            }
        }
        break;
    }

    // Push sprite to display atomically — no flicker
    C.pushSprite(0, 0);

    state.needsRedraw = false;
    state.lastRedrawMs = nowMs;
}

// ===========================================================================
// renderSinglePotScreen — pełny widok jednej doniczki (135×240)
// Wypełnia cały ekran: moisture → reservoir → env → mode → watering → dusk → raw → alerts
// ===========================================================================
void renderSinglePotScreen(uint32_t nowMs, const UiSnap& snap, uint8_t potIdx) {
    const PotSensorSnapshot& ps   = snap.sensors.pots[potIdx];
    const PlantProfile& prof      = getActiveProfile(snap.config, potIdx);
    const WateringCycle& cyc      = snap.cycles[potIdx];
    const EnvSnapshot& env        = snap.sensors.env;
    const WaterBudget& bud        = snap.budget;
    char buf[48];

    float moist = (ps.moistureEma > 0) ? ps.moistureEma : ps.moisturePct;
    uint16_t mColor = moistureColor(moist, prof);

    // ── Section 1: Moisture + profile (y 0-48) ────────────────
    iconSeedling(4, 4, mColor);      // 🌱
    C.setTextSize(3);
    C.setTextColor(mColor);
    snprintf(buf, sizeof(buf), "%.0f%%", moist);
    C.drawString(buf, TXI, 4);

    C.setTextSize(1);
    iconTarget(82, 4);               // ⬆
    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "%.0f%%", prof.targetMoisturePct);
    C.drawString(buf, 96, 6);

    // Profil: ikona + nazwa
    drawPlantIcon(82, 17, snap.config.pots[potIdx].plantProfileIndex);
    C.setTextColor(COL_CYAN);
    snprintf(buf, sizeof(buf), "%s", prof.name ? prof.name : "?");
    C.drawString(buf, 96, 20);

    // Pot indicator for dual mode
    if (snap.config.numPots > 1) {
        C.setTextSize(1);
        C.setTextColor(COL_DIM);
        snprintf(buf, sizeof(buf), "Pot %d", potIdx + 1);
        C.drawString(buf, 4, 20);
    }

    // Moisture bar
    drawProgressBar(4, 34, SCR_W - 8, 12, moist / 100.0f, mColor);

    drawSep(50);

    // ── Section 2: Reservoir + guards (y 52-80) ───────────────
    uint16_t rColor = (bud.daysRemaining > 3) ? COL_GREEN :
                      (bud.daysRemaining > 1) ? COL_YELLOW : COL_RED;
    C.setTextSize(1);
    iconDrop(4, 52);                 // 💧 zawsze niebieska
    C.setTextColor(rColor);
    snprintf(buf, sizeof(buf), "%.0fml", bud.reservoirCurrentMl);
    C.drawString(buf, TXI, 55);

    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "~%dd", (int)bud.daysRemaining);
    C.drawString(buf, 96, 55);

    bool overflow = (ps.waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool resLow   = (ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED);
    iconPot(4, 66);                  // 🪴 zawsze brązowa
    C.setTextColor(overflow ? COL_RED : COL_GREEN);
    C.drawString(overflow ? "OVFL" : "OK", TXI, 69);
    C.setTextColor(resLow ? COL_RED : COL_GREEN);
    C.drawString(resLow ? "TANK:LOW" : "TANK:OK", 72, 69);

    drawSep(82);

    // ── Section 3: Environment — temp + humidity (y 84-108) ───
    iconThermo(4, 84);               // 🌡
    C.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%.1fC", env.tempC);
    C.drawString(buf, TXI, 87);

    iconDrop(66, 84);                // 💧 humidity
    C.setTextColor(COL_CYAN);
    snprintf(buf, sizeof(buf), "%.0f%%", env.humidityPct);
    C.drawString(buf, 80, 87);

    // Lux + pressure
    iconSun(4, 98);                  // ☀
    C.setTextColor(COL_YELLOW);
    snprintf(buf, sizeof(buf), "%.0flx", env.lux);
    C.drawString(buf, TXI, 101);

    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "%.0fhPa", env.pressureHpa);
    C.drawString(buf, 72, 101);

    drawSep(114);

    // ── Section 4: Mode + WiFi (y 116-130) ────────────────────
    iconGear(4, 116);                // ⚙
    C.setTextColor(snap.config.mode == Mode::AUTO ? COL_GREEN : COL_YELLOW);
    C.drawString(snap.config.mode == Mode::AUTO ? "AUTO" : "MANUAL", TXI, 118);

    if (snap.config.vacationMode) {
        C.setTextColor(COL_CYAN);
        C.drawString("VAC", 60, 118);
    }

    iconWifi(88, 116, snap.wifiConnected ? COL_GREEN : COL_GRAY); // 📶
    C.setTextColor(snap.wifiConnected ? COL_GREEN : COL_GRAY);
    C.drawString(snap.wifiConnected ? "OK" : "--", 102, 118);

    drawSep(132);

    // ── Section 5: Watering phase (y 134-170) ─────────────────
    switch (cyc.phase) {
    case WateringPhase::IDLE:
        C.setTextColor(COL_GRAY);
        C.drawString("Watering: IDLE", 4, 136);
        break;

    case WateringPhase::PULSE: {
        C.setTextColor(COL_BLUE);
        snprintf(buf, sizeof(buf), "PULSE %d/%d  %.0fml",
                 cyc.pulseCount + 1, cyc.maxPulses, cyc.totalPumpedMl);
        C.drawString(buf, 4, 136);
        if (cyc.pulseDurationMs > 0) {
            float prog = (float)(nowMs - cyc.phaseStartMs) / cyc.pulseDurationMs;
            drawProgressBar(4, 150, SCR_W - 8, 10, prog, COL_BLUE);
        }
        break;
    }

    case WateringPhase::SOAK: {
        C.setTextColor(COL_CYAN);
        uint32_t elapsed = (nowMs > cyc.phaseStartMs) ? (nowMs - cyc.phaseStartMs) : 0;
        uint32_t rem = (elapsed < cyc.soakTimeMs) ? (cyc.soakTimeMs - elapsed) / 1000 : 0;
        snprintf(buf, sizeof(buf), "SOAK %ds  p%d/%d", rem, cyc.pulseCount, cyc.maxPulses);
        C.drawString(buf, 4, 136);
        float soakProg = (float)elapsed / cyc.soakTimeMs;
        drawProgressBar(4, 150, SCR_W - 8, 10, soakProg, COL_CYAN);
        break;
    }

    case WateringPhase::MEASURING:
        C.setTextColor(COL_YELLOW);
        C.drawString("MEASURING...", 4, 136);
        break;

    case WateringPhase::OVERFLOW_WAIT:
        C.setTextColor(COL_RED);
        C.drawString("! OVERFLOW WAIT", 4, 136);
        break;

    case WateringPhase::BLOCKED:
        C.setTextColor(COL_RED);
        C.drawString("BLOCKED", 4, 136);
        break;

    case WateringPhase::EVALUATING:
        C.setTextColor(COL_YELLOW);
        C.drawString("EVALUATING...", 4, 136);
        break;

    case WateringPhase::DONE:
        C.setTextColor(COL_GREEN);
        snprintf(buf, sizeof(buf), "DONE %.0fml", cyc.totalPumpedMl);
        C.drawString(buf, 4, 136);
        break;
    }

    // ── Section 6: Dusk + uptime (y 164-178) ─────────────────
    drawSep(164);
    iconSun(4, 166, snap.duskPhase == DuskPhase::DAY ? COL_YELLOW : COL_GRAY);
    C.setTextColor(COL_DIM);
    const char* duskStr = "?";
    switch (snap.duskPhase) {
        case DuskPhase::DAY:             duskStr = "DAY"; break;
        case DuskPhase::NIGHT:           duskStr = "NIGHT"; break;
        case DuskPhase::DUSK_TRANSITION: duskStr = "DUSK"; break;
        case DuskPhase::DAWN_TRANSITION: duskStr = "DAWN"; break;
    }
    C.drawString(duskStr, TXI, 168);

    iconClock(66, 166);              // ⏰
    uint32_t upSec = nowMs / 1000;
    uint32_t upMin = upSec / 60; upSec %= 60;
    uint32_t upHr  = upMin / 60; upMin %= 60;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", (int)upHr, (int)upMin, (int)upSec);
    C.drawString(buf, 80, 168);

    drawSep(182);

    // ── Section 7: Sensor raw data (y 184-200) ───────────────
    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "RAW:%d EMA:%.1f%%", ps.moistureRaw, ps.moistureEma);
    C.drawString(buf, 4, 184);
    snprintf(buf, sizeof(buf), "Comp:%.0f ct:%s",
             ps.moistureComp, ps.crosstalkUplift ? "Y" : "N");
    C.drawString(buf, 4, 196);

    // ── Section 8: Alert banners (y 210+) ────────────────────
    int16_t alertY = 212;
    if (bud.reservoirLow) {
        drawAlertBanner(alertY, "! RESERVOIR LOW", COL_RED);
        alertY += 24;
    }
    if (overflow) {
        drawAlertBanner(alertY, "! POT OVERFLOW", COL_RED);
    }
}

// ===========================================================================
// renderDualPotScreen — kompaktowy widok 2 doniczek (135×240)
// Wypełnia cały ekran: pot1 → pot2 → reservoir → env → mode → dusk → raw → alerts
// ===========================================================================
void renderDualPotScreen(uint32_t nowMs, const UiSnap& snap, const UiState& state) {
    char buf[48];

    // Każda doniczka: 46px (0-45, 48-93)
    for (uint8_t i = 0; i < 2; ++i) {
        if (!snap.config.pots[i].enabled) continue;
        const PotSensorSnapshot& ps = snap.sensors.pots[i];
        const PlantProfile& prof    = getActiveProfile(snap.config, i);
        const WateringCycle& cyc    = snap.cycles[i];

        int16_t baseY = i * 48;  // pot0: 0, pot1: 48
        float moist = (ps.moistureEma > 0) ? ps.moistureEma : ps.moisturePct;
        uint16_t mColor = moistureColor(moist, prof);

        // Selected pot highlight
        if (i == state.selectedPot) {
            C.fillRoundRect(0, baseY, SCR_W, 46, 3, COL_PANEL);
        }

        // Wiersz 1: ikona + numer + wilgotność duża + target
        iconSeedling(4, baseY + 2, mColor);  // 🌱
        C.setTextSize(2);
        C.setTextColor(mColor);
        snprintf(buf, sizeof(buf), "%d:%.0f%%", i + 1, moist);
        C.drawString(buf, TXI, baseY + 2);

        C.setTextSize(1);
        iconTarget(88, baseY + 2);     // ⬆ target
        C.setTextColor(COL_DIM);
        snprintf(buf, sizeof(buf), "%.0f%%", prof.targetMoisturePct);
        C.drawString(buf, 102, baseY + 4);

        // Wiersz 2: profil ikona + nazwa + faza podlewania
        drawPlantIcon(4, baseY + 18, snap.config.pots[i].plantProfileIndex);
        C.setTextColor(COL_CYAN);
        snprintf(buf, sizeof(buf), "%s", prof.name ? prof.name : "?");
        C.drawString(buf, TXI, baseY + 20);

        // Faza podlewania — kolorowa, prawo
        uint16_t phColor = COL_GRAY;
        switch (cyc.phase) {
            case WateringPhase::IDLE:     phColor = COL_GRAY; break;
            case WateringPhase::PULSE:    phColor = COL_BLUE; break;
            case WateringPhase::SOAK:     phColor = COL_CYAN; break;
            case WateringPhase::DONE:     phColor = COL_GREEN; break;
            default:                      phColor = COL_YELLOW; break;
        }
        C.setTextColor(phColor);
        const char* phStr = phaseStr(cyc.phase);
        if (cyc.phase == WateringPhase::PULSE) {
            snprintf(buf, sizeof(buf), "%s%d/%d", phStr, cyc.pulseCount + 1, cyc.maxPulses);
            C.drawString(buf, 72, baseY + 20);
        } else {
            C.drawString(phStr, 86, baseY + 20);
        }

        // Wiersz 3: pasek wilgotności + totalPumped jeśli aktywne
        drawProgressBar(4, baseY + 32, SCR_W - 8, 8, moist / 100.0f, mColor);

        if (cyc.totalPumpedMl > 0 && cyc.phase != WateringPhase::IDLE) {
            C.setTextColor(COL_DIM);
            snprintf(buf, sizeof(buf), "%.0fml", cyc.totalPumpedMl);
            int16_t tw = C.textWidth(buf);
            C.drawString(buf, SCR_W - 4 - tw, baseY + 42);
        }

        // Separator między doniczkami
        if (i == 0) drawSep(46);
    }

    drawSep(96);

    // ── Shared info (y 98-230) ────────────────────────────────
    const WaterBudget& bud = snap.budget;
    const EnvSnapshot& env = snap.sensors.env;

    // Reservoir (y 98-112)
    uint16_t rColor = (bud.daysRemaining > 3) ? COL_GREEN :
                      (bud.daysRemaining > 1) ? COL_YELLOW : COL_RED;
    C.setTextSize(1);
    iconDrop(4, 98);                 // 💧
    C.setTextColor(rColor);
    snprintf(buf, sizeof(buf), "%.0fml", bud.reservoirCurrentMl);
    C.drawString(buf, TXI, 101);
    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "~%dd", (int)bud.daysRemaining);
    C.drawString(buf, 60, 101);

    // Guards
    bool res0Ovf = (snap.sensors.pots[0].waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool res1Ovf = (snap.sensors.pots[1].waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool resLow  = bud.reservoirLow;
    iconPot(80, 98);                 // 🪴
    C.setTextColor(resLow ? COL_RED : COL_GREEN);
    C.drawString(resLow ? "LOW" : "OK", 94, 101);

    // Environment: temp + humidity (y 114-126)
    iconThermo(4, 114);              // 🌡
    C.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%.1fC", env.tempC);
    C.drawString(buf, TXI, 117);

    iconDrop(60, 114);               // 💧 humidity
    C.setTextColor(COL_CYAN);
    snprintf(buf, sizeof(buf), "%.0f%%", env.humidityPct);
    C.drawString(buf, 74, 117);

    // Lux + pressure (y 128-140)
    iconSun(4, 128);                 // ☀
    C.setTextColor(COL_YELLOW);
    snprintf(buf, sizeof(buf), "%.0flx", env.lux);
    C.drawString(buf, TXI, 131);

    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "%.0fhPa", env.pressureHpa);
    C.drawString(buf, 72, 131);

    drawSep(144);

    // Mode + WiFi (y 146-158)
    iconGear(4, 146);                // ⚙
    C.setTextColor(snap.config.mode == Mode::AUTO ? COL_GREEN : COL_YELLOW);
    snprintf(buf, sizeof(buf), "%s%s",
             snap.config.mode == Mode::AUTO ? "AUTO" : "MAN",
             snap.config.vacationMode ? " VAC" : "");
    C.drawString(buf, TXI, 149);

    iconWifi(80, 146, snap.wifiConnected ? COL_GREEN : COL_GRAY); // 📶
    C.setTextColor(snap.wifiConnected ? COL_GREEN : COL_GRAY);
    C.drawString(snap.wifiConnected ? "OK" : "--", 94, 149);

    // Dusk + uptime (y 160-174)
    iconSun(4, 162, snap.duskPhase == DuskPhase::DAY ? COL_YELLOW : COL_GRAY);
    C.setTextColor(COL_DIM);
    const char* duskStr2 = "?";
    switch (snap.duskPhase) {
        case DuskPhase::DAY:             duskStr2 = "DAY"; break;
        case DuskPhase::NIGHT:           duskStr2 = "NIGHT"; break;
        case DuskPhase::DUSK_TRANSITION: duskStr2 = "DUSK"; break;
        case DuskPhase::DAWN_TRANSITION: duskStr2 = "DAWN"; break;
    }
    C.drawString(duskStr2, TXI, 164);

    iconClock(66, 162);              // ⏰
    uint32_t upSec = nowMs / 1000;
    uint32_t upMin = upSec / 60; upSec %= 60;
    uint32_t upHr  = upMin / 60; upMin %= 60;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", (int)upHr, (int)upMin, (int)upSec);
    C.drawString(buf, 80, 164);

    drawSep(178);

    // Raw data per pot (y 180-204)
    C.setTextColor(COL_DIM);
    for (uint8_t i = 0; i < 2; ++i) {
        if (!snap.config.pots[i].enabled) continue;
        const PotSensorSnapshot& ps2 = snap.sensors.pots[i];
        int16_t ry = 180 + i * 12;
        snprintf(buf, sizeof(buf), "%d:RAW:%d E:%.1f%% C:%.0f",
                 i + 1, ps2.moistureRaw, ps2.moistureEma, ps2.moistureComp);
        C.drawString(buf, 4, ry);
    }

    // Alert banners (y 210+)
    int16_t alertY = 212;
    if (resLow) {
        drawAlertBanner(alertY, "! RESERVOIR LOW", COL_RED);
        alertY += 24;
    }
    if (res0Ovf) {
        drawAlertBanner(alertY, "! POT1 OVERFLOW", COL_RED);
        alertY += 24;
    }
    if (res1Ovf) {
        drawAlertBanner(alertY, "! POT2 OVERFLOW", COL_RED);
    }
}

// ===========================================================================
// renderSettingsScreen
// PLAN.md → "Ekran Settings"
// ===========================================================================

static constexpr float kResCapMinMl  = 500.0f;
static constexpr float kResCapMaxMl  = 20000.0f;
static constexpr float kResCapStepMl = 500.0f;
static constexpr float kResLowMinMl  = 300.0f;
static constexpr float kResLowMaxMl  = 3000.0f;
static constexpr float kResLowStepMl = 100.0f;
static constexpr uint16_t kPulseMinMl  = 10;
static constexpr uint16_t kPulseMaxMl  = 100;
static constexpr uint16_t kPulseStepMl = 5;

void renderSettingsScreen(const UiSnap& snap, const UiState& state) {
    const Config& cfg = snap.config;
    char buf[48];

    // Header
    C.setTextSize(2);
    C.setTextColor(COL_TEXT);
    C.drawString("SETTINGS", 4, 4);
    C.setTextSize(1);
    drawSep(24);

    // Items
    struct MenuItem { const char* label; char value[20]; bool visible; };
    MenuItem items[12];
    uint8_t itemCount = 0;

    { auto& m = items[itemCount++]; m.label = "Pompy";     snprintf(m.value, 20, "%d", cfg.numPots); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Profil 1";  snprintf(m.value, 20, "%s", getActiveProfile(cfg, 0).name); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Profil 2";  snprintf(m.value, 20, "%s", getActiveProfile(cfg, 1).name); m.visible = (cfg.numPots >= 2); }

    // Indeksy profili do rysowania ikon w Settings (oblicz po zbudowaniu items)
    uint8_t profIdx0 = cfg.pots[0].plantProfileIndex;
    uint8_t profIdx1 = cfg.pots[1].plantProfileIndex;
    { auto& m = items[itemCount++]; m.label = "Rezerwuar"; snprintf(m.value, 20, "%.1fL", cfg.reservoirCapacityMl / 1000.0f); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Rez. min";  snprintf(m.value, 20, "%.0fml", cfg.reservoirLowThresholdMl); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Puls";      snprintf(m.value, 20, "%dml", cfg.pots[0].pulseWaterMl); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "REFILL";    snprintf(m.value, 20, "(press)"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Tryb";      snprintf(m.value, 20, "%s", cfg.mode == Mode::AUTO ? "AUTO" : "MANUAL"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Vacation";  snprintf(m.value, 20, "%s", cfg.vacationMode ? "ON" : "OFF"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "WiFi";      snprintf(m.value, 20, "%s", snap.netConfig.provisioned ? snap.netConfig.wifiSsid : "Brak"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Reset";     snprintf(m.value, 20, "BtnA+B 5s"); m.visible = true; }

    uint8_t visIdx = 0;
    for (uint8_t i = 0; i < itemCount; ++i) {
        if (!items[i].visible) continue;
        int16_t y = 28 + visIdx * 22;
        bool sel = (visIdx == state.settingsIndex);

        if (sel) C.fillRoundRect(2, y - 2, SCR_W - 4, 20, 3, COL_PANEL);

        C.setTextColor(sel ? COL_HILITE : COL_TEXT);
        snprintf(buf, sizeof(buf), "%s %s", sel ? ">" : " ", items[i].label);
        C.drawString(buf, 4, y);

        // Value right-aligned
        C.setTextColor(sel ? COL_GREEN : COL_DIM);
        int16_t valW = C.textWidth(items[i].value);

        // Rysuj ikonę warzywa obok nazwy profilu
        if (i == 1) {  // Profil 1
            drawPlantIcon(SCR_W - 4 - valW - 12, y + 1, profIdx0);
        } else if (i == 2) {  // Profil 2
            drawPlantIcon(SCR_W - 4 - valW - 12, y + 1, profIdx1);
        }

        C.drawString(items[i].value, SCR_W - 4 - valW, y);

        visIdx++;
    }

    // Footer hint
    C.setTextColor(COL_GRAY);
    C.drawString("A:zmien  B:dalej", 4, SCR_H - 24);
    C.drawString("Przytrzym A: wroc", 4, SCR_H - 14);
}

// ===========================================================================
// Obsługa przycisków
// ===========================================================================

void uiHandleBtnA(UiState& state, const Config& cfg) {
    (void)cfg;
    if (state.screen == UiScreen::MAIN) {
        state.screen = UiScreen::SETTINGS;
        state.settingsIndex = 0;
    } else {
        state.screen = UiScreen::MAIN;
    }
    state.needsRedraw = true;
}

void uiHandleBtnB(UiState& state, const Config& cfg) {
    if (state.screen == UiScreen::SETTINGS) {
        uint8_t visCount = 10; // Pompy, Profil1, Rezerwuar, Rez.min, Puls, REFILL, Tryb, Vacation, WiFi, Reset
        if (cfg.numPots >= 2) visCount++;  // + Profil2
        state.settingsIndex = (state.settingsIndex + 1) % visCount;
    } else {
        if (cfg.numPots <= 1) return;
        switch (state.dualViewMode) {
            case DualViewMode::COMPACT:
                state.dualViewMode = DualViewMode::DETAIL_POT0;
                state.selectedPot = 0;
                break;
            case DualViewMode::DETAIL_POT0:
                state.dualViewMode = DualViewMode::DETAIL_POT1;
                state.selectedPot = 1;
                break;
            case DualViewMode::DETAIL_POT1:
            default:
                state.dualViewMode = DualViewMode::COMPACT;
                state.selectedPot = 0;
                break;
        }
    }
    state.needsRedraw = true;
}

bool uiHandleBtnBLong(UiState& state, Config& cfg, WaterBudget& budget) {
    if (state.screen != UiScreen::SETTINGS) return false;

    uint8_t actionMap[12];
    uint8_t actionCount = 0;
    actionMap[actionCount++] = 0;   // Pompy
    actionMap[actionCount++] = 1;   // Profil 1
    if (cfg.numPots >= 2) actionMap[actionCount++] = 2; // Profil 2
    actionMap[actionCount++] = 3;   // Rezerwuar
    actionMap[actionCount++] = 4;   // Rez.min
    actionMap[actionCount++] = 10;  // Puls
    actionMap[actionCount++] = 5;   // REFILL
    actionMap[actionCount++] = 6;   // Tryb
    actionMap[actionCount++] = 7;   // Vacation
    actionMap[actionCount++] = 8;   // WiFi
    actionMap[actionCount++] = 9;   // Reset

    if (state.settingsIndex >= actionCount) return false;
    uint8_t targetAction = actionMap[state.settingsIndex];

    bool changed = false;

    switch (targetAction) {
    case 0:
        cfg.numPots = (cfg.numPots == 1) ? 2 : 1;
        cfg.pots[1].enabled = (cfg.numPots == 2);
        Serial.printf("SETTINGS numPots=%d\n", cfg.numPots);
        changed = true;
        break;
    case 1:
        cfg.pots[0].plantProfileIndex = (cfg.pots[0].plantProfileIndex + 1) % kNumProfiles;
        changed = true;
        break;
    case 2:
        cfg.pots[1].plantProfileIndex = (cfg.pots[1].plantProfileIndex + 1) % kNumProfiles;
        changed = true;
        break;
    case 3: {
        float next = cfg.reservoirCapacityMl + kResCapStepMl;
        if (next > kResCapMaxMl + 0.1f) next = kResCapMinMl;
        cfg.reservoirCapacityMl = next;

        // Keep low threshold safely below capacity
        if (cfg.reservoirLowThresholdMl >= cfg.reservoirCapacityMl) {
            cfg.reservoirLowThresholdMl = cfg.reservoirCapacityMl - kResLowStepMl;
            if (cfg.reservoirLowThresholdMl < kResLowMinMl) {
                cfg.reservoirLowThresholdMl = kResLowMinMl;
            }
        }
        changed = true;
        break;
    }
    case 4:
        cfg.reservoirLowThresholdMl += kResLowStepMl;
        if (cfg.reservoirLowThresholdMl > kResLowMaxMl + 0.1f) {
            cfg.reservoirLowThresholdMl = kResLowMinMl;
        }
        if (cfg.reservoirLowThresholdMl >= cfg.reservoirCapacityMl) {
            cfg.reservoirLowThresholdMl = kResLowMinMl;
        }
        changed = true;
        break;
    case 5:
        handleRefill(budget, cfg);
        break;
    case 6:
        cfg.mode = (cfg.mode == Mode::AUTO) ? Mode::MANUAL : Mode::AUTO;
        changed = true;
        break;
    case 7:
        handleVacationToggle(!cfg.vacationMode, cfg);
        changed = true;
        break;
    case 8: {
        Serial.println("SETTINGS WiFi setup");
        M5.Display.fillScreen(COL_BG);
        M5.Display.setTextColor(COL_TEXT);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(5, 20);
        M5.Display.println("WiFi Setup");
        M5.Display.println("AP: autogarden");
        M5.Display.println("192.168.4.1");
        M5.Display.println("");
        M5.Display.println("Auto-off: 5 min");
        extern NetConfig g_netConfig;
        enterApMode(g_netConfig);
        break;
    }
    case 9:
        Serial.println("SETTINGS: hold BtnA+B 5s for reset");
        break;
    case 10: {
        // Pulse size — cyklicznie 10-100ml, step 5ml, ustawiane na oba poty
        uint16_t next = cfg.pots[0].pulseWaterMl + kPulseStepMl;
        if (next > kPulseMaxMl) next = kPulseMinMl;
        for (uint8_t p = 0; p < kMaxPots; ++p) cfg.pots[p].pulseWaterMl = next;
        Serial.printf("SETTINGS pulseWaterMl=%dml\n", next);
        changed = true;
        break;
    }
    }

    state.needsRedraw = true;
    return changed;
}
