#include "LineReader.h"

namespace proto {

bool LineReader::feed(char byte) {
    // The previous call completed a line; the caller has had its chance to read
    // it, so start the next one now. Resetting here rather than on completion
    // is what keeps `line()` valid until the following `feed`.
    if (complete_) {
        complete_ = false;
        length_ = 0;
        buffer_[0] = '\0';
    }

    if (byte == '\n') {
        if (overflowed_) {
            ++droppedLines_;
            overflowed_ = false;
            length_ = 0;
            buffer_[0] = '\0';
            return false;
        }
        if (length_ == 0) return false;   // blank line
        buffer_[length_] = '\0';
        complete_ = true;
        return true;
    }

    if (byte == '\r') return false;

    // A previous byte overflowed the buffer: swallow the rest of this line.
    if (overflowed_) return false;

    if (length_ >= kMaxLineBytes) {
        overflowed_ = true;
        length_ = 0;
        buffer_[0] = '\0';
        return false;
    }

    buffer_[length_++] = byte;
    return false;
}

void LineReader::reset() {
    length_ = 0;
    overflowed_ = false;
    complete_ = false;
    buffer_[0] = '\0';
}

}  // namespace proto
