#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Where the title-bar chicken stands, as a pure function of a step counter.
//
// Header-only and Arduino-free, like LinkTimeout.h, so the walk is tested on
// the host: `pio test -e native`.
// ---------------------------------------------------------------------------

namespace ui {

struct WalkPose {
    /// Offset from the left end of the lane, in pixels. Never past `travel`.
    int16_t x = 0;
    /// The way the chicken faces, which is the way it is about to walk.
    bool facingRight = true;
    /// The two leg frames alternate on every step, so it walks rather than
    /// slides.
    bool legsApart = true;
};

/// The chicken's pose at step `phase`, pacing back and forth over `travel`
/// pixels, `stepPx` at a time.
///
/// The position is derived from a counter rather than stored as pixels: the
/// lane is a different width on every screen, and this way the next draw just
/// maps the same step onto whatever lane it has.
inline WalkPose walkPose(uint32_t phase, int16_t travel, int16_t stepPx) {
    WalkPose pose;
    pose.legsApart = (phase % 2u) == 0u;
    if (travel <= 0 || stepPx <= 0) return pose;

    // Stops from one end to the other; the last one lands exactly on `travel`.
    const uint32_t reach = static_cast<uint32_t>(travel);
    const uint32_t stride = static_cast<uint32_t>(stepPx);
    const uint32_t steps = (reach + stride - 1u) / stride;

    const uint32_t cycle = phase % (2u * steps);
    const uint32_t stop = cycle <= steps ? cycle : 2u * steps - cycle;
    const uint32_t x = stop * stride;

    pose.x = static_cast<int16_t>(x > reach ? reach : x);
    // It turns round at each end instead of backing into the wall.
    pose.facingRight = cycle < steps;
    return pose;
}

}  // namespace ui
