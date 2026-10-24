#pragma once

#include <stdint.h>

namespace ui {

/// What the onboard status LED is doing.
enum class LedMode : uint8_t {
    Off,       ///< nothing needs attention
    Blink,     ///< a permission is waiting for a verdict
    Breathe,   ///< not connected to the Mac yet — a slow pulse
};

/// Which mode the LED should be in.
///
/// A waiting verdict outranks everything, because that is the one thing a person
/// has to act on; failing that, a missing link breathes; otherwise the LED is
/// dark, so a lit box always means "look at me", never just "powered on". Pure
/// and header-only, so the priority is unit-tested on the host.
inline LedMode ledModeFor(bool connected, bool actionPending) {
    if (actionPending) return LedMode::Blink;
    if (!connected) return LedMode::Breathe;
    return LedMode::Off;
}

/// The onboard status LED, driven over PWM so it can breathe as well as blink.
///
/// Does nothing at all when `config::kLedPin` is -1.
class Led {
public:
    void begin();

    /// Call every loop. Renders `mode` onto the pin; only writes when the duty
    /// actually changes, so Off and the steady parts of a blink cost nothing.
    void update(uint32_t nowMs, LedMode mode);

private:
    void writeDuty(uint8_t duty);

    bool initialised_ = false;
    LedMode mode_ = LedMode::Off;
    /// When the current mode began, so blink and breathe start from a clean
    /// phase rather than wherever the free-running clock happens to be.
    uint32_t modeSinceMs_ = 0;
    int currentDuty_ = -1;   // -1: nothing written yet
};

}  // namespace ui
