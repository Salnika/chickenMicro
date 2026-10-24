#include "ButtonState.h"

namespace input {

void ButtonState::begin(bool down, uint32_t nowMs) {
    *this = ButtonState();
    edgeDown_ = down;
    down_ = down;
    rawDown_ = down;
    lastChangeMs_ = nowMs;
    downSinceMs_ = nowMs;
    releasedAtMs_ = nowMs;
    nowMs_ = nowMs;
}

void ButtonState::notePress(uint32_t atMs) {
    pressPending_ = true;
    pressAtValid_ = true;
    pressAtMs_ = atMs;
}

void ButtonState::noteEdge(bool down, uint32_t atMs, uint32_t debounceMs) {
    // Back to where we already were: a bounce that settled.
    if (down == edgeDown_) return;
    // Too soon after the last accepted edge: still bouncing.
    if (edgeSeen_ && atMs - lastEdgeMs_ < debounceMs) return;

    edgeSeen_ = true;
    lastEdgeMs_ = atMs;
    edgeDown_ = down;
    // Only the press edge is latched; a release is picked up from the level.
    if (down) notePress(atMs);
}

void ButtonState::sample(bool down, uint32_t nowMs, uint32_t debounceMs) {
    nowMs_ = nowMs;

    if (down != rawDown_) {
        rawDown_ = down;
        lastChangeMs_ = nowMs;
        return;   // wait for the level to settle
    }
    if (down == down_) return;
    if (nowMs - lastChangeMs_ < debounceMs) return;

    down_ = down;
    if (down_) {
        // Measure a hold from the real press, not from whenever the level got
        // sampled — but only a press latched since the last release belongs to
        // this one; an older timestamp would make a fresh press look held.
        const bool fresh =
            pressAtValid_ && static_cast<int32_t>(pressAtMs_ - releasedAtMs_) >= 0;
        downSinceMs_ = fresh ? pressAtMs_ : nowMs;
        holdReported_ = false;
    } else {
        releasedAtMs_ = nowMs;
        releasePending_ = true;
        // A release before the hold threshold closes a click. After the hold
        // fired, it closes a long press instead, and latches nothing.
        if (!holdReported_) clickPending_ = true;
    }
}

bool ButtonState::takePress() {
    if (!pressPending_) return false;
    pressPending_ = false;
    return true;
}

bool ButtonState::takeRelease() {
    if (!releasePending_) return false;
    releasePending_ = false;
    return true;
}

bool ButtonState::takeClick() {
    if (!clickPending_) return false;
    clickPending_ = false;
    return true;
}

bool ButtonState::takeHold(uint32_t holdMs) {
    if (!down_ || holdReported_) return false;
    if (nowMs_ - downSinceMs_ < holdMs) return false;
    holdReported_ = true;
    return true;
}

}  // namespace input
