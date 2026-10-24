#pragma once

#include <stdint.h>

#include "ButtonBank.h"
#include "JoystickLogic.h"
#include "Protocol.h"

namespace input {

/// What the controls produced this tick.
struct ControlEvents {
    /// Joystick movement, or `Direction::None`.
    Direction direction = Direction::None;
    /// Joystick click: a short press, reported on release.
    bool select = false;
    /// Joystick held past the menu threshold — opens or closes the menu.
    bool selectHeld = false;

    bool approvePressed = false;
    bool denyPressed = false;
    bool stopPressed = false;
    bool actionPressed = false;
    /// ACTION held past the threshold — a secondary function, never a verdict.
    bool actionHeld = false;

    bool any() const {
        return direction != Direction::None || select || selectHeld || approvePressed ||
               denyPressed || stopPressed || actionPressed || actionHeld;
    }
};

/// The four buttons plus the joystick.
class Controls {
public:
    Controls();

    /// Configures the pins and calibrates the joystick centre. Must run with
    /// the stick at rest.
    void begin();

    /// Samples everything once and hands over what happened. Cheap enough to
    /// call every loop.
    ControlEvents poll(uint32_t nowMs);

    /// Reads the buttons without handing anything over.
    ///
    /// For use while the loop is stuck behind a panel refresh: presses are
    /// latched here and delivered by the next `poll`, so a press that starts and
    /// ends inside a refresh is not lost. The joystick is not read — a missed
    /// flick costs nothing, and its repeat timing belongs to `poll`.
    void sample(uint32_t nowMs);

    const JoystickCalibration& calibration() const { return calibration_; }

private:
    void calibrateJoystick();

    ButtonBank buttons_;

    JoystickCalibration calibration_;
    RepeatGate repeat_;
};

}  // namespace input
