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
#include <cstring>
#include "log_serial.h"

#define Serial AGSerial

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
static constexpr uint16_t COL_CHIP_DAY_BG   = 0x42A0;
static constexpr uint16_t COL_CHIP_NIGHT_BG = 0x0190;
static constexpr uint16_t COL_CHIP_TRANS_BG = 0x79E0;
static constexpr uint16_t COL_CHIP_UPTIME_BG = 0x19E3;

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

enum class TargetState : uint8_t {
    BELOW_TRIGGER,
    ON_TARGET,
    ABOVE_TARGET,
    ABOVE_MAX,
};

static float effectiveTargetPct(const PlantProfile& prof, const Config& cfg) {
    float target = prof.targetMoisturePct;
    if (cfg.vacationMode) {
        target -= cfg.vacationTargetReductionPct;
        if (target < 5.0f) {
            target = 5.0f;
        }
    }
    return target;
}

static TargetState targetStateForMoisture(float moisturePct,
                                          const PlantProfile& prof,
                                          const Config& cfg) {
    float target = effectiveTargetPct(prof, cfg);
    float trigger = target - prof.hysteresisPct;

    if (moisturePct < trigger) {
        return TargetState::BELOW_TRIGGER;
    }
    if (moisturePct <= target + 2.0f) {
        return TargetState::ON_TARGET;
    }
    if (moisturePct <= prof.maxMoisturePct) {
        return TargetState::ABOVE_TARGET;
    }
    return TargetState::ABOVE_MAX;
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

static void formatUptimeCompact(uint32_t nowMs, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    uint32_t totalSec = nowMs / 1000;
    uint32_t sec = totalSec % 60;
    uint32_t totalMin = totalSec / 60;
    uint32_t min = totalMin % 60;
    uint32_t totalHours = totalMin / 60;
    uint32_t hours = totalHours % 24;
    uint32_t days = totalHours / 24;

    if (days == 0) {
        snprintf(out, outSize, "UP %02lu:%02lu",
                 (unsigned long)hours,
                 (unsigned long)min);
        return;
    }

    if (days < 100) {
        snprintf(out, outSize, "UP %lud%02luh",
                 (unsigned long)days,
                 (unsigned long)hours);
        return;
    }

    if (days < 1000) {
        snprintf(out, outSize, "UP %lud", (unsigned long)days);
        return;
    }

    uint32_t weeks = days / 7;
    snprintf(out, outSize, "UP %luw", (unsigned long)weeks);
}

static bool uiMoistureLooksOutOfSoil(float moisturePct) {
    return moisturePct >= 0.0f && moisturePct <= 5.0f;
}

static void copyFittedText(const char* src, char* out, size_t outSize, int16_t maxWidth) {
    if (!out || outSize == 0) {
        return;
    }
    if (!src) {
        out[0] = '\0';
        return;
    }

    snprintf(out, outSize, "%s", src);
    if (C.textWidth(out) <= maxWidth) {
        return;
    }

    const char* ellipsis = "...";
    size_t srcLen = strlen(src);
    for (size_t keep = srcLen; keep > 0; --keep) {
        snprintf(out, outSize, "%.*s%s", static_cast<int>(keep), src, ellipsis);
        if (C.textWidth(out) <= maxWidth) {
            return;
        }
    }

    snprintf(out, outSize, "%s", ellipsis);
}

static void drawCompactChip(int16_t x, int16_t y, int16_t w, const char* text,
                            uint16_t bgColor, uint16_t textColor = COL_TEXT) {
    C.fillRoundRect(x, y, w, 14, 3, bgColor);
    C.drawRoundRect(x, y, w, 14, 3, COL_SEP);
    C.setTextSize(1);
    C.setTextDatum(textdatum_t::middle_center);
    C.setTextColor(textColor);
    C.drawString(text ? text : "", x + (w / 2), y + 7);
    C.setTextDatum(textdatum_t::top_left);
}

static void duskChipStyle(DuskPhase phase, uint16_t& bgColor, uint16_t& textColor) {
    switch (phase) {
        case DuskPhase::DAY:
            bgColor = COL_CHIP_DAY_BG;
            textColor = COL_YELLOW;
            break;
        case DuskPhase::NIGHT:
            bgColor = COL_CHIP_NIGHT_BG;
            textColor = COL_CYAN;
            break;
        case DuskPhase::DAWN_TRANSITION:
        case DuskPhase::DUSK_TRANSITION:
            bgColor = COL_CHIP_TRANS_BG;
            textColor = COL_TEXT;
            break;
        default:
            bgColor = COL_PANEL;
            textColor = COL_TEXT;
            break;
    }
}

static uint16_t duskTextColor(DuskPhase phase) {
    switch (phase) {
        case DuskPhase::DAY:
            return COL_YELLOW;
        case DuskPhase::NIGHT:
            return COL_CYAN;
        case DuskPhase::DAWN_TRANSITION:
        case DuskPhase::DUSK_TRANSITION:
            return COL_ORANGE;
        default:
            return COL_TEXT;
    }
}

static void formatDualPotStatus(uint32_t nowMs,
                                const UiSnap& snap,
                                uint8_t potIdx,
                                float moisturePct,
                                char* out,
                                size_t outSize,
                                uint16_t& color) {
    const PotSensorSnapshot& ps = snap.sensors.pots[potIdx];
    const WateringCycle& cyc = snap.cycles[potIdx];
    const PotConfig& potCfg = snap.config.pots[potIdx];
    const PlantProfile& prof = getActiveProfile(snap.config, potIdx);

    if (cyc.phase == WateringPhase::PULSE) {
        snprintf(out, outSize, "%u/%u",
                 static_cast<unsigned>(cyc.pulseCount + 1),
                 static_cast<unsigned>(cyc.maxPulses));
        color = COL_BLUE;
        return;
    }
    if (cyc.phase == WateringPhase::SOAK) {
        snprintf(out, outSize, "%s", "SOAK");
        color = COL_CYAN;
        return;
    }
    if (cyc.phase == WateringPhase::MEASURING) {
        snprintf(out, outSize, "%s", "MEAS");
        color = COL_CYAN;
        return;
    }
    if (cyc.phase == WateringPhase::OVERFLOW_WAIT) {
        snprintf(out, outSize, "%s", "OVFL");
        color = COL_ORANGE;
        return;
    }
    if (cyc.phase == WateringPhase::DONE) {
        snprintf(out, outSize, "%s", "DONE");
        color = COL_GREEN;
        return;
    }
    if (cyc.phase == WateringPhase::BLOCKED) {
        if (ps.waterGuards.potMax == WaterLevelState::TRIGGERED) {
            snprintf(out, outSize, "%s", "OVF");
        } else if (ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED ||
                   (snap.budget.reservoirLow && snap.budget.reservoirCurrentMl <= 0.0f)) {
            snprintf(out, outSize, "%s", "TANK");
        } else if ((snap.config.waterLevelUnknownPolicy == UnknownPolicy::BLOCK) &&
                   (ps.waterGuards.potMax == WaterLevelState::UNKNOWN ||
                    ps.waterGuards.reservoirMin == WaterLevelState::UNKNOWN)) {
            snprintf(out, outSize, "%s", "SNS?");
        } else if (potCfg.pumpMlPerSec <= 0.0f) {
            snprintf(out, outSize, "%s", "CFG?");
        } else {
            snprintf(out, outSize, "%s", "BLOCK");
        }
        color = COL_RED;
        return;
    }

    uint32_t effCooldown = snap.config.cooldownMs;
    if (snap.config.vacationMode) {
        effCooldown = static_cast<uint32_t>(effCooldown * snap.config.vacationCooldownMultiplier);
    }
    bool cooling = snap.lastCycleDoneMs[potIdx] != 0
        && (nowMs - snap.lastCycleDoneMs[potIdx]) < effCooldown;

    float targetPct = effectiveTargetPct(prof, snap.config);
    float triggerPct = targetPct - prof.hysteresisPct;

    if (snap.config.mode != Mode::AUTO) {
        snprintf(out, outSize, "%s", "MAN");
        color = COL_YELLOW;
    } else if (ps.waterGuards.potMax == WaterLevelState::TRIGGERED) {
        snprintf(out, outSize, "%s", "OVF");
        color = COL_RED;
    } else if (ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED ||
               (snap.budget.reservoirLow && snap.budget.reservoirCurrentMl <= 0.0f)) {
        snprintf(out, outSize, "%s", "TANK");
        color = COL_RED;
    } else if ((snap.config.waterLevelUnknownPolicy == UnknownPolicy::BLOCK) &&
               (ps.waterGuards.potMax == WaterLevelState::UNKNOWN ||
                ps.waterGuards.reservoirMin == WaterLevelState::UNKNOWN)) {
        snprintf(out, outSize, "%s", "SNS?");
        color = COL_ORANGE;
    } else if (uiMoistureLooksOutOfSoil(moisturePct)) {
        snprintf(out, outSize, "%s", "PROBE");
        color = COL_ORANGE;
    } else if (moisturePct >= prof.maxMoisturePct) {
        snprintf(out, outSize, "%s", "WET");
        color = COL_GREEN;
    } else if (moisturePct >= triggerPct) {
        snprintf(out, outSize, "%s", "OK");
        color = COL_GREEN;
    } else if (cooling) {
        snprintf(out, outSize, "%s", "COOL");
        color = COL_BLUE;
    } else if (potCfg.pumpMlPerSec <= 0.0f) {
        snprintf(out, outSize, "%s", "CFG?");
        color = COL_ORANGE;
    } else if (snap.sensors.env.tempC > snap.config.heatBlockTempC) {
        snprintf(out, outSize, "%s", "HEAT");
        color = COL_ORANGE;
    } else if (snap.sensors.env.lux > snap.config.directSunLuxThreshold) {
        snprintf(out, outSize, "%s", "SUN");
        color = COL_YELLOW;
    } else if (snap.duskPhase == DuskPhase::DAY || snap.duskPhase == DuskPhase::DAWN_TRANSITION) {
        snprintf(out, outSize, "%s", "WAIT");
        color = COL_DIM;
    } else {
        snprintf(out, outSize, "%s", "ARM");
        color = COL_CYAN;
    }
}

// ---------------------------------------------------------------------------
// Pixel-art icons (12×12 px) — rysowane prymitywami na M5Canvas
// Każda ikona rysuje się w kwadracie 12×12 od (x,y).
// ---------------------------------------------------------------------------

static constexpr int16_t ICO = 12;  // icon bounding box
static constexpr int16_t TXI = 20;  // text x after left icon (4 + 12 + 4)

// 🌱 Seedling — zielona łodyga + 2 czytelne listki
static void iconSeedling(int16_t x, int16_t y, uint16_t col = COL_GREEN) {
    // stem
    C.fillRect(x + 5, y + 5, 2, 6, col);

    // left leaf (rounded)
    C.fillCircle(x + 3, y + 3, 2, col);
    C.fillTriangle(x + 4, y + 4, x + 6, y + 5, x + 5, y + 2, col);

    // right leaf (rounded)
    C.fillCircle(x + 8, y + 3, 2, col);
    C.fillTriangle(x + 7, y + 4, x + 6, y + 5, x + 7, y + 2, col);
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

// ⏲ Barometer — mały manometr / tarcza z wskazówką
static void iconBarometer(int16_t x, int16_t y, uint16_t col = COL_TEXT) {
    C.drawCircle(x + 5, y + 6, 5, col);                                  // gauge body
    C.drawPixel(x + 5, y + 6, col);                                      // center
    C.drawLine(x + 5, y + 6, x + 8, y + 3, col);                         // needle
    C.drawFastHLine(x + 2, y + 11, 7, col);                              // base
}

// 🎯 Target indicator — dynamiczny wg odchyłki od targetu
static void iconTarget(int16_t x, int16_t y, TargetState state) {
    switch (state) {
        case TargetState::BELOW_TRIGGER: {
            uint16_t col = COL_YELLOW;
            C.fillTriangle(x + 5, y + 0, x + 0, y + 5, x + 10, y + 5, col);
            C.fillRect(x + 4, y + 4, 3, 7, col);
            break;
        }
        case TargetState::ON_TARGET: {
            uint16_t col = COL_GREEN;
            C.drawCircle(x + 5, y + 5, 5, col);
            C.drawCircle(x + 5, y + 5, 2, col);
            C.fillCircle(x + 5, y + 5, 1, col);
            break;
        }
        case TargetState::ABOVE_TARGET: {
            uint16_t col = COL_CYAN;
            C.fillRect(x + 4, y + 1, 3, 7, col);
            C.fillTriangle(x + 5, y + 11, x + 0, y + 6, x + 10, y + 6, col);
            break;
        }
        case TargetState::ABOVE_MAX:
        default: {
            uint16_t col = COL_RED;
            C.fillRect(x + 4, y + 1, 3, 7, col);
            C.fillTriangle(x + 5, y + 11, x + 0, y + 6, x + 10, y + 6, col);
            C.drawFastHLine(x + 1, y + 0, 9, col);
            break;
        }
    }
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
            renderDualPotScreen(nowMs, snap, state);
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
    float targetPct = effectiveTargetPct(prof, snap.config);
    TargetState targetState = targetStateForMoisture(moist, prof, snap.config);
    uint16_t mColor = moistureColor(moist, prof);

    // ── Section 1: Moisture + profile (y 0-48) ────────────────
    iconSeedling(4, 4, mColor);      // 🌱
    C.setTextSize(2);
    C.setTextColor(mColor);
    snprintf(buf, sizeof(buf), "%.0f %%", moist);
    C.drawString(buf, TXI, 4);

    C.setTextSize(1);
    iconTarget(82, 4, targetState);  // 🎯 dynamic
    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "%.0f %%", targetPct);
    C.drawString(buf, 96, 6);

    // Profil: ikona + nazwa
    drawPlantIcon(82, 20, snap.config.pots[potIdx].plantProfileIndex);
    C.setTextColor(COL_CYAN);
    snprintf(buf, sizeof(buf), "%s", prof.name ? prof.name : "?");
    C.drawString(buf, 96, 23);

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
    snprintf(buf, sizeof(buf), "%.0f ml", bud.reservoirCurrentMl);
    C.drawString(buf, TXI, 55);

    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "~%dd", (int)bud.daysRemaining);
    C.drawString(buf, 96, 55);

    bool overflow = (ps.waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool overflowPending = ps.waterGuards.potMaxStatus.pendingTrip || ps.waterGuards.potMaxStatus.pendingClear;
    bool resLow   = (ps.waterGuards.reservoirMin == WaterLevelState::TRIGGERED);
    bool resPending = ps.waterGuards.reservoirMinStatus.pendingTrip || ps.waterGuards.reservoirMinStatus.pendingClear;
    iconPot(4, 66);                  // 🪴 zawsze brązowa
    C.setTextColor((overflow || overflowPending) ? COL_RED : COL_GREEN);
    C.drawString(overflow ? "OVFL" : overflowPending ? "OVF~" : "OK", TXI, 69);
    C.setTextColor((resLow || resPending) ? COL_RED : COL_GREEN);
    C.drawString(resLow ? "TANK:LOW" : resPending ? "TANK:~" : "TANK:OK", 72, 69);

    drawSep(82);

    // ── Section 3: Environment — temp + humidity (y 84-108) ───
    iconThermo(4, 84);               // 🌡
    C.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%.1f C", env.tempC);
    C.drawString(buf, TXI, 87);

    iconDrop(66, 84);                // 💧 humidity
    C.setTextColor(COL_CYAN);
    snprintf(buf, sizeof(buf), "%.0f %%", env.humidityPct);
    C.drawString(buf, 80, 87);

    // Lux + pressure
    iconSun(4, 98);                  // ☀
    C.setTextColor(COL_YELLOW);
    snprintf(buf, sizeof(buf), "%.0f lx", env.lux);
    C.drawString(buf, TXI, 101);

    iconBarometer(66, 98, COL_TEXT);
    C.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%.0f hPa", env.pressureHpa);
    C.drawString(buf, 80, 101);

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
        snprintf(buf, sizeof(buf), "PULSE %d/%d  %.0f ml",
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
        snprintf(buf, sizeof(buf), "DONE %.0f ml", cyc.totalPumpedMl);
        C.drawString(buf, 4, 136);
        break;
    }

    // ── Section 6: Dusk + uptime (y 164-178) ─────────────────
    drawSep(164);
    iconSun(4, 166, snap.duskPhase == DuskPhase::DAY ? COL_YELLOW : COL_GRAY);
    C.setTextColor(duskTextColor(snap.duskPhase));
    const char* duskStr = "?";
    switch (snap.duskPhase) {
        case DuskPhase::DAY:             duskStr = "DAY"; break;
        case DuskPhase::NIGHT:           duskStr = "NIGHT"; break;
        case DuskPhase::DUSK_TRANSITION: duskStr = "DUSK"; break;
        case DuskPhase::DAWN_TRANSITION: duskStr = "DAWN"; break;
    }
    C.drawString(duskStr, TXI, 168);

    iconClock(66, 166, COL_TEXT);    // ⏰
    C.setTextColor(COL_TEXT);
    formatUptimeCompact(nowMs, buf, sizeof(buf));
    C.drawString(buf, 80, 168);

    drawSep(182);

    // ── Section 7: Sensor raw data (y 184-200) ───────────────
    C.setTextColor(COL_DIM);
    snprintf(buf, sizeof(buf), "RAWf:%d EMA:%.1f %%", ps.moistureRawFiltered, ps.moistureEma);
    C.drawString(buf, 4, 184);

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
// renderDualPotScreen — kompaktowy widok główny (135×240)
// Dla 2 potów: pot1 → pot2 → shared info.
// Dla 1 pota: pierwszy kafelek aktywny, drugi slot pozostaje pusty.
// ===========================================================================
void renderDualPotScreen(uint32_t nowMs, const UiSnap& snap, const UiState& state) {
    char buf[48];
    char labelBuf[24];

    // Każda doniczka: 50px (0-49, 52-101)
    for (uint8_t i = 0; i < 2; ++i) {
        if (!snap.config.pots[i].enabled) continue;
        const PotSensorSnapshot& ps = snap.sensors.pots[i];
        const PlantProfile& prof    = getActiveProfile(snap.config, i);
        int16_t baseY = i * 52;  // pot0: 0, pot1: 52
        float moist = (ps.moistureEma > 0) ? ps.moistureEma : ps.moisturePct;
        float targetPct = effectiveTargetPct(prof, snap.config);
        TargetState targetState = targetStateForMoisture(moist, prof, snap.config);
        uint16_t mColor = moistureColor(moist, prof);
        uint16_t statusColor = COL_GRAY;
        char statusBuf[16];
        formatDualPotStatus(nowMs, snap, i, moist, statusBuf, sizeof(statusBuf), statusColor);

        // Selected pot highlight
        if (i == state.selectedPot) {
            C.fillRoundRect(0, baseY, SCR_W, 50, 3, COL_PANEL);
        }

        // Wiersz 1: ikona + wilgotność duża + target
        iconSeedling(4, baseY + 2, mColor);  // 🌱
        C.setTextSize(2);
        C.setTextColor(mColor);
        snprintf(buf, sizeof(buf), "%.0f %%", moist);
        C.drawString(buf, TXI, baseY + 2);

        C.setTextSize(1);
        iconTarget(92, baseY + 2, targetState);     // 🎯 target dynamic
        C.setTextColor(COL_DIM);
        snprintf(buf, sizeof(buf), "%.0f %%", targetPct);
        C.drawString(buf, 106, baseY + 4);

        // Wiersz 2: profil ikona + nazwa + logiczny status po prawej
        drawPlantIcon(4, baseY + 18, snap.config.pots[i].plantProfileIndex);
        C.setTextColor(COL_CYAN);
        copyFittedText(prof.name ? prof.name : "?", labelBuf, sizeof(labelBuf), 64);
        C.drawString(labelBuf, TXI, baseY + 20);
        drawCompactChip(90, baseY + 19, 41, statusBuf, statusColor == COL_RED ? 0x7800 : COL_PANEL, statusColor);

        // Wiersz 3: pasek wilgotności
        drawProgressBar(4, baseY + 36, SCR_W - 8, 10, moist / 100.0f, mColor);

        // Separator między doniczkami
        if (i == 0) drawSep(50);
    }

    drawSep(104);

    // ── Shared info compact (y 108-214) ───────────────────────
    const WaterBudget& bud = snap.budget;
    const EnvSnapshot& env = snap.sensors.env;

    // Reservoir — pełna szerokość
    uint16_t rColor = (bud.daysRemaining > 3) ? COL_GREEN :
                      (bud.daysRemaining > 1) ? COL_YELLOW : COL_RED;
    C.setTextSize(1);
    snprintf(buf, sizeof(buf), "RES %.0fml  ~%dd", bud.reservoirCurrentMl, (int)bud.daysRemaining);
    drawCompactChip(4, 108, 127, buf, bud.reservoirLow ? 0x7800 : COL_DKBLUE, rColor);

    // Guards per pot — połowa szerokości każdy
    bool res0Ovf = (snap.sensors.pots[0].waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool res0Pending = snap.sensors.pots[0].waterGuards.potMaxStatus.pendingTrip
                    || snap.sensors.pots[0].waterGuards.potMaxStatus.pendingClear;
    bool res1Ovf = (snap.sensors.pots[1].waterGuards.potMax == WaterLevelState::TRIGGERED);
    bool res1Pending = snap.sensors.pots[1].waterGuards.potMaxStatus.pendingTrip
                    || snap.sensors.pots[1].waterGuards.potMaxStatus.pendingClear;
    snprintf(buf, sizeof(buf), "OVF1 %s", res0Ovf ? "HIT" : res0Pending ? "~" : "OK");
    drawCompactChip(4, 126, 61, buf, (res0Ovf || res0Pending) ? 0x7800 : COL_PANEL, (res0Ovf || res0Pending) ? COL_RED : COL_GREEN);
    snprintf(buf, sizeof(buf), "OVF2 %s", res1Ovf ? "HIT" : res1Pending ? "~" : "OK");
    drawCompactChip(70, 126, 61, buf, (res1Ovf || res1Pending) ? 0x7800 : COL_PANEL, (res1Ovf || res1Pending) ? COL_RED : COL_GREEN);

    // Env + humidity
    snprintf(buf, sizeof(buf), "T %.1fC", env.tempC);
    drawCompactChip(4, 144, 61, buf, COL_PANEL, COL_TEXT);
    snprintf(buf, sizeof(buf), "H %.0f%%", env.humidityPct);
    drawCompactChip(70, 144, 61, buf, COL_PANEL, COL_CYAN);

    // Lux + pressure
    snprintf(buf, sizeof(buf), "L %.0flx", env.lux);
    drawCompactChip(4, 162, 61, buf, COL_PANEL, COL_YELLOW);
    snprintf(buf, sizeof(buf), "P %.0f", env.pressureHpa);
    drawCompactChip(70, 162, 61, buf, COL_PANEL, COL_TEXT);

    // Mode + WiFi
    snprintf(buf, sizeof(buf), "%s%s",
             snap.config.mode == Mode::AUTO ? "AUTO" : "MAN",
             snap.config.vacationMode ? "/VAC" : "");
    drawCompactChip(4, 180, 61, buf, COL_PANEL,
                    snap.config.mode == Mode::AUTO ? COL_GREEN : COL_YELLOW);
    drawCompactChip(70, 180, 61, snap.wifiConnected ? "WIFI OK" : "WIFI --",
                    COL_PANEL,
                    snap.wifiConnected ? COL_GREEN : COL_GRAY);

    // Dusk + uptime
    const char* duskStr2 = "?";
    uint16_t duskChipBg = COL_PANEL;
    uint16_t duskChipText = COL_TEXT;
    switch (snap.duskPhase) {
        case DuskPhase::DAY:             duskStr2 = "DAY"; break;
        case DuskPhase::NIGHT:           duskStr2 = "NIGHT"; break;
        case DuskPhase::DUSK_TRANSITION: duskStr2 = "DUSK"; break;
        case DuskPhase::DAWN_TRANSITION: duskStr2 = "DAWN"; break;
    }
    duskChipStyle(snap.duskPhase, duskChipBg, duskChipText);
    drawCompactChip(4, 198, 61, duskStr2, duskChipBg, duskChipText);
    formatUptimeCompact(nowMs, buf, sizeof(buf));
    drawCompactChip(70, 198, 61, buf, COL_CHIP_UPTIME_BG, COL_TEXT);

    // RAWf per pot — dwa pola, bez pełnej szerokości
    snprintf(buf, sizeof(buf), "P1 %u", static_cast<unsigned>(snap.sensors.pots[0].moistureRawFiltered));
    drawCompactChip(4, 216, 61, buf, COL_PANEL, COL_DIM);
    snprintf(buf, sizeof(buf), "P2 %u", static_cast<unsigned>(snap.sensors.pots[1].moistureRawFiltered));
    drawCompactChip(70, 216, 61, buf, COL_PANEL, COL_DIM);
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
static constexpr uint16_t kMoistureRawMin = 0;
static constexpr uint16_t kMoistureRawMax = 4095;
static constexpr uint16_t kMoistureRawStep = 4;
static constexpr uint16_t kMoistureRawMinGap = 32;
static constexpr float kMoistureCurveExpMin = 0.10f;
static constexpr float kMoistureCurveExpMax = 12.00f;
static constexpr float kMoistureCurveExpStep = 0.01f;
static constexpr uint8_t kSettingsVisibleRows = 8;
static constexpr uint8_t kSettingsActionCountMax = 17;

static uint8_t uiSettingsVisibleCount(const Config& cfg) {
    uint8_t count = 13;  // base items for 1-pot layout
    if (cfg.numPots >= 2) {
        count += 4;      // Profil 2 + P2 Dry/Wet/Exp
    }
    return count;
}

static void uiClampSettingsIndex(UiState& state, const Config& cfg) {
    uint8_t visibleCount = uiSettingsVisibleCount(cfg);
    if (visibleCount == 0) {
        state.settingsIndex = 0;
        return;
    }
    if (state.settingsIndex >= visibleCount) {
        state.settingsIndex = visibleCount - 1;
    }
}

void renderSettingsScreen(const UiSnap& snap, const UiState& state) {
    const Config& cfg = snap.config;
    char buf[48];
    UiState clampedState = state;
    uiClampSettingsIndex(clampedState, cfg);

    // Header
    C.setTextSize(2);
    C.setTextColor(COL_TEXT);
    C.drawString("SETTINGS", 4, 4);
    C.setTextSize(1);
    drawSep(24);

    // Items
    struct MenuItem { const char* label; char value[20]; bool visible; };
    MenuItem items[20];
    uint8_t itemCount = 0;

    { auto& m = items[itemCount++]; m.label = "Pompy";     snprintf(m.value, 20, "%d", cfg.numPots); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Profil 1";  snprintf(m.value, 20, "%s", getActiveProfile(cfg, 0).name); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Profil 2";  snprintf(m.value, 20, "%s", getActiveProfile(cfg, 1).name); m.visible = (cfg.numPots >= 2); }
    { auto& m = items[itemCount++]; m.label = "P1 Dry";    snprintf(m.value, 20, "%u", cfg.pots[0].moistureDryRaw); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "P1 Wet";    snprintf(m.value, 20, "%u", cfg.pots[0].moistureWetRaw); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "P1 Exp";    snprintf(m.value, 20, "%.2f", cfg.pots[0].moistureCurveExponent); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "P2 Dry";    snprintf(m.value, 20, "%u", cfg.pots[1].moistureDryRaw); m.visible = (cfg.numPots >= 2); }
    { auto& m = items[itemCount++]; m.label = "P2 Wet";    snprintf(m.value, 20, "%u", cfg.pots[1].moistureWetRaw); m.visible = (cfg.numPots >= 2); }
    { auto& m = items[itemCount++]; m.label = "P2 Exp";    snprintf(m.value, 20, "%.2f", cfg.pots[1].moistureCurveExponent); m.visible = (cfg.numPots >= 2); }

    // Indeksy profili do rysowania ikon w Settings (oblicz po zbudowaniu items)
    uint8_t profIdx0 = cfg.pots[0].plantProfileIndex;
    uint8_t profIdx1 = cfg.pots[1].plantProfileIndex;
    { auto& m = items[itemCount++]; m.label = "Rezerwuar"; snprintf(m.value, 20, "%.1fL", cfg.reservoirCapacityMl / 1000.0f); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Rez. min";  snprintf(m.value, 20, "%.0f ml", cfg.reservoirLowThresholdMl); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Puls";      snprintf(m.value, 20, "%d ml", cfg.pots[0].pulseWaterMl); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "REFILL";    snprintf(m.value, 20, "(press)"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Tryb";      snprintf(m.value, 20, "%s", cfg.mode == Mode::AUTO ? "AUTO" : "MANUAL"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Vacation";  snprintf(m.value, 20, "%s", cfg.vacationMode ? "ON" : "OFF"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "WiFi";      snprintf(m.value, 20, "%s", snap.netConfig.provisioned ? snap.netConfig.wifiSsid : "Brak"); m.visible = true; }
    { auto& m = items[itemCount++]; m.label = "Reset";     snprintf(m.value, 20, "BtnA+B 5s"); m.visible = true; }

    uint8_t visibleCount = 0;
    for (uint8_t i = 0; i < itemCount; ++i) {
        if (items[i].visible) visibleCount++;
    }

    uint8_t startIdx = 0;
    if (clampedState.settingsIndex >= kSettingsVisibleRows) {
        startIdx = clampedState.settingsIndex - (kSettingsVisibleRows - 1);
    }
    if (visibleCount > kSettingsVisibleRows && startIdx > (visibleCount - kSettingsVisibleRows)) {
        startIdx = visibleCount - kSettingsVisibleRows;
    }

    uint8_t visIdx = 0;
    uint8_t drawRow = 0;
    for (uint8_t i = 0; i < itemCount; ++i) {
        if (!items[i].visible) continue;
        bool sel = (visIdx == clampedState.settingsIndex);
        if (visIdx < startIdx) {
            visIdx++;
            continue;
        }
        if (drawRow >= kSettingsVisibleRows) break;

        int16_t y = 28 + drawRow * 22;

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
        drawRow++;
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
        uint8_t visCount = uiSettingsVisibleCount(cfg);
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

    uiClampSettingsIndex(state, cfg);

    auto nextDryRaw = [&](uint8_t potIdx) {
        uint16_t minValue = cfg.pots[potIdx].moistureWetRaw + kMoistureRawMinGap;
        uint16_t next = cfg.pots[potIdx].moistureDryRaw + kMoistureRawStep;
        if (next > kMoistureRawMax) next = minValue;
        if (next < minValue) next = minValue;
        cfg.pots[potIdx].moistureDryRaw = next;
    };

    auto nextWetRaw = [&](uint8_t potIdx) {
        uint16_t maxValue = cfg.pots[potIdx].moistureDryRaw - kMoistureRawMinGap;
        uint16_t next = cfg.pots[potIdx].moistureWetRaw + kMoistureRawStep;
        if (next > maxValue) next = kMoistureRawMin;
        if (next > maxValue) next = maxValue;
        cfg.pots[potIdx].moistureWetRaw = next;
    };

    auto nextCurveExponent = [&](uint8_t potIdx) {
        float next = cfg.pots[potIdx].moistureCurveExponent + kMoistureCurveExpStep;
        if (next > kMoistureCurveExpMax + 0.0001f) next = kMoistureCurveExpMin;
        cfg.pots[potIdx].moistureCurveExponent = next;
    };

    uint8_t actionMap[kSettingsActionCountMax];
    uint8_t actionCount = 0;
    actionMap[actionCount++] = 0;   // Pompy
    actionMap[actionCount++] = 1;   // Profil 1
    if (cfg.numPots >= 2) actionMap[actionCount++] = 2; // Profil 2
    actionMap[actionCount++] = 11;  // P1 Dry
    actionMap[actionCount++] = 12;  // P1 Wet
    actionMap[actionCount++] = 15;  // P1 Exp
    if (cfg.numPots >= 2) actionMap[actionCount++] = 13; // P2 Dry
    if (cfg.numPots >= 2) actionMap[actionCount++] = 14; // P2 Wet
    if (cfg.numPots >= 2) actionMap[actionCount++] = 16; // P2 Exp
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
        uiClampSettingsIndex(state, cfg);
        Serial.printf("[UI] event=settings_update key=num_pots value=%u\n", cfg.numPots);
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
        Serial.println("[UI] event=settings_wifi_setup_request");
        changed = true;  // handled event-driven in ControlTask
        break;
    }
    case 9:
        Serial.println("[UI] event=settings_factory_reset_hint hold=A+B_5s");
        break;
    case 10: {
        // Pulse size — cyklicznie 10-100ml, step 5ml, ustawiane na oba poty
        uint16_t next = cfg.pots[0].pulseWaterMl + kPulseStepMl;
        if (next > kPulseMaxMl) next = kPulseMinMl;
        for (uint8_t p = 0; p < kMaxPots; ++p) cfg.pots[p].pulseWaterMl = next;
        Serial.printf("[UI] event=settings_update key=pulse_ml value=%u\n", next);
        changed = true;
        break;
    }
    case 11:
        nextDryRaw(0);
        changed = true;
        break;
    case 12:
        nextWetRaw(0);
        changed = true;
        break;
    case 13:
        nextDryRaw(1);
        changed = true;
        break;
    case 14:
        nextWetRaw(1);
        changed = true;
        break;
    case 15:
        nextCurveExponent(0);
        changed = true;
        break;
    case 16:
        nextCurveExponent(1);
        changed = true;
        break;
    }

    state.needsRedraw = true;
    return changed;
}
