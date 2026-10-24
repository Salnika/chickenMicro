#include "DisplayDriver.h"

#include <Arduino.h>

#include "Config.h"
#include "DisplayModel.h"
#include "Log.h"

// ---------------------------------------------------------------------------
// Both drivers live here so the GxEPD2 headers are included exactly once, only
// for the selected model. The mock build pulls in no panel library at all.
// ---------------------------------------------------------------------------

#if EINK_MODEL == EINK_MODEL_MOCK

namespace ui {
namespace {

/// Renders into RAM and reports what it drew as protocol `log` frames.
///
/// Serial carries the NDJSON protocol, so the mock cannot print ASCII art
/// directly without corrupting the stream — it goes out as `log` messages,
/// which the daemon re-prints and `pio device monitor` shows verbatim.
class MockDriver final : public DisplayDriver {
public:
    bool begin() override {
        logf(proto::LogLevel::Info, "[display] mock %dx%d, no panel", EINK_WIDTH, EINK_HEIGHT);
        return true;
    }

    int16_t width() const override { return EINK_WIDTH; }
    int16_t height() const override { return EINK_HEIGHT; }
    bool supportsPartial() const override { return true; }

    void render(RefreshMode mode, const DrawFunction& draw) override {
        canvas_.fillScreen(paper());
        canvas_.setTextColor(ink());
        draw(canvas_);
        ++frames_;
        logf(proto::LogLevel::Debug, "[display] mock frame %lu (%s)",
             static_cast<unsigned long>(frames_),
             mode == RefreshMode::Full ? "full" : "partial");
#if defined(CM_MOCK_DISPLAY_ASCII)
        dumpAscii();
#endif
    }

    void hibernate() override {}

    bool clean() override {
        logMessage(proto::LogLevel::Info, "[display] mock deep clean (no panel to scrub)");
        return true;
    }

    const char* modelName() const override { return EINK_MODEL_NAME; }

    Adafruit_GFX& metrics() override { return canvas_; }
    // GFXcanvas1 is 1 bit per pixel: 1 is set, 0 is clear.
    uint16_t ink() const override { return 1; }
    uint16_t paper() const override { return 0; }

private:
#if defined(CM_MOCK_DISPLAY_ASCII)
    /// Coarse 2x4-pixel-per-character rendering, for eyeballing layout without
    /// a panel.
    void dumpAscii() {
        char row[EINK_WIDTH / 2 + 1];
        for (int16_t y = 0; y < EINK_HEIGHT; y += 4) {
            size_t column = 0;
            for (int16_t x = 0; x < EINK_WIDTH && column + 1 < sizeof(row); x += 2) {
                bool ink = false;
                for (int16_t dy = 0; dy < 4 && !ink; ++dy) {
                    for (int16_t dx = 0; dx < 2 && !ink; ++dx) {
                        if (y + dy < EINK_HEIGHT && x + dx < EINK_WIDTH) {
                            ink = canvas_.getPixel(x + dx, y + dy);
                        }
                    }
                }
                row[column++] = ink ? '#' : '.';
            }
            row[column] = '\0';
            logf(proto::LogLevel::Debug, "[display] |%s|", row);
        }
    }
#endif

    GFXcanvas1 canvas_{EINK_WIDTH, EINK_HEIGHT};
    uint32_t frames_ = 0;
};

MockDriver driver;

}  // namespace

DisplayDriver& activeDriver() { return driver; }

}  // namespace ui

#else   // real panel

#include <SPI.h>
#if EINK_PANEL_IS_3C
#include <GxEPD2_3C.h>
#else
#include <GxEPD2_BW.h>
#endif

#if EINK_PANEL_CAN_CLEAN && !EINK_PANEL_IS_3C
// The deep clean drives the same glass through its tri-colour waveform. Only
// the low-level panel class is needed — it fills the controller's RAM planes
// directly, so none of GxEPD2_3C's framebuffer is instantiated.
#include <GxEPD2_3C.h>
#endif

// 3-colour glass driven in mono has no safe full-update path at all, so partial
// refresh is not optional there — it is the only way to reach the panel.
#if EINK_PANEL_OTP_IS_3C && !EINK_PANEL_SUPPORTS_PARTIAL
#error "EINK_PANEL_OTP_IS_3C needs EINK_PANEL_SUPPORTS_PARTIAL: a full update would load the tri-colour OTP waveform and render RAM 0x26 as red."
#endif

namespace ui {
namespace {

#if EINK_PANEL_IS_3C
using PanelDisplay = GxEPD2_3C<EINK_PANEL_CLASS, EINK_PANEL_CLASS::HEIGHT>;
#else
using PanelDisplay = GxEPD2_BW<EINK_PANEL_CLASS, EINK_PANEL_CLASS::HEIGHT>;
#endif

#if EINK_PANEL_GENTLE_LUT
// ---------------------------------------------------------------------------
// Softened partial-refresh LUT — the fix for "red creeps back in sometimes".
//
// This glass is tri-colour; we drive it mono, and the mono LUT cannot move red
// pigment on purpose — but GxEPD2's stock partial LUT still pulses *unchanged*
// pixels (LUT0 "white stays white" and LUT3 "black stays black" carry voltage).
// One pulse per refresh is invisible; hundreds of them — every walking-chicken
// step is a full-window partial — cumulatively nudge the red particles until a
// pink cast shows on the settled areas, which the periodic white flush then
// clears. Hence "parfois".
//
// This is a copy of GxEPD2_213_BN::lut_partial with LUT0 and LUT3 zeroed, so an
// unchanged pixel now receives no voltage at all. Only real black<->white
// transitions are driven. Measured on this panel in
// docs/reference/eink-hybrid-experiment.cpp; installed by hand because GxEPD2
// keeps its LUT private. SSD1680 layout: 5 rows x 12 (LUT0..LUT4), then 12
// groups x 7 (timing), then 9 config bytes = 153.
const uint8_t kGentlePartialLut[] = {
    0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT0 white->white  ZEROED
    0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT1 white->black
    0x40, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT2 black->white
    0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT3 black->black  ZEROED
    0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT4 VCOM
    0x0A, 0, 0, 0, 0, 0, 0x02,
    0x01, 0, 0, 0, 0, 0, 0x00,
    0x01, 0, 0, 0, 0, 0, 0x00,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

// GxEPD2_213_BN::lut_partial, byte for byte (GxEPD2 keeps its copy private),
// so the operator can flip between it and the gentle one from the menu and
// compare on the glass. Rows LUT0 and LUT3 carry the one-frame settle kick the
// gentle table removes.
const uint8_t kStockPartialLut[] = {
    0x00, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT0 white->white  1-frame VSH1 kick
    0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT1 white->black
    0x40, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT2 black->white
    0x00, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT3 black->black  1-frame VSL kick
    0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT4 VCOM
    0x0A, 0, 0, 0, 0, 0, 0x02,
    0x01, 0, 0, 0, 0, 0, 0x00,
    0x01, 0, 0, 0, 0, 0, 0x00,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

// Stock, with the black<->white drive run twice as long. GxEPD2's table was
// written for a black-and-white panel; this glass carries three pigments in a
// slower fluid, and 31 frames of VSL leave its black a dark grey, the red
// pigment not all the way back. Group 2 repeats group 0's 30 frames for pixels
// that change, so black gets 61 frames of VSL — and white as many of VSH1,
// which keeps a pixel toggled back and forth DC-balanced and whitens white
// too. The voltages stay the datasheet's own (VSH1 0x41, VSL 0x32, ±15 V):
// only the time changes, about 30 frames more per refresh. The one-frame kicks
// on unchanged pixels are stock's, and the idle frame that ends stock's table
// moves to group 3.
const uint8_t kDeepPartialLut[] = {
    0x00, 0x40, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT0 white->white  1-frame VSH1 kick
    0x80, 0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT1 white->black  VSL, groups 0-2
    0x40, 0x40, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT2 black->white  VSH1, groups 0-2
    0x00, 0x80, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT3 black->black  1-frame VSL kick
    0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // LUT4 VCOM
    0x0A, 0, 0, 0, 0, 0, 0x02,                     // group 0: 10 frames, 3 times
    0x01, 0, 0, 0, 0, 0, 0x00,                     // group 1: the kick, 1 frame
    0x0A, 0, 0, 0, 0, 0, 0x02,                     // group 2: 10 frames, 3 times more
    0x01, 0, 0, 0, 0, 0, 0x00,                     // group 3: 1 idle frame
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

// The SSD1680 LUT register is exactly this long; a short table would leave the
// tail of the previous waveform in place, silently.
constexpr size_t kLutBytes = 153;
static_assert(sizeof(kGentlePartialLut) == kLutBytes, "gentle LUT must be 153 bytes");
static_assert(sizeof(kStockPartialLut) == kLutBytes, "stock LUT must be 153 bytes");
static_assert(sizeof(kDeepPartialLut) == kLutBytes, "deep LUT must be 153 bytes");

const uint8_t* lutFor(Waveform waveform) {
    switch (waveform) {
        case Waveform::Stock: return kStockPartialLut;
        case Waveform::Deep: return kDeepPartialLut;
        case Waveform::Gentle:
        case Waveform::Count: break;
    }
    return kGentlePartialLut;
}

/// Writes a LUT straight to the controller (command 0x32).
///
/// GxEPD2 does not expose raw command writes, so this drives CS/DC and the SPI
/// bus itself — the same sequence GxEPD2 uses internally. Safe to call between
/// GxEPD2 operations: it opens and closes its own SPI transaction.
///
/// Persists because GxEPD2 only reloads its own LUT when it re-enters partial
/// mode (`_Init_Part`), which happens once at boot and never again unless the
/// panel is hibernated — which this firmware does not do.
void installLut(const uint8_t* lut) {
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(config::kEpdDc, LOW);
    digitalWrite(config::kEpdCs, LOW);
    SPI.transfer(0x32);
    digitalWrite(config::kEpdCs, HIGH);

    digitalWrite(config::kEpdDc, HIGH);
    digitalWrite(config::kEpdCs, LOW);
    for (size_t i = 0; i < kLutBytes; ++i) SPI.transfer(lut[i]);
    digitalWrite(config::kEpdCs, HIGH);
    SPI.endTransaction();
}
#endif  // EINK_PANEL_GENTLE_LUT

class GxEPD2Driver final : public DisplayDriver {
public:
    bool begin() override {
        // Custom SPI pins: MISO is unused by e-ink, hence -1.
        SPI.begin(config::kEpdSck, -1, config::kEpdMosi, config::kEpdCs);

        const uint32_t started = millis();
        enterMonoMode();

        if (display_.width() != EINK_WIDTH || display_.height() != EINK_HEIGHT) {
            logf(proto::LogLevel::Warn,
                 "[display] panel reports %dx%d but DisplayModel.h says %dx%d",
                 display_.width(), display_.height(), EINK_WIDTH, EINK_HEIGHT);
        }

        logf(proto::LogLevel::Info, "[display] %s, %dx%d, cleared to white in %lu ms",
             EINK_MODEL_NAME, display_.width(), display_.height(),
             static_cast<unsigned long>(millis() - started));
        return true;
    }

    bool clean() override {
#if EINK_PANEL_CAN_CLEAN && !EINK_PANEL_IS_3C
        // The mono waveform only moves black particles, so it can neither clear
        // red that is already on the glass nor fully re-saturate pigment that
        // has gone muddy. This borrows the panel's *tri-colour* waveform — the
        // one in its OTP — and drives the pigment to both limits before
        // settling on white: all black, all red, all white. Same three passes
        // as the standalone cleaner in src/tools/ClearPanel.cpp, which is where
        // the sequence was measured on this panel.
        //
        // Each pass is a full tri-colour refresh, ~15 s, so this blocks for
        // roughly 45 s. The busy callback keeps running throughout, so button
        // presses are still latched while it works.
        EINK_PANEL_CLEANER_CLASS cleaner(config::kEpdCs, config::kEpdDc, config::kEpdRst,
                                         config::kEpdBusy);
        cleaner.setBusyCallback(busyCallback_, busyContext_);
        // initial=true is correct here, and only here: this driver *wants* the
        // tri-colour OTP waveform, and under it RAM 0x26 really is the red plane.
        cleaner.init(0, /*initial=*/true, 50, false);

        cleaner.clearScreen(0x00, 0xFF);   // all black
        cleaner.clearScreen(0xFF, 0x00);   // all red
        cleaner.clearScreen(0xFF, 0xFF);   // all white

        // Hibernate resets the controller, so the mono init below starts from a
        // clean slate rather than on top of the tri-colour configuration.
        cleaner.hibernate();
        enterMonoMode();
        return true;
#else
        return false;
#endif
    }

    bool setWaveform(Waveform waveform) override {
#if EINK_PANEL_GENTLE_LUT
        if (!kAvoidFullUpdate) return false;
        waveform_ = waveform;
        // The panel is idle between refreshes, which is when GxEPD2 itself
        // writes 0x32, so the new table is simply in place for the next one.
        installLut(lutFor(waveform_));
        logf(proto::LogLevel::Info, "[display] mono LUT '%s' installed", waveformName(waveform_));
        return true;
#else
        (void)waveform;
        return false;
#endif
    }

    Waveform waveform() const override { return waveform_; }

    int16_t width() const override { return EINK_WIDTH; }
    int16_t height() const override { return EINK_HEIGHT; }

    bool supportsPartial() const override {
#if EINK_PANEL_SUPPORTS_PARTIAL
        return display_.epd2.hasFastPartialUpdate || display_.epd2.hasPartialUpdate;
#else
        return false;
#endif
    }

    void render(RefreshMode mode, const DrawFunction& draw) override {
        if (kAvoidFullUpdate) {
            // No full-update path exists here, so a Full request is served as a
            // white flash followed by the content — both on the mono LUT. That
            // is what clears accumulated ghosting without touching red.
            if (mode == RefreshMode::Full) paintWhite();
            drawFrame(/*partial=*/true, draw);
            return;
        }
        drawFrame(/*partial=*/mode == RefreshMode::Partial && supportsPartial(), draw);
    }

    void hibernate() override { display_.hibernate(); }
    const char* modelName() const override { return EINK_MODEL_NAME; }

    void setBusyCallback(void (*callback)(const void*), const void* context) override {
        // GxEPD2 calls this instead of delay(1) for as long as BUSY is held.
        // Remembered as well as forwarded, so the deep clean's temporary driver
        // can be given the same callback — 45 s with nothing sampling the
        // buttons would leave the box feeling dead.
        busyCallback_ = callback;
        busyContext_ = context;
        display_.epd2.setBusyCallback(callback, context);
    }

    Adafruit_GFX& metrics() override { return display_; }
    uint16_t ink() const override { return GxEPD_BLACK; }
    uint16_t paper() const override { return GxEPD_WHITE; }

private:
    /// True when the glass is 3-colour but driven in mono: every full-update
    /// path must then be avoided.
    static constexpr bool kAvoidFullUpdate = EINK_PANEL_OTP_IS_3C != 0;

    /// Kept so the deep clean's temporary driver can sample buttons too.
    void (*busyCallback_)(const void*) = nullptr;
    const void* busyContext_ = nullptr;

    /// Waveform currently in the LUT register; the menu can change it.
    Waveform waveform_ = config::kDefaultWaveform;

    void drawFrame(bool partial, const DrawFunction& draw) {
        if (partial) {
            display_.setPartialWindow(0, 0, EINK_WIDTH, EINK_HEIGHT);
        } else {
            display_.setFullWindow();
        }
        // GxEPD2 renders in bands on panels without a full framebuffer, so the
        // draw callback runs once per band.
        display_.firstPage();
        do {
            display_.fillScreen(GxEPD_WHITE);
            display_.setTextColor(GxEPD_BLACK);
            draw(display_);
        } while (display_.nextPage());
    }

    /// An all-white frame, on the same safe path as any other draw.
    void paintWhite() {
        drawFrame(/*partial=*/kAvoidFullUpdate || supportsPartial(),
                  [](Adafruit_GFX&) {});
    }

    /// Brings the panel up in mono mode and leaves it white on the softened
    /// waveform. Run at boot, and again after the deep clean — which leaves the
    /// controller holding the tri-colour configuration.
    ///
    /// serial_diag_bitrate = 0: GxEPD2 would otherwise call Serial.begin()
    /// itself, which resets the port the protocol is running on.
    void enterMonoMode() {
        // The `initial` flag is the critical one on 3-colour glass driven in
        // mono. It sets both _initial_write and _initial_refresh, and GxEPD2
        // turns either of those into a FULL update — which issues 0x22/0xF4 and
        // so loads the panel's OTP waveform. On this glass that waveform is the
        // tri-colour one, and under it RAM 0x26 is the RED plane rather than
        // "previous image": the panel comes up solid red.
        //
        // It bites twice, which is why `false` alone is not enough to remember:
        //   - init(initial=true) performs one full refresh immediately;
        //   - refresh(x, y, w, h) silently promotes the FIRST partial to a full
        //     one while _initial_refresh is still set.
        // initial=false clears both flags, so every update stays on the
        // MCU-supplied mono LUT.
        display_.init(0, /*initial=*/!kAvoidFullUpdate, 50, false);
        display_.setRotation(EINK_ROTATION);

        if (kAvoidFullUpdate) {
            // Prime both RAM planes to white without refreshing anything, so
            // the mono LUT starts from a known state and 0x26 holds white
            // instead of whatever survived the last power cycle — or, after a
            // deep clean, instead of the red plane the tri-colour pass wrote.
            display_.epd2.writeScreenBufferAgain(0xFF);
        }

        // Leave the panel white before any content is drawn, so boot reads as
        // "clear to white, then paint the content in black".
        paintWhite();

#if EINK_PANEL_GENTLE_LUT
        // Only now: paintWhite() ran the first partial, which is what makes
        // GxEPD2 load its own LUT and enter partial mode. Overwriting it here
        // means every subsequent partial uses the selected waveform. See
        // DisplayModel.h and include/Waveform.h.
        if (kAvoidFullUpdate) {
            installLut(lutFor(waveform_));
            logf(proto::LogLevel::Info, "[display] mono LUT '%s' installed",
                 waveformName(waveform_));
        }
#endif
    }

    PanelDisplay display_{EINK_PANEL_CLASS(
        config::kEpdCs, config::kEpdDc, config::kEpdRst, config::kEpdBusy)};
};

GxEPD2Driver driver;

}  // namespace

DisplayDriver& activeDriver() { return driver; }

}  // namespace ui

#endif  // EINK_MODEL == EINK_MODEL_MOCK
