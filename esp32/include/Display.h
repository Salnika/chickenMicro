#pragma once

#include <stdint.h>

#include "DisplayDriver.h"
#include "KeyLegend.h"
#include "State.h"

namespace ui {

/// The screens, the text layout, and the e-ink refresh policy.
///
/// Nothing here knows which panel is fitted: all drawing goes through
/// `DisplayDriver`. The refresh policy is the part that matters on e-ink —
/// redraws happen only when content changed, and a full refresh is forced
/// periodically to clear ghosting.
class Display {
public:
    explicit Display(DisplayDriver& driver) : driver_(driver) {}

    void begin();

    /// Number of pages the current screen needs, so navigation can clamp.
    uint16_t pageCountFor(const app::State& state) const;

    /// Draws the state's current screen, skipping the refresh when nothing on
    /// it changed, unless `force` is set.
    ///
    /// Returns false when a redraw is owed but was deferred by the rate limit;
    /// the caller must ask again rather than consider the screen up to date.
    bool draw(const app::State& state, uint32_t nowMs, bool force = false);

    /// Forces a full refresh on the next draw, even if nothing changed.
    void requestFullRefresh() { forceFull_ = true; }

    /// Runs the panel's deep clean and forgets what was on the glass, so the
    /// next draw repaints it. Blocks for tens of seconds. Returns false when
    /// the panel has no deep clean, leaving the screen untouched.
    bool clean();

    /// The partial-refresh waveform, where the panel lets the MCU choose one.
    bool setWaveform(Waveform waveform) { return driver_.setWaveform(waveform); }
    Waveform waveform() const { return driver_.waveform(); }

    /// Runs `callback` while the panel is busy refreshing, in place of the
    /// driver's own wait. See `DisplayDriver::setBusyCallback`.
    void onBusy(void (*callback)(const void*), const void* context) {
        driver_.setBusyCallback(callback, context);
    }

    int16_t width() const { return driver_.width(); }
    int16_t height() const { return driver_.height(); }

private:
    void drawConnecting(Adafruit_GFX& gfx, const app::State& state) const;
    void drawHome(Adafruit_GFX& gfx, const app::State& state) const;
    void drawSessions(Adafruit_GFX& gfx, const app::State& state) const;
    void drawMessage(Adafruit_GFX& gfx, const app::State& state) const;
    void drawMenu(Adafruit_GFX& gfx, const app::State& state) const;
    void drawOtpList(Adafruit_GFX& gfx, const app::State& state) const;
    void drawOtpCode(Adafruit_GFX& gfx, const app::State& state) const;
    void drawOtpSetup(Adafruit_GFX& gfx, const app::State& state) const;
    void drawPermission(Adafruit_GFX& gfx, const app::State& state) const;

    /// Title bar: the wordmark on the left, `right` as context, a double rule
    /// underneath, and the chicken pacing the empty lane between the two. Never
    /// a filled bar — see the note at the top of Display.cpp.
    void drawHeader(Adafruit_GFX& gfx, const app::State& state, const char* right) const;

    /// The key legend, from `keyLegendFor` — the same one the keycap lights
    /// follow. A key with no label is left blank, so a key that does nothing on
    /// this screen is never advertised, and the three others keep their places.
    void drawKeyBar(Adafruit_GFX& gfx, const KeyLegend& legend) const;

    /// Session name line above a body, drawn only when more than one session
    /// exists. Returns the y the body should start at.
    int16_t drawSessionContext(Adafruit_GFX& gfx, const app::State& state, const char* id) const;

    /// Body top for the current screen, which depends on whether the session
    /// context line is showing.
    int16_t bodyTopFor(const app::State& state) const;

    /// Wraps the permission's title into the shared title buffer and returns
    /// the y its payload starts at. Page 1 has less room for the payload than
    /// the pages after it, which is why the two have to be laid out together.
    int16_t layoutPermissionTitle(Adafruit_GFX& gfx, const app::State& state) const;

    /// Fingerprint of what is on screen *except* the chicken, so a step of the
    /// walk can be told apart from a real change.
    uint32_t contentHash(const app::State& state) const;

    DisplayDriver& driver_;
    uint32_t lastContentHash_ = 0;
    /// Content plus the chicken's position: what is actually on the glass.
    uint32_t lastFrameHash_ = 0;
    bool everDrawn_ = false;
    bool forceFull_ = true;
    uint16_t partialsSinceFull_ = 0;
    /// Chicken steps not yet counted toward `partialsSinceFull_`.
    uint16_t chickenSteps_ = 0;
    uint32_t lastDrawMs_ = 0;
    /// The connecting screen shares no layout with any other, so every pixel
    /// that differs flips on the way in and out. Both transitions are rare and
    /// are the cleanest moment to clear ghosting, so both force a full refresh.
    bool lastWasConnecting_ = false;
};

}  // namespace ui
