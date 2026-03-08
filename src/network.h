// ============================================================================
// network.h — WiFi provisioning, Telegram bot, remote commands
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "WiFi Provisioning",
//                "Powiadomienia Telegram (NotificationService)",
//                "Sieć / remote control", "Sekwencja boot"
// Architektura:  docs/ARCHITECTURE.md
//
// WiFi/Telegram jest OPCJONALNE — brak sieci nie blokuje ControlTask.
// ============================================================================
#pragma once

#include <cstdint>
#include "config.h"
#include "hardware.h"
#include "watering.h"
#include "analysis.h"

// ---------------------------------------------------------------------------
// ProvisioningState — FSM provisioning
// PLAN.md → "Stany provisioning (FSM)"
// ---------------------------------------------------------------------------
enum class ProvisioningState : uint8_t {
    BOOT_CHECK,
    AP_MODE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED,
};

// ---------------------------------------------------------------------------
// NetworkState — runtime state sieci
// ---------------------------------------------------------------------------
struct NetworkState {
    ProvisioningState provState    = ProvisioningState::BOOT_CHECK;
    bool              wifiConnected = false;
    bool              telegramEnabled = false;
    bool              apActive     = false;   // AP mode działa (non-blocking)
    uint32_t          lastReconnectAttemptMs = 0;
    uint32_t          reconnectBackoffMs     = 5000;
    uint8_t           reconnectAttempts      = 0;
    static constexpr uint32_t kMaxBackoffMs  = 300000;  // 5 min

    // AP mode
    uint32_t          apStartMs            = 0;
    uint32_t          apNoClientSinceMs    = 0;
    static constexpr uint32_t kApAutoOffMs = 5UL * 60 * 1000;  // 5 min

    // Telegram heartbeat
    uint32_t          lastHeartbeatMs      = 0;
    bool              heartbeatSentToday   = false;
};

// ---------------------------------------------------------------------------
// API — provisioning helpers
// ---------------------------------------------------------------------------

// Non-blocking AP mode — startuje AP + captive portal, zwraca od razu.
// apTick() musi być wywoływany w pętli NetTask.
void startApNonBlocking(NetConfig& netCfg, NetworkState& ns);
void apTick(NetworkState& ns);
void stopAp(NetworkState& ns);

// ---------------------------------------------------------------------------
// API — NetTask tick (wywoływane w pętli NetTask)
// ---------------------------------------------------------------------------

// Inicjalizacja NetTask (mDNS, Telegram setup)
void netTaskInit(const NetConfig& netCfg, NetworkState& ns);

// Tick — reconnect, poll Telegram, send notifications
// Nie blokuje — krótkie operacje + backoff
void netTaskTick(uint32_t nowMs, NetworkState& ns, const NetConfig& netCfg);

// ---------------------------------------------------------------------------
// API — Telegram
// ---------------------------------------------------------------------------

struct TelegramStatusData {
    SensorSnapshot  sensors;
    WaterBudget     budget;
    WateringCycle   cycles[kMaxPots];
    uint32_t        lastCycleDoneMs[kMaxPots] = {};
    uint32_t        lastFeedbackSeq[kMaxPots] = {};
    WateringFeedbackCode lastFeedbackCode[kMaxPots] = {};
    float           lastFeedbackValue1[kMaxPots] = {};
    float           lastFeedbackValue2[kMaxPots] = {};
    uint8_t         lastFeedbackPulseCount[kMaxPots] = {};
    TrendState      trends[kMaxPots];
    Config          config;
    DuskPhase       duskPhase = DuskPhase::NIGHT;
    bool            wifiConnected = false;
    bool            apActive = false;
    uint8_t         selectedPot = 0;
    uint32_t        uptimeMs = 0;
};

void applyLocalTelegramConfig(NetConfig& netCfg);
const char* telegramConfiguredBotName();
uint8_t telegramConfiguredTargetCount(const NetConfig& netCfg);

// Sprawdź nowe komendy z Telegram i push do EventQueue
void telegramPollCommands(uint32_t nowMs, const NetConfig& netCfg,
                          const TelegramStatusData& status);

// Wyślij wiadomość z retry + backoff
bool telegramSend(const char* msg, const NetConfig& netCfg,
                  uint8_t maxRetries = 3, uint32_t backoffMs = 5000);

// Formatuj raport dzienny
// (wymaga snapshot danych — przekazywany jako const ref)
struct DailyReportData {
    SensorSnapshot  sensors;
    WaterBudget     budget;
    TrendState      trends[kMaxPots];
    Config          config;
    uint32_t        uptimeMs;
};

void formatDailyReport(const DailyReportData& data, char* buf, size_t bufSize);
void formatTelegramStatusReport(const TelegramStatusData& data, char* buf, size_t bufSize);

// Sprawdź czy pora na heartbeat (SolarClock / NTP / fallback 24h)
bool isDailyHeartbeatTime(uint32_t nowMs, const SolarClock& clk,
                          const DuskDetector& det, bool ntpAvailable,
                          NetworkState& ns);

// ---------------------------------------------------------------------------
// API — factory reset sieci
// ---------------------------------------------------------------------------
void networkFactoryReset();
