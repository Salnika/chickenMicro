#include "KeyLegend.h"

#include "Config.h"

namespace ui {
namespace {

void set(KeyLegend& legend, Key key, const char* label, KeyRole role) {
    KeyHint& hint = legend.keys[static_cast<uint8_t>(key)];
    hint.label = label;
    hint.role = role;
}

/// `colour` scaled to `level` out of 255, rounded.
Rgb scaled(const uint8_t (&colour)[3], uint8_t level) {
    const auto channel = [level](uint8_t value) {
        return static_cast<uint8_t>((static_cast<uint16_t>(value) * level + 127) / 255);
    };
    Rgb out;
    out.r = channel(colour[0]);
    out.g = channel(colour[1]);
    out.b = channel(colour[2]);
    return out;
}

}  // namespace

bool isBusy(proto::ClaudeState state) {
    return state == proto::ClaudeState::Thinking || state == proto::ClaudeState::Writing ||
           state == proto::ClaudeState::WaitingPermission;
}

KeyLegend keyLegendFor(const app::State& state) {
    KeyLegend legend;
    switch (state.screen()) {
        case app::Screen::Connecting:
            // Nothing to press until the Mac is talking.
            break;

        case app::Screen::Home:
            if (isBusy(state.claudeState())) set(legend, Key::Square, "STOP", KeyRole::Stop);
            if (state.sessionCount() > 1) set(legend, Key::Return, "LIST", KeyRole::Navigate);
            break;

        case app::Screen::Sessions:
            if (state.sessionCount() == 0) {
                set(legend, Key::Return, "BACK", KeyRole::Navigate);
                break;
            }
            // STOP acts on the highlighted row here: the cursor is what the screen
            // is pointing at, so that is what the key must act on.
            if (isBusy(state.sessionAt(state.cursor()).state)) {
                set(legend, Key::Square, "STOP", KeyRole::Stop);
            }
            set(legend, Key::Return, "OPEN", KeyRole::Navigate);
            break;

        case app::Screen::Message:
            if (isBusy(state.claudeState())) set(legend, Key::Square, "STOP", KeyRole::Stop);
            set(legend, Key::Return, "BACK", KeyRole::Navigate);
            break;

        case app::Screen::Menu:
            set(legend, Key::Cross, "BACK", KeyRole::Navigate);
            set(legend, Key::Check, "SELECT", KeyRole::Navigate);
            break;

        case app::Screen::OtpList:
            set(legend, Key::Cross, "BACK", KeyRole::Navigate);
            // A copy needs an account to copy and the Mac's clock to make its code.
            if (state.otpCount() > 0 && state.hasClock()) {
                set(legend, Key::Check, "COPY", KeyRole::Navigate);
            }
            break;

        case app::Screen::OtpCode:
            set(legend, Key::Cross, "BACK", KeyRole::Navigate);
            if (state.hasClock()) set(legend, Key::Check, "AGAIN", KeyRole::Navigate);
            break;

        case app::Screen::OtpSetup:
            set(legend, Key::Cross, "DONE", KeyRole::Navigate);
            break;

        case app::Screen::Permission: {
            // Only the verdicts Claude actually offers are labelled, so the box
            // never invites a press that cannot be honoured.
            const app::ActivePermission& permission = state.permission();
            if (permission.offersDeny) set(legend, Key::Cross, "DENY", KeyRole::Deny);
            set(legend, Key::Square, "STOP", KeyRole::Stop);
            if (permission.offersApprove) set(legend, Key::Check, "ALLOW", KeyRole::Approve);
            if (state.pageCount() > 1) set(legend, Key::Return, "MORE", KeyRole::Navigate);
            break;
        }
    }
    return legend;
}

KeyLightFrame keyLightsFor(const KeyLegend& legend, const bool (&flashing)[kKeyCount]) {
    KeyLightFrame frame;
    for (uint8_t index = 0; index < kKeyCount; ++index) {
        Rgb& light = frame.keys[index];
        if (flashing[index]) {
            // The press flash wins over everything: it is the key saying "felt it".
            light = scaled(config::kKeyColourFlash, config::kKeyLightFlashLevel);
            continue;
        }
        switch (legend.keys[index].role) {
            case KeyRole::None: break;   // dark: a press would do nothing
            case KeyRole::Navigate:
                light = scaled(config::kKeyColourNavigate, config::kKeyLightNavigateLevel);
                break;
            case KeyRole::Stop:
                light = scaled(config::kKeyColourStop, config::kKeyLightLevel);
                break;
            case KeyRole::Deny:
                light = scaled(config::kKeyColourDeny, config::kKeyLightLevel);
                break;
            case KeyRole::Approve:
                light = scaled(config::kKeyColourApprove, config::kKeyLightLevel);
                break;
        }
    }
    return frame;
}

KeyLightFrame keyLightsForTest(uint8_t step) {
    KeyLightFrame frame;
    if (step >= kKeyLightTestSteps) return frame;   // the sweep is over: all dark
    // All four first: the one thing that says the chain is wired, before the
    // walk that says which LED is which.
    if (step == 0) {
        for (uint8_t index = 0; index < kKeyCount; ++index) {
            frame.keys[index] = scaled(config::kKeyColourFlash, config::kKeyLightTestLevel);
        }
        return frame;
    }
    // Then one key at a time, in the legend's own colours, so the sweep doubles
    // as a reminder of what each key does: refuse, stop, authorise, move around.
    const uint8_t key = static_cast<uint8_t>(step - 1);
    switch (static_cast<Key>(key)) {
        case Key::Cross:
            frame.keys[key] = scaled(config::kKeyColourDeny, config::kKeyLightTestLevel);
            break;
        case Key::Square:
            frame.keys[key] = scaled(config::kKeyColourStop, config::kKeyLightTestLevel);
            break;
        case Key::Check:
            frame.keys[key] = scaled(config::kKeyColourApprove, config::kKeyLightTestLevel);
            break;
        case Key::Return:
            frame.keys[key] = scaled(config::kKeyColourNavigate, config::kKeyLightTestLevel);
            break;
    }
    return frame;
}

}  // namespace ui
