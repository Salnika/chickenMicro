#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Protocol.h"

namespace proto {

/// Turns a byte stream into complete NDJSON lines.
///
/// Mirrors `LineAssembler` on the Mac side. A line that grows past
/// `kMaxLineBytes` is abandoned whole — the tail is *not* parsed as if it were
/// a fresh frame, which is what stops line noise from ever being interpreted
/// as an action.
///
/// Arduino-free so it can be unit-tested with `pio test -e native`.
class LineReader {
public:
    /// Feeds one byte. Returns true when `line()` holds a complete line.
    bool feed(char byte);

    /// The completed line, valid until the next `feed`.
    const char* line() const { return buffer_; }
    size_t length() const { return length_; }

    /// Lines abandoned for exceeding `kMaxLineBytes`.
    uint32_t droppedLines() const { return droppedLines_; }

    void reset();

private:
    char buffer_[kMaxLineBytes + 1] = {0};
    size_t length_ = 0;
    bool overflowed_ = false;
    bool complete_ = false;
    uint32_t droppedLines_ = 0;
};

}  // namespace proto
