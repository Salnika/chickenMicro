// The five inputs on five GPIO pins, each watched by its own interrupt.
// Used by every board except the ESP32-C3 SuperMini — see ButtonBank.h.

#include "ButtonBank.h"

#if !CM_INPUTS_ON_MCP23008

#include <Arduino.h>
#include <esp_timer.h>
#include <stdio.h>

namespace input {
namespace {

/// The GPIO behind each input, in ButtonId order.
constexpr uint8_t kPins[kButtonCount] = {
    config::kButtonDeny,
    config::kButtonStop,
    config::kButtonApprove,
    config::kButtonAction,
    config::kJoystickSwitch,
};

/// Milliseconds since boot, safe to read from an interrupt.
///
/// `esp_timer_get_time()` lives in IRAM and touches no flash, so it cannot fault
/// while the cache is disabled the way `millis()` occasionally can inside an ISR.
inline uint32_t nowMsFromIsr() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

// CHANGE, not FALLING: the ISR reads the level and tracks it, so release bounce
// — which also flicks the pin low — cannot masquerade as a press. Same rule as
// ButtonState::noteEdge, run here because it has to run in interrupt context.
void IRAM_ATTR ButtonBank::isr(void* arg) {
    PinWatch& pin = *static_cast<PinWatch*>(arg);
    const bool low = digitalRead(pin.gpio) == LOW;
    // Same level as last accepted: a bounce back to where we already were.
    if (low == pin.isrDown) return;
    // Within the bounce window of the last accepted edge: still settling.
    const uint32_t now = nowMsFromIsr();
    if (now - pin.lastIsrMs < config::kButtonDebounceMs) return;

    pin.lastIsrMs = now;
    pin.isrDown = low;
    // Only the high->low transition is a press. A release latches nothing; the
    // loop picks it up from the level.
    if (low) {
        pin.pressAtMs = now;
        pin.pressCount = pin.pressCount + 1;
    }
}

bool ButtonBank::begin(uint32_t nowMs) {
    for (uint8_t index = 0; index < kButtonCount; ++index) {
        PinWatch& pin = pins_[index];
        pin.gpio = kPins[index];
        // INPUT_PULLUP: the button pulls the pin to GND, so pressed reads LOW.
        pinMode(pin.gpio, INPUT_PULLUP);
        const bool down = digitalRead(pin.gpio) == LOW;
        pin.isrDown = down;
        states_[index].begin(down, nowMs);
        // attachInterruptArg carries the pin's own record, so one static ISR
        // serves all five.
        attachInterruptArg(digitalPinToInterrupt(pin.gpio), &ButtonBank::isr, &pin, CHANGE);
    }
    return true;
}

void ButtonBank::sample(uint32_t nowMs) {
    for (uint8_t index = 0; index < kButtonCount; ++index) {
        PinWatch& pin = pins_[index];
        const uint32_t count = pin.pressCount;
        if (count != pin.handledCount) {
            pin.handledCount = count;
            states_[index].notePress(pin.pressAtMs);
        }
        states_[index].sample(digitalRead(pin.gpio) == LOW, nowMs, config::kButtonDebounceMs);
    }
}

void ButtonBank::describe(char* out, size_t capacity) const {
    snprintf(out, capacity,
             "keys X=GPIO%u deny, #=GPIO%u stop, v=GPIO%u approve, <-=GPIO%u action, "
             "joystick click=GPIO%u",
             kPins[0], kPins[1], kPins[2], kPins[3], kPins[4]);
}

}  // namespace input

#endif  // !CM_INPUTS_ON_MCP23008
