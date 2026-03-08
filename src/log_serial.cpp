#include "log_serial.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static bool containsEventKey(const char* s) {
    return s && strstr(s, "event=") != nullptr;
}

static const char* skipSpaces(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) {
        ++s;
    }
    return s;
}

static void escapeForQuotes(const char* in, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    size_t w = 0;
    for (size_t i = 0; in && in[i] != '\0' && w + 1 < outSize; ++i) {
        char ch = in[i];
        if (ch == '"') {
            ch = '\'';
        }
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        out[w++] = ch;
    }
    out[w] = '\0';
}

static const char* normalizeLogLine(const char* in, char* out, size_t outSize) {
    if (!in) {
        if (outSize > 0) out[0] = '\0';
        return out;
    }

    const char* t = skipSpaces(in);
    if (containsEventKey(t)) {
        return t;
    }

    if (t[0] == '[') {
        const char* end = strchr(t, ']');
        if (end && end > t + 1) {
            size_t tagLen = static_cast<size_t>(end - t + 1);
            const char* msg = skipSpaces(end + 1);
            if (*msg == ':' || *msg == '-') msg = skipSpaces(msg + 1);
            char escaped[512];
            escapeForQuotes(msg, escaped, sizeof(escaped));
            snprintf(out, outSize, "%.*s event=msg text=\"%s\"",
                     static_cast<int>(tagLen), t, escaped);
            return out;
        }
    }

    char escaped[512];
    escapeForQuotes(t, escaped, sizeof(escaped));
    snprintf(out, outSize, "[LOG] event=msg text=\"%s\"", escaped);
    return out;
}

static SemaphoreHandle_t s_logMutex = nullptr;
static portMUX_TYPE s_seqMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_logSeq = 0;

static void ensureLogMutex() {
    if (s_logMutex) {
        return;
    }
    taskENTER_CRITICAL(&s_seqMux);
    if (!s_logMutex) {
        s_logMutex = xSemaphoreCreateMutex();
    }
    taskEXIT_CRITICAL(&s_seqMux);
}

static uint32_t nextLogSeq() {
    taskENTER_CRITICAL(&s_seqMux);
    uint32_t seq = ++s_logSeq;
    taskEXIT_CRITICAL(&s_seqMux);
    return seq;
}

static size_t writePrefixedLines(const char* text) {
    if (!text) {
        return 0;
    }

    ensureLogMutex();
    bool locked = true;
    if (s_logMutex) {
        xSemaphoreTake(s_logMutex, portMAX_DELAY);
    } else {
        locked = false;
    }

    const char* p = text;
    size_t outBytes = 0;

    while (*p) {
        const char* lineEnd = strchr(p, '\n');
        size_t lineLen = lineEnd ? static_cast<size_t>(lineEnd - p) : strlen(p);

        char prefix[32];
        uint32_t now = millis();
        uint32_t seq = nextLogSeq();
        int n = snprintf(prefix, sizeof(prefix), "[%10lu|%06lu] ",
                         static_cast<unsigned long>(now),
                         static_cast<unsigned long>(seq));

        if (lineLen > 0) {
            char lineBuf[768];
            if (lineLen >= sizeof(lineBuf)) {
                lineLen = sizeof(lineBuf) - 1;
            }
            memcpy(lineBuf, p, lineLen);
            lineBuf[lineLen] = '\0';

            char normalized[800];
            const char* finalLine = normalizeLogLine(lineBuf, normalized, sizeof(normalized));
            char fullLine[960];
            int fullLen = snprintf(fullLine, sizeof(fullLine), "%s%s\n",
                                   (n > 0) ? prefix : "",
                                   finalLine);
            if (fullLen > 0) {
                size_t writeLen = static_cast<size_t>(fullLen);
                if (writeLen >= sizeof(fullLine)) {
                    writeLen = sizeof(fullLine) - 1;
                    fullLine[writeLen - 1] = '\n';
                    fullLine[writeLen] = '\0';
                }
                ::Serial.write(reinterpret_cast<const uint8_t*>(fullLine), writeLen);
                outBytes += writeLen;
            }
        } else if (n > 0) {
            char fullLine[64];
            int fullLen = snprintf(fullLine, sizeof(fullLine), "%s\n", prefix);
            if (fullLen > 0) {
                ::Serial.write(reinterpret_cast<const uint8_t*>(fullLine), static_cast<size_t>(fullLen));
                outBytes += static_cast<size_t>(fullLen);
            }
        }

        if (!lineEnd) {
            break;
        }
        p = lineEnd + 1;
    }

    if (locked && s_logMutex) {
        xSemaphoreGive(s_logMutex);
    }

    return outBytes;
}

void LogSerialProxy::begin(unsigned long baud) {
    ::Serial.begin(baud);
}

LogSerialProxy::operator bool() const {
    return static_cast<bool>(::Serial);
}

void LogSerialProxy::setDebugOutput(bool enabled) {
    ::Serial.setDebugOutput(enabled);
}

void LogSerialProxy::flush() {
    ::Serial.flush();
}

size_t LogSerialProxy::println(const char* msg) {
    if (!msg) {
        msg = "";
    }
    return writePrefixedLines(msg);
}

size_t LogSerialProxy::printf(const char* fmt, ...) {
    if (!fmt) {
        return 0;
    }

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (n <= 0) {
        return 0;
    }

    if (n >= static_cast<int>(sizeof(buffer))) {
        buffer[sizeof(buffer) - 1] = '\0';
    }

    return writePrefixedLines(buffer);
}

LogSerialProxy AGSerial;
