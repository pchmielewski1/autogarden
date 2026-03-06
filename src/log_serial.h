#pragma once

#include <cstddef>

class LogSerialProxy {
public:
    void begin(unsigned long baud);
    operator bool() const;
    void setDebugOutput(bool enabled);

    void flush();

    size_t println(const char* msg = "");
    size_t printf(const char* fmt, ...);
};

extern LogSerialProxy AGSerial;
