#pragma once

#include <stdint.h>

namespace ui {

/// The partial-refresh waveform loaded into the SSD1680's LUT register
/// (command 0x32) while the panel is driven mono.
///
/// Which one suits the glass is a judgement made by eye on the panel, not in
/// code, so the on-screen menu switches between them at runtime and
/// `config::kDefaultWaveform` fixes the one loaded at boot. The byte tables
/// themselves live with the panel driver (DisplayDriver.cpp).
enum class Waveform : uint8_t {
    /// GxEPD2's stock partial LUT with its two "pixel unchanged" rows zeroed,
    /// so a settled pixel receives no voltage and nothing nudges the red
    /// pigment between refreshes. The trade: settled pixels are never
    /// re-kicked, so residue is not corrected either.
    Gentle,
    /// GxEPD2's stock partial LUT, byte for byte: a one-frame settle kick on
    /// every unchanged pixel keeps black saturated and white clean, at the cost
    /// of slowly agitating red on tri-colour glass.
    Stock,
    /// Stock with every black<->white transition driven twice as long: 61
    /// frames instead of 31, for a deeper black and a cleaner white on this
    /// slower three-pigment glass. Each refresh takes about twice as long.
    Deep,
    Count,
};

inline const char* waveformName(Waveform waveform) {
    switch (waveform) {
        case Waveform::Gentle: return "gentle";
        case Waveform::Stock: return "stock";
        case Waveform::Deep: return "deep";
        case Waveform::Count: break;
    }
    return "?";
}

/// The entry after `waveform`, wrapping — what the menu steps through.
inline Waveform nextWaveform(Waveform waveform) {
    const uint8_t next =
        static_cast<uint8_t>((static_cast<uint8_t>(waveform) + 1u) %
                             static_cast<uint8_t>(Waveform::Count));
    return static_cast<Waveform>(next);
}

}  // namespace ui
