#pragma once

#include <stdint.h>

namespace input {

/// What one button is doing: press, release, click and hold, debounced.
///
/// Hardware-free on purpose. Two very different wirings feed it — a GPIO pin
/// watched by its own interrupt, or an I2C expander read every few milliseconds
/// — and the rules that decide what a press *is* must not differ between them.
/// Being free of Arduino also means those rules run in `pio test -e native`.
///
/// It is fed from two sides, the way the hardware delivers information:
///  - **edges**, the instant something changed, which is what catches a tap
///    that starts and ends while the loop is stuck behind an e-ink refresh;
///  - **levels**, sampled whenever the loop gets round to it, which is what
///    measures a hold and decides that a release closed a click.
///
/// APPROVE and DENY act on `takePress()` only: a deliberate single press. They
/// must never be triggered by a hold or a repeat — see protocol/PROTOCOL.md §5.
class ButtonState {
public:
    /// Starts from the input's current level, so a key already held when the
    /// board boots is never reported as a fresh press.
    void begin(bool down, uint32_t nowMs);

    /// A press edge that the watcher has already debounced — a GPIO interrupt
    /// does that itself, in interrupt context.
    void notePress(uint32_t atMs);

    /// A raw edge from a watcher that cannot debounce: `down` is the level the
    /// input changed *to*. A change back to the level last accepted is bounce,
    /// and so is any change within `debounceMs` of the last accepted one — the
    /// rule that keeps release bounce, which also flicks the input low, from
    /// becoming a phantom press.
    void noteEdge(bool down, uint32_t atMs, uint32_t debounceMs);

    /// One sample of the level. A change only counts once it has held for
    /// `debounceMs`.
    void sample(bool down, uint32_t nowMs, uint32_t debounceMs);

    /// Consumes a pending press edge. True at most once per press.
    bool takePress();

    /// Consumes a pending release.
    bool takeRelease();

    /// Consumes a "click": a press released *without* first crossing the hold
    /// threshold. Reported on release, so a long press that opened a menu is
    /// never also read as a click. Pair it with `takeHold`, called every poll,
    /// so the hold can mark itself first.
    bool takeClick();

    /// Consumes the "held past the threshold" edge, once per press.
    bool takeHold(uint32_t holdMs);

    bool isDown() const { return down_; }

    /// How long the button has been down, or 0 when it is up.
    uint32_t heldMs(uint32_t nowMs) const { return down_ ? nowMs - downSinceMs_ : 0; }

private:
    // --- Edge side: what happened, the instant it happened -------------------
    bool edgeDown_ = false;
    bool edgeSeen_ = false;
    uint32_t lastEdgeMs_ = 0;
    bool pressPending_ = false;
    bool pressAtValid_ = false;
    uint32_t pressAtMs_ = 0;

    // --- Level side: where things settled, for hold and release -------------
    bool down_ = false;
    bool rawDown_ = false;
    uint32_t lastChangeMs_ = 0;
    uint32_t downSinceMs_ = 0;
    uint32_t releasedAtMs_ = 0;
    uint32_t nowMs_ = 0;
    bool releasePending_ = false;
    bool clickPending_ = false;
    bool holdReported_ = false;
};

}  // namespace input
