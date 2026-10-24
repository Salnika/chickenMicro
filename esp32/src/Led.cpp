#include "Led.h"

#include <Arduino.h>
#include <math.h>

#include "Config.h"

namespace ui {
namespace {

// PWM setup for the LED. 8-bit duty (0-255) at a few kHz — well above what the
// eye can see, so the breathe is smooth and the blink is clean.
constexpr uint32_t kPwmFrequency = 5000;
constexpr uint8_t kPwmResolution = 8;
constexpr uint8_t kPwmChannel = 0;   // arduino-esp32 2.x wants an explicit channel
constexpr uint8_t kDutyMax = 255;

/// Perceptual brightness for a linear 0..1 input.
///
/// The eye is roughly logarithmic, so a linear ramp looks like it snaps bright
/// and lingers dark. Squaring it turns an even ramp into an even-*looking* one,
/// which is what makes the pulse read as breathing rather than flickering.
uint8_t gamma(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    return static_cast<uint8_t>(level * level * kDutyMax + 0.5f);
}

}  // namespace

void Led::begin() {
    if (config::kLedPin < 0) return;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(config::kLedPin, kPwmFrequency, kPwmResolution);
#else
    ledcSetup(kPwmChannel, kPwmFrequency, kPwmResolution);
    ledcAttachPin(config::kLedPin, kPwmChannel);
#endif
    initialised_ = true;
    writeDuty(0);   // dark until something asks otherwise
}

void Led::update(uint32_t nowMs, LedMode mode) {
    if (!initialised_) return;

    if (mode != mode_) {
        mode_ = mode;
        modeSinceMs_ = nowMs;
    }
    const uint32_t elapsed = nowMs - modeSinceMs_;

    switch (mode_) {
        case LedMode::Off:
            writeDuty(0);
            break;

        case LedMode::Blink: {
            // On for a half-period, off for the next — starting lit, so the
            // first thing you see the instant a permission arrives is light.
            const bool on = (elapsed / config::kLedBlinkIntervalMs) % 2 == 0;
            writeDuty(on ? kDutyMax : 0);
            break;
        }

        case LedMode::Breathe: {
            // A raised cosine over the period: 0 -> 1 -> 0, smooth at both ends.
            const float phase =
                static_cast<float>(elapsed % config::kLedBreathePeriodMs) / config::kLedBreathePeriodMs;
            const float level = 0.5f - 0.5f * cosf(2.0f * static_cast<float>(M_PI) * phase);
            writeDuty(gamma(level));
            break;
        }
    }
}

void Led::writeDuty(uint8_t duty) {
    // Active-low boards light the LED when the pin is low, so the duty is
    // flipped rather than the logic everywhere else.
    const uint8_t out = config::kLedActiveHigh ? duty : static_cast<uint8_t>(kDutyMax - duty);
    if (out == currentDuty_) return;
    currentDuty_ = out;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(config::kLedPin, out);
#else
    ledcWrite(kPwmChannel, out);
#endif
}

}  // namespace ui
