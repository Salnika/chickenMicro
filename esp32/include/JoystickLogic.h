#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Joystick decision logic, with no hardware in it.
//
// Kept Arduino-free so the dead zone, the dominant-axis rule and the repeat
// timing can be unit-tested on the host: `pio test -e native`.
// ---------------------------------------------------------------------------

namespace input {

/// Resting position measured at boot. The centre is never assumed to be 2048:
/// these modules sit anywhere from ~1800 to ~2200 and drift with supply.
struct JoystickCalibration {
    int centerX = 2048;
    int centerY = 2048;
    int deadZone = 700;
};

enum class Direction : uint8_t {
    None,
    Up,
    Down,
    Left,
    Right,
};

/// Resolves a raw ADC pair into at most one direction.
///
/// Orientation is applied in two steps. `swapAxes` first maps the two channels
/// onto the logical axes, for a module mounted a quarter turn from the board's
/// X/Y labels; `invertX` / `invertY` then flip each logical axis. So the two
/// settings stay independent: swapping never changes what the inversions mean.
///
/// Diagonals are made predictable by picking the dominant axis: with
/// `abs(dx) > abs(dy)` the movement is horizontal, otherwise vertical. So a
/// diagonal push never fires two events, and never alternates.
Direction resolveDirection(
    int rawX,
    int rawY,
    const JoystickCalibration& calibration,
    bool invertX = false,
    bool invertY = false,
    bool swapAxes = false);

/// Turns a stream of directions into discrete events, applying the initial
/// delay and the repeat rate.
///
/// Separate from the reading so it can be driven by a fake clock in tests.
class RepeatGate {
public:
    RepeatGate(uint32_t initialDelayMs, uint32_t repeatRateMs)
        : initialDelayMs_(initialDelayMs), repeatRateMs_(repeatRateMs) {}

    /// Returns the direction to act on, or `None`.
    ///
    /// A fresh direction fires immediately; holding it fires again after
    /// `initialDelayMs`, then every `repeatRateMs`. Releasing to centre resets,
    /// so a flick is always exactly one event.
    Direction update(Direction current, uint32_t nowMs);

    void reset();

private:
    uint32_t initialDelayMs_;
    uint32_t repeatRateMs_;
    Direction held_ = Direction::None;
    uint32_t heldSinceMs_ = 0;
    uint32_t lastFireMs_ = 0;
    bool repeating_ = false;
};

}  // namespace input
