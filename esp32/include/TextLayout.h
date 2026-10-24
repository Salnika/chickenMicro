#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Word wrap and pagination, with no font and no display in them.
//
// Text measurement is injected, so the wrap can be exercised on the host with
// a synthetic monospace metric: `pio test -e native`. Display.cpp supplies the
// real metric by calling Adafruit_GFX::getTextBounds.
// ---------------------------------------------------------------------------

namespace ui {

/// A block of text broken into display lines.
struct WrappedText {
    static constexpr uint16_t kMaxLines = 40;
    /// Longest line stored. `wrapText` keeps lines under this itself, so
    /// `push`'s clamp is a backstop and never what decides a break.
    static constexpr size_t kMaxLineChars = 64;

    char lines[kMaxLines][kMaxLineChars] = {};
    uint16_t count = 0;

    void clear();
    void push(const char* source, size_t length);
};

/// Pixel width of `length` characters starting at `text`, in whatever font the
/// caller has selected. A plain function pointer plus a context, rather than
/// std::function, so nothing allocates.
using MeasureText = int16_t (*)(const char* text, size_t length, void* context);

/// Greedy word wrap.
///
/// Honours '\n' as a hard break. A word too long for one line is split rather
/// than clipped, so a long path or URL stays fully readable across pages.
void wrapText(
    const char* text,
    int16_t maxWidth,
    MeasureText measure,
    void* context,
    WrappedText& out);

/// How many lines fit between `top` and `bottom`. Never returns 0.
uint16_t linesPerPage(int16_t lineHeight, int16_t top, int16_t bottom);

/// Pages needed for `lineCount` lines. Never returns 0, so an empty screen is
/// still page 1 of 1.
uint16_t pageCountFrom(uint16_t lineCount, uint16_t perPage);

/// Pages when the first one holds fewer lines than the others.
///
/// The permission screen puts the request's title above the payload, so page 1
/// has less room for the payload than the pages that follow it.
uint16_t pageCountWithLead(uint16_t lineCount, uint16_t firstPageLines, uint16_t otherPageLines);

/// The slice of lines `page` shows under the same rule: `first` is the index to
/// start at and `count` how many fit. `count` is 0 past the end.
void pageSpan(
    uint16_t lineCount,
    uint16_t firstPageLines,
    uint16_t otherPageLines,
    uint16_t page,
    uint16_t& first,
    uint16_t& count);

}  // namespace ui
