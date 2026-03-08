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
static char g_tgReplyBuf[1024] = {};
static char g_tgKeyboardBuf[768] = {};
static uint32_t g_tgLastIdleLogMs = 0;
static uint32_t g_tgLastPollMs = 0;
static uint32_t g_tgFastPollUntilMs = 0;
static constexpr uint32_t kTelegramPollIntervalIdleMs = 2000;
static constexpr uint32_t kTelegramPollIntervalActiveMs = 400;
static constexpr uint32_t kTelegramFastPollWindowMs = 120000;
static constexpr uint32_t kTelegramRequestTimeoutMs = 8000;
static constexpr uint8_t kTelegramMaxBatchFetches = 10;

static uint8_t countChatIds(const char* chatIds);

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

    WateringPhase phase = status.cycles[potIdx].phase;
    if (phase != WateringPhase::IDLE) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: watering already active.");
        return true;
    }

    const PotConfig& potCfg = status.config.pots[potIdx];
    const PotSensorSnapshot& pot = status.sensors.pots[potIdx];
    if (status.config.antiOverflowEnabled) {
        if (pot.waterGuards.potMax == WaterLevelState::TRIGGERED) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: overflow sensor triggered.");
            return true;
        }
        if (pot.waterGuards.potMax == WaterLevelState::UNKNOWN) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: overflow sensor unknown.");
            return true;
        }
        if (pot.waterGuards.reservoirMin == WaterLevelState::TRIGGERED) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir sensor triggered.");
            return true;
        }
        if (pot.waterGuards.reservoirMin == WaterLevelState::UNKNOWN) {
            safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir sensor unknown.");
            return true;
        }
    }

    if (status.budget.reservoirLow && status.budget.reservoirCurrentMl <= 0.0f) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: reservoir empty.");
        return true;
    }
    if (!potCfg.pumpCalibrated || potCfg.pumpMlPerSec <= 0.0f) {
        safeCopy(reasonBuf, reasonBufSize, "Blocked: pump not calibrated.");
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

    if (status.cycles[potIdx].phase != WateringPhase::IDLE) {
        safeCopy(buf, bufSize, "\xF0\x9F\x92\xA7 Watering...");
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
        g_tgBot->longPoll = 0;
        g_tgBot->waitForResponse = 1500;
        safeCopy(g_tgTokenCache, sizeof(g_tgTokenCache), netCfg.telegramBotToken);
        g_tgLastPollMs = 0;
        g_tgFastPollUntilMs = 0;
        Serial.printf("[TG] event=client_ready poll_idle_ms=%lu poll_active_ms=%lu timeout_ms=%lu\n",
                      static_cast<unsigned long>(kTelegramPollIntervalIdleMs),
                      static_cast<unsigned long>(kTelegramPollIntervalActiveMs),
                      static_cast<unsigned long>(kTelegramRequestTimeoutMs));
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
    int count = g_tgBot->getUpdates(g_tgBot->last_message_received + 1);
    if (count > 0) {
        Serial.printf("[TG] event=updates count=%d last=%ld\n",
                      count, static_cast<long>(g_tgBot->last_message_received));
    }
    return count;
}

static bool telegramReplyToChat(const String& chatId, const String& msg,
                                const NetConfig& netCfg,
                                uint8_t maxRetries = 2,
                                uint32_t retryDelayMs = 150) {
    if (!telegramEnsureReady(netCfg)) {
        Serial.println("[TG] event=reply_fail reason=client_not_ready");
        return false;
    }

    for (uint8_t attempt = 0; attempt < maxRetries; ++attempt) {
        if (g_tgBot->sendMessage(chatId, msg, "")) {
            telegramCloseTransport("after_send_message");
            telegramBumpFastPollWindow(millis());
            Serial.printf("[TG] event=reply_ok len=%u attempt=%u\n",
                          static_cast<unsigned>(msg.length()),
                          static_cast<unsigned>(attempt + 1));
            return true;
        }
        telegramCloseTransport("send_message_fail", attempt == 0);
        Serial.printf("[TG] event=reply_fail len=%u attempt=%u\n",
                      static_cast<unsigned>(msg.length()),
                      static_cast<unsigned>(attempt + 1));
        if (attempt + 1 < maxRetries) {
            vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
        }
    }
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
                                             uint8_t maxRetries = 2,
                                             uint32_t retryDelayMs = 150) {
    if (!telegramEnsureReady(netCfg) || !msg || !keyboardJson) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < maxRetries; ++attempt) {
        int targetMessageId = messageId;
        if (g_tgBot->sendMessageWithInlineKeyboard(chatId, msg, "", keyboardJson, targetMessageId)) {
            telegramCloseTransport("after_send_inline_keyboard");
            telegramBumpFastPollWindow(millis());
            Serial.printf("[TG] event=inline_ok len=%u attempt=%u message_id=%d\n",
                          static_cast<unsigned>(strlen(msg)),
                          static_cast<unsigned>(attempt + 1), targetMessageId);
            return true;
        }
        telegramCloseTransport("send_inline_fail", attempt == 0);
        Serial.printf("[TG] event=inline_fail len=%u attempt=%u message_id=%d\n",
                      static_cast<unsigned>(strlen(msg)),
                      static_cast<unsigned>(attempt + 1), targetMessageId);

        if (targetMessageId != 0) {
            Serial.printf("[TG] event=inline_fallback_new_message old_message_id=%d\n",
                          targetMessageId);
            if (g_tgBot->sendMessageWithInlineKeyboard(chatId, msg, "", keyboardJson, 0)) {
                telegramCloseTransport("after_inline_fallback_new_message");
                telegramBumpFastPollWindow(millis());
                Serial.printf("[TG] event=inline_ok len=%u attempt=%u message_id=0 fallback=yes\n",
                              static_cast<unsigned>(strlen(msg)),
                              static_cast<unsigned>(attempt + 1));
                return true;
            }
            telegramCloseTransport("inline_fallback_new_message_fail", attempt == 0);
            Serial.printf("[TG] event=inline_fallback_fail len=%u attempt=%u old_message_id=%d\n",
                          static_cast<unsigned>(strlen(msg)),
                          static_cast<unsigned>(attempt + 1), targetMessageId);
        }

        if (attempt + 1 < maxRetries) {
            vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
        }
    }
    return false;
}

static void formatTelegramMenuSummary(const TelegramStatusData& data, char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "autogarden menu\n");
    pos = appendFmt(buf, bufSize, pos, "Mode: %s | Vac: %s | WiFi: %s\n",
                    modeStr(data.config.mode),
                    data.config.vacationMode ? "ON" : "OFF",
                    data.wifiConnected ? "UP" : "DOWN");
    pos = appendFmt(buf, bufSize, pos, "Reservoir: %.0fml (~%.1fd)%s\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.daysRemaining,
                    data.budget.reservoirLow ? " LOW" : "");

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PlantProfile& prof = getActiveProfile(data.config, i);
        pos = appendFmt(buf, bufSize, pos, "Pot%u %s: %.0f%%",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?",
                        data.sensors.pots[i].moisturePct);
        if (data.sensors.pots[i].waterGuards.potMax == WaterLevelState::TRIGGERED) {
            pos = appendFmt(buf, bufSize, pos, " overflow");
        }
        pos = appendFmt(buf, bufSize, pos, "\n");
    }

    pos = appendFmt(buf, bufSize, pos, "Use the buttons below.");
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
    pos = appendFmt(buf, bufSize, pos, "autogarden Telegram\n");
    if (telegramConfiguredBotName()[0] != '\0') {
        pos = appendFmt(buf, bufSize, pos, "Bot: @%s\n", telegramConfiguredBotName());
    }
    pos = appendFmt(buf, bufSize, pos,
                    "\nMain entry:\n"
                    "/ag -> interactive menu\n"
                    "\nFlow:\n"
                    "- open /ag\n"
                    "- use inline buttons for all actions\n"
                    "- status/history/profiles are shown from the menu\n"
                    "\nSafety:\n"
                    "- Water triggers one safe pulse\n"
                    "- Water requires AUTO mode\n"
                    "- Stop forces all pumps OFF\n");
}

static void formatTelegramProfilesMessage(char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "Profile indexes:\n");
    for (uint8_t i = 0; i < kNumProfiles; ++i) {
        pos = appendFmt(buf, bufSize, pos, "%u = %s\n", i,
                        kProfiles[i].name ? kProfiles[i].name : "?");
    }
}

static void formatTelegramHistoryReport(const TelegramStatusData& data,
                                        char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "autogarden history summary\n");
    pos = appendFmt(buf, bufSize, pos, "Reservoir: %.0fml / %.0fml\n",
                    data.budget.reservoirCurrentMl, data.budget.reservoirCapacityMl);
    pos = appendFmt(buf, bufSize, pos, "Total pumped: %.1fml\n",
                    data.budget.totalPumpedMl);

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const TrendState& ts = data.trends[i];
        pos = appendFmt(buf, bufSize, pos, "\nPot %u:\n", static_cast<unsigned>(i + 1));
        pos = appendFmt(buf, bufSize, pos, "  pumped=%.1fml\n",
                        data.budget.totalPumpedMlPerPot[i]);
        if (ts.baselineCalibrated && ts.count > 0) {
            uint8_t lastIdx = (ts.headIdx == 0) ? (TrendState::kHours - 1) : (ts.headIdx - 1);
            pos = appendFmt(buf, bufSize, pos,
                            "  trend=%.2f%%/h baseline=%.2f%%/h samples=%u\n",
                            ts.hourlyDeltas[lastIdx], ts.normalDryingRate, ts.count);
        } else {
            pos = appendFmt(buf, bufSize, pos, "  trend=learning baseline\n");
        }
    }
}

void formatTelegramStatusReport(const TelegramStatusData& data, char* buf, size_t bufSize) {
    int pos = 0;
    pos = appendFmt(buf, bufSize, pos, "autogarden status\n");
    pos = appendFmt(buf, bufSize, pos, "Mode: %s | Vacation: %s | WiFi: %s\n",
                    modeStr(data.config.mode),
                    data.config.vacationMode ? "ON" : "OFF",
                    data.wifiConnected ? "UP" : "DOWN");
    pos = appendFmt(buf, bufSize, pos, "Dusk: %s | Uptime: %lu min\n",
                    duskPhaseStr(data.duskPhase),
                    static_cast<unsigned long>(data.uptimeMs / 60000UL));
    pos = appendFmt(buf, bufSize, pos, "Reservoir: %.0fml (~%.1f days)%s\n",
                    data.budget.reservoirCurrentMl,
                    data.budget.daysRemaining,
                    data.budget.reservoirLow ? " LOW" : "");
    pos = appendFmt(buf, bufSize, pos, "Env: %.1fC %.0f%% %.0flux %.1fhPa\n",
                    data.sensors.env.tempC,
                    data.sensors.env.humidityPct,
                    data.sensors.env.lux,
                    data.sensors.env.pressureHpa);

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PlantProfile& prof = getActiveProfile(data.config, i);
        float targetPct = prof.targetMoisturePct;
        char waterState[96] = {};
        telegramWaterBlocked(data, i, waterState, sizeof(waterState));
        if (data.config.vacationMode) {
            targetPct -= data.config.vacationTargetReductionPct;
            if (targetPct < 5.0f) targetPct = 5.0f;
        }
        pos = appendFmt(buf, bufSize, pos,
                        "\nPot %u (%s)%s\n"
                        "  moisture=%.1f%% ema=%.1f%% target=%.1f%%\n"
                        "  raw=%u overflow=%s cycle=%d pulses=%u\n"
                        "  action=%s\n",
                        static_cast<unsigned>(i + 1),
                        prof.name ? prof.name : "?",
                        (i == data.selectedPot) ? " [selected]" : "",
                        data.sensors.pots[i].moisturePct,
                        data.sensors.pots[i].moistureEma,
                        targetPct,
                        data.sensors.pots[i].moistureRaw,
                        data.sensors.pots[i].waterGuards.potMax == WaterLevelState::TRIGGERED ? "TRIG" : "OK",
                        static_cast<int>(data.cycles[i].phase),
                        data.cycles[i].pulseCount,
                        waterState[0] ? waterState : "Ready: one safe pulse");
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
        formatTelegramProfilesMessage(g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:help") {
        formatTelegramHelpMessage(g_tgReplyBuf, sizeof(g_tgReplyBuf));
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:stop") {
        bool ok = pushEvent(Event{EventType::REQUEST_PUMP_OFF});
        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                 ok ? "Stop queued. Pumps will be forced OFF." : "Stop queue failed.");
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:refill") {
        bool ok = pushEvent(Event{EventType::REQUEST_REFILL});
        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                 ok ? "Reservoir refill queued." : "Refill queue failed.");
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:wifi") {
        bool ok = pushEvent(Event{EventType::REQUEST_START_WIFI_SETUP});
        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                 ok ? "WiFi setup queued. AP portal will start if possible." : "WiFi setup queue failed.");
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:vac:toggle") {
        bool enable = !status.config.vacationMode;
        bool ok = pushConfigEvent(EventType::REQUEST_SET_VACATION, 0, enable ? 1 : 0);
        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                 ok ? (enable ? "Vacation ON queued." : "Vacation OFF queued.")
                    : "Vacation queue failed.");
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
        return telegramSendInlineKeyboardToChat(chatId, g_tgReplyBuf, g_tgKeyboardBuf, netCfg, messageId);
    }

    if (data == "ag:mode:toggle") {
        Mode mode = (status.config.mode == Mode::AUTO) ? Mode::MANUAL : Mode::AUTO;
        bool ok = pushConfigEvent(EventType::REQUEST_SET_MODE, 0, static_cast<uint16_t>(mode));
        snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s",
                 ok ? (mode == Mode::AUTO ? "Mode AUTO queued." : "Mode MANUAL queued.")
                    : "Mode queue failed.");
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
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
                    snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf),
                             "Starting one safe pulse: %uml for pot %u.",
                             static_cast<unsigned>(status.config.pots[potIdx].pulseWaterMl),
                             static_cast<unsigned>(potIdx + 1));
                } else {
                    snprintf(g_tgReplyBuf, sizeof(g_tgReplyBuf), "%s", "Water queue failed.");
                }
            }
        }
        buildTelegramInlineMenuKeyboard(status, g_tgKeyboardBuf, sizeof(g_tgKeyboardBuf));
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

    uint32_t pollIntervalMs = telegramCurrentPollInterval(nowMs);
    if (g_tgLastPollMs != 0 && (nowMs - g_tgLastPollMs) < pollIntervalMs) {
        return;
    }
    g_tgLastPollMs = nowMs;

    int numNewMessages = telegramFetchUpdates(netCfg);
    if (numNewMessages == 0 && (nowMs - g_tgLastIdleLogMs) >= 60000) {
        g_tgLastIdleLogMs = nowMs;
        Serial.println("[TG] event=poll_idle");
    }
    uint8_t batchCount = 0;
    while (numNewMessages > 0 && batchCount < kTelegramMaxBatchFetches) {
        for (int i = 0; i < numNewMessages; ++i) {
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
                telegramSendAgMenu(chatId, status, netCfg);
                continue;
            }

            Serial.printf("[TG] event=text_without_menu_entry cmd=%s action=show_ag_menu\n",
                          cmd.c_str());
            telegramSendAgMenu(chatId, status, netCfg);
        }

        telegramCloseTransport("after_update_batch");

        ++batchCount;
        numNewMessages = telegramFetchUpdates(netCfg);
    }

    telegramCloseTransport("after_poll_cycle");
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
    pos += snprintf(buf + pos, bufSize - pos,
                    "autogarden - raport\n"
                    "====================\n");

    for (uint8_t i = 0; i < data.config.numPots; ++i) {
        if (!data.config.pots[i].enabled) continue;
        const PotSensorSnapshot& ps = data.sensors.pots[i];
        const PlantProfile& prof = getActiveProfile(data.config, i);
        const TrendState& ts = data.trends[i];

        pos += snprintf(buf + pos, bufSize - pos,
                        "\nPot %d (%s): %.0f%% (target %.0f%%)\n",
                        i + 1, prof.name, ps.moisturePct, prof.targetMoisturePct);

        if (ts.baselineCalibrated && ts.count > 0) {
            uint8_t lastIdx = (ts.headIdx == 0) ? (TrendState::kHours - 1) : (ts.headIdx - 1);
            pos += snprintf(buf + pos, bufSize - pos,
                            "  Trend: %.1f%%/h (baseline: %.1f%%/h)\n",
                            ts.hourlyDeltas[lastIdx], ts.normalDryingRate);
        }
    }

    pos += snprintf(buf + pos, bufSize - pos,
                    "\nTemp: %.1fC  Lux: %.0f\n",
                    data.sensors.env.tempC, data.sensors.env.lux);

    pos += snprintf(buf + pos, bufSize - pos,
                    "Reservoir: %.0fml (~%.0f days)\n",
                    data.budget.reservoirCurrentMl, data.budget.daysRemaining);

    if (data.config.vacationMode) {
        pos += snprintf(buf + pos, bufSize - pos, "\nVACATION MODE: ON\n");
    }

    pos += snprintf(buf + pos, bufSize - pos,
                    "\nUptime: %dh\n",
                    static_cast<int>(data.uptimeMs / 3600000));
}

bool isDailyHeartbeatTime(uint32_t nowMs, const SolarClock& clk,
                          const DuskDetector& det, bool ntpAvailable,
                          NetworkState& ns)
{
    // Debounce: max 1 heartbeat per 20h
    if (ns.lastHeartbeatMs > 0 &&
        (nowMs - ns.lastHeartbeatMs) < 20UL * 3600 * 1000) {
        return false;
    }

    // Strategy 1: SolarClock (dawn + 30 min)
    if (clk.calibrated) {
        uint32_t estDawn = estimateNextDawn(det, clk, nowMs);
        if (estDawn > 0) {
            uint32_t target = estDawn + 30UL * 60 * 1000;
            int32_t diff = static_cast<int32_t>(nowMs - target);
            if (abs(diff) < 5 * 60 * 1000) {
                if (!ns.heartbeatSentToday) {
                    ns.heartbeatSentToday = true;
                    ns.lastHeartbeatMs = nowMs;
                    return true;
                }
            } else {
                ns.heartbeatSentToday = false;
            }
            return false;
        }
    }

    // Strategy 2: NTP (8:00 local) — TODO when NTP available

    // Strategy 3: fallback — every 24h from last heartbeat (or 5 min from boot)
    if (ns.lastHeartbeatMs == 0) {
        if (nowMs > 5UL * 60 * 1000) {
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
