#pragma once

#include <stdint.h>

#include "KeyLegend.h"

namespace ui {

/// The four WS2812B LEDs under the keycaps, in one chain.
///
/// Each shows what its key does right now, straight from `keyLegendFor` — the
/// legend the panel draws — so a key is lit exactly when the screen offers it,
/// in the colour of what a press will do. A press flashes its key white on top.
///
/// Writes to the chain only when a colour actually changes. Driven through the
/// RMT peripheral, so a write never holds interrupts off long enough for a
/// button edge to be missed.
class KeyLights {
public:
    /// Starts the chain dark. Does nothing on a board without keycap lights
    /// (kKeyLightPin = -1).
    void begin();

    /// Lights each keycap in turn, then all four, then leaves them dark.
    ///
    /// Blocking, and called at boot where there is nothing else to do yet. It
    /// is the only thing that answers "are they wired?": at rest every key is
    /// dark, so a chain nobody connected looks just like one with nothing to
    /// say. kKeyLightTestStepMs = 0 skips it.
    void selfTest();

    /// Flashes `key` white for kKeyLightFlashMs: the box registered its press.
    void flash(Key key, uint32_t nowMs);

    /// Shows `legend`, plus any flash still running.
    void update(uint32_t nowMs, const KeyLegend& legend);

private:
    bool flashing_[kKeyCount] = {};
    uint32_t flashUntilMs_[kKeyCount] = {};
    KeyLightFrame shown_;
    bool started_ = false;
};

}  // namespace ui
