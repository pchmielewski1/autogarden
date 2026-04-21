#include "log_serial.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

struct LogScratch {
    char printfBuffer[768];
    char sanitized[768];
    char escaped[768];
    char normalized[832];
    char payload[896];
    char fullLine[1024];
};

static LogScratch s_logScratch;

static bool containsEventKey(const char* s) {
    return s && strstr(s, "event=") != nullptr;
}

static const char* skipSpaces(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) {
        ++s;
    }
    return s;
}

static void forceTailMarker(char* text, size_t textSize, const char* marker) {
    if (!text || textSize == 0 || !marker) {
        return;
    }

    if (strstr(text, marker) != nullptr) {
        return;
    }

    size_t markerLen = strlen(marker);
    if (textSize <= markerLen + 1) {
        return;
    }

    size_t len = strlen(text);
    if (len + markerLen >= textSize) {
        len = textSize - markerLen - 1;
        text[len] = '\0';
        while (len > 0 && text[len - 1] == ' ') {
            text[--len] = '\0';
        }
    }

    strncat(text, marker, textSize - strlen(text) - 1);
}

static void sanitizeLogRecord(const char* in,
                              char* out,
                              size_t outSize,
                              bool& hadEmbeddedNewline,
                              bool& truncated) {
    hadEmbeddedNewline = false;
    truncated = false;
    if (!out || outSize == 0) {
        return;
    }

    out[0] = '\0';
    if (!in) {
        return;
    }

    size_t w = 0;
    bool lastWasSpace = false;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        char ch = in[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            hadEmbeddedNewline = true;
            ch = ' ';
        }

        bool isSpace = (ch == ' ' || ch == '\t');
        if (isSpace && lastWasSpace) {
            continue;
        }

        if (w + 1 >= outSize) {
            truncated = true;
            break;
        }

        out[w++] = isSpace ? ' ' : ch;
        lastWasSpace = isSpace;
    }

    while (w > 0 && out[w - 1] == ' ') {
        --w;
    }
    out[w] = '\0';
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

static const char* normalizeLogLine(const char* in,
                                    char* escaped,
                                    size_t escapedSize,
                                    char* out,
                                    size_t outSize) {
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
            escapeForQuotes(msg, escaped, escapedSize);
            snprintf(out, outSize, "%.*s event=msg text=\"%s\"",
                     static_cast<int>(tagLen), t, escaped);
            return out;
        }
    }

    escapeForQuotes(t, escaped, escapedSize);
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

static bool takeLogMutex() {
    ensureLogMutex();
    if (!s_logMutex) {
        return false;
    }
    xSemaphoreTake(s_logMutex, portMAX_DELAY);
    return true;
}

static void giveLogMutex(bool locked) {
    if (locked && s_logMutex) {
        xSemaphoreGive(s_logMutex);
    }
}

static size_t writePrefixedLinesLocked(const char* text) {
    if (!text) {
        return 0;
    }

    size_t outBytes = 0;
    bool hadEmbeddedNewline = false;
    bool truncated = false;
    sanitizeLogRecord(text,
                      s_logScratch.sanitized,
                      sizeof(s_logScratch.sanitized),
                      hadEmbeddedNewline,
                      truncated);

    const char* finalLine = normalizeLogLine(s_logScratch.sanitized,
                                             s_logScratch.escaped,
                                             sizeof(s_logScratch.escaped),
                                             s_logScratch.normalized,
                                             sizeof(s_logScratch.normalized));

    int payloadLen = snprintf(s_logScratch.payload, sizeof(s_logScratch.payload), "%s", finalLine);
    if (payloadLen < 0) {
        s_logScratch.payload[0] = '\0';
    }
    if (hadEmbeddedNewline) {
        forceTailMarker(s_logScratch.payload, sizeof(s_logScratch.payload), " multiline=yes");
    }
    if (truncated || payloadLen >= static_cast<int>(sizeof(s_logScratch.payload))) {
        forceTailMarker(s_logScratch.payload, sizeof(s_logScratch.payload), " trunc=yes");
    }

    int fullLen = snprintf(s_logScratch.fullLine, sizeof(s_logScratch.fullLine), "[%10lu|%06lu] %s\n",
                           static_cast<unsigned long>(millis()),
                           static_cast<unsigned long>(nextLogSeq()),
                           s_logScratch.payload[0] != '\0' ? s_logScratch.payload : "[LOG] event=msg text=\"\"");
    if (fullLen > 0) {
        size_t writeLen = static_cast<size_t>(fullLen);
        if (writeLen >= sizeof(s_logScratch.fullLine)) {
            writeLen = sizeof(s_logScratch.fullLine) - 1;
            s_logScratch.fullLine[writeLen - 1] = '\n';
            s_logScratch.fullLine[writeLen] = '\0';
        }
        ::Serial.write(reinterpret_cast<const uint8_t*>(s_logScratch.fullLine), writeLen);
        outBytes += writeLen;
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
    bool locked = takeLogMutex();
    size_t outBytes = writePrefixedLinesLocked(msg);
    giveLogMutex(locked);
    return outBytes;
}

size_t LogSerialProxy::printf(const char* fmt, ...) {
    if (!fmt) {
        return 0;
    }

    bool locked = takeLogMutex();
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(s_logScratch.printfBuffer, sizeof(s_logScratch.printfBuffer), fmt, args);
    va_end(args);

    if (n <= 0) {
        giveLogMutex(locked);
        return 0;
    }

    if (n >= static_cast<int>(sizeof(s_logScratch.printfBuffer))) {
        s_logScratch.printfBuffer[sizeof(s_logScratch.printfBuffer) - 1] = '\0';
        forceTailMarker(s_logScratch.printfBuffer,
                        sizeof(s_logScratch.printfBuffer),
                        " trunc=yes");
    }

    size_t outBytes = writePrefixedLinesLocked(s_logScratch.printfBuffer);
    giveLogMutex(locked);
    return outBytes;
}

LogSerialProxy AGSerial;
