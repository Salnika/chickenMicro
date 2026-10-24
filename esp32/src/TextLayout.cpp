#include "TextLayout.h"

#include <string.h>

namespace ui {

void WrappedText::clear() {
    count = 0;
    lines[0][0] = '\0';
}

void WrappedText::push(const char* source, size_t length) {
    if (count >= kMaxLines) return;
    if (length >= kMaxLineChars) length = kMaxLineChars - 1;
    memcpy(lines[count], source, length);
    lines[count][length] = '\0';
    ++count;
}

void wrapText(
    const char* text,
    int16_t maxWidth,
    MeasureText measure,
    void* context,
    WrappedText& out) {
    out.clear();
    if (text == nullptr || text[0] == '\0' || maxWidth <= 0) return;

    const int16_t spaceWidth = measure(" ", 1, context);

    const char* lineStart = text;
    size_t lineLength = 0;
    int16_t lineWidth = 0;

    /// Emits the accumulated line with trailing spaces removed.
    auto flushLine = [&](bool keepEmpty) {
        size_t trimmed = lineLength;
        while (trimmed > 0 && lineStart[trimmed - 1] == ' ') --trimmed;
        if (trimmed > 0 || keepEmpty) out.push(lineStart, trimmed);
        lineStart += lineLength;
        lineLength = 0;
        lineWidth = 0;
    };

    const char* cursor = text;
    while (*cursor != '\0' && out.count < WrappedText::kMaxLines) {
        // A hard break ends the line even when it is empty, so a blank line in
        // the source stays a blank line on screen.
        if (*cursor == '\n') {
            flushLine(/*keepEmpty=*/true);
            ++cursor;
            lineStart = cursor;
            continue;
        }

        // Next word, plus the run of spaces that follows it.
        const char* wordStart = cursor;
        while (*cursor != '\0' && *cursor != '\n' && *cursor != ' ') ++cursor;
        const size_t wordLength = static_cast<size_t>(cursor - wordStart);
        while (*cursor == ' ') ++cursor;
        const size_t spaces = static_cast<size_t>(cursor - wordStart) - wordLength;

        const int16_t wordWidth = measure(wordStart, wordLength, context);

        // A word too long for an empty line is split at the last glyph that
        // fits, bounded by the line buffer so nothing is silently dropped.
        if (lineLength == 0 && wordWidth > maxWidth) {
            const size_t limit = wordLength < WrappedText::kMaxLineChars - 1
                                     ? wordLength
                                     : WrappedText::kMaxLineChars - 1;
            size_t fits = 1;
            while (fits < limit && measure(wordStart, fits + 1, context) <= maxWidth) ++fits;
            out.push(wordStart, fits);
            cursor = wordStart + fits;
            lineStart = cursor;
            continue;
        }

        const size_t candidateLength = static_cast<size_t>(cursor - lineStart);
        const int16_t needed =
            lineLength == 0 ? wordWidth : static_cast<int16_t>(lineWidth + spaceWidth + wordWidth);

        // Break on the pixel bound or the character bound, whichever comes
        // first, then retry this word on the fresh line.
        if (lineLength > 0 &&
            (needed > maxWidth || candidateLength >= WrappedText::kMaxLineChars)) {
            flushLine(/*keepEmpty=*/false);
            cursor = wordStart;
            continue;
        }

        lineLength = candidateLength;
        lineWidth = needed;
        if (spaces > 1) {
            lineWidth = static_cast<int16_t>(lineWidth + spaceWidth * (spaces - 1));
        }
    }

    if (lineLength > 0 && out.count < WrappedText::kMaxLines) {
        flushLine(/*keepEmpty=*/false);
    }
}

uint16_t linesPerPage(int16_t lineHeight, int16_t top, int16_t bottom) {
    const int16_t available = static_cast<int16_t>(bottom - top);
    if (available <= 0 || lineHeight <= 0) return 1;
    const uint16_t count = static_cast<uint16_t>(available / lineHeight);
    return count == 0 ? 1 : count;
}

uint16_t pageCountFrom(uint16_t lineCount, uint16_t perPage) {
    if (lineCount == 0 || perPage == 0) return 1;
    return static_cast<uint16_t>((lineCount + perPage - 1) / perPage);
}

uint16_t pageCountWithLead(uint16_t lineCount, uint16_t firstPageLines, uint16_t otherPageLines) {
    if (lineCount <= firstPageLines) return 1;
    if (otherPageLines == 0) return 1;
    const uint16_t remaining = static_cast<uint16_t>(lineCount - firstPageLines);
    return static_cast<uint16_t>(1 + (remaining + otherPageLines - 1) / otherPageLines);
}

void pageSpan(
    uint16_t lineCount,
    uint16_t firstPageLines,
    uint16_t otherPageLines,
    uint16_t page,
    uint16_t& first,
    uint16_t& count) {
    if (page == 0) {
        first = 0;
        count = lineCount < firstPageLines ? lineCount : firstPageLines;
        return;
    }
    first = static_cast<uint16_t>(firstPageLines + (page - 1) * otherPageLines);
    if (first >= lineCount) {
        first = lineCount;
        count = 0;
        return;
    }
    const uint16_t remaining = static_cast<uint16_t>(lineCount - first);
    count = remaining < otherPageLines ? remaining : otherPageLines;
}

}  // namespace ui
