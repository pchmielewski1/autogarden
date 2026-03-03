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
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <M5Unified.h>

// TODO(telegram): #include <UniversalTelegramBot.h>
// TODO(telegram): #include <WiFiClientSecure.h>

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
    String ssid     = sanitizeInput(g_webServer->arg("ssid"),     32);
    String pass     = sanitizeInput(g_webServer->arg("pass"),     64);
    String botToken = sanitizeInput(g_webServer->arg("bot_token"), 63);
    String chatId   = sanitizeInput(g_webServer->arg("chat_id"),  15);

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
    if (chatId.length() > 0 && !isNumericStr(chatId)) {
        g_webServer->send(400, "text/plain", "Chat ID must be numeric");
        return;
    }
    if (!isValidBotToken(botToken)) {
        g_webServer->send(400, "text/plain", "Invalid bot token format");
        return;
    }

    // --- Safe copy with guaranteed null termination ---
    memset(g_apNetCfg->wifiSsid,         0, sizeof(g_apNetCfg->wifiSsid));
    memset(g_apNetCfg->wifiPass,         0, sizeof(g_apNetCfg->wifiPass));
    memset(g_apNetCfg->telegramBotToken, 0, sizeof(g_apNetCfg->telegramBotToken));
    memset(g_apNetCfg->telegramChatId,   0, sizeof(g_apNetCfg->telegramChatId));

    strncpy(g_apNetCfg->wifiSsid, ssid.c_str(), sizeof(g_apNetCfg->wifiSsid) - 1);
    strncpy(g_apNetCfg->wifiPass, pass.c_str(), sizeof(g_apNetCfg->wifiPass) - 1);
    strncpy(g_apNetCfg->telegramBotToken, botToken.c_str(),
            sizeof(g_apNetCfg->telegramBotToken) - 1);
    strncpy(g_apNetCfg->telegramChatId, chatId.c_str(),
            sizeof(g_apNetCfg->telegramChatId) - 1);
    g_apNetCfg->provisioned = true;

    netConfigSave(*g_apNetCfg);
    Serial.printf("[PROV] Config saved (SSID='%s', chatId='%s'), restarting...\n",
                  g_apNetCfg->wifiSsid, g_apNetCfg->telegramChatId);

    g_webServer->send_P(200, "text/html", SUCCESS_HTML);
    delay(1000);
    ESP.restart();
}

// ---------------------------------------------------------------------------
// enterApMode — blokuje do restartu
// ---------------------------------------------------------------------------
void enterApMode(NetConfig& netCfg) {
    Serial.println("PROV state=AP_MODE");

    WiFi.mode(WIFI_AP);
    WiFi.softAP("autogarden", "");   // open, no password
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    // DNS captive portal
    static DNSServer dnsServer;
    g_dnsServer = &dnsServer;
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

    // mDNS backup
    MDNS.begin("autogarden");

    // Web server
    static WebServer webServer(80);
    g_webServer = &webServer;
    g_apNetCfg = &netCfg;

    webServer.on("/",     handleRoot);
    webServer.on("/scan", handleScan);
    webServer.on("/save", HTTP_POST, handleSave);
    webServer.on("/skip", []() {
        if (g_webServer) {
            g_webServer->send(200, "text/plain", "OK");
            Serial.println("PROV skip WiFi — restarting offline");
            delay(500);
            ESP.restart();
        }
    });
    // Captive portal detection paths (Android, iOS, Windows, etc.)
    webServer.on("/generate_204",      handleRoot);
    webServer.on("/gen_204",           handleRoot);
    webServer.on("/hotspot-detect.html", handleRoot);
    webServer.on("/library/test/success.html", handleRoot);
    webServer.on("/connecttest.txt",   handleRoot);
    webServer.on("/redirect",          handleRoot);
    webServer.on("/canonical.html",    handleRoot);
    webServer.on("/success.txt",       handleRoot);
    webServer.on("/ncsi.txt",          handleRoot);
    webServer.on("/favicon.ico",       []() {
        if (g_webServer) g_webServer->send(204, "text/plain", " ");
    });
    webServer.onNotFound(handleRoot);
    webServer.begin();

    // LCD info
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(0, 0);
    M5.Display.println("WiFi Setup");
    M5.Display.println("AP: autogarden");
    M5.Display.println("http://192.168.4.1");

    uint32_t apNoClientSinceMs = millis();

    // Blocking loop — AP mode
    for (;;) {
        dnsServer.processNextRequest();
        webServer.handleClient();
        M5.update();

        // Auto-off: 5 min bez klienta
        if (WiFi.softAPgetStationNum() > 0) {
            apNoClientSinceMs = millis();
        } else if ((millis() - apNoClientSinceMs) >= NetworkState::kApAutoOffMs) {
            Serial.println("AP auto-off: no client 5 min");
            WiFi.softAPdisconnect(true);
            delay(2000);
            ESP.restart();
        }

        delay(1);
    }
}

// ---------------------------------------------------------------------------
// Non-blocking AP mode — startuje AP + captive portal, zwraca od razu.
// apTick() musi być wywoływany co ~100ms w pętli NetTask.
// ---------------------------------------------------------------------------
static DNSServer  s_apDns;
static WebServer  s_apWeb(80);

void startApNonBlocking(NetConfig& netCfg, NetworkState& ns) {
    Serial.println("[AP] Starting non-blocking AP mode...");

    WiFi.mode(WIFI_AP);
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
            Serial.println("[AP] skip WiFi — staying offline");
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

    Serial.printf("[AP] AP 'autogarden' active, IP=192.168.4.1 (auto-off in %ds)\n",
                  (int)(NetworkState::kApAutoOffMs / 1000));
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
        Serial.println("[AP] auto-off: no client for 5 min");
        stopAp(ns);
    }
}

void stopAp(NetworkState& ns) {
    if (!ns.apActive) return;

    s_apWeb.stop();
    s_apDns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    g_webServer = nullptr;
    g_dnsServer = nullptr;
    g_apNetCfg  = nullptr;

    ns.apActive = false;
    Serial.println("[AP] AP mode stopped — offline");
}

// ---------------------------------------------------------------------------
// tryConnectWifi — max retries z timeoutem
// ---------------------------------------------------------------------------
bool tryConnectWifi(const NetConfig& netCfg, uint8_t maxRetries,
                    uint32_t timeoutPerRetryMs)
{
    Serial.printf("PROV state=WIFI_CONNECTING ssid=%s\n", netCfg.wifiSsid);
    WiFi.mode(WIFI_STA);

    for (uint8_t attempt = 1; attempt <= maxRetries; ++attempt) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(0, 0);
        M5.Display.printf("WiFi %d/%d...", attempt, maxRetries);

        WiFi.begin(netCfg.wifiSsid, netCfg.wifiPass);

        uint32_t deadline = millis() + timeoutPerRetryMs;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
            delay(100);
            M5.update();
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("PROV WIFI_CONNECTED ip=%s\n",
                          WiFi.localIP().toString().c_str());
            M5.Display.printf("\nOK: %s", WiFi.localIP().toString().c_str());
            return true;
        }

        Serial.printf("PROV wifi attempt %d failed status=%d\n",
                      attempt, WiFi.status());
        WiFi.disconnect();
        delay(1000);
    }

    Serial.println("PROV WIFI_FAILED after retries");
    return false;
}

// ---------------------------------------------------------------------------
// networkProvisioning — wywoływane w setup()
// ---------------------------------------------------------------------------
bool networkProvisioning(NetConfig& netCfg) {
    if (!netCfg.provisioned || strlen(netCfg.wifiSsid) == 0) {
        enterApMode(netCfg);   // blokuje — nigdy nie wraca (restart)
        return false;          // unreachable
    }

    if (tryConnectWifi(netCfg)) {
        MDNS.begin("autogarden");
        return true;
    }

    // WiFi fail → reset provisioned, restart → AP mode
    netCfg.provisioned = false;
    netConfigSave(netCfg);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("WiFi FAILED!");
    M5.Display.println("Entering setup...");
    delay(2000);
    ESP.restart();
    return false;  // unreachable
}

// ---------------------------------------------------------------------------
// netTaskInit — mDNS & Telegram init
// Jeśli WiFi nie jest skonfigurowane, uruchamia AP w trybie non-blocking.
// ---------------------------------------------------------------------------
void netTaskInit(const NetConfig& netCfg, NetworkState& ns) {
    ns.wifiConnected = (WiFi.status() == WL_CONNECTED);
    ns.telegramEnabled = (strlen(netCfg.telegramBotToken) > 0 &&
                          strlen(netCfg.telegramChatId) > 0);

    if (ns.telegramEnabled) {
        Serial.println("NET Telegram enabled");
    }

    // Jeśli WiFi nie provisioned → auto-start AP w tle
    if (!netCfg.provisioned || strlen(netCfg.wifiSsid) == 0) {
        Serial.println("[NET] WiFi not provisioned — starting AP 'autogarden' in background");
        // const_cast bo startApNonBlocking potrzebuje writeable NetConfig*
        // dla handleSave — ale g_apNetCfg jest ustawiany globalnie
        extern NetConfig g_netConfig;
        startApNonBlocking(g_netConfig, ns);
    }

    Serial.println("NET task initialized");
}

// ---------------------------------------------------------------------------
// netTaskTick — reconnect + AP tick + poll + notifications
// ---------------------------------------------------------------------------
void netTaskTick(uint32_t nowMs, NetworkState& ns, const NetConfig& netCfg) {
    // --- Periodic network status log (every 30s) ---
    static uint32_t s_lastNetLogMs = 0;
    if (nowMs - s_lastNetLogMs >= 30000) {
        s_lastNetLogMs = nowMs;
        if (ns.apActive) {
            Serial.printf("[NET] AP active, clients=%d\n",
                          WiFi.softAPgetStationNum());
        } else if (ns.wifiConnected) {
            Serial.printf("[NET] WiFi OK rssi=%d ip=%s\n",
                          WiFi.RSSI(), WiFi.localIP().toString().c_str());
        } else if (netCfg.provisioned) {
            Serial.printf("[NET] WiFi disconnected, reconnect #%d backoff=%ds\n",
                          ns.reconnectAttempts, (int)(ns.reconnectBackoffMs / 1000));
        } else {
            Serial.println("[NET] offline (not provisioned)");
        }
    }

    // === AP mode active → obsłuż captive portal ===
    if (ns.apActive) {
        apTick(ns);
        return;  // nie robimy nic innego gdy AP jest aktywne
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
            Serial.println("NET WiFi reconnecting...");
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
        Serial.printf("NET WiFi reconnected ip=%s\n",
                      WiFi.localIP().toString().c_str());
        // TODO(events): push WIFI_CONNECTED event
    }

    // === Telegram poll ===
    if (ns.telegramEnabled) {
        telegramPollCommands(nowMs, netCfg);
    }
}

// ---------------------------------------------------------------------------
// Telegram — stubs (do implementacji z UniversalTelegramBot)
// ---------------------------------------------------------------------------

void telegramPollCommands(uint32_t nowMs, const NetConfig& netCfg) {
    // TODO(telegram): Implementacja z UniversalTelegramBot
    // bot.getUpdates() → parsuj komendy → push do EventQueue
    (void)nowMs;
    (void)netCfg;
}

bool telegramSend(const char* msg, const NetConfig& netCfg,
                  uint8_t maxRetries, uint32_t backoffMs)
{
    // TODO(telegram): bot.sendMessage(netCfg.telegramChatId, msg)
    (void)msg;
    (void)netCfg;
    (void)maxRetries;
    (void)backoffMs;
    Serial.printf("TG_SEND (stub): %s\n", msg);
    return false;
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
    Serial.println("NET factory reset done");
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
  <label>Bot Token</label>
  <input type="text" name="bot_token" maxlength="63" autocomplete="off" pattern="[0-9]+:[A-Za-z0-9_-]+" placeholder="123456:ABC-DEF...">
  <label>Chat ID</label>
  <input type="text" name="chat_id" maxlength="15" autocomplete="off" pattern="-?[0-9]+" inputmode="numeric" placeholder="e.g. 123456789">
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
