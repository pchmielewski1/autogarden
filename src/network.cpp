// ============================================================================
// network.cpp — WiFi provisioning, Telegram bot, remote commands
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "WiFi Provisioning",
//                "Powiadomienia Telegram", "Sieć / remote control"
//
// UWAGA: Telegram wymaga lib_deps: UniversalTelegramBot, ArduinoJson
//        → dodać do platformio.ini gdy gotowe.
// ============================================================================

#include "network.h"
#include "events.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <UniversalTelegramBot.h>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "log_serial.h"

#define Serial AGSerial

#if __has_include(<telegram_local_config.h>)
#include <telegram_local_config.h>
#else
#define AG_TELEGRAM_LOCAL_ENABLED 0
#define AG_TELEGRAM_BOT_NAME ""
#define AG_TELEGRAM_BOT_TOKEN ""
#define AG_TELEGRAM_CHAT_IDS ""
#endif

#ifndef AG_TELEGRAM_CHAT_IDS
#ifdef AG_TELEGRAM_CHAT_ID
#define AG_TELEGRAM_CHAT_IDS AG_TELEGRAM_CHAT_ID
#else
#define AG_TELEGRAM_CHAT_IDS ""
#endif
#endif

// ---------------------------------------------------------------------------
// Captive portal HTML — stored in PROGMEM
// ---------------------------------------------------------------------------
extern const char PROVISIONING_HTML[] PROGMEM;

static const char SUCCESS_HTML[] PROGMEM = R"(
<!DOCTYPE html><html><body>
<h1>Saved!</h1><p>Restarting... Connect to your WiFi network.</p>
</body></html>
)";

// ---------------------------------------------------------------------------
// Statyczne instancje serwerów (tylko w AP mode)
// ---------------------------------------------------------------------------
static WebServer* g_webServer  = nullptr;
static DNSServer* g_dnsServer  = nullptr;
static NetConfig* g_apNetCfg   = nullptr;   // pointer do NetConfig w AP mode
static WiFiClientSecure g_tgClient;
static std::unique_ptr<UniversalTelegramBot> g_tgBot;
static char g_tgTokenCache[64] = {};
static char g_tgConfiguredBotName[64] = {};
static char g_tgReplyBuf[2048] = {};
static char g_tgKeyboardBuf[768] = {};

enum class TelegramPanelView : uint8_t {
    MENU = 0,
    PROFILES = 1,
    REFILL = 2,
};

static char g_tgActivePanelChatId[24] = {};
static int g_tgActivePanelMessageId = 0;
static TelegramPanelView g_tgActivePanelView = TelegramPanelView::MENU;
static uint8_t g_tgActiveProfilePot = 0;
static uint16_t g_tgActiveRefillCapacityMl = 0;
static uint32_t g_tgLastIdleLogMs = 0;
static uint32_t g_tgLastPollMs = 0;
static uint32_t g_tgFastPollUntilMs = 0;
static uint32_t g_tgLastPollFailMs = 0;
static uint32_t g_tgLastPollCooldownLogMs = 0;
static uint32_t g_tgLastStatsLogMs = 0;
static char g_tgLastMenuChatId[24] = {};
static uint32_t g_tgLastMenuMs = 0;
static constexpr uint32_t kTelegramPollIntervalIdleMs = 2000;
static constexpr uint32_t kTelegramPollIntervalActiveMs = 1200;
static constexpr uint32_t kTelegramFastPollWindowMs = 120000;
static constexpr uint32_t kTelegramRequestTimeoutMs = 3500;
static constexpr uint8_t kTelegramMaxBatchFetches = 3;
static constexpr uint32_t kTelegramPollFailCooldownMs = 800;
static constexpr uint32_t kTelegramPollWorkBudgetMs = 1200;
static constexpr uint32_t kTelegramPollSlowLogMs = 2500;
static constexpr uint32_t kTelegramMenuDedupMs = 6000;

struct TelegramRuntimeStats {
    uint32_t fetchCalls = 0;
    uint32_t fetchFailures = 0;
    uint32_t fetchTimeoutLike = 0;
    uint32_t fetchMaxMs = 0;
    uint32_t pollCycles = 0;
    uint32_t pollBudgetBreaks = 0;
    uint32_t sendAttempts = 0;
    uint32_t sendRetries = 0;
};

static TelegramRuntimeStats g_tgStats;

static uint8_t countChatIds(const char* chatIds);
static void safeCopy(char* dst, size_t dstSize, const char* src);
static void formatDurationCompact(uint32_t durationMs, char* buf, size_t bufSize);
static void formatDaysRemainingShort(float daysRemaining, char* buf, size_t bufSize);
static void formatElapsedShort(uint32_t nowMs, uint32_t sinceMs, char* buf, size_t bufSize);
static float trendRecentAverage(const TrendState& ts, uint8_t windowSamples);
static void trendRecentMinMax(const TrendState& ts, float& minValue, float& maxValue);
static const char* classifyTrendPace(float lastRate, float baselineRate);
static const char* telegramWaterLevelStateShort(WaterLevelState state);
static const char* telegramCyclePhaseShort(WateringPhase phase);
static const char* telegramTrendArrow(float rate);
static const char* telegramTrendClassLabel(const char* pace);
static const char* telegramDuskPhasePretty(DuskPhase phase);
static void formatPotActionLine(const TelegramStatusData& status,
                                uint8_t potIdx,
                                char* buf,
                                size_t bufSize);
static void buildTelegramInlineMenuKeyboard(const TelegramStatusData& status,
                                            char* buf, size_t bufSize);
static void buildTelegramProfilesKeyboard(const TelegramStatusData& status,
                                          uint8_t activePot,
                                          char* buf,
                                          size_t bufSize);
static void buildTelegramRefillKeyboard(char* buf, size_t bufSize);
static void buildTelegramPanelKeyboard(const TelegramStatusData& status,
                                       TelegramPanelView panelView,
                                       uint8_t activePot,
                                       char* buf,
                                       size_t bufSize);
static bool telegramSendInlineKeyboardToChat(const String& chatId,
                                             const char* msg,
                                             const char* keyboardJson,
                                             const NetConfig& netCfg,
                                             int messageId,
                                             uint8_t maxRetries,
                                             uint32_t retryDelayMs,
                                             TelegramPanelView panelView,
                                             uint8_t panelPot,
                                             bool allowNewMessageFallback);

static void telegramRememberActivePanel(const String& chatId,
                                        int messageId,
                                        TelegramPanelView panelView,
                                        uint8_t panelPot) {
    if (messageId <= 0) {
        return;
    }
    safeCopy(g_tgActivePanelChatId, sizeof(g_tgActivePanelChatId), chatId.c_str());
    g_tgActivePanelMessageId = messageId;
    g_tgActivePanelView = panelView;
    g_tgActiveProfilePot = panelPot;
}

static void telegramLogStatsIfDue(uint32_t nowMs) {
    if (g_tgLastStatsLogMs != 0 && (nowMs - g_tgLastStatsLogMs) < 60000) {
        return;
    }
    g_tgLastStatsLogMs = nowMs;
    Serial.printf("[TG] event=stats fetch_calls=%lu fetch_fail=%lu fetch_timeout_like=%lu fetch_max_ms=%lu poll_cycles=%lu budget_breaks=%lu send_attempts=%lu send_retries=%lu\n",
                  static_cast<unsigned long>(g_tgStats.fetchCalls),
                  static_cast<unsigned long>(g_tgStats.fetchFailures),
                  static_cast<unsigned long>(g_tgStats.fetchTimeoutLike),
                  static_cast<unsigned long>(g_tgStats.fetchMaxMs),
                  static_cast<unsigned long>(g_tgStats.pollCycles),
                  static_cast<unsigned long>(g_tgStats.pollBudgetBreaks),
                  static_cast<unsigned long>(g_tgStats.sendAttempts),
                  static_cast<unsigned long>(g_tgStats.sendRetries));
}

static void telegramBumpFastPollWindow(uint32_t nowMs) {
    g_tgFastPollUntilMs = nowMs + kTelegramFastPollWindowMs;
}

static uint32_t telegramCurrentPollInterval(uint32_t nowMs) {
    return (nowMs < g_tgFastPollUntilMs)
        ? kTelegramPollIntervalActiveMs
        : kTelegramPollIntervalIdleMs;
}

static int appendFmt(char* buf, size_t bufSize, int pos, const char* fmt, ...) {
    if (!buf || bufSize == 0) {
        return 0;
    }
    if (pos < 0) {
        pos = 0;
    }
    if (static_cast<size_t>(pos) >= bufSize) {
        buf[bufSize - 1] = '\0';
        return static_cast<int>(bufSize - 1);
    }

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + pos, bufSize - static_cast<size_t>(pos), fmt, args);
    va_end(args);
    if (n <= 0) {
        return pos;
    }
    if (static_cast<size_t>(n) >= (bufSize - static_cast<size_t>(pos))) {
        buf[bufSize - 1] = '\0';
        return static_cast<int>(bufSize - 1);
    }
    return pos + n;
}

static void safeCopy(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

static bool localTelegramSecretsAvailable() {
    return AG_TELEGRAM_LOCAL_ENABLED && AG_TELEGRAM_BOT_TOKEN[0] != '\0' &&
           AG_TELEGRAM_CHAT_IDS[0] != '\0';
}

const char* telegramConfiguredBotName() {
    return g_tgConfiguredBotName;
}

uint8_t telegramConfiguredTargetCount(const NetConfig& netCfg) {
    return countChatIds(netCfg.telegramChatIds);
}

bool telegramInteractionActive(uint32_t nowMs) {
    return nowMs < g_tgFastPollUntilMs;
}

bool telegramHasActivePanel() {
    return g_tgActivePanelChatId[0] != '\0' && g_tgActivePanelMessageId > 0;
}

bool telegramSendToActivePanel(const char* msg,
                               const TelegramStatusData& status,
                               const NetConfig& netCfg) {
    if (!msg || msg[0] == '\0') {
        return false;
    }
    if (!telegramHasActivePanel()) {
        return false;
    }
    buildTelegramPanelKeyboard(status,
                               g_tgActivePanelView,
                               g_tgActiveProfilePot,
                               g_tgKeyboardBuf,
                               sizeof(g_tgKeyboardBuf));
    return telegramSendInlineKeyboardToChat(g_tgActivePanelChatId,
                                            msg,
                                            g_tgKeyboardBuf,
                                            netCfg,
                                            g_tgActivePanelMessageId,
                                            1,
                                            0,
                                            g_tgActivePanelView,
                                            g_tgActiveProfilePot,
                                            false);
}

static bool isChatListSeparator(char c) {
    return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool isValidChatIdToken(const String& token) {
    if (token.length() == 0) return false;
    size_t start = (token.charAt(0) == '-') ? 1 : 0;
    if (start >= token.length()) return false;
    for (size_t i = start; i < token.length(); ++i) {
        char c = token.charAt(i);
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static String normalizeChatIdList(const String& raw, size_t maxLen) {
    String normalized;
    String token;
    normalized.reserve(raw.length());
    token.reserve(20);

    auto flushToken = [&]() {
        token.trim();
        if (token.length() == 0) {
            token = "";
            return;
        }
        if (!isValidChatIdToken(token)) {
            token = "";
            return;
        }
        String wrapped = String(",") + normalized + ",";
        String needle = String(",") + token + ",";
        if (normalized.length() == 0) {
            if (token.length() <= maxLen) {
                normalized = token;
            }
        } else if (wrapped.indexOf(needle) < 0) {
            if ((normalized.length() + 1 + token.length()) <= maxLen) {
                normalized += ",";
                normalized += token;
            }
        }
        token = "";
    };

    for (size_t i = 0; i < raw.length(); ++i) {
        char c = raw.charAt(i);
        if (isChatListSeparator(c)) {
            flushToken();
        } else {
            token += c;
        }
    }
    flushToken();
    return normalized;
}

static bool chatIdListHasAny(const char* chatIds) {
    return chatIds && chatIds[0] != '\0';
}

static uint8_t countChatIds(const char* chatIds) {
    if (!chatIdListHasAny(chatIds)) return 0;
    uint8_t count = 0;
    bool inToken = false;
    for (size_t i = 0; chatIds[i] != '\0'; ++i) {
        if (isChatListSeparator(chatIds[i])) {
            inToken = false;
        } else if (!inToken) {
            inToken = true;
            if (count < 255) ++count;
        }
    }
    return count;
}

template<typename Callback>
static void forEachChatId(const char* chatIds, Callback cb) {
    if (!chatIdListHasAny(chatIds)) return;
    char token[20];
    size_t w = 0;
    for (size_t i = 0;; ++i) {
        char c = chatIds[i];
        bool end = (c == '\0');
        if (end || isChatListSeparator(c)) {
            if (w > 0) {
                token[w] = '\0';
                cb(token);
                w = 0;
            }
            if (end) break;
        } else if (w + 1 < sizeof(token)) {
            token[w++] = c;
        }
    }
}

bool telegramSendInlineMessage(const char* msg,
                               const TelegramStatusData& status,
                               const NetConfig& netCfg) {
    if (!msg || msg[0] == '\0') {
        return false;
    }
    if (telegramHasActivePanel()) {
        return telegramSendToActivePanel(msg, status, netCfg);
    }
    if (!chatIdListHasAny(netCfg.telegramChatIds) || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    buildTelegramPanelKeyboard(status,
                               TelegramPanelView::MENU,
                               status.selectedPot,
                               g_tgKeyboardBuf,
                               sizeof(g_tgKeyboardBuf));

    bool anyOk = false;
    forEachChatId(netCfg.telegramChatIds, [&](const char* token) {
        bool sent = telegramSendInlineKeyboardToChat(String(token),
                                                     msg,
                                                     g_tgKeyboardBuf,
                                                     netCfg,
                                                     0,
                                                     1,
                                                     0,
                                                     TelegramPanelView::MENU,
                                                     status.selectedPot,
                                                     false);
        anyOk = anyOk || sent;
    });
    return anyOk;
}

void applyLocalTelegramConfig(NetConfig& netCfg) {
    const char* cfgName = netCfg.telegramBotName[0] ? netCfg.telegramBotName : AG_TELEGRAM_BOT_NAME;
    safeCopy(g_tgConfiguredBotName, sizeof(g_tgConfiguredBotName), cfgName);

    if (!localTelegramSecretsAvailable()) {
        return;
    }
    if (netCfg.telegramBotName[0] == '\0' && AG_TELEGRAM_BOT_NAME[0] != '\0') {
        safeCopy(netCfg.telegramBotName, sizeof(netCfg.telegramBotName), AG_TELEGRAM_BOT_NAME);
    }
    if (netCfg.telegramBotToken[0] == '\0') {
        safeCopy(netCfg.telegramBotToken, sizeof(netCfg.telegramBotToken), AG_TELEGRAM_BOT_TOKEN);
    }
    if (netCfg.telegramChatIds[0] == '\0') {
        String normalized = normalizeChatIdList(String(AG_TELEGRAM_CHAT_IDS), sizeof(netCfg.telegramChatIds) - 1);
        safeCopy(netCfg.telegramChatIds, sizeof(netCfg.telegramChatIds), normalized.c_str());
    }

    const char* finalName = netCfg.telegramBotName[0] ? netCfg.telegramBotName : AG_TELEGRAM_BOT_NAME;
    safeCopy(g_tgConfiguredBotName, sizeof(g_tgConfiguredBotName), finalName);
}

static const char* modeStr(Mode mode) {
    return mode == Mode::AUTO ? "AUTO" : "MANUAL";
}

static const char* duskPhaseStr(DuskPhase phase) {
    switch (phase) {
        case DuskPhase::NIGHT: return "NIGHT";
        case DuskPhase::DAWN_TRANSITION: return "DAWN_TR";
        case DuskPhase::DAY: return "DAY";
        case DuskPhase::DUSK_TRANSITION: return "DUSK_TR";
    }
    return "?";
}

static void formatLastFeedback(const TelegramStatusData& data,
                               uint8_t potIdx,
                               char* buf,
                               size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }
    buf[0] = '\0';

    switch (data.lastFeedbackCode[potIdx]) {
        case WateringFeedbackCode::CYCLE_START_SCHEDULE:
            snprintf(buf, bufSize, "auto_start %.1f%%", data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::CYCLE_START_RESCUE:
            snprintf(buf, bufSize, "rescue_start %.1f%%", data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::SKIP_ALREADY_WET:
            snprintf(buf, bufSize, "skip wet %.1f/%.1f%%",
                     data.lastFeedbackValue1[potIdx], data.lastFeedbackValue2[potIdx]);
            break;
        case WateringFeedbackCode::SKIP_ABOVE_MAX:
            snprintf(buf, bufSize, "skip max %.1f/%.1f%%",
                     data.lastFeedbackValue1[potIdx], data.lastFeedbackValue2[potIdx]);
            break;
        case WateringFeedbackCode::OVERFLOW_DETECTED:
            snprintf(buf, bufSize, "overflow pulse=%u",
                     static_cast<unsigned>(data.lastFeedbackPulseCount[potIdx]));
            break;
        case WateringFeedbackCode::OVERFLOW_RESUME:
            snprintf(buf, bufSize, "overflow_resume target=%.1f%%",
                     data.lastFeedbackValue2[potIdx]);
            break;
        case WateringFeedbackCode::TARGET_REACHED:
            snprintf(buf, bufSize, "target_reached %.1f%%", data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::STOP_MAX_EXCEEDED:
            snprintf(buf, bufSize, "stop max %.1f/%.1f%%",
                     data.lastFeedbackValue1[potIdx], data.lastFeedbackValue2[potIdx]);
            break;
        case WateringFeedbackCode::STOP_MAX_PULSES:
            snprintf(buf, bufSize, "stop max_pulses %u",
                     static_cast<unsigned>(data.lastFeedbackPulseCount[potIdx]));
            break;
        case WateringFeedbackCode::OVERFLOW_TIMEOUT:
            snprintf(buf, bufSize, "overflow_timeout %.0fs",
                     data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_RISK:
            safeCopy(buf, bufSize, "blocked overflow_risk");
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_TANK_EMPTY:
        case WateringFeedbackCode::SAFETY_BLOCK_RESERVOIR_EMPTY:
            safeCopy(buf, bufSize, "blocked reservoir_empty");
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_OVERFLOW_SENSOR_UNKNOWN:
            safeCopy(buf, bufSize, "blocked overflow_unknown");
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_TANK_SENSOR_UNKNOWN:
            safeCopy(buf, bufSize, "blocked reservoir_unknown");
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_PUMP_CONFIG_INVALID:
            safeCopy(buf, bufSize, "blocked pump_config_invalid");
            break;
        case WateringFeedbackCode::SAFETY_BLOCK_PUMP_STOP_FAILED:
            safeCopy(buf, bufSize, "blocked pump_stop_failed");
            break;
        case WateringFeedbackCode::HARD_TIMEOUT:
            snprintf(buf, bufSize, "hard_timeout %.0fms", data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::PUMP_STOP_RECOVERED:
            safeCopy(buf, bufSize, "pump_stop_recovered");
            break;
        case WateringFeedbackCode::SAFETY_UNBLOCK:
            safeCopy(buf, bufSize, "safety_unblock");
            break;
        case WateringFeedbackCode::CYCLE_DONE_GENERIC:
            snprintf(buf, bufSize, "cycle_done %.1f%%", data.lastFeedbackValue1[potIdx]);
            break;
        case WateringFeedbackCode::NONE:
        default:
            safeCopy(buf, bufSize, "-" );
            break;
    }
}

static bool telegramWaterBlocked(const TelegramStatusData& status,
                                 uint8_t potIdx,
                                 char* reasonBuf,
                                 size_t reasonBufSize,
                                 uint32_t* cooldownRemainingMs = nullptr) {
    if (cooldownRemainingMs) {
        *cooldownRemainingMs = 0;
    }
    if (reasonBuf && reasonBufSize > 0) {
        reasonBuf[0] = '\0';
    }

    if (potIdx >= status.config.numPots || !status.config.pots[potIdx].enabled) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: pot disabled.");
        return true;
    }
    if (status.config.mode != Mode::AUTO) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: water requires AUTO mode.");
        return true;
    }

    if (status.pumpStop[potIdx].pending
        || status.pumpStop[potIdx].faultState == PumpStopFaultState::STOP_FAILED_LATCHED) {
        safeCopy(reasonBuf,
                 reasonBufSize,
                 status.pumpStop[potIdx].faultState == PumpStopFaultState::STOP_FAILED_LATCHED
                     ? "Blocked: pump stop failed, retrying recovery."
                     : "Blocked: pump stop pending.");
        return true;
    }

    WateringPhase phase = status.cycles[potIdx].phase;
    if (phase != WateringPhase::IDLE) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: watering already active.");
        return true;
    }

    const PotConfig& potCfg = status.config.pots[potIdx];
    const PotSensorSnapshot& pot = status.sensors.pots[potIdx];
    const PlantProfile& prof = getActiveProfile(status.config, potIdx);
    float effectiveTarget = prof.targetMoisturePct;
    float effectiveMax = prof.maxMoisturePct;
    if (status.config.vacationMode) {
        effectiveTarget -= status.config.vacationTargetReductionPct;
        if (effectiveTarget < 5.0f) {
            effectiveTarget = 5.0f;
        }
    }

    if (pot.moisturePct >= effectiveTarget) {
        if (reasonBuf && reasonBufSize > 0) {
            snprintf(reasonBuf, reasonBufSize,
                     "Blocked: already wet %.1f%% >= %.1f%% target.",
                     pot.moisturePct, effectiveTarget);
        }
        return true;
    }
    if (pot.moisturePct >= effectiveMax) {
        if (reasonBuf && reasonBufSize > 0) {
            snprintf(reasonBuf, reasonBufSize,
                     "Blocked: above max %.1f%% >= %.1f%%.",
                     pot.moisturePct, effectiveMax);
        }
        return true;
    }

    if (status.config.antiOverflowEnabled) {
        if (pot.waterGuards.potMax == WaterLevelState::TRIGGERED) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: overflow sensor triggered.");
            return true;
        }
        if (status.config.waterLevelUnknownPolicy == UnknownPolicy::BLOCK
            && pot.waterGuards.potMax == WaterLevelState::UNKNOWN) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: overflow sensor unknown.");
            return true;
        }
        if (pot.waterGuards.reservoirMin == WaterLevelState::TRIGGERED) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir sensor triggered.");
            return true;
        }
        if (status.config.waterLevelUnknownPolicy == UnknownPolicy::BLOCK
            && pot.waterGuards.reservoirMin == WaterLevelState::UNKNOWN) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir sensor unknown.");
            return true;
        }
    }

    if (status.budget.reservoirLow && status.budget.reservoirCurrentMl <= 0.0f) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir empty.");
        return true;
    }
    if (potCfg.pumpMlPerSec <= 0.0f) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: fixed pump parameters invalid.");
        return true;
    }

    uint32_t effCooldown = status.config.cooldownMs;
    if (status.config.vacationMode) {
        effCooldown = static_cast<uint32_t>(effCooldown * status.config.vacationCooldownMultiplier);
    }
    uint32_t lastDoneMs = status.lastCycleDoneMs[potIdx];
    if (lastDoneMs != 0 && status.uptimeMs >= lastDoneMs) {
        uint32_t elapsed = status.uptimeMs - lastDoneMs;
        if (elapsed < effCooldown) {
            uint32_t remaining = effCooldown - elapsed;
            if (cooldownRemainingMs) {
                *cooldownRemainingMs = remaining;
            }
            if (reasonBuf && reasonBufSize > 0) {
                snprintf(reasonBuf, reasonBufSize, "Blocked: cooldown %lus remaining.",
                         static_cast<unsigned long>((remaining + 999) / 1000));
            }
            return true;
        }
    }

    return false;
}

static void telegramWaterButtonLabel(const TelegramStatusData& status,
                                     uint8_t potIdx,
                                     char* buf,
                                     size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }
    buf[0] = '\0';
    char reason[96] = {};
    uint32_t cooldownRemainingMs = 0;
    bool blocked = telegramWaterBlocked(status, potIdx, reason, sizeof(reason), &cooldownRemainingMs);

    if (!blocked) {
        snprintf(buf, bufSize, "\xF0\x9F\x92\xA7 Water %uml",
                 static_cast<unsigned>(status.config.pots[potIdx].pulseWaterMl));
        return;
    }

    if (status.pumpStop[potIdx].faultState == PumpStopFaultState::STOP_FAILED_LATCHED) {
        safeCopy(buf, bufSize, "\xF0\x9F\x9A\xAB Stop fault");
    } else if (status.cycles[potIdx].phase == WateringPhase::STOPPING) {
        safeCopy(buf, bufSize, "\xE2\x8F\xB9 Stopping...");
    } else if (status.cycles[potIdx].phase != WateringPhase::IDLE) {
        safeCopy(buf, bufSize, "\xF0\x9F\x92\xA7 Watering...");
    } else if (strstr(reason, "already wet") != nullptr || strstr(reason, "above max") != nullptr) {
        safeCopy(buf, bufSize, "\xE2\x9C\x85 Wet enough");
    } else if (cooldownRemainingMs > 0) {
        snprintf(buf, bufSize, "\xE2\x8F\xB3 Water %lus",
                 static_cast<unsigned long>((cooldownRemainingMs + 999) / 1000));
    } else if (status.config.mode != Mode::AUTO) {
        safeCopy(buf, bufSize, "\xF0\x9F\x9A\xAB Water AUTO");
    } else {
        safeCopy(buf, bufSize, "\xF0\x9F\x9A\xAB Water blocked");
    }
}

static String normalizeTelegramCommand(String text) {
    text.trim();
    int nl = text.indexOf('\n');
    if (nl >= 0) {
        text = text.substring(0, nl);
    }
    int cr = text.indexOf('\r');
    if (cr >= 0) {
        text = text.substring(0, cr);
    }
    text.trim();

    int space = text.indexOf(' ');
    String head = (space >= 0) ? text.substring(0, space) : text;
    int at = head.indexOf('@');
    if (at >= 0) {
        head = head.substring(0, at);
    }
    if (space >= 0) {
        text = head + text.substring(space);
    } else {
        text = head;
    }
    text.trim();
    return text;
}

static bool telegramEnsureReady(const NetConfig& netCfg) {
    if (netCfg.telegramBotToken[0] == '\0') {
        return false;
    }
    if (!g_tgBot || strcmp(g_tgTokenCache, netCfg.telegramBotToken) != 0) {
        g_tgClient.stop();
        g_tgClient.setInsecure();
        g_tgClient.setTimeout(kTelegramRequestTimeoutMs);
        g_tgBot.reset(new UniversalTelegramBot(netCfg.telegramBotToken, g_tgClient));
        g_tgBot->maxMessageLength = 4096;
        g_tgBot->longPoll = 0;
        g_tgBot->waitForResponse = 1500;
        safeCopy(g_tgTokenCache, sizeof(g_tgTokenCache), netCfg.telegramBotToken);
        g_tgLastPollMs = 0;
        g_tgFastPollUntilMs = 0;
        g_tgLastPollFailMs = 0;
        Serial.printf("[TG] event=client_ready poll_idle_ms=%lu poll_active_ms=%lu timeout_ms=%lu\n",
                      static_cast<unsigned long>(kTelegramPollIntervalIdleMs),
                      static_cast<unsigned long>(kTelegramPollIntervalActiveMs),
                      static_cast<unsigned long>(kTelegramRequestTimeoutMs));
        Serial.printf("[TG] event=client_cfg max_msg_len=%d long_poll_s=%d wait_resp_ms=%u\n",
                  g_tgBot->maxMessageLength,
                  g_tgBot->longPoll,
                  static_cast<unsigned>(g_tgBot->waitForResponse));
    }
    return g_tgBot != nullptr;
}

static void telegramCloseTransport(const char* reason, bool logClose = false) {
    if (g_tgClient.connected()) {
        g_tgClient.stop();
        if (logClose) {
            Serial.printf("[TG] event=transport_close reason=%s\n",
                          reason ? reason : "unknown");
        }
    }
}

static int telegramFetchUpdates(const NetConfig& netCfg) {
    if (!telegramEnsureReady(netCfg)) {
        return -999;
    }
    uint32_t startedMs = millis();
    int count = g_tgBot->getUpdates(g_tgBot->last_message_received + 1);
    uint32_t durationMs = millis() - startedMs;
    ++g_tgStats.fetchCalls;
    if (durationMs > g_tgStats.fetchMaxMs) {
        g_tgStats.fetchMaxMs = durationMs;
    }

    if (count < 0) {
        g_tgLastPollFailMs = millis();
        ++g_tgStats.fetchFailures;
        if (durationMs + 50 >= kTelegramRequestTimeoutMs) {
            ++g_tgStats.fetchTimeoutLike;
        }
        const int botErrCode = g_tgBot ? g_tgBot->_lastError : 0;
        const char* botErrMsg = (g_tgBot && g_tgBot->lastErrorMessage.length() > 0)
            ? g_tgBot->lastErrorMessage.c_str()
            : "-";
        Serial.printf("[TG] event=updates_fail count=%d dur_ms=%lu timeout_like=%s bot_err=%d bot_msg=%s\n",
                      count,
                      static_cast<unsigned long>(durationMs),
                      (durationMs + 50 >= kTelegramRequestTimeoutMs) ? "yes" : "no",
                      botErrCode,
                      botErrMsg);
        return count;
    }

    if (count > 0) {
        Serial.printf("[TG] event=updates count=%d dur_ms=%lu last=%ld\n",
                      count,
                      static_cast<unsigned long>(durationMs),
                      static_cast<long>(g_tgBot->last_message_received));
    }
    return count;
}

static bool telegramReplyToChat(const String& chatId, const String& msg,
                                const NetConfig& netCfg) {
    if (!telegramEnsureReady(netCfg)) {
        Serial.println("[TG] event=reply_fail reason=client_not_ready");
        return false;
    }

    uint32_t startedMs = millis();
    if (g_tgBot->sendMessage(chatId, msg, "")) {
        uint32_t durationMs = millis() - startedMs;
        telegramBumpFastPollWindow(millis());
        Serial.printf("[TG] event=reply_ok len=%u dur_ms=%lu\n",
                      static_cast<unsigned>(msg.length()),
                      static_cast<unsigned long>(durationMs));
        return true;
    }

    uint32_t durationMs = millis() - startedMs;
    const int botErrCode = g_tgBot ? g_tgBot->_lastError : 0;
    const char* botErrMsg = (g_tgBot && g_tgBot->lastErrorMessage.length() > 0)
        ? g_tgBot->lastErrorMessage.c_str()
        : "-";
    Serial.printf("[TG] event=reply_fail len=%u dur_ms=%lu bot_err=%d bot_msg=%s\n",
                  static_cast<unsigned>(msg.length()),
                  static_cast<unsigned long>(durationMs),
                  botErrCode,
                  botErrMsg);
    return false;
}

static bool telegramChatAuthorized(const String& chatId, const NetConfig& netCfg) {
    bool authorized = false;
    forEachChatId(netCfg.telegramChatIds, [&](const char* token) {
        if (!authorized && chatId.equals(token)) {
            authorized = true;
        }
    });
    return authorized;
}

static bool telegramSendToChat(const String& chatId, const String& msg,
                               const NetConfig& netCfg) {
    return telegramReplyToChat(chatId, msg, netCfg);
}

static bool telegramSendInlineKeyboardToChat(const String& chatId,
                                             const char* msg,
                                             const char* keyboardJson,
                                             const NetConfig& netCfg,
                                             int messageId = 0,
                                             uint8_t maxRetries = 1,
                                             uint32_t retryDelayMs = 150,
                                             TelegramPanelView panelView = TelegramPanelView::MENU,
                                             uint8_t panelPot = 0,
                                             bool allowNewMessageFallback = true) {
    if (!telegramEnsureReady(netCfg) || !msg || !keyboardJson) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < maxRetries; ++attempt) {
        int targetMessageId = messageId;
        if (g_tgBot->sendMessageWithInlineKeyboard(chatId, msg, "", keyboardJson, targetMessageId)) {
            int actualMessageId = targetMessageId;
            if (actualMessageId == 0 && g_tgBot) {
                actualMessageId = g_tgBot->last_sent_message_id;
            }
            telegramRememberActivePanel(chatId, actualMessageId, panelView, panelPot);
            telegramBumpFastPollWindow(millis());
            Serial.printf("[TG] event=inline_ok len=%u attempt=%u message_id=%d\n",
                          static_cast<unsigned>(strlen(msg)),
                          static_cast<unsigned>(attempt + 1), actualMessageId);
            return true;
        }
        const int botErrCode = g_tgBot ? g_tgBot->_lastError : 0;
        const char* botErrMsg = (g_tgBot && g_tgBot->lastErrorMessage.length() > 0)
            ? g_tgBot->lastErrorMessage.c_str()
            : "-";
        Serial.printf("[TG] event=inline_fail len=%u attempt=%u message_id=%d bot_err=%d bot_msg=%s\n",
                      static_cast<unsigned>(strlen(msg)),
                      static_cast<unsigned>(attempt + 1), targetMessageId,
                      botErrCode,
                      botErrMsg);

        if (allowNewMessageFallback && targetMessageId != 0) {
            Serial.printf("[TG] event=inline_fallback_new_message old_message_id=%d\n",
                          targetMessageId);
            if (g_tgBot->sendMessageWithInlineKeyboard(chatId, msg, "", keyboardJson, 0)) {
                int actualMessageId = g_tgBot ? g_tgBot->last_sent_message_id : 0;
                telegramRememberActivePanel(chatId, actualMessageId, panelView, panelPot);
                telegramBumpFastPollWindow(millis());
                Serial.printf("[TG] event=inline_ok len=%u attempt=%u message_id=0 fallback=yes\n",
                              static_cast<unsigned>(strlen(msg)),
                              static_cast<unsigned>(attempt + 1));
                return true;
            }
            const int fbErrCode = g_tgBot ? g_tgBot->_lastError : 0;
            const char* fbErrMsg = (g_tgBot && g_tgBot->lastErrorMessage.length() > 0)
                ? g_tgBot->lastErrorMessage.c_str()
                : "-";
            Serial.printf("[TG] event=inline_fallback_fail len=%u attempt=%u old_message_id=%d bot_err=%d bot_msg=%s\n",
                          static_cast<unsigned>(strlen(msg)),
                          static_cast<unsigned>(attempt + 1), targetMessageId,
                          fbErrCode,
                          fbErrMsg);
        } else if (!allowNewMessageFallback && targetMessageId != 0) {
            Serial.printf("[TG] event=inline_no_fallback old_message_id=%d\n",
                          targetMessageId);
        }

        if (attempt + 1 < maxRetries) {
            vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
        }
    }
    return false;
}

static void formatTelegramMenuSummary(const TelegramStatusData& data, char* buf, size_t bufSize) {
    int pos = 0;
    char uptimeBuf[24] = {};
    char daysBuf[16] = {};
    formatDurationCompact(data.uptimeMs, uptimeBuf, sizeof(uptimeBuf));
    formatDaysRemainingShort(data.budget.daysRemaining, daysBuf, sizeof(daysBuf));

    pos = appendFmt(buf, bufSize, pos, "▣ AUTOGARDEN\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    pos = appendFmt(buf, bufSize, pos, "🧠 %s | 🏖 %s | 📶 %s | AP %s\n",
                    modeStr(data.config.mode),
                    data.config.vacationMode ? "ON" : "OFF",
                    data.wifiConnected ? "UP" : "DOWN",
                    data.apActive ? "ON" : "OFF");
    pos = appendFmt(buf, bufSize, pos, "⏱ %s | 🌗 %s\n",
                    uptimeBuf,
                    telegramDuskPhasePretty(data.duskPhase));
    pos = appendFmt(buf, bufSize, pos, "🪣 %.0f / %.0f ml | left %s%s\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.reservoirCapacityMl,
                    daysBuf,
                    data.budget.reservoirLow ? " | LOW" : "");
    pos = appendFmt(buf, bufSize, pos, "🌡 %.1fC | 💧 %.0f%% | ☀ %.0f lx\n",
                    data.sensors.env.tempC,
                    data.sensors.env.humidityPct,
                    data.sensors.env.lux);

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PlantProfile& prof = getActiveProfile(data.config, i);
        char actionLine[96] = {};
        char sinceBuf[24] = {};
        formatPotActionLine(data, i, actionLine, sizeof(actionLine));
        formatElapsedShort(data.uptimeMs, data.lastPumpActivityMs[i], sinceBuf, sizeof(sinceBuf));
        float last1h = trendRecentAverage(data.trends[i], 1);

        pos = appendFmt(buf, bufSize, pos, "\n🪴 POT %u | %s%s\n",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?",
                        (i == data.selectedPot) ? " [selected]" : "");
        pos = appendFmt(buf, bufSize, pos,
                        "  water %.1f%% | ema %.1f%% | trend %s %.2f%%/h\n",
                        data.sensors.pots[i].moisturePct,
                        data.sensors.pots[i].moistureEma,
                        telegramTrendArrow(last1h),
                        isnan(last1h) ? 0.0f : last1h);
        pos = appendFmt(buf, bufSize, pos,
                        "  action %s\n"
                        "  ovf %s | pump %s\n",
                        actionLine,
                        telegramWaterLevelStateShort(data.sensors.pots[i].waterGuards.potMax),
                        sinceBuf);
    }

    pos = appendFmt(buf, bufSize, pos, "\nUse the buttons below.");
}

static void buildTelegramInlineMenuKeyboard(const TelegramStatusData& status,
                                            char* buf, size_t bufSize) {
    int pos = 0;
    char waterBtn0[48] = {};
    char waterBtn1[48] = {};
    const char* statusLabel = "\xF0\x9F\x93\x8A Status";
    const char* historyLabel = "\xF0\x9F\x93\x88 History";
    const char* profilesLabel = "\xF0\x9F\x8C\xBF Profiles";
    const char* helpLabel = "\xE2\x9D\x93 Help";
    const char* stopLabel = "\xF0\x9F\x9B\x91 Stop";
    const char* refillLabel = "\xF0\x9F\xAA\xA3 Refill";
    const char* wifiLabel = "\xF0\x9F\x93\xB6 WiFi";
    const char* menuLabel = "\xF0\x9F\x8F\xA0 Menu";
    const char* waterLabel0 = "Water";
    const char* waterLabel1 = "Water P2";
    const char* vacationLabel = status.config.vacationMode
        ? "\xF0\x9F\x8F\x96\xEF\xB8\x8F Vacation OFF"
        : "\xF0\x9F\x8F\x96\xEF\xB8\x8F Vacation ON";
    const char* modeLabel = status.config.mode == Mode::AUTO
        ? "\xE2\x9A\x99\xEF\xB8\x8F Mode MANUAL"
        : "\xE2\x9A\x99\xEF\xB8\x8F Mode AUTO";
    if (status.config.numPots > 0) {
        telegramWaterButtonLabel(status, 0, waterBtn0, sizeof(waterBtn0));
        if (waterBtn0[0]) {
            waterLabel0 = waterBtn0;
        }
    }
    if (status.config.numPots > 1) {
        telegramWaterButtonLabel(status, 1, waterBtn1, sizeof(waterBtn1));
        if (waterBtn1[0]) {
            waterLabel1 = waterBtn1;
        }
    }

    pos = appendFmt(buf, bufSize, pos,
                    "[[{\"text\":\"%s\",\"callback_data\":\"ag:status\"},{\"text\":\"%s\",\"callback_data\":\"ag:history\"}],"
                    "[{\"text\":\"%s\",\"callback_data\":\"ag:profiles\"},{\"text\":\"%s\",\"callback_data\":\"ag:help\"}],",
                    statusLabel, historyLabel, profilesLabel, helpLabel);

    if (status.config.numPots <= 1) {
        pos = appendFmt(buf, bufSize, pos,
                        "[{\"text\":\"%s\",\"callback_data\":\"ag:water:0\"},"
                        "{\"text\":\"%s\",\"callback_data\":\"ag:stop\"}],",
                        waterLabel0, stopLabel);
    } else {
        pos = appendFmt(buf, bufSize, pos,
                        "[{\"text\":\"%s\",\"callback_data\":\"ag:water:0\"},"
                        "{\"text\":\"%s\",\"callback_data\":\"ag:water:1\"}],"
                        "[{\"text\":\"%s\",\"callback_data\":\"ag:stop\"},"
                        "{\"text\":\"%s\",\"callback_data\":\"ag:refill\"}],",
                        waterLabel0,
                        waterLabel1,
                        stopLabel,
                        refillLabel);
    }

    if (status.config.numPots <= 1) {
        pos = appendFmt(buf, bufSize, pos,
                        "[{\"text\":\"%s\",\"callback_data\":\"ag:refill\"},"
                        "{\"text\":\"%s\",\"callback_data\":\"ag:wifi\"}],",
                        refillLabel, wifiLabel);
    } else {
        pos = appendFmt(buf, bufSize, pos,
                        "[{\"text\":\"%s\",\"callback_data\":\"ag:wifi\"},"
                        "{\"text\":\"%s\",\"callback_data\":\"ag:menu\"}],",
                        wifiLabel, menuLabel);
    }

    pos = appendFmt(buf, bufSize, pos,
                    "[{\"text\":\"%s\",\"callback_data\":\"ag:vac:toggle\"},"
                    "{\"text\":\"%s\",\"callback_data\":\"ag:mode:toggle\"}]]",
                    vacationLabel,
                    modeLabel);
}

static uint8_t telegramClampProfilePot(const TelegramStatusData& status, uint8_t activePot) {
    if (status.config.numPots == 0) {
        return 0;
    }
    if (activePot < status.config.numPots) {
        return activePot;
    }
    if (status.selectedPot < status.config.numPots) {
        return status.selectedPot;
    }
    return 0;
}

static uint16_t telegramClampReservoirCapacityMl(const TelegramStatusData& status,
                                                 int32_t capacityMl) {
    int32_t minCapacityMl = 1000;
    int32_t requiredMinMl = static_cast<int32_t>(status.config.reservoirLowThresholdMl + 100.0f);
    if (requiredMinMl > minCapacityMl) {
        minCapacityMl = ((requiredMinMl + 249) / 250) * 250;
    }

    if (capacityMl < minCapacityMl) {
        capacityMl = minCapacityMl;
    }
    if (capacityMl > 50000) {
        capacityMl = 50000;
    }
    return static_cast<uint16_t>(capacityMl);
}

static uint16_t telegramCurrentReservoirCapacityMl(const TelegramStatusData& status) {
    return telegramClampReservoirCapacityMl(
        status,
        static_cast<int32_t>(status.config.reservoirCapacityMl + 0.5f));
}

static void formatElapsedShort(uint32_t nowMs,
                               uint32_t sinceMs,
                               char* buf,
                               size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }
    if (sinceMs == 0 || sinceMs > nowMs) {
        safeCopy(buf, bufSize, "-");
        return;
    }

    formatDurationCompact(nowMs - sinceMs, buf, bufSize);
}

static float trendRecentAverage(const TrendState& ts, uint8_t windowSamples) {
    if (ts.count == 0 || windowSamples == 0) {
        return NAN;
    }

    uint8_t samples = (ts.count < windowSamples) ? ts.count : windowSamples;
    float sum = 0.0f;
    for (uint8_t i = 0; i < samples; ++i) {
        uint8_t idx = (ts.headIdx + TrendState::kHours - 1 - i) % TrendState::kHours;
        sum += ts.hourlyDeltas[idx];
    }
    return sum / static_cast<float>(samples);
}

static void trendRecentMinMax(const TrendState& ts,
                              float& minValue,
                              float& maxValue) {
    if (ts.count == 0) {
        minValue = NAN;
        maxValue = NAN;
        return;
    }

    uint8_t oldestIdx = (ts.headIdx + TrendState::kHours - ts.count) % TrendState::kHours;
    minValue = ts.hourlyDeltas[oldestIdx];
    maxValue = minValue;
    for (uint8_t i = 1; i < ts.count; ++i) {
        uint8_t idx = (oldestIdx + i) % TrendState::kHours;
        float value = ts.hourlyDeltas[idx];
        if (value < minValue) {
            minValue = value;
        }
        if (value > maxValue) {
            maxValue = value;
        }
    }
}

static const char* classifyTrendPace(float lastRate, float baselineRate) {
    if (lastRate > 0.15f) {
        return "wetting";
    }
    if (baselineRate >= -0.05f) {
        return "learning";
    }

    float factor = fabsf(lastRate) / fabsf(baselineRate);
    if (factor >= 2.0f) {
        return "dry_fast";
    }
    if (factor >= 1.35f) {
        return "dry_up";
    }
    if (factor <= 0.70f) {
        return "dry_slow";
    }
    return "dry_ok";
}

static const char* telegramWaterLevelStateShort(WaterLevelState state) {
    switch (state) {
        case WaterLevelState::OK: return "ok";
        case WaterLevelState::TRIGGERED: return "trig";
        case WaterLevelState::UNKNOWN: return "unk";
    }
    return "?";
}

static const char* telegramCyclePhaseShort(WateringPhase phase) {
    switch (phase) {
        case WateringPhase::IDLE: return "IDLE";
        case WateringPhase::EVALUATING: return "EVAL";
        case WateringPhase::PULSE: return "PULSE";
        case WateringPhase::STOPPING: return "STOP";
        case WateringPhase::SOAK: return "SOAK";
        case WateringPhase::MEASURING: return "MEAS";
        case WateringPhase::OVERFLOW_WAIT: return "OFLOW";
        case WateringPhase::DONE: return "DONE";
        case WateringPhase::BLOCKED: return "BLOCK";
    }
    return "?";
}

static const char* telegramTrendArrow(float rate) {
    if (isnan(rate)) {
        return "·";
    }
    if (rate > 0.15f) {
        return "↑";
    }
    if (rate > -0.05f) {
        return "→";
    }
    if (rate > -0.45f) {
        return "↘";
    }
    return "↓";
}

static const char* telegramTrendClassLabel(const char* pace) {
    if (!pace) {
        return "learning";
    }
    if (strcmp(pace, "wetting") == 0) {
        return "wetting";
    }
    if (strcmp(pace, "dry_fast") == 0) {
        return "dry fast";
    }
    if (strcmp(pace, "dry_up") == 0) {
        return "drying up";
    }
    if (strcmp(pace, "dry_slow") == 0) {
        return "dry slow";
    }
    if (strcmp(pace, "dry_ok") == 0) {
        return "dry ok";
    }
    return pace;
}

static const char* telegramDuskPhasePretty(DuskPhase phase) {
    switch (phase) {
        case DuskPhase::NIGHT: return "NIGHT";
        case DuskPhase::DAWN_TRANSITION: return "DAWN";
        case DuskPhase::DAY: return "DAY";
        case DuskPhase::DUSK_TRANSITION: return "DUSK";
    }
    return "?";
}

static void formatDurationCompact(uint32_t durationMs, char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }

    uint32_t totalMin = durationMs / 60000UL;
    uint32_t days = totalMin / 1440UL;
    uint32_t hours = (totalMin % 1440UL) / 60UL;
    uint32_t mins = totalMin % 60UL;

    if (days > 0) {
        snprintf(buf, bufSize, "%lud %luh %lum",
                 static_cast<unsigned long>(days),
                 static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(mins));
        return;
    }
    if (hours > 0) {
        snprintf(buf, bufSize, "%luh %lum",
                 static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(mins));
        return;
    }
    snprintf(buf, bufSize, "%lum", static_cast<unsigned long>(mins));
}

static void formatDaysRemainingShort(float daysRemaining, char* buf, size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }
    if (isnan(daysRemaining) || daysRemaining >= 900.0f) {
        safeCopy(buf, bufSize, "learn");
        return;
    }
    snprintf(buf, bufSize, "%.1fd", daysRemaining);
}

static void formatPotActionLine(const TelegramStatusData& status,
                                uint8_t potIdx,
                                char* buf,
                                size_t bufSize) {
    if (!buf || bufSize == 0) {
        return;
    }
    buf[0] = '\0';

    char reason[96] = {};
    uint32_t cooldownRemainingMs = 0;
    bool blocked = telegramWaterBlocked(status, potIdx, reason, sizeof(reason), &cooldownRemainingMs);
    if (!blocked) {
        snprintf(buf, bufSize, "ready / pulse %u ml",
                 static_cast<unsigned>(status.config.pots[potIdx].pulseWaterMl));
        return;
    }

    if (status.pumpStop[potIdx].faultState == PumpStopFaultState::STOP_FAILED_LATCHED) {
        safeCopy(buf, bufSize, "pump stop fault");
    } else if (status.cycles[potIdx].phase == WateringPhase::STOPPING) {
        safeCopy(buf, bufSize, "stopping pump");
    } else if (status.cycles[potIdx].phase != WateringPhase::IDLE) {
        safeCopy(buf, bufSize, "watering active");
    } else if (cooldownRemainingMs > 0) {
        snprintf(buf, bufSize, "cooldown / %lus left",
                 static_cast<unsigned long>((cooldownRemainingMs + 999) / 1000));
    } else if (strstr(reason, "already wet") != nullptr || strstr(reason, "above max") != nullptr) {
        safeCopy(buf, bufSize, "wet enough");
    } else if (strstr(reason, "AUTO mode") != nullptr) {
        safeCopy(buf, bufSize, "AUTO required");
    } else if (strstr(reason, "overflow") != nullptr || strstr(reason, "sensor") != nullptr) {
        safeCopy(buf, bufSize, "guard blocked");
    } else if (strstr(reason, "reservoir empty") != nullptr) {
        safeCopy(buf, bufSize, "reservoir empty");
    } else if (strstr(reason, "pump") != nullptr) {
        safeCopy(buf, bufSize, "pump config invalid");
    } else {
        safeCopy(buf, bufSize, "blocked");
    }
}

static void formatTelegramProfilesMessage(const TelegramStatusData& data,
                                          uint8_t activePot,
                                          char* buf,
                                          size_t bufSize) {
    activePot = telegramClampProfilePot(data, activePot);
    uint8_t profileIdx = data.config.pots[activePot].plantProfileIndex;
    if (profileIdx >= kNumProfiles) {
        profileIdx = 0;
    }

    const PlantProfile& current = getActiveProfile(data.config, activePot);
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "▣ PROFILES\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    pos = appendFmt(buf, bufSize, pos, "🪴 POT %u%s\n",
                    static_cast<unsigned>(activePot + 1),
                    activePot == data.selectedPot ? " [local]" : "");
    pos = appendFmt(buf, bufSize, pos, "🌿 NOW %s%s%s\n",
                    current.icon ? current.icon : "",
                    (current.icon && current.icon[0] != '\0') ? " " : "",
                    current.name ? current.name : "?");

    if (profileIdx == (kNumProfiles - 1)) {
        pos = appendFmt(buf, bufSize, pos,
                        "TARGET %.0f%% | MAX %.0f%% | HYST %.1f%%\n",
                        data.config.pots[activePot].customTargetPct,
                        data.config.pots[activePot].customMaxMoisturePct,
                        data.config.pots[activePot].customHysteresisPct);
        pos = appendFmt(buf, bufSize, pos,
                        "PULSE %uml | SOAK %lus | MAX %u\n",
                        static_cast<unsigned>(data.config.pots[activePot].pulseWaterMl),
                        static_cast<unsigned>(data.config.pots[activePot].customSoakTimeMs / 1000UL),
                        static_cast<unsigned>(data.config.pots[activePot].customMaxPulsesPerCycle));
    } else {
        pos = appendFmt(buf, bufSize, pos,
                        "TARGET %.0f%% | MAX %.0f%% | HYST %.1f%%\n",
                        current.targetMoisturePct,
                        current.maxMoisturePct,
                        current.hysteresisPct);
        pos = appendFmt(buf, bufSize, pos,
                        "PULSE %uml | SOAK %lus | MAX %u\n",
                        static_cast<unsigned>(data.config.pots[activePot].pulseWaterMl),
                        static_cast<unsigned>(current.soakTimeMs / 1000UL),
                        static_cast<unsigned>(current.maxPulsesPerCycle));
    }

    pos = appendFmt(buf, bufSize, pos, "\nChoose a profile below.");
}

static void buildTelegramProfilesKeyboard(const TelegramStatusData& status,
                                          uint8_t activePot,
                                          char* buf,
                                          size_t bufSize) {
    activePot = telegramClampProfilePot(status, activePot);

    char labels[kNumProfiles][40] = {};
    for (uint8_t i = 0; i < kNumProfiles; ++i) {
        const bool selected = status.config.pots[activePot].plantProfileIndex == i;
        snprintf(labels[i],
                 sizeof(labels[i]),
                 "%s%s%s%s",
                 selected ? "* " : "",
                 kProfiles[i].icon ? kProfiles[i].icon : "",
                 (kProfiles[i].icon && kProfiles[i].icon[0] != '\0') ? " " : "",
                 kProfiles[i].name ? kProfiles[i].name : "?");
    }

    int pos = 0;
    if (status.config.numPots > 1) {
        pos = appendFmt(buf, bufSize, pos,
                        "[[{\"text\":\"%sPot 1\",\"callback_data\":\"ag:profiles:pot:0\"},"
                        "{\"text\":\"%sPot 2\",\"callback_data\":\"ag:profiles:pot:1\"}],",
                        activePot == 0 ? "* " : "",
                        activePot == 1 ? "* " : "");
    } else {
        pos = appendFmt(buf, bufSize, pos, "[");
    }

    pos = appendFmt(buf, bufSize, pos,
                    "[{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:0\"},"
                    "{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:1\"}],"
                    "[{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:2\"},"
                    "{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:3\"}],"
                    "[{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:4\"},"
                    "{\"text\":\"%s\",\"callback_data\":\"ag:profiles:set:%u:5\"}],"
                    "[{\"text\":\"\xF0\x9F\x8F\xA0 Menu\",\"callback_data\":\"ag:menu\"}]]",
                    labels[0], static_cast<unsigned>(activePot),
                    labels[1], static_cast<unsigned>(activePot),
                    labels[2], static_cast<unsigned>(activePot),
                    labels[3], static_cast<unsigned>(activePot),
                    labels[4], static_cast<unsigned>(activePot),
                    labels[5], static_cast<unsigned>(activePot));
}

static void formatTelegramRefillMessage(const TelegramStatusData& data,
                                        uint16_t draftCapacityMl,
                                        char* buf,
                                        size_t bufSize) {
    draftCapacityMl = telegramClampReservoirCapacityMl(data, draftCapacityMl);
    uint16_t currentCapacityMl = telegramCurrentReservoirCapacityMl(data);
    int32_t deltaMl = static_cast<int32_t>(draftCapacityMl) - static_cast<int32_t>(currentCapacityMl);
    char daysBuf[16] = {};
    formatDaysRemainingShort(data.budget.daysRemaining, daysBuf, sizeof(daysBuf));

    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "▣ RESERVOIR\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    pos = appendFmt(buf, bufSize, pos, "🪣 NOW %.0f / %.0f ml%s\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.reservoirCapacityMl,
                    data.budget.reservoirLow ? " LOW" : "");
    pos = appendFmt(buf, bufSize, pos, "⏳ LEFT %s\n", daysBuf);
    pos = appendFmt(buf, bufSize, pos, "⚙ DRAFT %u ml",
                    static_cast<unsigned>(draftCapacityMl));
    if (deltaMl != 0) {
        pos = appendFmt(buf, bufSize, pos, " (%+ld)", static_cast<long>(deltaMl));
    }
    pos = appendFmt(buf, bufSize, pos, "\n");
    pos = appendFmt(buf, bufSize, pos, "🧯 LOW %.0f ml\n",
                    data.config.reservoirLowThresholdMl);
    if (data.budget.reservoirCurrentMl > draftCapacityMl) {
        pos = appendFmt(buf, bufSize, pos,
                        "WARN  Apply clamps current fill to %u ml.\n",
                        static_cast<unsigned>(draftCapacityMl));
    }
    pos = appendFmt(buf, bufSize, pos,
                    "APPLY sets capacity and marks tank full.\n"
                    "MENU cancels without changes.");
}

static void buildTelegramRefillKeyboard(char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos,
                    "[[{\"text\":\"-250 ml\",\"callback_data\":\"ag:refill:dec250\"},"
                    "{\"text\":\"+250 ml\",\"callback_data\":\"ag:refill:inc250\"}],"
                    "[{\"text\":\"-1000 ml\",\"callback_data\":\"ag:refill:dec1000\"},"
                    "{\"text\":\"+1000 ml\",\"callback_data\":\"ag:refill:inc1000\"}],"
                    "[{\"text\":\"\xF0\x9F\xAA\xA3 Apply refill\",\"callback_data\":\"ag:refill:apply\"},"
                    "{\"text\":\"\xF0\x9F\x8F\xA0 Menu\",\"callback_data\":\"ag:menu\"}]]");
}

static void buildTelegramPanelKeyboard(const TelegramStatusData& status,
                                       TelegramPanelView panelView,
                                       uint8_t activePot,
                                       char* buf,
                                       size_t bufSize) {
    if (panelView == TelegramPanelView::PROFILES) {
        buildTelegramProfilesKeyboard(status, activePot, buf, bufSize);
        return;
    }
    if (panelView == TelegramPanelView::REFILL) {
        buildTelegramRefillKeyboard(buf, bufSize);
        return;
    }
    buildTelegramInlineMenuKeyboard(status, buf, bufSize);
}

static bool telegramSendAgMenu(const String& chatId,
                               const TelegramStatusData& status,
                               const NetConfig& netCfg,
                               int messageId = 0) {
    formatTelegramMenuSummary(status, g_tgReplyBuf, sizeof(g_tgReplyBuf));
    buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
    return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf,
                                            netCfg, messageId);
}

static void formatTelegramHelpMessage(char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "▣ HELP\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    if (telegramConfiguredBotName()[0] != '\0') {
        pos = appendFmt(buf, bufSize, pos, "🤖 BOT @%s\n", telegramConfiguredBotName());
    }
    pos = appendFmt(buf, bufSize, pos,
                    "\nENTRY\n"
                    "/ag -> main panel\n"
                    "\nCALIBRATION\n"
                    "/agraw <pot> <dry> <wet>\n"
                    "/agdry <pot> <dry>\n"
                    "/agwet <pot> <wet>\n"
                    "/agexp <pot> <exp>\n"
                    "\nUSE\n"
                    "- open /ag\n"
                    "- use inline buttons for actions\n"
                    "- status/history/profiles stay in panel\n"
                    "\nSAFETY\n"
                    "- Water runs one safe pulse\n"
                    "- AUTO mode required for remote water\n"
                    "- Stop forces all pumps OFF\n");
}

static void formatTelegramHistoryReport(const TelegramStatusData& data,
                                        char* buf, size_t bufSize) {
    int pos = 0;
    char daysBuf[16] = {};
    char uptimeBuf[24] = {};
    formatDaysRemainingShort(data.budget.daysRemaining, daysBuf, sizeof(daysBuf));
    formatDurationCompact(data.uptimeMs, uptimeBuf, sizeof(uptimeBuf));

    pos = appendFmt(buf, bufSize, pos, "▣ HISTORY & TRENDS\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    pos = appendFmt(buf, bufSize, pos,
                    "🪣 %.0f / %.0f ml | total %.1f ml\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.reservoirCapacityMl,
                    data.budget.totalPumpedMl);
    pos = appendFmt(buf, bufSize, pos, "⏱ %s | left %s%s\n",
                    uptimeBuf,
                    daysBuf,
                    data.budget.reservoirLow ? " | LOW" : "");

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const TrendState& ts = data.trends[i];
        const PlantProfile& prof = getActiveProfile(data.config, i);
        char sinceBuf[24] = {};
        formatElapsedShort(data.uptimeMs, data.lastPumpActivityMs[i], sinceBuf, sizeof(sinceBuf));

        pos = appendFmt(buf, bufSize, pos, "\n🪴 POT %u | %s\n",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?");
        pos = appendFmt(buf, bufSize, pos,
                        "  water %.1f%% | ema %.1f%% | pumped %.1f ml\n",
                        data.sensors.pots[i].moisturePct,
                        data.sensors.pots[i].moistureEma,
                        data.budget.totalPumpedMlPerPot[i]);
        pos = appendFmt(buf, bufSize, pos, "  last pump %s\n", sinceBuf);

        if (ts.count > 0) {
            float last1h = trendRecentAverage(ts, 1);
            float avg3h = trendRecentAverage(ts, 3);
            float avg6h = trendRecentAverage(ts, 6);
            float min24h = NAN;
            float max24h = NAN;
            trendRecentMinMax(ts, min24h, max24h);

            pos = appendFmt(buf, bufSize, pos,
                            "  1h %s %.2f | 3h %s %.2f | 6h %s %.2f %%/h\n",
                            telegramTrendArrow(last1h),
                            last1h,
                            telegramTrendArrow(avg3h),
                            avg3h,
                            telegramTrendArrow(avg6h),
                            avg6h);

            if (ts.baselineCalibrated && !isnan(ts.normalDryingRate)) {
                float factor = (fabsf(ts.normalDryingRate) > 0.05f)
                    ? (fabsf(last1h) / fabsf(ts.normalDryingRate))
                    : 0.0f;
                pos = appendFmt(buf, bufSize, pos,
                                "  base %.2f | 24h %.2f..%.2f | %s x%.1f\n",
                                ts.normalDryingRate,
                                min24h,
                                max24h,
                                telegramTrendClassLabel(classifyTrendPace(last1h, ts.normalDryingRate)),
                                factor);
            } else {
                pos = appendFmt(buf, bufSize, pos,
                                "  24h %.2f..%.2f | learning baseline (%u/24)\n",
                                min24h,
                                max24h,
                                static_cast<unsigned>(ts.count));
            }
        } else {
            pos = appendFmt(buf, bufSize, pos, "  trend collecting first samples\n");
        }
    }
}

void formatTelegramStatusReport(const TelegramStatusData& data, char* buf, size_t bufSize) {
    int pos = 0;
    char uptimeBuf[24] = {};
    char daysBuf[16] = {};
    formatDurationCompact(data.uptimeMs, uptimeBuf, sizeof(uptimeBuf));
    formatDaysRemainingShort(data.budget.daysRemaining, daysBuf, sizeof(daysBuf));

    pos = appendFmt(buf, bufSize, pos, "▣ STATUS\n");
    pos = appendFmt(buf, bufSize, pos, "━━━━━━━━━━━━━━━━━━━━\n");
    pos = appendFmt(buf, bufSize, pos, "🧠 %s | 🏖 %s | 📶 %s | AP %s\n",
                    modeStr(data.config.mode),
                    data.config.vacationMode ? "ON" : "OFF",
                    data.wifiConnected ? "UP" : "DOWN",
                    data.apActive ? "ON" : "OFF");
    pos = appendFmt(buf, bufSize, pos, "⏱ %s | 🌗 %s\n",
                    uptimeBuf,
                    telegramDuskPhasePretty(data.duskPhase));
    pos = appendFmt(buf, bufSize, pos, "🪣 %.0f / %.0f ml | left %s%s\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.reservoirCapacityMl,
                    daysBuf,
                    data.budget.reservoirLow ? " LOW" : "");
    pos = appendFmt(buf, bufSize, pos, "🌡 %.1fC | 💧 %.0f%% | ☀ %.0f lx | 🧭 %.1f hPa\n",
                    data.sensors.env.tempC,
                    data.sensors.env.humidityPct,
                    data.sensors.env.lux,
                    data.sensors.env.pressureHpa);

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PlantProfile& prof = getActiveProfile(data.config, i);
        float targetPct = prof.targetMoisturePct;
        char actionLine[96] = {};
        char lastState[64] = {};
        char sinceBuf[24] = {};
        formatElapsedShort(data.uptimeMs, data.lastPumpActivityMs[i], sinceBuf, sizeof(sinceBuf));
        formatPotActionLine(data, i, actionLine, sizeof(actionLine));
        formatLastFeedback(data, i, lastState, sizeof(lastState));
        if (data.config.vacationMode) {
            targetPct -= data.config.vacationTargetReductionPct;
            if (targetPct < 5.0f) targetPct = 5.0f;
        }
        float last1h = trendRecentAverage(data.trends[i], 1);
        const char* phaseShort = (data.pumpStop[i].pending && data.cycles[i].phase == WateringPhase::IDLE)
            ? "STOP"
            : telegramCyclePhaseShort(data.cycles[i].phase);
        pos = appendFmt(buf, bufSize, pos,
                "\n🪴 POT %u | %s%s\n"
                        "  water %.1f%% | target %.1f%% | ema %.1f%%\n"
                        "  trend %s %.2f%%/h | raw %u | exp %.2f\n"
                        "  dry/wet %u/%u | phase %s | pulses %u\n"
                        "  action %s\n"
                    "  ovf %s | res %s | pump %s (%s)\n",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?",
                        (i == data.selectedPot) ? " [selected]" : "",
                        data.sensors.pots[i].moisturePct,
                        targetPct,
                        data.sensors.pots[i].moistureEma,
                        telegramTrendArrow(last1h),
                        isnan(last1h) ? 0.0f : last1h,
                        data.sensors.pots[i].moistureRawFiltered,
                        data.config.pots[i].moistureCurveExponent,
                        data.config.pots[i].moistureDryRaw,
                        data.config.pots[i].moistureWetRaw,
                        phaseShort,
                        data.cycles[i].pulseCount,
                        actionLine,
                        telegramWaterLevelStateShort(data.sensors.pots[i].waterGuards.potMax),
                        telegramWaterLevelStateShort(data.sensors.pots[i].waterGuards.reservoirMin),
                        sinceBuf,
                        lastState);
    }
}

static bool pushEvent(const Event& evt) {
    if (!g_eventQueue.push(evt, 0)) {
        Serial.println("[TG] event=queue_push_fail");
        return false;
    }
    return true;
}

static bool pushConfigEvent(EventType type, uint8_t key, uint16_t valueU16, float valueF = 0.0f) {
    Event evt{};
    evt.type = type;
    evt.payload.config.key = key;
    evt.payload.config.valueU16 = valueU16;
    evt.payload.config.valueF = valueF;
    return pushEvent(evt);
}

static bool anyWateringActive(const TelegramStatusData& status) {
    for (uint8_t i = 0; i < status.config.numPots; ++i) {
        if (status.cycles[i].phase != WateringPhase::IDLE || status.pumpStop[i].pending) {
            return true;
        }
    }
    return false;
}

static bool parseUIntArg(const String& token, uint8_t& out) {
    if (token.length() == 0) return false;
    for (size_t i = 0; i < token.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(token.charAt(i)))) {
            return false;
        }
    }
    int v = token.toInt();
    if (v < 0 || v > 255) return false;
    out = static_cast<uint8_t>(v);
    return true;
}

static bool parseUInt16Arg(const String& token, uint16_t& out) {
    if (token.length() == 0) return false;
    for (size_t i = 0; i < token.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(token.charAt(i)))) {
            return false;
        }
    }
    unsigned long v = token.toInt();
    if (v > 65535UL) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

static bool parseFloatArg(const String& token, float& out) {
    if (token.length() == 0) return false;
    char buf[24];
    size_t len = token.length();
    if (len >= sizeof(buf)) return false;
    bool seenDigit = false;
    for (size_t i = 0; i < len; ++i) {
        char c = token.charAt(i);
        if (c >= '0' && c <= '9') {
            seenDigit = true;
            buf[i] = c;
            continue;
        }
        if (c == '.' || (c == '-' && i == 0)) {
            buf[i] = c;
            continue;
        }
        return false;
    }
    if (!seenDigit) return false;
    buf[len] = '\0';
    char* endPtr = nullptr;
    out = strtof(buf, &endPtr);
    return endPtr == (buf + len);
}

static bool resolvePotArgument(const String& token, const TelegramStatusData& status, uint8_t& potIdx) {
    if (token.length() == 0) {
        potIdx = (status.selectedPot < status.config.numPots) ? status.selectedPot : 0;
        return true;
    }
    uint8_t oneBased = 0;
    if (!parseUIntArg(token, oneBased) || oneBased < 1 || oneBased > status.config.numPots) {
        return false;
    }
    potIdx = static_cast<uint8_t>(oneBased - 1);
    return true;
}

static bool handleTelegramCallback(const telegramMessage& message,
                                   const TelegramStatusData& status,
                                   const NetConfig& netCfg) {
    const String chatId = message.chat_id;
    const String data = message.text;
    const int messageId = message.message_id;
    TelegramStatusData replyStatus = status;

    if (message.query_id.length() > 0) {
        if (!g_tgBot->answerCallbackQuery(message.query_id, "")) {
            Serial.printf("[TG] event=callback_ack_fail message_id=%d\n", messageId);
        }
    }

    if (data == "ag:menu" || data == "ag:status") {
        if (data == "ag:menu") {
            return telegramSendAgMenu(chatId, status, netCfg, messageId);
        }
        formatTelegramStatusReport(status, g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:history") {
        formatTelegramHistoryReport(status, g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:profiles") {
        uint8_t activePot = telegramClampProfilePot(status, status.selectedPot);
        formatTelegramProfilesMessage(status, activePot, g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramProfilesKeyboard(status, activePot, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::PROFILES,
                                                activePot);
    }

    if (data.startsWith("ag:profiles:pot:")) {
        String potTok = data.substring(String("ag:profiles:pot:").length());
        uint8_t activePot = 0;
        if (!parseUIntArg(potTok, activePot) || activePot >= status.config.numPots) {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Invalid pot in profiles.");
            buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
            return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
        }

        formatTelegramProfilesMessage(status, activePot, g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramProfilesKeyboard(status, activePot, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::PROFILES,
                                                activePot);
    }

    if (data.startsWith("ag:profiles:set:")) {
        const size_t prefixLen = String("ag:profiles:set:").length();
        int sep = data.indexOf(':', static_cast<int>(prefixLen));
        uint8_t activePot = 0;
        uint8_t profileIdx = 0;
        bool valid = false;

        if (sep > 0) {
            String potTok = data.substring(prefixLen, sep);
            String profileTok = data.substring(sep + 1);
            valid = parseUIntArg(potTok, activePot)
                && parseUIntArg(profileTok, profileIdx)
                && activePot < status.config.numPots
                && profileIdx < kNumProfiles;
        }

        if (!valid) {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Invalid profile selection.");
            buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
            return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
        }

        bool ok = pushConfigEvent(EventType::REQUEST_SET_PLANT, activePot, profileIdx);
        if (ok) {
            replyStatus.config.pots[activePot].plantProfileIndex = profileIdx;
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                     "Pot %u profile requested: %s%s%s.",
                     static_cast<unsigned>(activePot + 1),
                     kProfiles[profileIdx].icon ? kProfiles[profileIdx].icon : "",
                     (kProfiles[profileIdx].icon && kProfiles[profileIdx].icon[0] != '\0') ? " " : "",
                     kProfiles[profileIdx].name ? kProfiles[profileIdx].name : "?");
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Profile queue failed.");
        }

        buildTelegramProfilesKeyboard(replyStatus, activePot, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::PROFILES,
                                                activePot);
    }

    if (data == "ag:help") {
        formatTelegramHelpMessage(g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:stop") {
        bool ok = pushEvent(Event{EventType::REQUEST_PUMP_OFF});
        bool anyActive = anyWateringActive(status);
        if (ok) {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                     anyActive
                         ? "Stop requested. Active watering is being forced OFF."
                         : "Stop requested, but nothing is active right now.");
            for (uint8_t i = 0; i < replyStatus.config.numPots; ++i) {
                if (replyStatus.cycles[i].phase != WateringPhase::IDLE || replyStatus.currentPumpOwner[i] != PumpOwner::NONE) {
                    replyStatus.cycles[i].phase = WateringPhase::STOPPING;
                    replyStatus.cycles[i].phaseStartMs = replyStatus.uptimeMs;
                    replyStatus.pumpStop[i].pending = true;
                    replyStatus.pumpStop[i].faultState = PumpStopFaultState::STOP_PENDING;
                    replyStatus.pumpStop[i].reason = PumpStopReason::REMOTE_STOP;
                }
            }
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Stop queue failed.");
        }
        buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:refill") {
        g_tgActiveRefillCapacityMl = telegramCurrentReservoirCapacityMl(status);
        formatTelegramRefillMessage(status,
                                    g_tgActiveRefillCapacityMl,
                                    g_tgReplyBuf,
                                    sizeof(g_tgReplyBuf));
        buildTelegramRefillKeyboard(g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::REFILL,
                                                0);
    }

    if (data == "ag:refill:dec250" || data == "ag:refill:inc250"
        || data == "ag:refill:dec1000" || data == "ag:refill:inc1000") {
        int32_t draftCapacityMl = (g_tgActiveRefillCapacityMl > 0)
            ? static_cast<int32_t>(g_tgActiveRefillCapacityMl)
            : static_cast<int32_t>(telegramCurrentReservoirCapacityMl(status));
        if (data == "ag:refill:dec250") {
            draftCapacityMl -= 250;
        } else if (data == "ag:refill:inc250") {
            draftCapacityMl += 250;
        } else if (data == "ag:refill:dec1000") {
            draftCapacityMl -= 1000;
        } else {
            draftCapacityMl += 1000;
        }

        g_tgActiveRefillCapacityMl = telegramClampReservoirCapacityMl(status, draftCapacityMl);
        formatTelegramRefillMessage(status,
                                    g_tgActiveRefillCapacityMl,
                                    g_tgReplyBuf,
                                    sizeof(g_tgReplyBuf));
        buildTelegramRefillKeyboard(g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::REFILL,
                                                0);
    }

    if (data == "ag:refill:apply") {
        uint16_t draftCapacityMl = (g_tgActiveRefillCapacityMl > 0)
            ? g_tgActiveRefillCapacityMl
            : telegramCurrentReservoirCapacityMl(status);
        bool capacityChanged = fabsf(status.config.reservoirCapacityMl - draftCapacityMl) >= 0.5f;
        bool capOk = true;
        if (capacityChanged) {
            capOk = pushConfigEvent(EventType::REQUEST_SET_RESERVOIR, 0, 0, draftCapacityMl);
        }
        bool refillOk = capOk && pushEvent(Event{EventType::REQUEST_REFILL});

        if (capOk && refillOk) {
            replyStatus.config.reservoirCapacityMl = draftCapacityMl;
            replyStatus.budget.reservoirCapacityMl = draftCapacityMl;
            replyStatus.budget.reservoirCurrentMl = draftCapacityMl;
            replyStatus.budget.reservoirLow = false;
            formatTelegramMenuSummary(replyStatus, g_tgReplyBuf, sizeof(g_tgReplyBuf));
            buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
            return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
        }

        if (capOk && capacityChanged) {
            replyStatus.config.reservoirCapacityMl = draftCapacityMl;
            replyStatus.budget.reservoirCapacityMl = draftCapacityMl;
            if (replyStatus.budget.reservoirCurrentMl > draftCapacityMl) {
                replyStatus.budget.reservoirCurrentMl = draftCapacityMl;
            }
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                     "Capacity queued: %u ml, but refill queue failed.",
                     static_cast<unsigned>(draftCapacityMl));
        } else if (!capOk) {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Reservoir capacity queue failed.");
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Refill queue failed.");
        }

        buildTelegramRefillKeyboard(g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId,
                                                g_tgReplyBuf,
                                                g_tgKeyboardBuf,
                                                netCfg,
                                                messageId,
                                                1,
                                                150,
                                                TelegramPanelView::REFILL,
                                                0);
    }

    if (data == "ag:wifi") {
        bool ok = pushEvent(Event{EventType::REQUEST_START_WIFI_SETUP});
        if (ok) {
            replyStatus.apActive = true;
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                     status.apActive
                         ? "WiFi portal is already active. Open autogarden / 192.168.4.1."
                         : "WiFi setup requested. AP portal should appear at autogarden / 192.168.4.1.");
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "WiFi setup queue failed.");
        }
        buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:vac:toggle") {
        bool enable = !status.config.vacationMode;
        bool ok = pushConfigEvent(EventType::REQUEST_SET_VACATION, 0, enable ? 1 : 0);
        if (ok) {
            replyStatus.config.vacationMode = enable;
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                     enable
                         ? "Vacation mode requested: ON. Targets and cooldowns are now relaxed."
                         : "Vacation mode requested: OFF. Normal targets and cooldowns restored.");
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Vacation queue failed.");
        }
        buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:mode:toggle") {
        Mode mode = (status.config.mode == Mode::AUTO) ? Mode::MANUAL : Mode::AUTO;
        bool ok = pushConfigEvent(EventType::REQUEST_SET_MODE, 0, static_cast<uint16_t>(mode));
        if (ok) {
            replyStatus.config.mode = mode;
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                     mode == Mode::AUTO
                         ? "Mode change requested: AUTO. Remote water is enabled again."
                         : "Mode change requested: MANUAL. Remote water is now blocked.");
        } else {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Mode queue failed.");
        }
        buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data.startsWith("ag:water:")) {
        String potTok = data.substring(String("ag:water:").length());
        uint8_t potIdx = 0;
        if (!parseUIntArg(potTok, potIdx) || potIdx >= status.config.numPots) {
            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Invalid pot in menu action.");
        } else {
            char preflightReason[96] = {};
            if (telegramWaterBlocked(status, potIdx, preflightReason, sizeof(preflightReason))) {
                safeCopy(g_tgReplyBuf, sizeof(g_tgReplyBuf), preflightReason);
            } else {
                bool ok = pushConfigEvent(EventType::REQUEST_MANUAL_WATER, potIdx, 1);
                if (ok) {
                    replyStatus.cycles[potIdx].phase = WateringPhase::EVALUATING;
                    replyStatus.cycles[potIdx].potIndex = potIdx;
                    replyStatus.cycles[potIdx].phaseStartMs = replyStatus.uptimeMs;
                    snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                             "Water requested for pot %u: one safe pulse %uml. Preflight OK; control task is starting evaluation.",
                             static_cast<unsigned>(potIdx + 1),
                             static_cast<unsigned>(status.config.pots[potIdx].pulseWaterMl),
                             static_cast<unsigned>(potIdx + 1));
                } else {
                    snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Water queue failed.");
                }
            }
        }
        buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    return telegramSendAgMenu(chatId, status, netCfg, messageId);
}

static uint32_t hashIpAddress(const IPAddress& ip) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < 4; ++i) {
        hash ^= static_cast<uint32_t>(ip[i]);
        hash *= 16777619u;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Input sanitization helpers
// ---------------------------------------------------------------------------

// Strip control chars (ASCII < 32 except space), trim, enforce max length.
// Returns sanitized copy.
static String sanitizeInput(const String& raw, size_t maxLen) {
    String out;
    out.reserve(raw.length());
    for (size_t i = 0; i < raw.length() && out.length() < maxLen; ++i) {
        char c = raw.charAt(i);
        if (c >= ' ' || c == '\t') {       // allow printable + tab
            out += c;
        }
        // skip NUL, CR, LF, ESC, and other control chars
    }
    out.trim();
    return out;
}

// Check if string contains only digits (and optional leading '-').
static bool isNumericStr(const String& s) {
    if (s.length() == 0) return false;
    size_t start = (s.charAt(0) == '-') ? 1 : 0;
    if (start >= s.length()) return false;
    for (size_t i = start; i < s.length(); ++i) {
        if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
    }
    return true;
}

static bool isValidChatIdList(const String& s) {
    if (s.length() == 0) return true;
    String normalized = normalizeChatIdList(s, 127);
    return normalized.length() > 0;
}

// Rough Telegram bot token format: digits:alphanumeric-_
static bool isValidBotToken(const String& s) {
    if (s.length() == 0) return true;  // optional field
    int colonPos = s.indexOf(':');
    if (colonPos <= 0 || colonPos >= (int)s.length() - 1) return false;
    // Part before colon must be all digits
    for (int i = 0; i < colonPos; ++i) {
        if (s.charAt(i) < '0' || s.charAt(i) > '9') return false;
    }
    // Part after colon: alphanumeric, -, _
    for (int i = colonPos + 1; i < (int)s.length(); ++i) {
        char c = s.charAt(i);
        if (!isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AP mode handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
    if (g_webServer) {
        g_webServer->send_P(200, "text/html", PROVISIONING_HTML);
    }
}

static void handleScan() {
    if (!g_webServer) return;

    int n = WiFi.scanNetworks();
    // Deduplicate by SSID, keep strongest signal
    String json = "[";
    bool first = true;
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // skip hidden
        // Check if already added (keep strongest)
        bool dup = false;
        for (int j = 0; j < i; ++j) {
            if (WiFi.SSID(j) == ssid) { dup = true; break; }
        }
        if (dup) continue;
        if (!first) json += ",";
        first = false;
        // Proper JSON-escape SSID (backslash, quotes, control chars)
        String escaped;
        escaped.reserve(ssid.length() + 8);
        for (size_t ci = 0; ci < ssid.length(); ++ci) {
            char c = ssid.charAt(ci);
            if (c == '"')       escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else if ((uint8_t)c < 0x20) {
                char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                escaped += buf;
            } else {
                escaped += c;
            }
        }
        json += "{\"ssid\":\"" + escaped + "\","
                "\"rssi\":" + String(WiFi.RSSI(i)) + ","
                "\"enc\":" + String(WiFi.encryptionType(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    g_webServer->send(200, "application/json", json);
}

static void handleSave() {
    if (!g_webServer || !g_apNetCfg) return;

    // --- Sanitize all inputs (strip control chars, enforce max length) ---
    String ssid      = sanitizeInput(g_webServer->arg("ssid"),      32);
    String pass      = sanitizeInput(g_webServer->arg("pass"),      64);
    String botName   = sanitizeInput(g_webServer->arg("bot_name"),  63);
    String botToken  = sanitizeInput(g_webServer->arg("bot_token"), 63);
    String chatIds   = normalizeChatIdList(g_webServer->arg("chat_ids"), 127);

    // --- Validate ---
    if (ssid.length() == 0) {
        g_webServer->send(400, "text/plain", "SSID required");
        return;
    }
    if (ssid.length() > 32) {
        g_webServer->send(400, "text/plain", "SSID too long (max 32)");
        return;
    }
    if (pass.length() > 0 && pass.length() < 8) {
        g_webServer->send(400, "text/plain", "Password min 8 chars (WPA)");
        return;
    }
    if ((botToken.length() > 0 || chatIds.length() > 0 || botName.length() > 0) && botName.length() == 0) {
        g_webServer->send(400, "text/plain", "Bot name required when Telegram is configured");
        return;
    }
    if (chatIds.length() > 0 && !isValidChatIdList(chatIds)) {
        g_webServer->send(400, "text/plain", "Chat IDs must be numeric (comma separated)");
        return;
    }
    if (!isValidBotToken(botToken)) {
        g_webServer->send(400, "text/plain", "Invalid bot token format");
        return;
    }

    // --- Safe copy with guaranteed null termination ---
    memset(g_apNetCfg->wifiSsid,         0, sizeof(g_apNetCfg->wifiSsid));
    memset(g_apNetCfg->wifiPass,         0, sizeof(g_apNetCfg->wifiPass));
        memset(g_apNetCfg->telegramBotName,  0, sizeof(g_apNetCfg->telegramBotName));
    memset(g_apNetCfg->telegramBotToken, 0, sizeof(g_apNetCfg->telegramBotToken));
        memset(g_apNetCfg->telegramChatIds,  0, sizeof(g_apNetCfg->telegramChatIds));

    strncpy(g_apNetCfg->wifiSsid, ssid.c_str(), sizeof(g_apNetCfg->wifiSsid) - 1);
    strncpy(g_apNetCfg->wifiPass, pass.c_str(), sizeof(g_apNetCfg->wifiPass) - 1);
        strncpy(g_apNetCfg->telegramBotName, botName.c_str(), sizeof(g_apNetCfg->telegramBotName) - 1);
    strncpy(g_apNetCfg->telegramBotToken, botToken.c_str(),
            sizeof(g_apNetCfg->telegramBotToken) - 1);
        strncpy(g_apNetCfg->telegramChatIds, chatIds.c_str(),
            sizeof(g_apNetCfg->telegramChatIds) - 1);
    g_apNetCfg->provisioned = true;
        g_apNetCfg->schemaVersion = kNetConfigSchema;

        safeCopy(g_tgConfiguredBotName, sizeof(g_tgConfiguredBotName), g_apNetCfg->telegramBotName);

    netConfigSave(*g_apNetCfg);
        Serial.printf("[PROV] event=config_saved ssid=%s bot=%s chat_targets=%u action=restart\n",
              g_apNetCfg->wifiSsid,
              g_apNetCfg->telegramBotName[0] ? g_apNetCfg->telegramBotName : "-",
              countChatIds(g_apNetCfg->telegramChatIds));

    g_webServer->send_P(200, "text/html", SUCCESS_HTML);
    delay(1000);
    ESP.restart();
}


// ---------------------------------------------------------------------------
// Non-blocking AP mode — startuje AP + captive portal, zwraca od razu.
// apTick() musi być wywoływany co ~100ms w pętli NetTask.
// ---------------------------------------------------------------------------
static DNSServer  s_apDns;
static WebServer  s_apWeb(80);

void startApNonBlocking(NetConfig& netCfg, NetworkState& ns) {
    Serial.println("[AP] event=start mode=non_blocking");

    const bool keepSta = ns.wifiConnected ||
                         (netCfg.provisioned && strlen(netCfg.wifiSsid) > 0);

    WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP("autogarden", "");   // open, no password
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    // DNS captive portal
    g_dnsServer = &s_apDns;
    s_apDns.start(53, "*", IPAddress(192, 168, 4, 1));

    // mDNS
    MDNS.begin("autogarden");

    // Web server
    g_webServer = &s_apWeb;
    g_apNetCfg  = &netCfg;

    s_apWeb.on("/",     handleRoot);
    s_apWeb.on("/scan", handleScan);
    s_apWeb.on("/save", HTTP_POST, handleSave);
    s_apWeb.on("/skip", []() {
        if (g_webServer) {
            g_webServer->send(200, "text/plain", "OK — going offline");
            Serial.println("[AP] event=skip_wifi state=offline");
        }
    });
    // Captive portal detection paths (Android, iOS, Windows, etc.)
    s_apWeb.on("/generate_204",      handleRoot);
    s_apWeb.on("/gen_204",           handleRoot);
    s_apWeb.on("/hotspot-detect.html", handleRoot);
    s_apWeb.on("/library/test/success.html", handleRoot);
    s_apWeb.on("/connecttest.txt",   handleRoot);
    s_apWeb.on("/redirect",          handleRoot);
    s_apWeb.on("/canonical.html",    handleRoot);
    s_apWeb.on("/success.txt",       handleRoot);
    s_apWeb.on("/ncsi.txt",          handleRoot);
    s_apWeb.on("/favicon.ico",       []() {
        if (g_webServer) g_webServer->send(204, "text/plain", " ");
    });
    s_apWeb.onNotFound(handleRoot);
    s_apWeb.begin();

    ns.apActive          = true;
    ns.apStartMs         = millis();
    ns.apNoClientSinceMs = millis();
    ns.provState         = ProvisioningState::AP_MODE;

    Serial.printf("[AP] event=active ssid=autogarden ip=192.168.4.1 sta=%s auto_off_s=%u\n",
                  keepSta ? "kept" : "off",
                  static_cast<unsigned>(NetworkState::kApAutoOffMs / 1000));
}

void apTick(NetworkState& ns) {
    if (!ns.apActive) return;

    s_apDns.processNextRequest();
    s_apWeb.handleClient();

    // Track client presence
    if (WiFi.softAPgetStationNum() > 0) {
        ns.apNoClientSinceMs = millis();
    }

    // Auto-off: 5 min bez klienta
    if ((millis() - ns.apNoClientSinceMs) >= NetworkState::kApAutoOffMs) {
        Serial.println("[AP] event=auto_off reason=no_client_5min");
        stopAp(ns);
    }
}

void stopAp(NetworkState& ns) {
    if (!ns.apActive) return;

    s_apWeb.stop();
    s_apDns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    g_webServer = nullptr;
    g_dnsServer = nullptr;
    g_apNetCfg  = nullptr;

    ns.apActive = false;
    Serial.println("[AP] event=stop state=sta_only");
}

// ---------------------------------------------------------------------------
// netTaskInit — mDNS & Telegram init
// Jeśli WiFi nie jest skonfigurowane, uruchamia AP w trybie non-blocking.
// ---------------------------------------------------------------------------
void netTaskInit(const NetConfig& netCfg, NetworkState& ns) {
    ns.wifiConnected = (WiFi.status() == WL_CONNECTED);
    ns.telegramEnabled = (strlen(netCfg.telegramBotToken) > 0 &&
                          chatIdListHasAny(netCfg.telegramChatIds));

    if (ns.telegramEnabled) {
        Serial.printf("[NET] event=telegram_enabled state=yes bot=%s targets=%u\n",
                      netCfg.telegramBotName[0] ? netCfg.telegramBotName :
                      (telegramConfiguredBotName()[0] ? telegramConfiguredBotName() : "configured"),
                      countChatIds(netCfg.telegramChatIds));
    }

    // Jeśli WiFi nie provisioned → auto-start AP w tle
    if (!netCfg.provisioned || strlen(netCfg.wifiSsid) == 0) {
        Serial.println("[NET] event=wifi_not_provisioned action=start_ap_background");
        // const_cast bo startApNonBlocking potrzebuje writeable NetConfig*
        // dla handleSave — ale g_apNetCfg jest ustawiany globalnie
        extern NetConfig g_netConfig;
        startApNonBlocking(g_netConfig, ns);
    }

    Serial.println("[NET] event=task_initialized");
}

// ---------------------------------------------------------------------------
// netTaskTick — reconnect + AP tick + poll + notifications
// ---------------------------------------------------------------------------
void netTaskTick(uint32_t nowMs, NetworkState& ns, const NetConfig& netCfg) {
    // --- Network status log (change-driven + keepalive) ---
    static uint32_t s_lastNetEvalMs = 0;
    static uint32_t s_lastNetDigest = 0;
    static uint32_t s_lastNetEmitMs = 0;
    static bool s_netDigestInit = false;
    if (nowMs - s_lastNetEvalMs >= 5000) {
        s_lastNetEvalMs = nowMs;
        uint32_t digest = 2166136261u;
        digest = (digest ^ (ns.apActive ? 1u : 0u)) * 16777619u;
        digest = (digest ^ (ns.wifiConnected ? 1u : 0u)) * 16777619u;
        digest = (digest ^ (netCfg.provisioned ? 1u : 0u)) * 16777619u;
        digest = (digest ^ static_cast<uint32_t>(ns.reconnectAttempts)) * 16777619u;
        digest = (digest ^ static_cast<uint32_t>(ns.reconnectBackoffMs / 1000)) * 16777619u;
        digest = (digest ^ static_cast<uint32_t>(WiFi.softAPgetStationNum())) * 16777619u;
        if (ns.wifiConnected) {
            digest = (digest ^ hashIpAddress(WiFi.localIP())) * 16777619u;
        }

        bool changed = (!s_netDigestInit) || (digest != s_lastNetDigest);
        bool keepaliveDue = (s_lastNetEmitMs == 0) || ((nowMs - s_lastNetEmitMs) >= 600000);

        if (changed || keepaliveDue) {
            s_netDigestInit = true;
            s_lastNetDigest = digest;
            s_lastNetEmitMs = nowMs;
            if (ns.apActive) {
                Serial.printf("[NET] event=status ap_active=yes clients=%u\n",
                              WiFi.softAPgetStationNum());
            } else if (ns.wifiConnected) {
                Serial.printf("[NET] event=status wifi=up rssi=%d ip=%s\n",
                              WiFi.RSSI(), WiFi.localIP().toString().c_str());
            } else if (netCfg.provisioned) {
                Serial.printf("[NET] event=status wifi=down reconnect_attempt=%u backoff_s=%u\n",
                              ns.reconnectAttempts, (int)(ns.reconnectBackoffMs / 1000));
            } else {
                Serial.println("[NET] event=status wifi=offline reason=not_provisioned");
            }
        }
    }

    // === AP mode active → obsłuż captive portal ===
    if (ns.apActive) {
        apTick(ns);
        // Gdy działamy w AP+STA, nadal utrzymujemy STA i Telegram.
        // Wyjście tylko w trybie AP-only bez provisioningu.
        if (!netCfg.provisioned || strlen(netCfg.wifiSsid) == 0) {
            return;
        }
    }

    // === Skip if not provisioned (offline mode, AP already timed out) ===
    if (!netCfg.provisioned || strlen(netCfg.wifiSsid) == 0) {
        ns.wifiConnected = false;
        return;  // nothing to do — no credentials
    }

    // === WiFi reconnect with backoff ===
    if (WiFi.status() != WL_CONNECTED) {
        ns.wifiConnected = false;

        if ((nowMs - ns.lastReconnectAttemptMs) >= ns.reconnectBackoffMs) {
            Serial.println("[NET] event=wifi_reconnect_start");
            WiFi.disconnect();
            WiFi.begin(netCfg.wifiSsid, netCfg.wifiPass);

            ns.lastReconnectAttemptMs = nowMs;
            ns.reconnectAttempts++;

            // Exponential backoff: 5s → 10s → 20s → ... → max 5min
            ns.reconnectBackoffMs = ns.reconnectBackoffMs * 2;
            if (ns.reconnectBackoffMs > NetworkState::kMaxBackoffMs) {
                ns.reconnectBackoffMs = NetworkState::kMaxBackoffMs;
            }
        }
        return;  // nic do roboty bez WiFi
    }

    // WiFi OK
    if (!ns.wifiConnected) {
        ns.wifiConnected = true;
        ns.reconnectBackoffMs = 5000;
        ns.reconnectAttempts = 0;
        Serial.printf("[NET] event=wifi_reconnected ip=%s\n",
                      WiFi.localIP().toString().c_str());
        // TODO(events): push WIFI_CONNECTED event
    }

}

// ---------------------------------------------------------------------------
// Telegram — runtime
// ---------------------------------------------------------------------------

void telegramPollCommands(uint32_t nowMs, const NetConfig& netCfg,
                          const TelegramStatusData& status) {
    if (netCfg.telegramBotToken[0] == '\0' || !chatIdListHasAny(netCfg.telegramChatIds)) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    if (!telegramEnsureReady(netCfg)) {
        return;
    }
    if (g_tgLastPollFailMs != 0 && (nowMs - g_tgLastPollFailMs) < kTelegramPollFailCooldownMs) {
        if ((nowMs - g_tgLastPollCooldownLogMs) >= 5000) {
            g_tgLastPollCooldownLogMs = nowMs;
            Serial.printf("[TG] event=poll_cooldown remaining_ms=%lu\n",
                          static_cast<unsigned long>(kTelegramPollFailCooldownMs - (nowMs - g_tgLastPollFailMs)));
        }
        return;
    }

    uint32_t pollIntervalMs = telegramCurrentPollInterval(nowMs);
    if (g_tgLastPollMs != 0 && (nowMs - g_tgLastPollMs) < pollIntervalMs) {
        return;
    }

    ++g_tgStats.pollCycles;
    uint32_t pollStartedMs = millis();

    int numNewMessages = telegramFetchUpdates(netCfg);
    if (numNewMessages < 0) {
        telegramLogStatsIfDue(nowMs);
        return;
    }

    uint16_t fetchedTotal = static_cast<uint16_t>(numNewMessages > 0 ? numNewMessages : 0);
    uint16_t processedTotal = 0;

    if (numNewMessages == 0 && (nowMs - g_tgLastIdleLogMs) >= 60000) {
        g_tgLastIdleLogMs = nowMs;
        Serial.println("[TG] event=poll_idle");
    }

    uint8_t batchCount = 0;
    while (numNewMessages > 0 && batchCount < kTelegramMaxBatchFetches) {
        for (int i = 0; i < numNewMessages; ++i) {
            ++processedTotal;
            const String chatId = g_tgBot->messages[i].chat_id;
            const String fromName = g_tgBot->messages[i].from_name;
            const long updateId = g_tgBot->messages[i].update_id;
            const String msgType = g_tgBot->messages[i].type;
            if (!telegramChatAuthorized(chatId, netCfg)) {
                Serial.printf("[TG] event=unauthorized_chat update=%ld chat_id=%s from=%s ignored=yes\n",
                              updateId, chatId.c_str(), fromName.c_str());
                continue;
            }

            String text = normalizeTelegramCommand(g_tgBot->messages[i].text);
            String lower = text;
            lower.toLowerCase();

            Serial.printf("[TG] event=cmd_rx update=%ld type=%s chat_id=%s from=%s stack_hw=%u text=%s\n",
                          updateId, msgType.c_str(), chatId.c_str(), fromName.c_str(),
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                          text.c_str());

            telegramBumpFastPollWindow(nowMs);

            if (msgType == "callback_query") {
                handleTelegramCallback(g_tgBot->messages[i], status, netCfg);
                continue;
            }

            int firstSpace = lower.indexOf(' ');
            String cmd = (firstSpace >= 0) ? lower.substring(0, firstSpace) : lower;

            g_tgReplyBuf[0] = '\0';

            if (cmd == "/ag" || cmd == "/start") {
                if (chatId.equals(g_tgLastMenuChatId) && (nowMs - g_tgLastMenuMs) < kTelegramMenuDedupMs) {
                    Serial.printf("[TG] event=menu_dedup cmd=%s age_ms=%lu chat_id=%s\n",
                                  cmd.c_str(),
                                  static_cast<unsigned long>(nowMs - g_tgLastMenuMs),
                                  chatId.c_str());
                    continue;
                }
                safeCopy(g_tgLastMenuChatId, sizeof(g_tgLastMenuChatId), chatId.c_str());
                g_tgLastMenuMs = nowMs;
                telegramSendAgMenu(chatId, status, netCfg);
                continue;
            }

            if (cmd == "/agraw" || cmd == "/agdry" || cmd == "/agwet" || cmd == "/agexp") {
                TelegramStatusData replyStatus = status;
                String args = (firstSpace >= 0) ? lower.substring(firstSpace + 1) : "";
                args.trim();

                int sp1 = args.indexOf(' ');
                String potTok = (sp1 >= 0) ? args.substring(0, sp1) : args;
                String rest = (sp1 >= 0) ? args.substring(sp1 + 1) : "";
                rest.trim();

                uint8_t potIdx = 0;
                if (!resolvePotArgument(potTok, status, potIdx)) {
                    snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                             "Invalid pot. Use 1..%u.",
                             static_cast<unsigned>(status.config.numPots));
                    buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
                    telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg);
                    continue;
                }

                bool ok = false;
                if (cmd == "/agraw") {
                    int sp2 = rest.indexOf(' ');
                    String dryTok = (sp2 >= 0) ? rest.substring(0, sp2) : "";
                    String wetTok = (sp2 >= 0) ? rest.substring(sp2 + 1) : "";
                    dryTok.trim();
                    wetTok.trim();
                    uint16_t dryRaw = 0;
                    uint16_t wetRaw = 0;

                    if (!parseUInt16Arg(dryTok, dryRaw) || !parseUInt16Arg(wetTok, wetRaw)) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                                 "Usage: /agraw <pot> <dry> <wet>");
                    } else if (dryRaw > 4095 || wetRaw > 4095 || wetRaw + 31 >= dryRaw) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                                 "Rejected: require 0..4095 and wet < dry by at least 32.");
                    } else {
                        ok = pushConfigEvent(EventType::REQUEST_SET_MOISTURE_DRY_RAW, potIdx, dryRaw)
                          && pushConfigEvent(EventType::REQUEST_SET_MOISTURE_WET_RAW, potIdx, wetRaw);
                        if (ok) {
                            replyStatus.config.pots[potIdx].moistureDryRaw = dryRaw;
                            replyStatus.config.pots[potIdx].moistureWetRaw = wetRaw;
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                     "Pot %u RAWf endpoints queued: dry=%u wet=%u.",
                                     static_cast<unsigned>(potIdx + 1),
                                     static_cast<unsigned>(dryRaw),
                                     static_cast<unsigned>(wetRaw));
                        } else {
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Queue failed for /agraw.");
                        }
                    }
                } else if (cmd == "/agdry" || cmd == "/agwet") {
                    uint16_t value = 0;
                    if (!parseUInt16Arg(rest, value)) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                 "Usage: %s <pot> <value>",
                                 cmd.c_str());
                    } else if (value > 4095) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Rejected: value must be 0..4095.");
                    } else if (cmd == "/agdry") {
                        uint16_t wetRaw = status.config.pots[potIdx].moistureWetRaw;
                        if (value <= wetRaw + 31) {
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                     "Rejected: dry must be at least 32 above wet=%u.",
                                     static_cast<unsigned>(wetRaw));
                        } else {
                            ok = pushConfigEvent(EventType::REQUEST_SET_MOISTURE_DRY_RAW, potIdx, value);
                            if (ok) {
                                replyStatus.config.pots[potIdx].moistureDryRaw = value;
                                snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                         "Pot %u RAWf DRY queued: %u.",
                                         static_cast<unsigned>(potIdx + 1),
                                         static_cast<unsigned>(value));
                            } else {
                                snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Queue failed for /agdry.");
                            }
                        }
                    } else {
                        uint16_t dryRaw = status.config.pots[potIdx].moistureDryRaw;
                        if (value + 31 >= dryRaw) {
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                     "Rejected: wet must be at least 32 below dry=%u.",
                                     static_cast<unsigned>(dryRaw));
                        } else {
                            ok = pushConfigEvent(EventType::REQUEST_SET_MOISTURE_WET_RAW, potIdx, value);
                            if (ok) {
                                replyStatus.config.pots[potIdx].moistureWetRaw = value;
                                snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                         "Pot %u RAWf WET queued: %u.",
                                         static_cast<unsigned>(potIdx + 1),
                                         static_cast<unsigned>(value));
                            } else {
                                snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Queue failed for /agwet.");
                            }
                        }
                    }
                } else {
                    float value = 0.0f;
                    if (!parseFloatArg(rest, value)) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                 "Usage: /agexp <pot> <value>");
                    } else if (value < 0.1f || value > 12.0f) {
                        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                                 "Rejected: exponent must be 0.10..12.00.");
                    } else {
                        ok = pushConfigEvent(EventType::REQUEST_SET_MOISTURE_CURVE_EXPONENT,
                                             potIdx, 0, value);
                        if (ok) {
                            replyStatus.config.pots[potIdx].moistureCurveExponent = value;
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                                     "Pot %u curve exponent queued: %.2f.",
                                     static_cast<unsigned>(potIdx + 1), value);
                        } else {
                            snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Queue failed for /agexp.");
                        }
                    }
                }

                buildTelegramInlineMenuKeyboard(replyStatus, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
                telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg);
                continue;
            }

            Serial.printf("[TG] event=text_without_menu_entry cmd=%s action=show_ag_menu\n",
                          cmd.c_str());
            telegramSendAgMenu(chatId, status, netCfg);
        }

        ++batchCount;
        if ((millis() - pollStartedMs) >= kTelegramPollWorkBudgetMs) {
            ++g_tgStats.pollBudgetBreaks;
            Serial.printf("[TG] event=poll_budget_break batches=%u fetched=%u processed=%u budget_ms=%lu\n",
                          static_cast<unsigned>(batchCount),
                          static_cast<unsigned>(fetchedTotal),
                          static_cast<unsigned>(processedTotal),
                          static_cast<unsigned long>(kTelegramPollWorkBudgetMs));
            break;
        }

        numNewMessages = telegramFetchUpdates(netCfg);
        if (numNewMessages < 0) {
            break;
        }
        if (numNewMessages > 0) {
            fetchedTotal = static_cast<uint16_t>(fetchedTotal + numNewMessages);
        }
    }

    uint32_t pollDurationMs = millis() - pollStartedMs;
    if (fetchedTotal > 0 || pollDurationMs >= kTelegramPollSlowLogMs || batchCount > 1) {
        Serial.printf("[TG] event=poll_cycle fetched=%u processed=%u batches=%u dur_ms=%lu\n",
                      static_cast<unsigned>(fetchedTotal),
                      static_cast<unsigned>(processedTotal),
                      static_cast<unsigned>(batchCount),
                      static_cast<unsigned long>(pollDurationMs));
    }

    // Gating od zakończenia polla, nie od startu — unikamy ciągłego back-to-back
    // przy wolnym łączu i zmniejszamy opóźnienia callbacków powodowane przez zalew requestów.
    g_tgLastPollMs = millis();

    telegramLogStatsIfDue(nowMs);
}

bool telegramSend(const char* msg, const NetConfig& netCfg,
                  uint8_t maxRetries, uint32_t backoffMs)
{
    if (!msg || msg[0] == '\0') {
        return false;
    }
    if (!chatIdListHasAny(netCfg.telegramChatIds) || WiFi.status() != WL_CONNECTED) {
        return false;
    }
    if (!telegramEnsureReady(netCfg)) {
        return false;
    }

    uint32_t waitMs = backoffMs;
    if (waitMs > 250) {
        waitMs = 250;
    }

    bool anyOk = false;
    uint8_t targets = 0;
    forEachChatId(netCfg.telegramChatIds, [&](const char* token) {
        ++targets;
        bool sent = false;
        for (uint8_t attempt = 0; attempt < maxRetries; ++attempt) {
            ++g_tgStats.sendAttempts;
            if (telegramSendToChat(token, msg, netCfg)) {
                Serial.printf("[TG] event=send_ok chat_id=%s len=%u attempt=%u\n",
                              token,
                              static_cast<unsigned>(strlen(msg)),
                              static_cast<unsigned>(attempt + 1));
                sent = true;
                anyOk = true;
                break;
            }
            if (attempt + 1 < maxRetries) {
                ++g_tgStats.sendRetries;
                vTaskDelay(pdMS_TO_TICKS(waitMs));
            }
        }
        if (!sent) {
            Serial.printf("[TG] event=send_fail chat_id=%s len=%u retries=%u\n",
                          token,
                          static_cast<unsigned>(strlen(msg)),
                          static_cast<unsigned>(maxRetries));
        }
    });

    if (targets == 0) {
        Serial.println("[TG] event=send_fail reason=no_targets");
    }
    return anyOk;
}

void formatDailyReport(const DailyReportData& data, char* buf, size_t bufSize) {
    int pos = 0;
    char uptimeBuf[24] = {};
    char daysBuf[16] = {};
    formatDurationCompact(data.uptimeMs, uptimeBuf, sizeof(uptimeBuf));
    formatDaysRemainingShort(data.budget.daysRemaining, daysBuf, sizeof(daysBuf));

    float estimatedDays = NAN;
    if (!isnan(data.budget.daysRemaining) && data.budget.daysRemaining < 900.0f) {
        estimatedDays = data.budget.daysRemaining;
    } else if (data.pumped24hMl > 1.0f) {
        estimatedDays = data.budget.reservoirCurrentMl / data.pumped24hMl;
    }

    const char* title = (data.kind == DailyReportData::Kind::STARTUP_CHECK)
        ? "▣ DAILY REPORT [startup check]\n"
        : "▣ DAILY REPORT\n";

    pos = appendFmt(buf, bufSize, pos,
                    "%s"
                    "━━━━━━━━━━━━━━━━━━━━\n"
                    "🪣 %.0f / %.0f ml%s\n"
                    "📉 24h use %.0f ml / %u cycles | ",
                    title,
                    data.budget.reservoirCurrentMl,
                    data.budget.reservoirCapacityMl,
                    data.budget.reservoirLow ? " | LOW" : "",
                    data.pumped24hMl,
                    static_cast<unsigned>(data.wateringEvents24h));

    if (!isnan(estimatedDays)) {
        pos = appendFmt(buf, bufSize, pos, "est %.1fd\n", estimatedDays);
    } else {
        pos = appendFmt(buf, bufSize, pos, "est learning\n");
    }

    pos = appendFmt(buf, bufSize, pos,
                    "🌗 %s | solar %s | ⏱ %s\n"
                    "🌡 %.1fC | 💧 %.0f%% | ☀ %.0f lx\n",
                    telegramDuskPhasePretty(data.duskPhase),
                    data.solarCalibrated ? "cal" : "learn",
                    uptimeBuf,
                    data.sensors.env.tempC,
                    data.sensors.env.humidityPct,
                    data.sensors.env.lux);

    if (data.kind == DailyReportData::Kind::STARTUP_CHECK) {
        pos = appendFmt(buf, bufSize, pos,
                        "🕘 schedule boot+5m, then dawn+3h\n");
    }

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PotSensorSnapshot& ps = data.sensors.pots[i];
        const PlantProfile& prof = getActiveProfile(data.config, i);
        const TrendState& ts = data.trends[i];
        char sinceBuf[24] = {};
        formatElapsedShort(data.uptimeMs, data.lastPumpActivityMs[i], sinceBuf, sizeof(sinceBuf));
        float targetPct = prof.targetMoisturePct;
        if (data.config.vacationMode) {
            targetPct -= data.config.vacationTargetReductionPct;
            if (targetPct < 5.0f) targetPct = 5.0f;
        }

        pos = appendFmt(buf, bufSize, pos,
                "\n🪴 POT %u | %s\n"
                        "  water %.1f%% | target %.1f%% | ema %.1f%%\n"
                    "  24h water %.0f ml / %u cycles | pump %s\n",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?",
                        ps.moisturePct,
                        targetPct,
                        ps.moistureEma,
                        data.pumped24hMlPerPot[i],
                        static_cast<unsigned>(data.wateringEvents24hPerPot[i]),
                        sinceBuf);

        if (ts.baselineCalibrated && ts.count > 0) {
            uint8_t lastIdx = (ts.headIdx == 0) ? (TrendState::kHours - 1) : (ts.headIdx - 1);
            float avg6h = trendRecentAverage(ts, 6);
            pos = appendFmt(buf, bufSize, pos,
                            "  trend 1h %s %.2f | 6h %s %.2f | base %.2f %%/h\n",
                            telegramTrendArrow(ts.hourlyDeltas[lastIdx]),
                            ts.hourlyDeltas[lastIdx],
                            telegramTrendArrow(avg6h),
                            avg6h,
                            ts.normalDryingRate);
        } else {
            pos = appendFmt(buf, bufSize, pos, "  trend learning baseline\n");
        }
    }

    if (data.config.vacationMode) {
        pos = appendFmt(buf, bufSize, pos, "\nVACATION MODE: ON\n");
    }
}

bool isDailyHeartbeatTime(uint32_t nowMs, const SolarClock& clk,
                          const DuskDetector& det,
                          NetworkState& ns)
{
    static constexpr uint32_t kDailyHeartbeatMinGapMs = 20UL * 3600 * 1000;
    static constexpr uint32_t kDailyHeartbeatOffsetMs = 3UL * 3600 * 1000;
    static constexpr uint32_t kDailyHeartbeatWindowMs = 10UL * 60 * 1000;

    // Debounce: max 1 daily heartbeat per ~day window.
    if (ns.lastHeartbeatMs > 0 &&
        (nowMs - ns.lastHeartbeatMs) < kDailyHeartbeatMinGapMs) {
        return false;
    }

    // Strategy 1: SolarClock (3h after dawn).
    if (clk.calibrated) {
        uint32_t targetDawnMs = 0;
        uint32_t targetMs = 0;

        if ((det.phase == DuskPhase::DAY || det.phase == DuskPhase::DUSK_TRANSITION)
            && det.lastDawnMs > 0) {
            uint32_t todayTargetMs = det.lastDawnMs + kDailyHeartbeatOffsetMs;
            if (nowMs <= (todayTargetMs + kDailyHeartbeatWindowMs)) {
                targetDawnMs = det.lastDawnMs;
                targetMs = todayTargetMs;
            }
        }

        if (targetMs == 0) {
            uint32_t estDawn = estimateNextDawn(det, clk, nowMs);
            if (estDawn > 0) {
                targetDawnMs = estDawn;
                targetMs = estDawn + kDailyHeartbeatOffsetMs;
            }
        }

        if (targetMs > 0) {
            int32_t diff = static_cast<int32_t>(nowMs - targetMs);
            if (abs(diff) <= static_cast<int32_t>(kDailyHeartbeatWindowMs)
                && ns.lastDailyAnchorMs != targetDawnMs) {
                ns.lastDailyAnchorMs = targetDawnMs;
                ns.lastHeartbeatMs = nowMs;
                return true;
            }
            return false;
        }
    }

    // Strategy 2: fallback — every 24h of uptime.
    if (ns.lastHeartbeatMs == 0) {
        if (nowMs >= 24UL * 3600 * 1000) {
            ns.lastHeartbeatMs = nowMs;
            return true;
        }
        return false;
    }
    if ((nowMs - ns.lastHeartbeatMs) >= 24UL * 3600 * 1000) {
        ns.lastHeartbeatMs = nowMs;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Factory reset — sieć only
// ---------------------------------------------------------------------------
void networkFactoryReset() {
    NetConfig empty{};
    netConfigSave(empty);
    Serial.println("[NET] event=factory_reset_done");
}

// ---------------------------------------------------------------------------
// PROVISIONING_HTML — pełna strona z WiFi picker (AJAX scan)
// ---------------------------------------------------------------------------
extern const char PROVISIONING_HTML[] PROGMEM;
extern const char PROVISIONING_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="pl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>autogarden Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:420px;margin:0 auto}
h1{color:#4ecca3;font-size:1.4em;margin-bottom:12px;text-align:center}
.card{background:#16213e;border-radius:12px;padding:16px;margin-bottom:12px;border:1px solid #0f3460}
label{display:block;font-size:.85em;color:#a0a0a0;margin-bottom:4px}
input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid #0f3460;border-radius:8px;background:#1a1a2e;color:#fff;font-size:1em;margin-bottom:10px}
input:focus{border-color:#4ecca3;outline:none}
.wifi-list{max-height:200px;overflow-y:auto;margin-bottom:10px}
.wifi-item{display:flex;justify-content:space-between;align-items:center;padding:10px;border-radius:8px;cursor:pointer;margin-bottom:4px;background:#1a1a2e;border:1px solid transparent;transition:all .2s}
.wifi-item:hover,.wifi-item.selected{border-color:#4ecca3;background:#0f3460}
.wifi-item .ssid{font-weight:600}
.wifi-item .meta{font-size:.75em;color:#888}
.signal{width:20px;height:16px;display:flex;align-items:flex-end;gap:1px}
.signal span{display:block;width:3px;background:#555;border-radius:1px}
.signal.s4 span:nth-child(1),.signal.s3 span:nth-child(1),.signal.s2 span:nth-child(1),.signal.s1 span:nth-child(1){background:#4ecca3}
.signal.s4 span:nth-child(2),.signal.s3 span:nth-child(2),.signal.s2 span:nth-child(2){background:#4ecca3}
.signal.s4 span:nth-child(3),.signal.s3 span:nth-child(3){background:#4ecca3}
.signal.s4 span:nth-child(4){background:#4ecca3}
btn,.btn{display:block;width:100%;padding:12px;border:none;border-radius:8px;font-size:1em;font-weight:600;cursor:pointer;text-align:center;margin-top:8px}
.btn-scan{background:#0f3460;color:#4ecca3}.btn-scan:hover{background:#162d50}
.btn-save{background:#4ecca3;color:#1a1a2e}.btn-save:hover{background:#3dbb91}
.btn-skip{background:transparent;color:#888;border:1px solid #333;margin-top:12px}.btn-skip:hover{color:#fff;border-color:#666}
.status{text-align:center;padding:8px;font-size:.85em;color:#888}
.lock{font-size:.7em;margin-left:4px}
.section-title{font-size:.8em;color:#4ecca3;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}
</style>
</head>
<body>
<h1>&#127793; autogarden</h1>

<form id="f" method="POST" action="/save">
<div class="card">
  <div class="section-title">WiFi</div>
  <div id="wl" class="wifi-list"><div class="status">Scanning...</div></div>
  <button type="button" class="btn btn-scan" onclick="scan()">&#128270; Scan again</button>
  <div style="margin-top:10px">
    <label>SSID</label>
    <input type="text" name="ssid" id="ssid" required maxlength="32" autocomplete="off" placeholder="Select from list or type">
    <label>Password</label>
    <input type="password" name="pass" id="pass" maxlength="64" autocomplete="off" placeholder="WiFi password (min 8 chars)">
  </div>
</div>

<div class="card">
  <div class="section-title">Telegram (optional)</div>
    <label>Bot Name</label>
    <input type="text" name="bot_name" maxlength="63" autocomplete="off" placeholder="smartrozi_bot">
  <label>Bot Token</label>
  <input type="text" name="bot_token" maxlength="63" autocomplete="off" pattern="[0-9]+:[A-Za-z0-9_-]+" placeholder="123456:ABC-DEF...">
    <label>Chat IDs</label>
    <input type="text" name="chat_ids" maxlength="127" autocomplete="off" placeholder="e.g. 5952898918,6371618192">
</div>

<button type="submit" class="btn btn-save">&#128190; Save &amp; Restart</button>
</form>

<button class="btn btn-skip" onclick="if(confirm('Skip WiFi? Device will work offline.'))fetch('/skip').then(()=>document.body.innerHTML='<h1>OK! Restarting...</h1>')">
  Skip WiFi &mdash; work offline
</button>

<script>
function sigClass(r){return r>-50?'s4':r>-65?'s3':r>-75?'s2':'s1'}
function scan(){
  document.getElementById('wl').innerHTML='<div class="status">Scanning...</div>';
  fetch('/scan').then(r=>r.json()).then(list=>{
    if(!list.length){document.getElementById('wl').innerHTML='<div class="status">No networks found</div>';return}
    let h='';
    var wl=document.getElementById('wl');
    wl.innerHTML='';
    list.forEach(w=>{
      let lock=w.enc!=0?'\u{1F512}':'';
      let sc=sigClass(w.rssi);
      let row=document.createElement('div');
      row.className='wifi-item';
      row.addEventListener('click',function(){pickWifi(this,w.ssid)});
      let info=document.createElement('div');
      let sn=document.createElement('div');
      sn.className='ssid';
      sn.textContent=w.ssid+' '+lock;
      info.appendChild(sn);
      let meta=document.createElement('div');
      meta.className='meta';
      meta.textContent=w.rssi+' dBm';
      info.appendChild(meta);
      row.appendChild(info);
      let sig=document.createElement('div');
      sig.className='signal '+sc;
      sig.innerHTML='<span style="height:4px"></span><span style="height:7px"></span><span style="height:11px"></span><span style="height:16px"></span>';
      row.appendChild(sig);
      wl.appendChild(row);
    });
  }).catch(()=>document.getElementById('wl').innerHTML='<div class="status">Scan failed</div>')
}
function pickWifi(el,ssid){
  document.querySelectorAll('.wifi-item').forEach(e=>e.classList.remove('selected'));
  el.classList.add('selected');
  document.getElementById('ssid').value=ssid;
  document.getElementById('pass').focus();
}
scan();
</script>
</body>
</html>
)rawhtml";
