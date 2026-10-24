#pragma once

#include "Protocol.h"

// ---------------------------------------------------------------------------
// Device logging.
//
// Serial carries the NDJSON protocol, so logs cannot be plain `Serial.print`
// without corrupting the stream. They go out as protocol `log` frames instead:
// `pio device monitor` shows them verbatim, and the daemon re-prints them as
// `[DEVICE] ...` lines in its own log.
//
// Messages carry a subsystem tag, matching the format in the README:
//     [SERIAL] connected
//     [STATE] waiting_permission
//     [INPUT] approve
// ---------------------------------------------------------------------------

/// Lowest level actually transmitted. Raise it to cut serial traffic.
void setLogLevel(proto::LogLevel level);

void logMessage(proto::LogLevel level, const char* message);

/// printf-style; the formatted message is capped at 200 characters.
void logf(proto::LogLevel level, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
