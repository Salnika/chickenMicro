#include "Log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

namespace {

proto::LogLevel minimumLevel = proto::LogLevel::Info;

constexpr size_t kMaxFormattedChars = 201;
constexpr size_t kFrameCapacity = 320;

bool enabled(proto::LogLevel level) {
    return static_cast<uint8_t>(level) >= static_cast<uint8_t>(minimumLevel);
}

}  // namespace

void setLogLevel(proto::LogLevel level) { minimumLevel = level; }

void logMessage(proto::LogLevel level, const char* message) {
    if (!enabled(level) || message == nullptr) return;
    char frame[kFrameCapacity];
    const size_t length = proto::encodeLog(frame, sizeof(frame), level, message);
    if (length > 0) Serial.write(reinterpret_cast<const uint8_t*>(frame), length);
}

void logf(proto::LogLevel level, const char* format, ...) {
    if (!enabled(level)) return;
    char formatted[kMaxFormattedChars];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(formatted, sizeof(formatted), format, arguments);
    va_end(arguments);
    logMessage(level, formatted);
}
