#pragma once

#include <stdint.h>

namespace app {

/**
 * Whether the link counts as up, given when the last inbound frame arrived.
 *
 * Looks trivial, and is not: both timestamps are `uint32_t`, and the caller's
 * `now` can legitimately be *older* than `lastInbound`. `App::loop` samples
 * `millis()` once at the top, then `SerialProtocol::poll` dispatches frames
 * whose handlers stamp `lastInbound` with a fresh, later `millis()`. The naive
 *
 *     (now - lastInbound) < timeout
 *
 * then underflows to ~4.29e9 and declares the link dead once per received
 * frame — the link flaps at exactly the frame rate.
 *
 * Casting the modular difference to `int32_t` fixes it: a few milliseconds of
 * "negative" elapsed time reads as a small negative number rather than a huge
 * positive one, and the arithmetic stays correct across the 49.7-day `millis()`
 * rollover, which a plain `now >= lastInbound` guard would break.
 *
 * Header-only so the native test build can use it without linking the firmware.
 */
inline bool linkIsUp(uint32_t nowMs, uint32_t lastInboundMs, uint32_t timeoutMs) {
    if (lastInboundMs == 0) return false;   // nothing has ever arrived
    const int32_t elapsed = static_cast<int32_t>(nowMs - lastInboundMs);
    return elapsed < static_cast<int32_t>(timeoutMs);
}

}  // namespace app
