#include "App.h"

#include <Arduino.h>
#include <string.h>

#include "Config.h"
#include "LinkTimeout.h"
#include "DisplayModel.h"
#include "Log.h"

namespace app {
namespace {

constexpr const char* kAppName = "chickenmicro-fw";
constexpr const char* kAppVersion = "1.1.0";

bool isOtpScreen(Screen screen) {
    return screen == Screen::OtpList || screen == Screen::OtpCode || screen == Screen::OtpSetup;
}

}  // namespace

App::App()
    : display_(ui::activeDriver()), armingDelayMs_(config::kApproveArmingDelayMs) {}

void App::setup() {
    // A full e-ink refresh blocks this loop for hundreds of milliseconds, and
    // nothing drains the UART while it does. The default 256-byte RX buffer
    // holds ~22 ms at 115200, so inbound frames were being shredded mid-refresh.
    // 4 KB covers ~350 ms of traffic; the buffer must be sized before begin().
    Serial.setRxBufferSize(config::kSerialRxBufferBytes);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
    // Native USB (ESP32-C3, ESP32-S3): its TX ring defaults to 256 bytes, less
    // than one full frame (768), so a frame could only go out in pieces, each
    // piece waiting for the host. Room for a whole frame means a write returns
    // at once while the Mac is reading; the timeout bounds the stall when it is
    // not, so the box stays responsive with no daemon attached.
    Serial.setTxBufferSize(config::kSerialTxBufferBytes);
    Serial.setTxTimeoutMs(config::kSerialTxTimeoutMs);
#endif
    Serial.begin(config::kSerialBaud);
    // Give the USB-UART bridge a moment before the first frame, so the boot
    // log is not lost.
    delay(300);

    logf(proto::LogLevel::Info, "[boot] %s %s, panel %s", kAppName, kAppVersion, EINK_MODEL_NAME);

    display_.begin();
    led_.begin();
    keyLights_.begin();
    // Two seconds that say whether the chain is wired, and in which order.
    keyLights_.selfTest();
    controls_.begin();   // logs which input carries which key
    otpStore_.begin();
    refreshOtpAccounts();
    settings_.begin();
    state_.setPermissionMode(settings_.permissionMode());
    // Only once the inputs are configured: the callback reads them.
    display_.onBusy(&App::sampleWhileBusy, this);
    link_.begin(*this);
    link_.sendHello(kAppName, kAppVersion);
    announceMode();

    logMessage(proto::LogLevel::Info, "[state] waiting for the Mac");
    waitingSinceMs_ = millis();
    refreshPageCount();
    display_.draw(state_, millis(), /*force=*/true);
}

void App::loop() {
    const uint32_t nowMs = millis();

    link_.poll();
    handleControls(nowMs);
    updateLinkState(nowMs);
    tickConnecting(nowMs);
    tickChicken(nowMs);
    tickOtp(nowMs);
    tickVerdict(nowMs);
    // Blink while a verdict is waiting, breathe while waiting for the Mac, dark
    // otherwise. The LED is separate from the e-ink, so it keeps its rhythm even
    // while nothing on the panel changes.
    led_.update(nowMs, ui::ledModeFor(state_.linkUp(), state_.actionPending()));

    // Settle which screen owns the display first, so the keycaps light up the
    // moment a permission arrives rather than after the panel's slow refresh.
    // The draw below reconciles too; that second call finds nothing to do.
    if (dirty_ && state_.reconcileScreen()) refreshPageCount();
    keyLights_.update(nowMs, ui::keyLegendFor(state_));

    if (nowMs - lastPingMs_ >= config::kPingIntervalMs) {
        lastPingMs_ = nowMs;
        link_.sendPing(++pingSeq_);
    }

    bool refreshed = false;
    if (dirty_) {
        // `reconcileScreen` enforces the screens that own the display before
        // anything is drawn.
        if (state_.reconcileScreen()) refreshPageCount();
        // The display may defer a redraw to its rate limit; the flag stays set
        // until the redraw has really happened, so no change is dropped.
        if (display_.draw(state_, nowMs)) {
            dirty_ = false;
            refreshed = true;
        }
    }

    // A refresh blocks the loop for a few hundred milliseconds; a press caught by
    // the interrupt during it is waiting to be acted on, so loop straight back
    // rather than sleeping first. Otherwise a short idle keeps the ADC and UART
    // quiet without adding noticeable button latency.
    if (!refreshed) delay(4);
}

void App::markChanged() {
    dirty_ = true;
    lastActivityMs_ = millis();
}

void App::sampleWhileBusy(const void* context) {
    // GxEPD2 calls this in place of its own 1 ms delay for as long as the panel
    // reports busy: a few hundred milliseconds per refresh, once a second while
    // the chicken walks. Without it, a press that started and ended inside a
    // refresh never reached the loop — and STOP gets pressed while Claude is
    // working, which is exactly when the chicken is walking.
    const_cast<App*>(static_cast<const App*>(context))->controls_.sample(millis());
    delay(1);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void App::handleControls(uint32_t nowMs) {
    const input::ControlEvents events = controls_.poll(nowMs);
    if (!events.any()) return;
    lastActivityMs_ = nowMs;

    // Every keycap press flashes its own light, whatever the screen: a press the
    // box registered is one you can see, before anything else happens.
    if (events.denyPressed) keyLights_.flash(ui::Key::Cross, nowMs);
    if (events.stopPressed) keyLights_.flash(ui::Key::Square, nowMs);
    if (events.approvePressed) keyLights_.flash(ui::Key::Check, nowMs);
    if (events.actionPressed) keyLights_.flash(ui::Key::Return, nowMs);

    // Any press means someone is at the desk: start the connecting indicator
    // moving again, so a box that went still does not look dead.
    if (state_.screen() == Screen::Connecting) {
        waitingSinceMs_ = nowMs;
        markDirty();
    }

    // A long joystick press toggles the menu from anywhere it is allowed. Handled
    // first, and on its own, so the release that follows is never also acted on.
    if (events.selectHeld) {
        toggleMenu();
        return;
    }

    // While the menu is up it owns every control.
    if (state_.screen() == Screen::Menu) {
        handleMenuControls(events);
        return;
    }

    // So do the one-time-code screens reached from it.
    if (isOtpScreen(state_.screen())) {
        handleOtpControls(events, nowMs);
        return;
    }

    if (events.direction != input::Direction::None) {
        handleNavigation(events.direction, nowMs);
    }

    // The joystick click and the return key are the same gesture.
    if (events.select) {
        logMessage(proto::LogLevel::Info, "[input] select");
        link_.sendNavigation(proto::Navigation::Select);
        handleEnter();
    }

    // The two critical buttons. Single deliberate press only.
    if (events.approvePressed) {
        logMessage(proto::LogLevel::Info, "[input] approve");
        sendVerdict(proto::Action::Approve, nowMs);
    }
    if (events.denyPressed) {
        logMessage(proto::LogLevel::Info, "[input] deny");
        sendVerdict(proto::Action::Deny, nowMs);
    }

    if (events.stopPressed) {
        logMessage(proto::LogLevel::Info, "[input] stop");
        sendStop();
    }

    if (events.actionPressed) {
        logMessage(proto::LogLevel::Info, "[input] context");
        handleEnter();
    }

    // Secondary function on a hold: clear ghosting. Deliberately not a verdict.
    if (events.actionHeld) {
        logMessage(proto::LogLevel::Info, "[input] context held — full refresh");
        display_.requestFullRefresh();
        markDirty();
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

void App::toggleMenu() {
    const Screen screen = state_.screen();
    if (screen == Screen::Menu) {
        closeMenu();
    } else if (isOtpScreen(screen)) {
        backToMenu();
    } else {
        openMenu();
    }
}

void App::openMenu() {
    // Only from the browsable screens: Connecting and Permission own the display
    // and must not be pushed aside by a menu.
    const Screen screen = state_.screen();
    if (screen != Screen::Home && screen != Screen::Sessions && screen != Screen::Message) return;

    screenBeforeMenu_ = screen;
    state_.resetMenuCursor();
    logMessage(proto::LogLevel::Info, "[menu] open");
    if (state_.setScreen(Screen::Menu)) refreshPageCount();
    markDirty();
}

void App::closeMenu() {
    logMessage(proto::LogLevel::Info, "[menu] close");
    if (state_.setScreen(screenBeforeMenu_)) refreshPageCount();
    markDirty();
}

void App::handleMenuControls(const input::ControlEvents& events) {
    if (events.direction == input::Direction::Up) {
        if (state_.moveMenuCursor(-1)) markDirty();
    } else if (events.direction == input::Direction::Down) {
        if (state_.moveMenuCursor(1)) markDirty();
    }

    // Confirm with the joystick click, the check key, or the return key; cancel
    // with the cross key. The verdict keys are inert here — there is no
    // permission on screen — so reusing them for the menu is safe.
    if (events.select || events.approvePressed || events.actionPressed) {
        activateMenuItem();
    } else if (events.denyPressed) {
        closeMenu();
    }
}

void App::announceMode() {
    link_.sendMode(state_.permissionMode());
}

void App::activateMenuItem() {
    switch (state_.menuItem()) {
        case MenuItem::CleanScreen: {
            logMessage(proto::LogLevel::Info,
                       "[menu] deep clean: black -> red -> white, about 45 s");
            const uint32_t started = millis();
            if (!display_.clean()) {
                // Nothing deeper available on this panel; an ordinary full
                // refresh is still the best scrub it has.
                logMessage(proto::LogLevel::Info, "[menu] no deep clean here — full refresh");
                display_.requestFullRefresh();
            }
            const uint32_t elapsed = millis() - started;

            // The loop was blocked for the whole cycle, so the Mac's silence
            // during it says nothing about the link. Forgiving the elapsed time
            // stops the box from declaring the link lost and wiping the session
            // list the moment it finishes cleaning. The ping clock is shifted
            // for the same reason.
            state_.noteInbound(state_.lastInboundMs() + elapsed);
            lastPingMs_ += elapsed;

            logf(proto::LogLevel::Info, "[menu] clean done in %lu ms",
                 static_cast<unsigned long>(elapsed));
            closeMenu();
            break;
        }

        case MenuItem::Resync:
            logMessage(proto::LogLevel::Info, "[menu] resync with the Mac");
            // A device hello makes the daemon resend the whole snapshot.
            link_.sendHello(kAppName, kAppVersion);
            closeMenu();
            break;

        case MenuItem::Waveform: {
            // Steps to the next waveform and stays in the menu, so the entry can
            // be flicked through while watching the glass. The panel takes the
            // new table from its next refresh; the menu redraw is that refresh.
            const ui::Waveform next = ui::nextWaveform(display_.waveform());
            if (display_.setWaveform(next)) {
                logf(proto::LogLevel::Info, "[menu] waveform -> %s", ui::waveformName(next));
            } else {
                logMessage(proto::LogLevel::Info, "[menu] this panel has a fixed waveform");
            }
            markDirty();
            break;
        }

        case MenuItem::Mode: {
            // Flips between the two and stays in the menu, like the waveform. The
            // choice is kept in flash, and the Mac is told at once: it answers
            // permissions the new way from the next request.
            const proto::PermissionMode next = proto::nextPermissionMode(state_.permissionMode());
            state_.setPermissionMode(next);
            if (!settings_.savePermissionMode(next)) {
                logMessage(proto::LogLevel::Warn,
                           "[menu] could not save the answer mode: it is back to the default at reboot");
            }
            logf(proto::LogLevel::Info, "[menu] answer mode -> %s", proto::permissionModeName(next));
            announceMode();
            markDirty();
            break;
        }

        case MenuItem::Otp:
            openOtpList();
            break;

        case MenuItem::AddOtp:
            openOtpSetup();
            break;

        case MenuItem::Close:
            closeMenu();
            break;
    }
}

// ---------------------------------------------------------------------------
// One-time codes
// ---------------------------------------------------------------------------

void App::refreshOtpAccounts() {
    const char* names[kMaxOtpAccounts];
    const uint8_t count = otpStore_.names(names, kMaxOtpAccounts);
    state_.setOtpAccounts(count, names);
}

void App::openOtpList() {
    refreshOtpAccounts();
    logMessage(proto::LogLevel::Info, "[menu] one-time codes");
    if (state_.setScreen(Screen::OtpList)) refreshPageCount();
    markDirty();
}

void App::openOtpSetup() {
    if (!otpStore_.ready()) {
        logMessage(proto::LogLevel::Warn, "[otp] one-time codes are disabled; see the boot log");
        return;
    }
    if (!otpSetup_.start(millis())) return;
    state_.setOtpSetup(otpSetup_.network(), otpSetup_.password(), otpSetup_.address(),
                       otpSetup_.qr());
    if (state_.setScreen(Screen::OtpSetup)) refreshPageCount();
    markDirty();
}

void App::backToMenu() {
    closeOtpSetup();
    if (state_.setScreen(Screen::Menu)) refreshPageCount();
    markDirty();
}

void App::closeOtpSetup() {
    if (!otpSetup_.running()) return;
    otpSetup_.stop();
    state_.clearOtpSetup();
}

void App::handleOtpControls(const input::ControlEvents& events, uint32_t nowMs) {
    // Confirm with the joystick click, the check key or the return key; back
    // with the cross. The verdict keys are inert here — there is no permission
    // on screen, and one arriving takes the screen over before any key is read.
    const bool confirm = events.select || events.approvePressed || events.actionPressed;

    switch (state_.screen()) {
        case Screen::OtpList:
            if (events.direction == input::Direction::Up) {
                if (state_.moveOtpCursor(-1)) markDirty();
            } else if (events.direction == input::Direction::Down) {
                if (state_.moveOtpCursor(1)) markDirty();
            }
            if (confirm) {
                copyOtp(state_.otpCursor(), nowMs);
            } else if (events.denyPressed) {
                backToMenu();
            }
            break;

        case Screen::OtpCode:
            if (confirm) {
                copyOtp(state_.otpCursor(), nowMs);   // a fresh code for the same account
            } else if (events.denyPressed) {
                if (state_.setScreen(Screen::OtpList)) refreshPageCount();
                markDirty();
            }
            break;

        case Screen::OtpSetup:
            if (events.denyPressed) backToMenu();
            break;

        default:
            break;
    }
}

void App::copyOtp(uint8_t index, uint32_t nowMs) {
    if (!state_.hasClock()) {
        logMessage(proto::LogLevel::Warn, "[otp] no clock from the Mac yet: no code to make");
        return;
    }
    if (index >= otpStore_.count()) return;

    char code[kOtpCodeCapacity];
    uint16_t secondsLeft = 0;
    if (!otpStore_.code(index, state_.unixSeconds(nowMs), code, sizeof(code), secondsLeft)) {
        logMessage(proto::LogLevel::Error, "[otp] could not make a code");
        return;
    }
    const char* name = otpStore_.name(index);
    const bool sent = link_.sendOtp(name, code, static_cast<uint32_t>(secondsLeft) * 1000u);
    // The log carries the account, never the code.
    logf(sent ? proto::LogLevel::Info : proto::LogLevel::Warn,
         sent ? "[otp] code for \"%s\" sent to the Mac's clipboard, %u s left"
              : "[otp] could not send the code for \"%s\" to the Mac (%u s left)",
         name, static_cast<unsigned>(secondsLeft));

    state_.showOtpCode(name, code, secondsLeft,
                       nowMs + static_cast<uint32_t>(secondsLeft) * 1000u);
    memset(code, 0, sizeof(code));
    if (state_.setScreen(Screen::OtpCode)) refreshPageCount();
    markChanged();
}

void App::tickOtp(uint32_t nowMs) {
    if (otpSetup_.running()) {
        // Whatever took the screen away — a permission, a lost link — the page
        // goes with it: Wi-Fi is only on while the setup screen is shown.
        if (state_.screen() != Screen::OtpSetup) {
            closeOtpSetup();
        } else {
            otpSetup_.poll();
            char note[kSetupNoteCapacity];
            if (otpSetup_.takeNote(note, sizeof(note))) {
                state_.setSetupNote(note);
                refreshOtpAccounts();
                markChanged();
            }
            if (otpSetup_.idleMs(millis()) > config::kOtpSetupTimeoutMs) {
                logMessage(proto::LogLevel::Info, "[otp] setup page idle for 5 min: closing it");
                backToMenu();
            }
        }
    }

    if (state_.screen() == Screen::OtpCode && !state_.otpExpired() &&
        static_cast<int32_t>(nowMs - state_.otpExpiresAtMs()) >= 0) {
        if (state_.expireOtpCode()) markDirty();
    }
}

void App::handleNavigation(input::Direction direction, uint32_t nowMs) {
    switch (direction) {
        case input::Direction::Up:
        case input::Direction::Down: {
            const bool up = direction == input::Direction::Up;
            link_.sendNavigation(up ? proto::Navigation::Up : proto::Navigation::Down);

            switch (state_.screen()) {
                case Screen::Sessions:
                    if (state_.moveCursor(up ? -1 : 1)) markDirty();
                    break;

                case Screen::Home: {
                    // Flicking up and down on the home screen steps through the
                    // sessions without opening the list.
                    const int index = state_.focusedIndex();
                    const int next = (index < 0 ? 0 : index) + (up ? -1 : 1);
                    if (next >= 0 && next < state_.sessionCount()) {
                        focusSession(state_.sessionAt(static_cast<uint8_t>(next)).id);
                    }
                    break;
                }

                default:
                    if (up) {
                        if (state_.page() > 0 && state_.setPage(state_.page() - 1)) markDirty();
                    } else if (state_.setPage(state_.page() + 1)) {
                        markDirty();
                    }
                    break;
            }
            break;
        }

        case input::Direction::Left:
        case input::Direction::Right: {
            const bool left = direction == input::Direction::Left;
            link_.sendNavigation(left ? proto::Navigation::Left : proto::Navigation::Right);

            // A flick right on the permission screen, while a queue is waiting,
            // approves the whole queue. Left/right otherwise do nothing on that
            // screen, so this steals no existing gesture — and it is the
            // joystick, not the ✓ button, so a slip on ✓ still approves only the
            // one request on screen.
            if (!left && state_.screen() == Screen::Permission && state_.waitingCount() > 1) {
                logMessage(proto::LogLevel::Info, "[input] approve-all (joystick right)");
                sendApproveAll(nowMs);
                break;
            }

            // A pending permission owns the screen: left/right must not
            // navigate away from a decision that is waiting.
            if (state_.cycleScreen(left ? -1 : 1)) {
                refreshPageCount();
                markDirty();
            }
            break;
        }

        case input::Direction::None:
            break;
    }
}

void App::handleEnter() {
    switch (state_.screen()) {
        case Screen::Home:
            if (state_.setScreen(Screen::Sessions)) {
                refreshPageCount();
                markDirty();
            }
            break;

        case Screen::Sessions:
            // Open what the cursor is on: the Mac starts reporting that
            // session, and the home screen shows it.
            focusSession(state_.cursorSessionId());
            if (state_.setScreen(Screen::Home)) refreshPageCount();
            markDirty();
            break;

        case Screen::Message:
            if (state_.setScreen(Screen::Home)) {
                refreshPageCount();
                markDirty();
            }
            break;

        case Screen::Permission: {
            // Nothing to open here; the key turns the page instead, wrapping so
            // a long command can be read with one finger.
            const uint16_t next =
                state_.pageCount() == 0 ? 0
                                        : static_cast<uint16_t>((state_.page() + 1) %
                                                                state_.pageCount());
            if (state_.setPage(next)) markDirty();
            break;
        }

        case Screen::Menu:
        case Screen::OtpList:
        case Screen::OtpCode:
        case Screen::OtpSetup:
        case Screen::Connecting:
            // None of these reaches here: the menu and the one-time-code screens
            // consume their own keys, and the connecting screen ignores them.
            break;
    }
}

const char* App::targetSessionId() const {
    if (state_.screen() == Screen::Sessions) return state_.cursorSessionId();
    if (state_.permission().valid && state_.permission().session[0] != '\0') {
        return state_.permission().session;
    }
    return state_.focusId();
}

void App::focusSession(const char* sessionId) {
    if (sessionId == nullptr || sessionId[0] == '\0') return;
    if (strcmp(sessionId, state_.focusId()) == 0) return;
    logf(proto::LogLevel::Info, "[input] follow %s", sessionId);
    // The Mac owns the focus: it answers with a fresh `sessions` frame plus the
    // state and message of the session it switched to, so the device never
    // shows one session's name above another's text.
    link_.sendFocus(sessionId);
}

/// The bulk verdict.
///
/// It is gated exactly like a single approve — a permission must be on screen,
/// offer approve, and have passed its arming delay — and it quotes that shown
/// request's id, so the Mac can apply the same staleness check before it fans
/// the approval out to the rest of the queue. With only one request waiting it
/// is just an approve.
void App::sendApproveAll(uint32_t nowMs) {
    const ActivePermission& permission = state_.permission();

    if (!state_.permissionOnScreen() || !permission.offersApprove) {
        logMessage(proto::LogLevel::Warn, "[input] approve-all ignored: nothing to approve");
        return;
    }
    if (state_.waitingCount() <= 1) {
        sendVerdict(proto::Action::Approve, nowMs);
        return;
    }
    if (!state_.canSendVerdict(permission.id, nowMs, armingDelayMs_)) {
        logf(proto::LogLevel::Warn, "[input] approve-all ignored: %s arming delay not elapsed",
             permission.id);
        return;
    }
    if (link_.sendAction(proto::Action::ApproveAll, permission.id)) {
        if (state_.markPermissionAnswered(nowMs)) markChanged();
        logf(proto::LogLevel::Info, "[input] sent approve_all (%u waiting) anchored on %s",
             static_cast<unsigned>(state_.waitingCount()), permission.id);
    }
}

void App::sendStop() {
    const char* session = targetSessionId();
    link_.sendAction(proto::Action::Stop, nullptr, session[0] == '\0' ? nullptr : session);
}

/// The device's half of the approve guard.
///
/// A verdict is only sent when it quotes the id of the permission currently on
/// screen and the arming delay has elapsed. The Mac checks the same thing again
/// against the live Accessibility tree, so neither side is trusted alone.
void App::sendVerdict(proto::Action action, uint32_t nowMs) {
    const ActivePermission& permission = state_.permission();

    if (!state_.permissionOnScreen()) {
        logMessage(proto::LogLevel::Warn, "[input] ignored: no permission on screen");
        return;
    }
    if (action == proto::Action::Approve && !permission.offersApprove) {
        logMessage(proto::LogLevel::Warn, "[input] ignored: no approve offered");
        return;
    }
    if (action == proto::Action::Deny && !permission.offersDeny) {
        logMessage(proto::LogLevel::Warn, "[input] ignored: no deny offered");
        return;
    }
    if (!state_.canSendVerdict(permission.id, nowMs, armingDelayMs_)) {
        logf(proto::LogLevel::Warn, "[input] ignored %s: %s arming delay not elapsed",
             proto::toString(action), permission.id);
        return;
    }

    if (link_.sendAction(action, permission.id)) {
        logf(proto::LogLevel::Info, "[input] sent %s for %s", proto::toString(action),
             permission.id);
        // The screen moves on now rather than when the Mac gets back: pressing
        // a key should feel like it did something. tickVerdict puts the request
        // back if the Mac never confirms.
        if (state_.markPermissionAnswered(nowMs)) markChanged();
    }
}

/// A verdict the Mac never confirmed: the request comes back rather than being
/// quietly lost, and its arming delay starts again on the way in.
void App::tickVerdict(uint32_t nowMs) {
    if (!state_.restorePermission(nowMs, config::kVerdictWaitMs)) return;
    logf(proto::LogLevel::Warn, "[input] no word from the Mac about %s: back on screen",
         state_.permission().id);
    markChanged();
}

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------

void App::updateLinkState(uint32_t nowMs) {
    // The device cannot detect the host closing the port, so silence is the
    // only signal available. `linkIsUp` handles the case where a frame arrived
    // *after* `nowMs` was sampled, which is the normal case here — see
    // include/LinkTimeout.h.
    const bool up = linkIsUp(nowMs, state_.lastInboundMs(), config::kLinkTimeoutMs);
    if (!state_.setLinkUp(up)) return;

    logf(proto::LogLevel::Info, "[serial] %s", up ? "connected" : "link lost");
    if (!up) {
        // Nothing on the Mac is authoritative any more: drop the pending
        // permission and the session list rather than leaving a stale decision
        // on screen.
        state_.clearPermission(nullptr);
        state_.clearSessions();
        state_.setClaudeState(proto::ClaudeState::Disconnected, "");
        state_.setLoaderFrame(0);
        waitingSinceMs_ = nowMs;
        lastLoaderStepMs_ = nowMs;
        refreshPageCount();
    }
    markChanged();
}

void App::tickConnecting(uint32_t nowMs) {
    if (state_.screen() != Screen::Connecting) return;
    if (nowMs - waitingSinceMs_ >= config::kConnectingAnimateMs) return;
    if (nowMs - lastLoaderStepMs_ < config::kConnectingStepMs) return;

    lastLoaderStepMs_ = nowMs;
    if (state_.setLoaderFrame(static_cast<uint8_t>(state_.loaderFrame() + 1))) markDirty();
}

void App::tickChicken(uint32_t nowMs) {
    // Home, Sessions and Message only. The connecting screen has its own
    // loader, and the permission screen is a decision: nothing on it moves, and
    // nothing refreshes under a finger that is about to press.
    const Screen screen = state_.screen();
    if (screen != Screen::Home && screen != Screen::Sessions && screen != Screen::Message) return;

    // It walks while Claude works, and for a while after anything happens; then
    // it sits down and the panel stops refreshing.
    const bool recentActivity = nowMs - lastActivityMs_ < config::kChickenWalkMs;
    if (!state_.anySessionWorking() && !recentActivity) return;
    if (nowMs - lastChickenStepMs_ < config::kChickenStepMs) return;

    lastChickenStepMs_ = nowMs;
    state_.advanceWalk();
    markDirty();
}

void App::refreshPageCount() {
    state_.setPageCount(display_.pageCountFor(state_));
}

// ---------------------------------------------------------------------------
// Inbound frames
// ---------------------------------------------------------------------------

void App::onHello(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (frame.hasApproveDelay) {
        armingDelayMs_ = frame.approveDelayMs;
        logf(proto::LogLevel::Info, "[serial] host hello, arming delay %lu ms",
             static_cast<unsigned long>(armingDelayMs_));
    } else {
        logMessage(proto::LogLevel::Info, "[serial] host hello");
    }
    // Announce ourselves so the Mac resends its full snapshot. A Mac that has
    // just started knows nothing of the mode this box chose, so it is told again.
    link_.sendHello(kAppName, kAppVersion);
    announceMode();
    markDirty();
}

void App::onState(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (state_.setClaudeState(frame.state, frame.hasDetail ? frame.detail : "")) {
        logf(proto::LogLevel::Info, "[state] %s", proto::toString(frame.state));
        refreshPageCount();
        markChanged();
    }
    // A `state` frame describes the *followed* session only, so it says nothing
    // about a permission raised by another one. Dropping the permission here —
    // which is what this used to do — would blank a decision belonging to a
    // different window. The `sessions` frame carries the active permission id
    // and is what reconciles it.
}

void App::onMessage(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (state_.setMessage(frame.text)) {
        refreshPageCount();
        markChanged();
    }
}

void App::onSessions(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (state_.setSessions(frame)) {
        logf(proto::LogLevel::Debug, "[state] %u session(s), following %s",
             static_cast<unsigned>(frame.sessionCount),
             frame.focus[0] != '\0' ? frame.focus : "-");
        state_.reconcileScreen();
        refreshPageCount();
        markChanged();
    }
}

void App::onPermission(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (state_.setPermission(frame, millis())) {
        logf(proto::LogLevel::Info, "[state] waiting_permission %s (%s)", frame.id, frame.kind);
        state_.reconcileScreen();
        refreshPageCount();
        markChanged();
    }
}

void App::onPermissionCleared(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    if (state_.clearPermission(frame.id[0] != '\0' ? frame.id : nullptr)) {
        logMessage(proto::LogLevel::Info, "[state] permission cleared");
        state_.reconcileScreen();
        refreshPageCount();
        markChanged();
    }
}

void App::onError(const proto::InboundFrame& frame) {
    state_.noteInbound(millis());
    logf(proto::LogLevel::Error, "[host] %s", frame.text);
    if (state_.setClaudeState(proto::ClaudeState::Error, frame.text)) {
        refreshPageCount();
        markChanged();
    }
}

void App::onTime(const proto::InboundFrame& frame) {
    const uint32_t nowMs = millis();
    state_.noteInbound(nowMs);
    const bool first = !state_.hasClock();
    state_.setClock(frame.unixMs, nowMs);
    // The list's COPY key depends on having a clock.
    if (first) markDirty();
}

void App::onPing(int seq) {
    state_.noteInbound(millis());
    link_.sendPong(seq);
}

void App::onPong(int seq) {
    (void)seq;
    state_.noteInbound(millis());
}

}  // namespace app
