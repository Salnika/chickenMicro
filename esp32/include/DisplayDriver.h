#pragma once

#include <Adafruit_GFX.h>
#include <stdint.h>

#include <functional>

#include "Waveform.h"

// ---------------------------------------------------------------------------
// The panel behind an interface.
//
// Everything above this file — Display, App, Controls — draws through
// Adafruit_GFX and never names a GxEPD2 class, so the panel is selected in one
// place (include/DisplayModel.h) and the rest of the firmware is unaffected.
// ---------------------------------------------------------------------------

namespace ui {

enum class RefreshMode : uint8_t {
    /// Fast, localised update. Falls back to Full on panels without it.
    Partial,
    /// Slow, complete update. Clears accumulated ghosting.
    Full,
};

/// Draw callback. Called once per page by drivers that render in bands, so it
/// must be repeatable and must not mutate state.
using DrawFunction = std::function<void(Adafruit_GFX&)>;

class DisplayDriver {
public:
    virtual ~DisplayDriver() = default;

    /// Brings the panel up. Returns false when it does not answer.
    virtual bool begin() = 0;

    virtual int16_t width() const = 0;
    virtual int16_t height() const = 0;
    virtual bool supportsPartial() const = 0;

    /// Runs one refresh cycle, invoking `draw` as many times as the panel needs.
    virtual void render(RefreshMode mode, const DrawFunction& draw) = 0;

    /// A canvas usable for *measurement only* — `getTextBounds` and friends.
    /// Drawing on it outside `render` has no defined effect. This is what lets
    /// the layout compute page counts without triggering a refresh.
    virtual Adafruit_GFX& metrics() = 0;

    /// Colour values differ between panels and the mock canvas, so the driver
    /// owns them rather than the layout hardcoding GxEPD2 macros.
    virtual uint16_t ink() const = 0;
    virtual uint16_t paper() const = 0;

    /// Puts the panel into deep sleep. E-ink holds its image without power.
    virtual void hibernate() = 0;

    /// Runs the panel's deep clean, if it has one: the slow full-screen pigment
    /// cycle that drives every particle to its limit and back, scrubbing
    /// ghosting and any stuck colour an ordinary refresh cannot reach.
    ///
    /// Blocks for tens of seconds and leaves the panel white. Returns false on
    /// a panel with nothing beyond a normal refresh, so the caller can fall
    /// back to one rather than claim it cleaned something.
    virtual bool clean() { return false; }

    /// Selects the partial-refresh waveform, on panels whose waveform the MCU
    /// supplies. Takes effect from the next refresh. Returns false on a panel
    /// that only has its built-in waveform, leaving it unchanged.
    virtual bool setWaveform(Waveform waveform) {
        (void)waveform;
        return false;
    }
    virtual Waveform waveform() const { return Waveform::Gentle; }

    /// Human-readable model name, for the boot log.
    virtual const char* modelName() const = 0;

    /// Called over and over while the panel is busy refreshing, in place of the
    /// driver's own wait. A refresh holds up the loop for a few hundred
    /// milliseconds; this is how the firmware keeps reading its buttons through
    /// it. A driver that never blocks can ignore it.
    virtual void setBusyCallback(void (*callback)(const void*), const void* context) {
        (void)callback;
        (void)context;
    }
};

/// Returns the driver selected by EINK_MODEL. Owned by the implementation; the
/// caller must not delete it.
DisplayDriver& activeDriver();

}  // namespace ui
