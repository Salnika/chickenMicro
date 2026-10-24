#include "Controls.h"

#include <Arduino.h>

#include "Config.h"
#include "Log.h"

namespace input {

Controls::Controls()
    : repeat_(config::kJoystickRepeatDelayMs, config::kJoystickRepeatRateMs) {}

void Controls::begin() {
    buttons_.begin(millis());
    // Printed every boot because it is the one thing that cannot be checked from
    // outside the box: which cap sits on which input. Press the keys left to
    // right and the log should read deny, stop, approve, context.
    char wiring[160];
    buttons_.describe(wiring, sizeof(wiring));
    logf(proto::LogLevel::Info, "[boot] %s", wiring);

    // 12-bit range over the full 0–3.3 V span, so a centred stick reads near
    // mid-scale on both axes.
    analogReadResolution(12);
    analogSetPinAttenuation(config::kJoystickX, ADC_11db);
    analogSetPinAttenuation(config::kJoystickY, ADC_11db);

    calibrateJoystick();
}

void Controls::calibrateJoystick() {
    // The resting centre is measured, never assumed to be 2048: these modules
    // sit anywhere from ~1800 to ~2200 and drift with supply voltage.
    long sumX = 0;
    long sumY = 0;
    for (int sample = 0; sample < config::kJoystickCalibrationSamples; ++sample) {
        sumX += analogRead(config::kJoystickX);
        sumY += analogRead(config::kJoystickY);
        delay(2);
    }
    calibration_.centerX = static_cast<int>(sumX / config::kJoystickCalibrationSamples);
    calibration_.centerY = static_cast<int>(sumY / config::kJoystickCalibrationSamples);
    calibration_.deadZone = config::kJoystickDeadZone;

    logf(proto::LogLevel::Info, "[input] joystick centre x=%d y=%d, dead zone %d",
         calibration_.centerX, calibration_.centerY, calibration_.deadZone);

    // A centre far from mid-scale usually means the stick was held during boot,
    // or that VCC is on 5 V instead of 3V3.
    if (calibration_.centerX < 1200 || calibration_.centerX > 2900 ||
        calibration_.centerY < 1200 || calibration_.centerY > 2900) {
        logMessage(proto::LogLevel::Warn,
                   "[input] joystick centre looks off — was the stick held at boot, "
                   "or is VCC on 5V instead of 3V3?");
    }
}

void Controls::sample(uint32_t nowMs) {
    buttons_.sample(nowMs);
}

ControlEvents Controls::poll(uint32_t nowMs) {
    ControlEvents events;

    sample(nowMs);

    // APPROVE and DENY are read as single deliberate presses only. No hold, no
    // repeat, no double click — see protocol/PROTOCOL.md §5.
    events.approvePressed = buttons_[ButtonId::Approve].takePress();
    events.denyPressed = buttons_[ButtonId::Deny].takePress();
    events.stopPressed = buttons_[ButtonId::Stop].takePress();
    events.actionPressed = buttons_[ButtonId::Action].takePress();
    events.actionHeld = buttons_[ButtonId::Action].takeHold(config::kButtonHoldMs);
    // The joystick click is a click-or-hold: takeHold must run first so it can
    // mark a long press before takeClick decides the release was not a click.
    events.selectHeld = buttons_[ButtonId::Joystick].takeHold(config::kMenuHoldMs);
    events.select = buttons_[ButtonId::Joystick].takeClick();

    const Direction raw = resolveDirection(
        analogRead(config::kJoystickX),
        analogRead(config::kJoystickY),
        calibration_,
        config::kJoystickInvertX,
        config::kJoystickInvertY,
        config::kJoystickSwapAxes);
    events.direction = repeat_.update(raw, nowMs);

    return events;
}

}  // namespace input
