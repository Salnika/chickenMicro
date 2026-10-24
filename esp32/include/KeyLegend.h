#pragma once

#include <stdint.h>

#include "State.h"

namespace ui {

/// The four front keys, in the order their caps read left to right.
///
/// The enum is the layout, not the wiring: `Config.h` decides which input sits
/// under which cap, and both the key bar and the keycap lights follow this order,
/// so what the box offers is always what the finger above it does.
enum class Key : uint8_t {
    Cross = 0,    ///< deny
    Square = 1,   ///< stop
    Check = 2,    ///< approve
    Return = 3,   ///< contextual: open / back / next page
};
constexpr uint8_t kKeyCount = 4;

/// What pressing a key does on the screen being shown.
enum class KeyRole : uint8_t {
    None,       ///< nothing: no label, light off
    Navigate,   ///< open, back, select, next page — moves around the box
    Stop,       ///< interrupts the running turn
    Deny,       ///< refuses the request on screen
    Approve,    ///< authorises the request on screen
};

struct KeyHint {
    const char* label = nullptr;
    KeyRole role = KeyRole::None;
};

/// One hint per key, in keycap order.
struct KeyLegend {
    KeyHint keys[kKeyCount];

    const KeyHint& operator[](Key key) const { return keys[static_cast<uint8_t>(key)]; }
};

/// What each key does right now: the single source of truth for the key bar
/// drawn along the bottom of the panel *and* for the light under each keycap.
///
/// A key that does nothing on this screen gets no label and no light, never a
/// label whose press would be refused. Hardware-free, so every screen's legend
/// is checked in `pio test -e native`.
KeyLegend keyLegendFor(const app::State& state);

/// Whether a session in `state` is doing something STOP can interrupt.
bool isBusy(proto::ClaudeState state);

// ---------------------------------------------------------------------------
// Keycap lights
// ---------------------------------------------------------------------------

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
    bool operator!=(const Rgb& other) const { return !(*this == other); }
};

/// The colour under each keycap, in keycap order.
struct KeyLightFrame {
    Rgb keys[kKeyCount];

    bool operator==(const KeyLightFrame& other) const {
        for (uint8_t index = 0; index < kKeyCount; ++index) {
            if (keys[index] != other.keys[index]) return false;
        }
        return true;
    }
    bool operator!=(const KeyLightFrame& other) const { return !(*this == other); }
};

/// The lights for `legend`: a key is lit if and only if it has a role, in that
/// role's colour; `flashing` keys show the press flash instead. Hardware-free.
KeyLightFrame keyLightsFor(const KeyLegend& legend, const bool (&flashing)[kKeyCount]);

/// Steps in the keycap self-test: all four together, then each on its own.
constexpr uint8_t kKeyLightTestSteps = kKeyCount + 1;

/// The frame for step `step` of that self-test: every key lit at step 0, which
/// is what says the chain is wired at all, then one key per step in the colours
/// the legend uses, which says which LED sits under which key. Everything is
/// dark once the last step has passed.
KeyLightFrame keyLightsForTest(uint8_t step);

}  // namespace ui
