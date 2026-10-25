#include "State.h"

#include <string.h>

namespace app {
namespace {

/// Copies into a fixed buffer, reporting whether the content changed.
bool assign(char* destination, size_t capacity, const char* source) {
    const char* value = source == nullptr ? "" : source;
    if (strncmp(destination, value, capacity - 1) == 0) return false;
    size_t index = 0;
    while (index + 1 < capacity && value[index] != '\0') {
        destination[index] = value[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

bool sameEntry(const proto::SessionEntry& left, const proto::SessionEntry& right) {
    return left.state == right.state && strcmp(left.id, right.id) == 0 &&
           strcmp(left.name, right.name) == 0;
}

}  // namespace

// --- Focused session --------------------------------------------------------

bool State::setClaudeState(proto::ClaudeState state, const char* detail) {
    bool changed = false;
    if (claudeState_ != state) {
        claudeState_ = state;
        changed = true;
    }
    if (assign(detail_, sizeof(detail_), detail)) changed = true;
    return changed;
}

bool State::setMessage(const char* text) {
    const bool changed = assign(message_, sizeof(message_), text);
    if (changed) {
        page_ = 0;   // a new message starts at the top
    }
    return changed;
}

// --- Sessions ---------------------------------------------------------------

int State::focusedIndex() const {
    if (focusId_[0] == '\0') return -1;
    for (uint8_t index = 0; index < sessionCount_; ++index) {
        if (strcmp(sessions_[index].id, focusId_) == 0) return static_cast<int>(index);
    }
    return -1;
}

const char* State::nameFor(const char* id) const {
    if (id == nullptr || id[0] == '\0') return "";
    for (uint8_t index = 0; index < sessionCount_; ++index) {
        if (strcmp(sessions_[index].id, id) == 0) return sessions_[index].name;
    }
    return id;
}

uint8_t State::waitingCount() const {
    uint8_t waiting = 0;
    for (uint8_t index = 0; index < sessionCount_; ++index) {
        if (sessions_[index].state == proto::ClaudeState::WaitingPermission) ++waiting;
    }
    return waiting;
}

bool State::setSessions(const proto::InboundFrame& frame) {
    bool changed = frame.sessionCount != sessionCount_ || frame.sessionTotal != sessionTotal_;

    // The cursor follows the session it was on, not the row index: a session
    // appearing or closing must not silently move the selection onto another
    // one.
    char anchor[proto::kMaxSessionIdChars];
    assign(anchor, sizeof(anchor), cursorSessionId());

    for (uint8_t index = 0; index < frame.sessionCount; ++index) {
        if (index >= sessionCount_ || !sameEntry(sessions_[index], frame.sessions[index])) {
            changed = true;
        }
        sessions_[index] = frame.sessions[index];
    }
    sessionCount_ = frame.sessionCount;
    sessionTotal_ = frame.sessionTotal < frame.sessionCount ? frame.sessionCount
                                                            : frame.sessionTotal;
    if (assign(focusId_, sizeof(focusId_), frame.focus)) changed = true;

    cursor_ = 0;
    for (uint8_t index = 0; index < sessionCount_; ++index) {
        if (strcmp(sessions_[index].id, anchor) == 0) {
            cursor_ = index;
            break;
        }
    }

    // Level-triggered reconcile: whatever the Mac says is active wins, so a
    // `permission_cleared` lost on the wire cannot leave a dead request on
    // screen waiting for a verdict that would be refused anyway.
    if (permission_.valid && strcmp(permission_.id, frame.activePermission) != 0) {
        clearPermission(nullptr);
        changed = true;
    }
    return changed;
}

bool State::clearSessions() {
    if (sessionCount_ == 0 && focusId_[0] == '\0' && sessionTotal_ == 0) return false;
    sessionCount_ = 0;
    sessionTotal_ = 0;
    cursor_ = 0;
    focusId_[0] = '\0';
    return true;
}

// --- Session list cursor ----------------------------------------------------

bool State::setCursor(uint8_t index) {
    if (sessionCount_ == 0) return false;
    const uint8_t clamped = index >= sessionCount_ ? sessionCount_ - 1 : index;
    if (clamped == cursor_) return false;
    cursor_ = clamped;
    return true;
}

bool State::moveCursor(int delta) {
    if (sessionCount_ == 0) return false;
    int target = static_cast<int>(cursor_) + delta;
    if (target < 0) target = 0;
    if (target > sessionCount_ - 1) target = sessionCount_ - 1;
    return setCursor(static_cast<uint8_t>(target));
}

const char* State::cursorSessionId() const {
    return cursor_ < sessionCount_ ? sessions_[cursor_].id : "";
}

// --- Permission -------------------------------------------------------------

bool State::setPermission(const proto::InboundFrame& frame, uint32_t nowMs) {
    if (permission_.valid && strcmp(permission_.id, frame.id) == 0) {
        // The Mac re-sending a request a verdict has already gone out for says
        // that verdict did not land: back on screen, and armed afresh.
        if (permission_.answered) {
            permission_.answered = false;
            permission_.shownAtMs = nowMs;
            page_ = 0;
            return true;
        }
        // Otherwise a repeat must not restart the arming delay, or a frame
        // arriving every poll could keep the request permanently unarmed.
        return false;
    }

    permission_ = ActivePermission();
    permission_.valid = true;
    assign(permission_.id, sizeof(permission_.id), frame.id);
    assign(permission_.kind, sizeof(permission_.kind), frame.kind);
    assign(permission_.title, sizeof(permission_.title), frame.title);
    assign(permission_.content, sizeof(permission_.content), frame.content);
    assign(permission_.session, sizeof(permission_.session), frame.session);
    permission_.offersApprove = frame.offersApprove;
    permission_.offersDeny = frame.offersDeny;
    permission_.shownAtMs = nowMs;
    page_ = 0;
    return true;
}

bool State::clearPermission(const char* id) {
    if (!permission_.valid) return false;
    // A clear naming a different request belongs to an older dialog; ignoring
    // it stops a late frame from dismissing the request now on screen.
    if (id != nullptr && id[0] != '\0' && strcmp(permission_.id, id) != 0) return false;
    permission_ = ActivePermission();
    page_ = 0;
    return true;
}

bool State::markPermissionAnswered(uint32_t nowMs) {
    if (!permission_.valid || permission_.answered) return false;
    permission_.answered = true;
    permission_.answeredAtMs = nowMs;
    return true;
}

bool State::restorePermission(uint32_t nowMs, uint32_t waitMs) {
    if (!permission_.valid || !permission_.answered) return false;
    if (nowMs - permission_.answeredAtMs < waitMs) return false;
    permission_.answered = false;
    permission_.shownAtMs = nowMs;
    page_ = 0;
    return true;
}

bool State::canSendVerdict(const char* id, uint32_t nowMs, uint32_t armingDelayMs) const {
    // A verdict already sent holds the request off screen: one press, one
    // verdict, whatever the keys do next.
    if (!permission_.valid || permission_.answered) return false;
    if (id == nullptr || strcmp(permission_.id, id) != 0) return false;
    // Absorbs a press that was already in flight when the screen changed.
    return (nowMs - permission_.shownAtMs) >= armingDelayMs;
}

// --- Link -------------------------------------------------------------------

bool State::setLinkUp(bool up) {
    if (linkUp_ == up) return false;
    linkUp_ = up;
    return true;
}

bool State::setLoaderFrame(uint8_t frame) {
    if (loaderFrame_ == frame) return false;
    loaderFrame_ = frame;
    return true;
}

bool State::actionPending() const {
    // Either half can lead: a permission is on screen before its session list
    // has caught up, and a session can be waiting before the device has surfaced
    // its permission frame.
    return permissionOnScreen() || waitingCount() > 0;
}

bool State::anySessionWorking() const {
    const auto working = [](proto::ClaudeState state) {
        return state == proto::ClaudeState::Thinking || state == proto::ClaudeState::Writing;
    };
    for (uint8_t index = 0; index < sessionCount_; ++index) {
        if (working(sessions_[index].state)) return true;
    }
    // Until a session list arrives, the followed session's state is all there
    // is to go on.
    return sessionCount_ == 0 && working(claudeState_);
}

// --- Navigation -------------------------------------------------------------

bool State::setScreen(Screen screen) {
    if (screen_ == screen) return false;
    screen_ = screen;
    page_ = 0;
    return true;
}

bool State::cycleScreen(int delta) {
    static const Screen kOrder[] = {Screen::Home, Screen::Sessions, Screen::Message};
    constexpr int kCount = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));

    int position = -1;
    for (int index = 0; index < kCount; ++index) {
        if (kOrder[index] == screen_) position = index;
    }
    // Connecting and Permission own the display; cycling out of them is the
    // one navigation the device must refuse.
    if (position < 0) return false;

    int target = (position + delta) % kCount;
    if (target < 0) target += kCount;
    return setScreen(kOrder[target]);
}

// --- Clock ------------------------------------------------------------------

void State::setClock(uint64_t unixMs, uint32_t nowMs) {
    clockUnixMs_ = unixMs;
    clockAnchorMs_ = nowMs;
    hasClock_ = true;
}

uint64_t State::unixSeconds(uint32_t nowMs) const {
    const uint32_t sinceSync = nowMs - clockAnchorMs_;
    return (clockUnixMs_ + sinceSync) / 1000u;
}

// --- One-time codes -----------------------------------------------------------

void State::setOtpAccounts(uint8_t count, const char* const* names) {
    if (count > kMaxOtpAccounts) count = kMaxOtpAccounts;
    for (uint8_t index = 0; index < count; ++index) {
        assign(otpNames_[index], kOtpNameCapacity, names[index]);
    }
    otpCount_ = count;
    if (otpCursor_ >= otpCount_) otpCursor_ = otpCount_ == 0 ? 0 : static_cast<uint8_t>(otpCount_ - 1);
}

const char* State::otpName(uint8_t index) const {
    return index < otpCount_ ? otpNames_[index] : "";
}

bool State::moveOtpCursor(int delta) {
    if (otpCount_ == 0) return false;
    int target = static_cast<int>(otpCursor_) + delta;
    if (target < 0) target = 0;
    if (target > otpCount_ - 1) target = otpCount_ - 1;
    if (static_cast<uint8_t>(target) == otpCursor_) return false;
    otpCursor_ = static_cast<uint8_t>(target);
    return true;
}

void State::showOtpCode(const char* name, const char* code, uint16_t secondsLeft,
                        uint32_t expiresAtMs) {
    assign(otpCodeName_, sizeof(otpCodeName_), name);
    assign(otpCode_, sizeof(otpCode_), code);
    otpSecondsLeft_ = secondsLeft;
    otpExpiresAtMs_ = expiresAtMs;
    otpExpired_ = false;
}

bool State::expireOtpCode() {
    if (otpExpired_) return false;
    otpExpired_ = true;
    return true;
}

void State::setOtpSetup(const char* network, const char* password, const char* address,
                        const otp::QrBitmap& qr) {
    assign(setupNetwork_, sizeof(setupNetwork_), network);
    assign(setupPassword_, sizeof(setupPassword_), password);
    assign(setupAddress_, sizeof(setupAddress_), address);
    setupQr_ = qr;
    setupNote_[0] = '\0';
}

void State::clearOtpSetup() {
    memset(setupNetwork_, 0, sizeof(setupNetwork_));
    memset(setupPassword_, 0, sizeof(setupPassword_));
    memset(setupAddress_, 0, sizeof(setupAddress_));
    memset(setupNote_, 0, sizeof(setupNote_));
    setupQr_ = otp::QrBitmap();
}

bool State::setSetupNote(const char* note) {
    return assign(setupNote_, sizeof(setupNote_), note);
}

bool State::moveMenuCursor(int delta) {
    int target = static_cast<int>(menuCursor_) + delta;
    if (target < 0) target = 0;
    if (target > kMenuItemCount - 1) target = kMenuItemCount - 1;
    if (static_cast<uint8_t>(target) == menuCursor_) return false;
    menuCursor_ = static_cast<uint8_t>(target);

    // Scroll only as far as needed: the highlighted row stays on screen, and the
    // list does not jump when the cursor moves inside the window.
    if (menuCursor_ < menuFirstRow_) {
        menuFirstRow_ = menuCursor_;
    } else if (menuCursor_ >= menuFirstRow_ + kMenuVisibleRows) {
        menuFirstRow_ = static_cast<uint8_t>(menuCursor_ - kMenuVisibleRows + 1);
    }
    return true;
}

bool State::setPermissionMode(proto::PermissionMode mode) {
    if (mode == permissionMode_) return false;
    permissionMode_ = mode;
    return true;
}

bool State::setPage(uint16_t page) {
    const uint16_t clamped = pageCount_ == 0 ? 0 : (page >= pageCount_ ? pageCount_ - 1 : page);
    if (page_ == clamped) return false;
    page_ = clamped;
    return true;
}

void State::setPageCount(uint16_t count) {
    pageCount_ = count == 0 ? 1 : count;
    if (page_ >= pageCount_) page_ = pageCount_ - 1;
}

bool State::reconcileScreen() {
    if (!linkUp_) return setScreen(Screen::Connecting);
    if (permissionOnScreen()) return setScreen(Screen::Permission);
    if (screen_ == Screen::Permission || screen_ == Screen::Connecting) {
        return setScreen(Screen::Home);
    }
    return false;
}

}  // namespace app
