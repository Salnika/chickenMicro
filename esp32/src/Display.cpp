#include "Display.h"

#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <stdio.h>
#include <string.h>

#include "ChickenWalk.h"
#include "Config.h"
#include "Log.h"
#include "TextLayout.h"

namespace ui {
namespace {

// ---------------------------------------------------------------------------
// Layout for a 250x122 landscape panel. Bands rather than magic numbers
// scattered through the drawing code.
//
//    0 ┌──────────────────────────────────────┐
//      │ CHICKEN MICRO    ..chicken..     2/3 │  title bar; the chicken paces the
//   16 ├══════════════════════════════════════┤  empty lane, feet on the rule
//      │ body                                 │
//  102 ├──────────────────────────────────────┤
//      │ [x]DENY  [#]STOP  [v]ALLOW  [<]MORE  │  key legend, one cap per key
//  122 └──────────────────────────────────────┘
// ---------------------------------------------------------------------------
//
// Nothing here fills a large area with ink, and that is a hardware constraint
// rather than a taste: the glass is 3-colour driven with a monochrome waveform,
// and a big solid black field drags red pigment to the surface — the panel goes
// maroon. Weight comes from rules, outlines and glyphs instead. An inverted
// title bar and a black "connecting" screen both had to go for this reason.
constexpr int16_t kMargin = 4;
constexpr int16_t kHeaderHeight = 16;
constexpr int16_t kHeaderTextTop = 3;
/// Double rule under the header: the weight an inverted bar would have given.
constexpr int16_t kHeaderRule = 14;
/// Room kept clear between the chicken's lane and the text on either side.
constexpr int16_t kLaneGap = 8;
constexpr int16_t kBodyTop = 20;
/// Where the permission screen's title and payload start, under the meta row.
constexpr int16_t kPermissionTop = 32;
constexpr int16_t kBodyBottom = 102;
/// The monospace font hangs 4px below its baseline, so its band stops short of
/// the key legend: a descender crossing that rule looks like a fault.
constexpr int16_t kMonoBottom = 100;
constexpr int16_t kKeyBarTop = 104;

/// Line box heights, including leading, for the fonts used below.
constexpr int16_t kSmallLineHeight = 10;    // built-in 6x8 font
constexpr int16_t kMonoLineHeight = 16;     // FreeMonoBold9pt7b (yAdvance 18)

/// Session list geometry.
constexpr int16_t kRowHeight = 14;
constexpr uint8_t kVisibleRows = 6;

/// Key legend geometry. Four equal cells, so a cap never moves between
/// screens: the third cap is always the third key under your finger.
constexpr int16_t kCapSize = 11;
constexpr int16_t kCapTop = 107;
constexpr int16_t kKeyLabelTop = 109;

constexpr const char* kBrand = "CHICKEN MICRO";

/// Wrapped text buffers.
///
/// File-scope rather than on the stack: drawing is single-threaded, and
/// GxEPD2's paged rendering already uses a good part of the task stack. Two of
/// them, because the permission screen lays out a title and a payload at once.
WrappedText wrapped;
WrappedText wrappedTitle;

/// Adafruit_GFX text metric, handed to `wrapText` as its measurement function.
///
/// Measured by cursor advance, not by `getTextBounds`. Two reasons, both found
/// by rendering a screen and looking at it:
///
///  - `getTextBounds` reports the *inked* box, so a space measures 0 and every
///    word loses its right side bearing. Widths came out short by a character
///    or so per word, and long lines were wrapped a word too late.
///  - it also runs GFX's own wrapping, so nothing ever measured wider than the
///    panel: an over-long line looked like it fit, and GFX then broke it onto
///    the row the next line was already using.
///
/// Printing at a cursor far off the canvas costs the same work and is exact:
/// every pixel is clipped away, and `getCursorX` is the width the string will
/// occupy when it is drawn for real.
int16_t measureWithGfx(const char* text, size_t length, void* context) {
    if (length == 0) return 0;
    Adafruit_GFX& gfx = *static_cast<Adafruit_GFX*>(context);

    char scratch[WrappedText::kMaxLineChars];
    if (length >= sizeof(scratch)) length = sizeof(scratch) - 1;
    memcpy(scratch, text, length);
    scratch[length] = '\0';

    const int16_t cursorX = gfx.getCursorX();
    const int16_t cursorY = gfx.getCursorY();
    gfx.setTextWrap(false);
    gfx.setCursor(0, -1000);
    gfx.print(scratch);
    const int16_t width = gfx.getCursorX();
    gfx.setCursor(cursorX, cursorY);
    return width;
}

int16_t measure(Adafruit_GFX& gfx, const char* text, size_t length) {
    return measureWithGfx(text, length, &gfx);
}

int16_t measure(Adafruit_GFX& gfx, const char* text) {
    return measure(gfx, text, strlen(text));
}

/// Wraps against the font currently selected on `gfx`.
void wrap(Adafruit_GFX& gfx, const char* text, int16_t maxWidth, WrappedText& out) {
    wrapText(text, maxWidth, measureWithGfx, &gfx, out);
}

/// Draws wrapped lines with the built-in font, where the cursor is the glyph's
/// top-left corner.
void drawLinesTop(
    Adafruit_GFX& gfx,
    const WrappedText& text,
    uint16_t first,
    uint16_t count,
    int16_t x,
    int16_t top,
    int16_t lineHeight) {
    for (uint16_t index = first; index < text.count && index < first + count; ++index) {
        gfx.setCursor(x, top);
        gfx.print(text.lines[index]);
        top = static_cast<int16_t>(top + lineHeight);
    }
}

/// Draws wrapped lines with a custom font, where the cursor is the baseline.
void drawLinesBaseline(
    Adafruit_GFX& gfx,
    const WrappedText& text,
    uint16_t first,
    uint16_t count,
    int16_t x,
    int16_t baseline,
    int16_t lineHeight) {
    for (uint16_t index = first; index < text.count && index < first + count; ++index) {
        gfx.setCursor(x, baseline);
        gfx.print(text.lines[index]);
        baseline = static_cast<int16_t>(baseline + lineHeight);
    }
}

void drawRightAligned(Adafruit_GFX& gfx, const char* text, int16_t right, int16_t y) {
    gfx.setCursor(static_cast<int16_t>(right - measure(gfx, text)), y);
    gfx.print(text);
}

void drawCentred(Adafruit_GFX& gfx, const char* text, int16_t width, int16_t y) {
    gfx.setCursor(static_cast<int16_t>((width - measure(gfx, text)) / 2), y);
    gfx.print(text);
}

/// Uppercase copy, for the kind tag.
void toUpper(const char* source, char* destination, size_t capacity) {
    size_t index = 0;
    for (; source[index] != '\0' && index + 1 < capacity; ++index) {
        const char character = source[index];
        destination[index] =
            (character >= 'a' && character <= 'z') ? static_cast<char>(character - 32) : character;
    }
    destination[index] = '\0';
}

/// Full state label, for the one line that has room for it.
const char* stateLabel(proto::ClaudeState state) {
    switch (state) {
        case proto::ClaudeState::Disconnected: return "No session";
        case proto::ClaudeState::Idle: return "Idle";
        case proto::ClaudeState::Thinking: return "Thinking...";
        case proto::ClaudeState::Writing: return "Writing...";
        case proto::ClaudeState::WaitingPermission: return "Permission";
        case proto::ClaudeState::Done: return "Done";
        case proto::ClaudeState::Error: return "Error";
    }
    return "?";
}

/// Short tag for a session row, 8 characters at most.
const char* stateTag(proto::ClaudeState state) {
    switch (state) {
        case proto::ClaudeState::Disconnected: return "OFFLINE";
        case proto::ClaudeState::Idle: return "IDLE";
        case proto::ClaudeState::Thinking: return "THINKING";
        case proto::ClaudeState::Writing: return "WRITING";
        case proto::ClaudeState::WaitingPermission: return "WAITING";
        case proto::ClaudeState::Done: return "DONE";
        case proto::ClaudeState::Error: return "ERROR";
    }
    return "?";
}

const char* menuLabel(app::MenuItem item) {
    switch (item) {
        // The duration is part of the label on purpose: the panel flashes black
        // and red for most of it, which looks like a fault if it is a surprise.
        case app::MenuItem::Otp: return "OTP codes";
        case app::MenuItem::AddOtp: return "Add OTP";
        case app::MenuItem::CleanScreen: return "Clean the screen (45 s)";
        // Rendered with the current waveform's name appended; see drawMenu.
        case app::MenuItem::Waveform: return "Waveform: ";
        case app::MenuItem::Resync: return "Resync with the Mac";
        case app::MenuItem::Close: return "Close menu";
    }
    return "?";
}

// --- The chicken ------------------------------------------------------------

/// 12x12, facing left. Each row is 12 bits with the leftmost pixel in bit 11.
/// The legs are kept out of the body because the walk alternates them.
constexpr int16_t kChickenSize = 12;
const uint16_t kChickenBody[kChickenSize - 1] = {
    0x0C0,   // ....##......
    0x1E0,   // ...####.....
    0x3F0,   // ..######....
    0x2F0,   // ..#.####....   the gap is the eye
    0xEF0,   // ###.####....   the spur on the left is the beak
    0x3F0,   // ..######....
    0x3FC,   // ..########..
    0x7FE,   // .##########.
    0x7FE,   // .##########.
    0x7FE,   // .##########.
    0x3FC,   // ..########..
};
constexpr uint16_t kLegsApart = 0x108;      // ...#....#...
constexpr uint16_t kLegsTogether = 0x060;   // .....##.....

/// Draws the chicken with its top-left corner at (x, y). Facing right is the
/// same sprite read from the other end, rather than a second copy of it.
void drawChicken(
    Adafruit_GFX& gfx, int16_t x, int16_t y, bool facingRight, bool legsApart, uint16_t colour) {
    for (int16_t row = 0; row < kChickenSize; ++row) {
        const uint16_t bits = row < kChickenSize - 1 ? kChickenBody[row]
                                                     : (legsApart ? kLegsApart : kLegsTogether);
        for (int16_t column = 0; column < kChickenSize; ++column) {
            if ((bits & (1u << (kChickenSize - 1 - column))) == 0) continue;
            const int16_t pixelX = facingRight ? static_cast<int16_t>(x + kChickenSize - 1 - column)
                                               : static_cast<int16_t>(x + column);
            gfx.drawPixel(pixelX, static_cast<int16_t>(y + row), colour);
        }
    }
}

// --- Key caps ---------------------------------------------------------------
//
// The glyphs are drawn with primitives rather than taken from a font: they have
// to match the physical caps (a cross, a square, a check, a return arrow), and
// no font in the library carries all four.

void drawCrossGlyph(Adafruit_GFX& gfx, int16_t x, int16_t y, uint16_t colour) {
    for (int16_t offset = 0; offset < 2; ++offset) {
        gfx.drawLine(x + 3 + offset, y + 3, x + 7 + offset, y + 7, colour);
        gfx.drawLine(x + 7 - offset, y + 3, x + 3 - offset, y + 7, colour);
    }
}

void drawSquareGlyph(Adafruit_GFX& gfx, int16_t x, int16_t y, uint16_t colour) {
    gfx.fillRect(x + 3, y + 3, 5, 5, colour);
}

void drawCheckGlyph(Adafruit_GFX& gfx, int16_t x, int16_t y, uint16_t colour) {
    for (int16_t offset = 0; offset < 2; ++offset) {
        gfx.drawLine(x + 2, y + 5 + offset, x + 4, y + 7 + offset, colour);
        gfx.drawLine(x + 4, y + 7 + offset, x + 8, y + 3 + offset, colour);
    }
}

void drawReturnGlyph(Adafruit_GFX& gfx, int16_t x, int16_t y, uint16_t colour) {
    gfx.drawFastHLine(x + 2, y + 6, 6, colour);
    gfx.drawFastVLine(x + 8, y + 3, 4, colour);
    gfx.drawLine(x + 2, y + 6, x + 4, y + 4, colour);
    gfx.drawLine(x + 2, y + 6, x + 4, y + 8, colour);
}

void drawKeyCap(Adafruit_GFX& gfx, Key key, int16_t x, int16_t y, uint16_t ink) {
    gfx.drawRoundRect(x, y, kCapSize, kCapSize, 2, ink);
    switch (key) {
        case Key::Cross: drawCrossGlyph(gfx, x, y, ink); break;
        case Key::Square: drawSquareGlyph(gfx, x, y, ink); break;
        case Key::Check: drawCheckGlyph(gfx, x, y, ink); break;
        case Key::Return: drawReturnGlyph(gfx, x, y, ink); break;
    }
}

uint32_t hashString(uint32_t seed, const char* text) {
    uint32_t hash = seed;
    if (text == nullptr) return hash;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        hash ^= static_cast<uint8_t>(*cursor);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t hashByte(uint32_t seed, uint8_t value) {
    return (seed ^ value) * 16777619u;
}

/// Folds the chicken's step into a content hash: a step changes the glass, but
/// not what the screen says.
uint32_t hashWalk(uint32_t seed, uint32_t phase) {
    uint32_t hash = seed;
    for (int shift = 0; shift < 32; shift += 8) {
        hash = hashByte(hash, static_cast<uint8_t>(phase >> shift));
    }
    return hash;
}

}  // namespace

// ---------------------------------------------------------------------------

bool Display::clean() {
    if (!driver_.clean()) return false;

    // The glass is white again and the driver has been re-initialised, so
    // nothing on the panel matches the cached frame. Forgetting it is what makes
    // the next draw repaint everything — and, exactly as after `begin()`, the
    // background is already white, so that draw is a plain partial with no
    // second white flash.
    everDrawn_ = false;
    forceFull_ = false;
    partialsSinceFull_ = 0;
    chickenSteps_ = 0;
    return true;
}

void Display::begin() {
    driver_.begin();
    logf(proto::LogLevel::Info, "[display] %s, partial refresh %s", driver_.modelName(),
         driver_.supportsPartial() ? "available" : "unavailable");
    // `driver_.begin()` has already left the panel white, so the first content
    // draw is a plain partial onto a clean background — no second white flash.
    forceFull_ = false;
    partialsSinceFull_ = 0;
}

int16_t Display::bodyTopFor(const app::State& state) const {
    // The session name only earns a line when there is more than one session to
    // tell apart.
    return state.sessionCount() > 1 ? static_cast<int16_t>(kBodyTop + 12) : kBodyTop;
}

uint16_t Display::pageCountFor(const app::State& state) const {
    // Pagination depends on the font, so the text is wrapped against the
    // driver's measurement canvas. This performs no refresh.
    Adafruit_GFX& gfx = driver_.metrics();
    const int16_t usableWidth = static_cast<int16_t>(driver_.width() - 2 * kMargin);

    switch (state.screen()) {
        case app::Screen::Permission: {
            const app::ActivePermission& permission = state.permission();
            const bool hasPayload = permission.content[0] != '\0';
            const char* body = hasPayload ? permission.content : permission.title;
            if (body[0] == '\0') return 1;

            const int16_t contentTop = layoutPermissionTitle(gfx, state);
            gfx.setFont(&FreeMonoBold9pt7b);
            wrap(gfx, body, usableWidth, wrapped);
            return pageCountWithLead(
                wrapped.count,
                linesPerPage(kMonoLineHeight, contentTop, kMonoBottom),
                linesPerPage(kMonoLineHeight, kPermissionTop, kMonoBottom));
        }
        case app::Screen::Message: {
            if (state.message()[0] == '\0') return 1;
            gfx.setFont(nullptr);
            wrap(gfx, state.message(), usableWidth, wrapped);
            return pageCountFrom(wrapped.count,
                                 linesPerPage(kSmallLineHeight, bodyTopFor(state), kBodyBottom));
        }
        case app::Screen::Connecting:
        case app::Screen::Home:
        case app::Screen::Sessions:
        case app::Screen::Menu:
        case app::Screen::OtpList:
        case app::Screen::OtpCode:
        case app::Screen::OtpSetup:
            return 1;
    }
    return 1;
}

bool Display::draw(const app::State& state, uint32_t nowMs, bool force) {
    const uint32_t content = contentHash(state);
    const uint32_t frame = hashWalk(content, state.walkPhase());
    // A requested full refresh is honoured even when nothing changed: holding
    // the return key asks for exactly that, and it used to be skipped as a
    // no-op.
    if (!force && !forceFull_ && everDrawn_ && frame == lastFrameHash_) return true;

    // E-ink refreshes are slow and visible; rate-limit bursts so a flurry of
    // frames does not queue up a second of flashing. The redraw stays owed:
    // returning early without saying so used to leave a stale screen until
    // something else happened to change.
    if (!force && everDrawn_ && (nowMs - lastDrawMs_) < config::kMinRedrawIntervalMs) {
        return false;
    }

    // The connecting screen shares no layout with any other screen, so going
    // into or out of it flips most of the panel. It happens rarely, which makes
    // it the cheapest moment to clear accumulated ghosting.
    const bool connecting = state.screen() == app::Screen::Connecting;
    if (connecting != lastWasConnecting_) forceFull_ = true;

    // A step of the chicken moves one 12x12 sprite. Counted like any other
    // partial it would force a full refresh — a white flash of the whole panel —
    // every 20 seconds of walking, so steps only count for a fraction of one.
    const bool chickenOnly = everDrawn_ && content == lastContentHash_;
    if (chickenOnly && ++chickenSteps_ >= config::kChickenStepsPerPartial) {
        chickenSteps_ = 0;
        ++partialsSinceFull_;
    }

    const bool needFull = forceFull_ || !driver_.supportsPartial() ||
                          partialsSinceFull_ >= config::kPartialsBeforeFullRefresh;

    driver_.render(needFull ? RefreshMode::Full : RefreshMode::Partial,
                   [&](Adafruit_GFX& gfx) {
                       gfx.setTextSize(1);
                       gfx.setTextWrap(false);
                       gfx.setTextColor(driver_.ink());
                       switch (state.screen()) {
                           case app::Screen::Connecting: drawConnecting(gfx, state); break;
                           case app::Screen::Permission: drawPermission(gfx, state); break;
                           case app::Screen::Message: drawMessage(gfx, state); break;
                           case app::Screen::Sessions: drawSessions(gfx, state); break;
                           case app::Screen::Menu: drawMenu(gfx, state); break;
                           case app::Screen::OtpList: drawOtpList(gfx, state); break;
                           case app::Screen::OtpCode: drawOtpCode(gfx, state); break;
                           case app::Screen::OtpSetup: drawOtpSetup(gfx, state); break;
                           case app::Screen::Home: drawHome(gfx, state); break;
                       }
                   });

    if (needFull) {
        forceFull_ = false;
        partialsSinceFull_ = 0;
        chickenSteps_ = 0;
    } else if (!chickenOnly) {
        ++partialsSinceFull_;
    }
    lastContentHash_ = content;
    lastFrameHash_ = frame;
    everDrawn_ = true;
    lastDrawMs_ = nowMs;
    lastWasConnecting_ = connecting;
    return true;
}

// ---------------------------------------------------------------------------
// Shared furniture
// ---------------------------------------------------------------------------

void Display::drawHeader(Adafruit_GFX& gfx, const app::State& state, const char* right) const {
    const int16_t width = driver_.width();
    const uint16_t ink = driver_.ink();

    gfx.setFont(nullptr);
    gfx.setTextColor(ink);
    gfx.setCursor(kMargin, kHeaderTextTop);
    gfx.print(kBrand);

    int16_t laneEnd = static_cast<int16_t>(width - kMargin);
    if (right != nullptr && right[0] != '\0') {
        drawRightAligned(gfx, right, laneEnd, kHeaderTextTop);
        laneEnd = static_cast<int16_t>(laneEnd - measure(gfx, right) - kLaneGap);
    }

    // The chicken lives in whatever room the wordmark and the context label
    // leave, standing on the rule. Its step counter is mapped onto this lane
    // afresh on every draw, so a wider label just shortens its beat.
    const int16_t laneStart = static_cast<int16_t>(kMargin + measure(gfx, kBrand) + kLaneGap);
    const WalkPose pose =
        walkPose(state.walkPhase(), static_cast<int16_t>(laneEnd - laneStart - kChickenSize),
                 config::kChickenStepPx);
    drawChicken(gfx, static_cast<int16_t>(laneStart + pose.x),
                static_cast<int16_t>(kHeaderRule - kChickenSize), pose.facingRight,
                pose.legsApart, ink);

    gfx.drawFastHLine(0, kHeaderRule, width, ink);
    gfx.drawFastHLine(0, kHeaderRule + 2, width, ink);
}

void Display::drawKeyBar(Adafruit_GFX& gfx, const KeyLegend& legend) const {
    const int16_t width = driver_.width();
    gfx.drawFastHLine(0, kKeyBarTop - 2, width, driver_.ink());
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());

    const int16_t cell = static_cast<int16_t>(width / 4);
    for (int16_t index = 0; index < 4; ++index) {
        const char* label = legend.keys[index].label;
        if (label == nullptr || label[0] == '\0') continue;
        const int16_t x = static_cast<int16_t>(index * cell + kMargin);
        drawKeyCap(gfx, static_cast<Key>(index), x, kCapTop, driver_.ink());
        gfx.setCursor(static_cast<int16_t>(x + kCapSize + 3), kKeyLabelTop);
        gfx.print(label);
    }
}

int16_t Display::drawSessionContext(
    Adafruit_GFX& gfx, const app::State& state, const char* id) const {
    if (state.sessionCount() <= 1) return kBodyTop;
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());
    gfx.setCursor(kMargin, kBodyTop);
    gfx.print(state.nameFor(id));
    return static_cast<int16_t>(kBodyTop + 12);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

/// Shown until the first frame arrives from the Mac, and again whenever the
/// link goes quiet.
///
/// This was a black screen, which read beautifully across a desk and turned the
/// panel maroon: 122x250 of solid ink is the worst case for 3-colour glass on a
/// mono waveform. Same layout, drawn the way the hardware can hold.
void Display::drawConnecting(Adafruit_GFX& gfx, const app::State& state) const {
    const int16_t width = driver_.width();
    const uint16_t ink = driver_.ink();
    gfx.setTextColor(ink);

    // Wordmark: the chicken and the name, centred as one group.
    gfx.setFont(&FreeSansBold9pt7b);
    const int16_t brandWidth = measure(gfx, kBrand);
    const int16_t groupWidth = static_cast<int16_t>(kChickenSize + 6 + brandWidth);
    const int16_t groupLeft = static_cast<int16_t>((width - groupWidth) / 2);
    drawChicken(gfx, groupLeft, 28, /*facingRight=*/false, /*legsApart=*/true, ink);
    gfx.setCursor(static_cast<int16_t>(groupLeft + kChickenSize + 6), 40);
    gfx.print(kBrand);

    gfx.drawFastHLine(groupLeft, 46, groupWidth, ink);

    gfx.setFont(nullptr);
    drawCentred(gfx, "Connecting", width, 56);

    // The loader. E-ink cannot animate, so this steps: one cell at a time, at
    // most once a second, and it stops on its own after a while — see
    // config::kConnectingStepMs.
    constexpr int16_t kCells = 5;
    constexpr int16_t kCellWidth = 14;
    constexpr int16_t kCellHeight = 8;
    constexpr int16_t kCellGap = 4;
    const int16_t barWidth = kCells * kCellWidth + (kCells - 1) * kCellGap;
    int16_t x = static_cast<int16_t>((width - barWidth) / 2);
    for (int16_t index = 0; index < kCells; ++index) {
        if (index == state.loaderFrame() % kCells) {
            gfx.fillRect(x, 70, kCellWidth, kCellHeight, ink);
        } else {
            gfx.drawRect(x, 70, kCellWidth, kCellHeight, ink);
        }
        x = static_cast<int16_t>(x + kCellWidth + kCellGap);
    }

    drawCentred(gfx, "start the Mac daemon: claude-remote run", width, 92);
}

/// The focused session at a glance.
void Display::drawHome(Adafruit_GFX& gfx, const app::State& state) const {
    char right[12] = "";
    if (state.sessionTotal() > 1) {
        const int index = state.focusedIndex();
        const uint8_t position = index < 0 ? 0 : static_cast<uint8_t>(index + 1);
        snprintf(right, sizeof(right), "%u/%u", static_cast<unsigned>(position),
                 static_cast<unsigned>(state.sessionTotal()));
    }
    drawHeader(gfx, state, right);

    const int16_t width = driver_.width();
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());
    if (state.sessionCount() > 0) {
        gfx.setCursor(kMargin, kBodyTop);
        gfx.print(state.nameFor(state.focusId()));
    }

    gfx.setFont(&FreeSansBold12pt7b);
    gfx.setCursor(kMargin, 50);
    gfx.print(stateLabel(state.claudeState()));

    // Whatever context there is: the last thing asked of Claude, falling back
    // to the beginning of its reply.
    const char* body = state.detail()[0] != '\0' ? state.detail() : state.message();
    gfx.setFont(nullptr);
    if (body[0] != '\0') {
        constexpr int16_t kQuoteTop = 60;
        wrap(gfx, body, static_cast<int16_t>(width - kMargin - 12), wrapped);
        const uint16_t lines = linesPerPage(kSmallLineHeight, kQuoteTop, kBodyBottom);
        const uint16_t shown = wrapped.count < lines ? wrapped.count : lines;
        gfx.fillRect(kMargin, kQuoteTop, 2, static_cast<int16_t>(shown * kSmallLineHeight - 2),
                     driver_.ink());
        drawLinesTop(gfx, wrapped, 0, lines, kMargin + 8, kQuoteTop, kSmallLineHeight);
    } else if (state.sessionCount() == 0) {
        gfx.setCursor(kMargin, 62);
        gfx.print("No Claude Desktop window found.");
    }

    drawKeyBar(gfx, keyLegendFor(state));
}

/// Every session the Mac can see, so a box on the desk is not blind to the
/// window that is not in front.
void Display::drawSessions(Adafruit_GFX& gfx, const app::State& state) const {
    char right[16] = "SESSIONS";
    if (state.sessionCount() > 0) {
        snprintf(right, sizeof(right), "%u/%u", static_cast<unsigned>(state.cursor() + 1),
                 static_cast<unsigned>(state.sessionTotal()));
    }
    drawHeader(gfx, state, right);

    const int16_t width = driver_.width();
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());

    if (state.sessionCount() == 0) {
        gfx.setCursor(kMargin, kBodyTop + 20);
        gfx.print("No session.");
        gfx.setCursor(kMargin, kBodyTop + 34);
        gfx.print("Open a Claude Desktop window.");
        drawKeyBar(gfx, keyLegendFor(state));
        return;
    }

    // Scroll the window of rows so the cursor stays inside it.
    uint8_t first = 0;
    if (state.sessionCount() > kVisibleRows && state.cursor() >= kVisibleRows - 1) {
        first = static_cast<uint8_t>(state.cursor() - (kVisibleRows - 2));
        const uint8_t last = static_cast<uint8_t>(state.sessionCount() - kVisibleRows);
        if (first > last) first = last;
    }

    int16_t y = kBodyTop - 2;
    for (uint8_t index = first; index < state.sessionCount() && index < first + kVisibleRows;
         ++index) {
        const proto::SessionEntry& session = state.sessionAt(index);
        const bool selected = index == state.cursor();
        const bool focused = strcmp(session.id, state.focusId()) == 0;

        gfx.setTextColor(driver_.ink());
        if (selected) {
            // An outline rather than an inverted row: a filled bar this size is
            // what makes 3-colour glass go maroon under a mono waveform.
            gfx.drawRect(1, y, static_cast<int16_t>(width - 2), kRowHeight, driver_.ink());
        }

        // A filled dot marks the session the Home screen is reporting on.
        if (focused) gfx.fillCircle(kMargin + 4, y + 6, 2, driver_.ink());
        gfx.setCursor(kMargin + 10, y + 3);
        gfx.print(session.name);

        const char* tag = stateTag(session.state);
        drawRightAligned(gfx, tag, static_cast<int16_t>(width - kMargin), y + 3);
        if (session.state == proto::ClaudeState::WaitingPermission) {
            gfx.setCursor(static_cast<int16_t>(width - kMargin - measure(gfx, tag) - 10), y + 3);
            gfx.print("!");
        }
        y = static_cast<int16_t>(y + kRowHeight);
    }

    gfx.setTextColor(driver_.ink());

    // STOP acts on the highlighted row here — see keyLegendFor.
    drawKeyBar(gfx, keyLegendFor(state));
}

/// The long-press menu: device-side commands, like scrubbing the screen.
void Display::drawMenu(Adafruit_GFX& gfx, const app::State& state) const {
    drawHeader(gfx, state, "MENU");

    const int16_t width = driver_.width();
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());

    int16_t y = kBodyTop;
    for (uint8_t index = 0; index < state.menuItemCount(); ++index) {
        const bool selected = index == state.menuCursor();
        if (selected) {
            // An outline, not a fill: a filled bar makes the 3-colour glass go
            // maroon. A caret marks the row too, so it reads even mid-refresh.
            gfx.drawRect(1, static_cast<int16_t>(y - 2), static_cast<int16_t>(width - 2), 13,
                         driver_.ink());
            gfx.setCursor(kMargin, y);
            gfx.print('>');
        }
        gfx.setCursor(kMargin + 10, y);
        gfx.print(menuLabel(static_cast<app::MenuItem>(index)));
        // The waveform entry shows which one is loaded, so a flick is enough to
        // see where you are while comparing them on the glass.
        if (static_cast<app::MenuItem>(index) == app::MenuItem::Waveform) {
            gfx.print(waveformName(driver_.waveform()));
        }
        // The session list's row pitch: six entries fit above the key bar.
        y = static_cast<int16_t>(y + kRowHeight);
    }

    drawKeyBar(gfx, keyLegendFor(state));
}

namespace {

/// "123 456", "123 4567", "1234 5678": the way codes are read aloud and typed.
void groupCode(const char* code, char* out, size_t capacity) {
    const size_t length = strlen(code);
    const size_t split = length == 8 ? 4 : 3;
    size_t written = 0;
    for (size_t index = 0; index < length && written + 1 < capacity; ++index) {
        if (index == split && written + 2 < capacity) out[written++] = ' ';
        out[written++] = code[index];
    }
    out[written] = '\0';
}

}  // namespace

/// The stored accounts. Pick one and its code goes to the Mac's clipboard.
void Display::drawOtpList(Adafruit_GFX& gfx, const app::State& state) const {
    char right[16] = "OTP";
    if (!state.hasClock()) {
        // Codes need the Mac's clock; the COPY key stays dark until it comes.
        snprintf(right, sizeof(right), "NO CLOCK");
    } else if (state.otpCount() > 0) {
        snprintf(right, sizeof(right), "OTP %u/%u", static_cast<unsigned>(state.otpCursor() + 1),
                 static_cast<unsigned>(state.otpCount()));
    }
    drawHeader(gfx, state, right);

    const int16_t width = driver_.width();
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());

    if (state.otpCount() == 0) {
        gfx.setCursor(kMargin, kBodyTop + 20);
        gfx.print("No account yet.");
        gfx.setCursor(kMargin, kBodyTop + 34);
        gfx.print("Menu > Add OTP to add one.");
        drawKeyBar(gfx, keyLegendFor(state));
        return;
    }

    // Scrolls the way the session list does, keeping the cursor in view.
    uint8_t first = 0;
    if (state.otpCount() > kVisibleRows && state.otpCursor() >= kVisibleRows - 1) {
        first = static_cast<uint8_t>(state.otpCursor() - (kVisibleRows - 2));
        const uint8_t last = static_cast<uint8_t>(state.otpCount() - kVisibleRows);
        if (first > last) first = last;
    }

    int16_t y = kBodyTop - 2;
    for (uint8_t index = first; index < state.otpCount() && index < first + kVisibleRows;
         ++index) {
        if (index == state.otpCursor()) {
            gfx.drawRect(1, y, static_cast<int16_t>(width - 2), kRowHeight, driver_.ink());
        }
        gfx.setCursor(kMargin + 10, y + 3);
        gfx.print(state.otpName(index));
        y = static_cast<int16_t>(y + kRowHeight);
    }

    drawKeyBar(gfx, keyLegendFor(state));
}

/// The code just copied: big enough to type from, and whether it still holds.
void Display::drawOtpCode(Adafruit_GFX& gfx, const app::State& state) const {
    drawHeader(gfx, state, "OTP");

    const int16_t width = driver_.width();
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());
    gfx.setCursor(kMargin, kBodyTop + 2);
    gfx.print(state.otpCodeName());

    // An expired code is not shown at all: typing it would only fail.
    char shown[12];
    if (state.otpExpired()) {
        snprintf(shown, sizeof(shown), "--- ---");
    } else {
        groupCode(state.otpCode(), shown, sizeof(shown));
    }
    gfx.setFont(&FreeSansBold12pt7b);
    drawCentred(gfx, shown, width, 62);

    gfx.setFont(nullptr);
    char line[48];
    if (state.otpExpired()) {
        drawCentred(gfx, "Expired. Press v for a fresh one.", width, 78);
    } else {
        drawCentred(gfx, "Copied to the Mac's clipboard", width, 78);
        snprintf(line, sizeof(line), "with %u s left in its window",
                 static_cast<unsigned>(state.otpSecondsLeft()));
        drawCentred(gfx, line, width, 90);
    }

    drawKeyBar(gfx, keyLegendFor(state));
}

namespace {

/// The setup screen gives up its title bar so the QR code can have the height:
/// a square from the top edge down to the key bar's rule, the code centred in
/// it, the words in the column to its right. Three pixels a module is 0.6 mm on
/// this glass, a 17 mm code a phone reads from a hand's width away.
constexpr int16_t kQrBox = kKeyBarTop - 2;
/// White kept clear around the code, in modules. The standard asks for four;
/// phones read two without trouble when what lies beyond is blank glass.
constexpr int16_t kQrQuietModules = 2;
constexpr int16_t kQrMaxScale = 3;

/// One rectangle per run of dark modules rather than one per module.
void drawQr(Adafruit_GFX& gfx, const otp::QrBitmap& qr, int16_t left, int16_t top, int16_t scale,
            uint16_t ink) {
    for (uint8_t y = 0; y < qr.size; ++y) {
        uint8_t x = 0;
        while (x < qr.size) {
            if (!qr.module(x, y)) {
                ++x;
                continue;
            }
            const uint8_t start = x;
            while (x < qr.size && qr.module(x, y)) ++x;
            gfx.fillRect(static_cast<int16_t>(left + start * scale),
                         static_cast<int16_t>(top + y * scale),
                         static_cast<int16_t>((x - start) * scale), scale, ink);
        }
    }
}

}  // namespace

/// "Add OTP": a QR code that joins the setup Wi-Fi, and the same network
/// spelled out for whatever has no camera. The password is only ever shown here.
void Display::drawOtpSetup(Adafruit_GFX& gfx, const app::State& state) const {
    const otp::QrBitmap& qr = state.setupQr();
    int16_t column = kMargin;
    if (qr.size > 0) {
        int16_t scale = kQrMaxScale;
        while (scale > 1 && (qr.size + 2 * kQrQuietModules) * scale > kQrBox) --scale;
        const int16_t origin = static_cast<int16_t>((kQrBox - qr.size * scale) / 2);
        drawQr(gfx, qr, origin, origin, scale, driver_.ink());
        column = kQrBox;
    }
    const int16_t columnWidth = static_cast<int16_t>(driver_.width() - kMargin - column);

    gfx.setTextColor(driver_.ink());
    gfx.setFont(&FreeSansBold9pt7b);
    gfx.setCursor(column, 15);
    gfx.print("ADD OTP");

    gfx.setFont(nullptr);
    gfx.setCursor(column, 24);
    gfx.print(qr.size > 0 ? "Scan to join, then open" : "Join this Wi-Fi, then open");
    gfx.setCursor(column, 34);
    gfx.print(state.setupAddress());

    char line[app::kSetupFieldCapacity + 8];
    snprintf(line, sizeof(line), "Wi-Fi %s", state.setupNetwork());
    gfx.setCursor(column, 48);
    gfx.print(line);
    snprintf(line, sizeof(line), "Pass  %s", state.setupPassword());
    gfx.setCursor(column, 58);
    gfx.print(line);

    // Two lines for what just happened: "Added" and a long account name wrap.
    const char* note = state.setupNote()[0] != '\0' ? state.setupNote() : "Waiting for a phone...";
    wrap(gfx, note, columnWidth, wrapped);
    drawLinesTop(gfx, wrapped, 0, 2, column, 72, kSmallLineHeight);
    gfx.setCursor(column, 92);
    gfx.print("Closes after 5 min idle.");

    drawKeyBar(gfx, keyLegendFor(state));
}

/// The last assistant message, paginated.
void Display::drawMessage(Adafruit_GFX& gfx, const app::State& state) const {
    char right[16] = "MESSAGE";
    if (state.pageCount() > 1) {
        snprintf(right, sizeof(right), "%u/%u", state.page() + 1, state.pageCount());
    }
    drawHeader(gfx, state, right);

    const int16_t top = drawSessionContext(gfx, state, state.focusId());
    gfx.setFont(nullptr);
    gfx.setTextColor(driver_.ink());
    if (state.message()[0] == '\0') {
        gfx.setCursor(kMargin, top);
        gfx.print("No message yet.");
    } else {
        wrap(gfx, state.message(), static_cast<int16_t>(driver_.width() - 2 * kMargin), wrapped);
        const uint16_t perPage = linesPerPage(kSmallLineHeight, top, kBodyBottom);
        drawLinesTop(gfx, wrapped, static_cast<uint16_t>(state.page() * perPage), perPage, kMargin,
                     top, kSmallLineHeight);
    }

    drawKeyBar(gfx, keyLegendFor(state));
}

int16_t Display::layoutPermissionTitle(Adafruit_GFX& gfx, const app::State& state) const {
    const app::ActivePermission& permission = state.permission();
    wrappedTitle.clear();
    // With no payload the title *is* the payload, and is drawn in the monospace
    // font below rather than as a heading.
    if (permission.content[0] == '\0' || permission.title[0] == '\0') {
        return kPermissionTop;
    }
    gfx.setFont(nullptr);
    wrap(gfx, permission.title, static_cast<int16_t>(driver_.width() - 2 * kMargin), wrappedTitle);
    const uint16_t lines = wrappedTitle.count > 3 ? 3 : wrappedTitle.count;
    return static_cast<int16_t>(kPermissionTop + lines * kSmallLineHeight + 2);
}

/// The priority screen: the only one that can appear without being asked for.
void Display::drawPermission(Adafruit_GFX& gfx, const app::State& state) const {
    const app::ActivePermission& permission = state.permission();
    const int16_t width = driver_.width();

    char right[16] = "PERMISSION";
    if (state.pageCount() > 1) {
        snprintf(right, sizeof(right), "%u/%u", state.page() + 1, state.pageCount());
    }
    drawHeader(gfx, state, right);

    // Meta row: what kind of thing is being authorised, in which session, and
    // whether anything else is queued behind it.
    char kind[proto::kMaxKindChars];
    toUpper(permission.kind[0] != '\0' ? permission.kind : "tool", kind, sizeof(kind));
    gfx.setFont(nullptr);
    const int16_t pillWidth = static_cast<int16_t>(measure(gfx, kind) + 7);
    gfx.drawRoundRect(kMargin, kBodyTop - 3, pillWidth, 13, 2, driver_.ink());
    gfx.setTextColor(driver_.ink());
    gfx.setCursor(kMargin + 4, kBodyTop);
    gfx.print(kind);

    if (state.sessionCount() > 1 && permission.session[0] != '\0') {
        gfx.setCursor(static_cast<int16_t>(kMargin + pillWidth + 5), kBodyTop);
        gfx.print(state.nameFor(permission.session));
    }
    if (state.waitingCount() > 1) {
        // The queue count, and how to clear it in one go: a flick right on the
        // joystick. "(o)>" is the stick pushed right. It shares the meta row's
        // right end, clear of the session name at the left.
        char queued[20];
        snprintf(queued, sizeof(queued), "+%u  (o)>ALL",
                 static_cast<unsigned>(state.waitingCount() - 1));
        drawRightAligned(gfx, queued, static_cast<int16_t>(width - kMargin), kBodyTop);
    }

    const int16_t contentTop = layoutPermissionTitle(gfx, state);
    if (state.page() == 0 && wrappedTitle.count > 0) {
        gfx.setFont(nullptr);
        drawLinesTop(gfx, wrappedTitle, 0, 3, kMargin, kPermissionTop,
                     kSmallLineHeight);
    }

    // The payload gets a monospace font: it is the one thing on this device
    // that must be read exactly, and mono keeps l/1/I and O/0 apart.
    const bool hasPayload = permission.content[0] != '\0';
    const char* body = hasPayload ? permission.content : permission.title;
    if (body[0] == '\0') body = "(no details)";
    gfx.setFont(&FreeMonoBold9pt7b);
    wrap(gfx, body, static_cast<int16_t>(width - 2 * kMargin), wrapped);

    const uint16_t firstPageLines = linesPerPage(kMonoLineHeight, contentTop, kMonoBottom);
    const uint16_t otherPageLines =
        linesPerPage(kMonoLineHeight, kPermissionTop, kMonoBottom);
    uint16_t first = 0;
    uint16_t count = 0;
    pageSpan(wrapped.count, firstPageLines, otherPageLines, state.page(), first, count);
    const int16_t top = state.page() == 0 ? contentTop : kPermissionTop;
    drawLinesBaseline(gfx, wrapped, first, count, kMargin,
                      static_cast<int16_t>(top + kMonoLineHeight - 4), kMonoLineHeight);

    // Only the verdicts Claude actually offers are labelled — see keyLegendFor.
    drawKeyBar(gfx, keyLegendFor(state));
}

// ---------------------------------------------------------------------------

uint32_t Display::contentHash(const app::State& state) const {
    uint32_t hash = 2166136261u;
    hash = hashByte(hash, static_cast<uint8_t>(state.screen()));
    hash = hashByte(hash, static_cast<uint8_t>(state.claudeState()));
    hash = hashByte(hash, static_cast<uint8_t>(state.page()));
    hash = hashByte(hash, state.linkUp() ? 1u : 0u);
    hash = hashByte(hash, state.loaderFrame());
    hash = hashByte(hash, state.cursor());
    hash = hashByte(hash, state.menuCursor());
    // Part of what the menu shows, so switching waveform redraws its label.
    hash = hashByte(hash, static_cast<uint8_t>(driver_.waveform()));
    hash = hashByte(hash, state.sessionTotal());
    hash = hashString(hash, state.focusId());
    hash = hashString(hash, state.detail());
    hash = hashString(hash, state.message());
    for (uint8_t index = 0; index < state.sessionCount(); ++index) {
        const proto::SessionEntry& session = state.sessionAt(index);
        hash = hashString(hash, session.id);
        hash = hashString(hash, session.name);
        hash = hashByte(hash, static_cast<uint8_t>(session.state));
    }
    // One-time codes: every field their three screens draw.
    hash = hashByte(hash, state.hasClock() ? 1u : 0u);
    hash = hashByte(hash, state.otpCount());
    hash = hashByte(hash, state.otpCursor());
    for (uint8_t index = 0; index < state.otpCount(); ++index) {
        hash = hashString(hash, state.otpName(index));
    }
    hash = hashString(hash, state.otpCodeName());
    hash = hashString(hash, state.otpCode());
    hash = hashByte(hash, state.otpExpired() ? 1u : 0u);
    hash = hashByte(hash, static_cast<uint8_t>(state.otpSecondsLeft()));
    hash = hashString(hash, state.setupNetwork());
    hash = hashString(hash, state.setupPassword());
    hash = hashString(hash, state.setupNote());
    if (state.permission().valid) {
        hash = hashString(hash, state.permission().id);
        hash = hashString(hash, state.permission().kind);
        hash = hashString(hash, state.permission().content);
        hash = hashString(hash, state.permission().title);
        hash = hashString(hash, state.permission().session);
    }
    return hash;
}

}  // namespace ui
