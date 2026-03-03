// ============================================================================
// events.h — EventQueue i typy zdarzeń
// ============================================================================
// Źródło prawdy: docs/PLAN.md → "Zdarzenia (EventQueue)",
//                                "Zaktualizowane zdarzenia (EventQueue)"
// Architektura:  docs/ARCHITECTURE.md
//
// Header-only. Zero zależności poza FreeRTOS + Arduino types.
// ============================================================================
#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---------------------------------------------------------------------------
// Konfiguracja kolejki
// ---------------------------------------------------------------------------
static constexpr uint8_t kEventQueueSize = 32;

// ---------------------------------------------------------------------------
// Typy zdarzeń
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
    // --- Ticki (generowane programowo, bez ISR) ---
    TICK_10MS,
    TICK_100MS,
    TICK_1S,

    // --- Przyciski ---
    BUTTON_PRESS,          // payload: buttonId
    BUTTON_LONG_PRESS,     // payload: buttonId

    // --- Komendy zdalne (Telegram / HTTP) ---
    REMOTE_CMD,            // payload: cmdType

    // --- Watering cycle events (per-pot, potIndex w payload) ---
    WATERING_CYCLE_START,
    WATERING_PULSE_START,
    WATERING_PULSE_END,
    WATERING_SOAK_DONE,
    WATERING_OVERFLOW_DETECTED,
    WATERING_OVERFLOW_CLEARED,
    WATERING_TARGET_REACHED,
    WATERING_CYCLE_DONE,

    // --- Reservoir ---
    RESERVOIR_LOW,
    RESERVOIR_EMPTY,
    RESERVOIR_REFILLED,
    RESERVOIR_WARNING,

    // --- Sensor ---
    SENSOR_FAULT,
    SENSOR_ANOMALY,

    // --- Config requests (UI/NET → ControlTask) ---
    REQUEST_SET_MODE,
    REQUEST_SET_PLANT,
    REQUEST_SET_SOIL_CALIB,
    REQUEST_MANUAL_WATER,
    REQUEST_PUMP_OFF,
    REQUEST_REFILL,
    REQUEST_VACATION_TOGGLE,
    REQUEST_SET_NUM_POTS,
    REQUEST_SET_RESERVOIR,
    REQUEST_SET_RESERVOIR_LOW,
    REQUEST_SET_PULSE_ML,
    REQUEST_SET_VACATION,
    REQUEST_START_WIFI_SETUP,

    // --- Config acks ---
    CONFIG_CHANGED,
    CONFIG_SAVE_REQUEST,

    // --- System ---
    SYSTEM_BOOT,
    SYSTEM_FACTORY_RESET,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,
};

// ---------------------------------------------------------------------------
// Identyfikatory przycisków
// ---------------------------------------------------------------------------
enum class ButtonId : uint8_t {
    BTN_A = 0,           // StickS3 wbudowany frontowy
    BTN_B = 1,           // StickS3 wbudowany boczny
    DUAL_BLUE = 2,       // Dual Button CH5 pin0 (niebieski)
    DUAL_RED  = 3,       // Dual Button CH5 pin1 (czerwony)
};

// ---------------------------------------------------------------------------
// Typy komend zdalnych
// ---------------------------------------------------------------------------
enum class RemoteCmdType : uint8_t {
    STATUS,
    WATER,
    STOP,
    REFILL,
    SET_PLANT,
    HISTORY,
    VACATION_ON,
    VACATION_OFF,
    HELP,
};

// ---------------------------------------------------------------------------
// Struktura zdarzenia
// ---------------------------------------------------------------------------
struct Event {
    EventType type;
    union {
        ButtonId   buttonId;
        RemoteCmdType cmdType;
        struct {
            uint8_t  potIndex;
            uint8_t  pulseNum;
            uint16_t valueMl;    // np. pumpedMl * 10
            float    moisturePct;
        } watering;
        struct {
            uint8_t sensorId;
            uint8_t code;
        } sensor;
        struct {
            uint8_t  key;        // enum klucz konfiguracji
            uint16_t valueU16;
            float    valueF;
        } config;
    } payload;
};

// ---------------------------------------------------------------------------
// EventQueue — wrapper na FreeRTOS xQueue
// ---------------------------------------------------------------------------
class EventQueue {
public:
    bool init() {
        _handle = xQueueCreate(kEventQueueSize, sizeof(Event));
        return _handle != nullptr;
    }

    bool push(const Event& evt, TickType_t wait = 0) {
        if (!_handle) return false;
        return xQueueSend(_handle, &evt, wait) == pdTRUE;
    }

    bool pushFromISR(const Event& evt) {
        if (!_handle) return false;
        BaseType_t woken = pdFALSE;
        auto ok = xQueueSendFromISR(_handle, &evt, &woken);
        portYIELD_FROM_ISR(woken);
        return ok == pdTRUE;
    }

    bool pop(Event& evt, TickType_t wait = portMAX_DELAY) {
        if (!_handle) return false;
        return xQueueReceive(_handle, &evt, wait) == pdTRUE;
    }

    bool peek(Event& evt) {
        if (!_handle) return false;
        return xQueuePeek(_handle, &evt, 0) == pdTRUE;
    }

    UBaseType_t count() const {
        if (!_handle) return 0;
        return uxQueueMessagesWaiting(_handle);
    }

    // Szybkie helpy do tworzenia typowych eventów
    static Event tick(EventType tickType) {
        Event e{}; e.type = tickType; return e;
    }

    static Event button(EventType type, ButtonId id) {
        Event e{}; e.type = type; e.payload.buttonId = id; return e;
    }

    static Event wateringEvt(EventType type, uint8_t potIdx) {
        Event e{}; e.type = type; e.payload.watering.potIndex = potIdx; return e;
    }

private:
    QueueHandle_t _handle = nullptr;
};

// Globalna instancja — deklaracja extern, definicja w main.cpp
extern EventQueue g_eventQueue;
