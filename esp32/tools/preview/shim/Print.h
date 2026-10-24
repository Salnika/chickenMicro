#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "WString.h"

/// Arduino's Print, reduced to what Adafruit_GFX inherits and uses.
class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t character) = 0;
    virtual size_t write(const uint8_t* buffer, size_t size) {
        size_t written = 0;
        while (size-- > 0) written += write(*buffer++);
        return written;
    }

    size_t print(const char* text) {
        size_t written = 0;
        while (*text != '\0') written += write(static_cast<uint8_t>(*text++));
        return written;
    }
    size_t print(char character) { return write(static_cast<uint8_t>(character)); }
    size_t print(const String& text) { return print(text.c_str()); }
    size_t print(int value) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%d", value);
        return print(buffer);
    }
    size_t println(const char* text) { return print(text) + write('\n'); }
};
