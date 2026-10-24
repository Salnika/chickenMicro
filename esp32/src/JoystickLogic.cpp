#include "JoystickLogic.h"

namespace input {
namespace {

int absolute(int value) { return value < 0 ? -value : value; }

}  // namespace

Direction resolveDirection(
    int rawX,
    int rawY,
    const JoystickCalibration& calibration,
    bool invertX,
    bool invertY,
    bool swapAxes) {
    int dx = rawX - calibration.centerX;
    int dy = rawY - calibration.centerY;
    // Each delta carries its own centre, so the two can simply trade places: the
    // channels land on the logical axes before anything is flipped.
    if (swapAxes) {
        const int horizontal = dy;
        dy = dx;
        dx = horizontal;
    }
    if (invertX) dx = -dx;
    if (invertY) dy = -dy;

    const bool xActive = absolute(dx) >= calibration.deadZone;
    const bool yActive = absolute(dy) >= calibration.deadZone;
    if (!xActive && !yActive) return Direction::None;

    // Dominant axis wins, so a diagonal is one predictable direction rather
    // than two competing events.
    if (xActive && (!yActive || absolute(dx) > absolute(dy))) {
        return dx > 0 ? Direction::Right : Direction::Left;
    }
    return dy > 0 ? Direction::Up : Direction::Down;
}

Direction RepeatGate::update(Direction current, uint32_t nowMs) {
    if (current == Direction::None) {
        reset();
        return Direction::None;
    }

    // A new direction fires at once: the box should feel immediate.
    if (current != held_) {
        held_ = current;
        heldSinceMs_ = nowMs;
        lastFireMs_ = nowMs;
        repeating_ = false;
        return current;
    }

    // Same direction still held. Wait out the initial delay, then repeat at a
    // controlled rate so a long push scrolls instead of firing once.
    const uint32_t sinceLastFire = nowMs - lastFireMs_;
    if (!repeating_) {
        if (nowMs - heldSinceMs_ >= initialDelayMs_) {
            repeating_ = true;
            lastFireMs_ = nowMs;
            return current;
        }
        return Direction::None;
    }
    if (sinceLastFire >= repeatRateMs_) {
        lastFireMs_ = nowMs;
        return current;
    }
    return Direction::None;
}

void RepeatGate::reset() {
    held_ = Direction::None;
    heldSinceMs_ = 0;
    lastFireMs_ = 0;
    repeating_ = false;
}

}  // namespace input
