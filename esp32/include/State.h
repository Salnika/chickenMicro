#pragma once

#include <stdint.h>

#include "Protocol.h"
#include "WifiQr.h"

namespace app {

/// The permission currently on screen, and the only one the device will quote
/// back in an action.
struct ActivePermission {
    bool valid = false;
    char id[proto::kMaxRequestIdChars] = {0};
    char kind[proto::kMaxKindChars] = {0};
    char title[proto::kMaxTitleChars] = {0};
    char content[proto::kMaxContentChars] = {0};
    /// Session that raised it, so the screen can name which Claude is asking.
    char session[proto::kMaxSessionIdChars] = {0};
    bool offersApprove = false;
    bool offersDeny = false;
    /// When it was displayed, for the arming delay.
    uint32_t shownAtMs = 0;
    /// Set once a verdict for it has gone out: the screen moves on at once and
    /// the request is kept aside, to come back if the Mac never confirms.
    bool answered = false;
    uint32_t answeredAtMs = 0;
};

/// Which screen the device is showing.
///
/// Two of them are not navigable: `Connecting` owns the display until a frame
/// arrives, and `Permission` owns it while a request is pending. `Menu` is
/// modal — opened by a long joystick press and closed again — and the three
/// one-time-code screens are reached from it. The other three cycle.
enum class Screen : uint8_t {
    Connecting,
    Home,
    Sessions,
    Message,
    Menu,
    OtpList,    ///< the stored one-time-code accounts, to pick one to copy
    OtpCode,    ///< the code just copied to the Mac, and how long it lasts
    OtpSetup,   ///< the Wi-Fi setup page is open for adding accounts
    Permission,
};

/// Entries in the long-press menu, in display order. Keep in sync with the
/// labels in Display.cpp and the actions in App.cpp.
enum class MenuItem : uint8_t {
    Otp,           ///< one-time codes: pick an account, its code goes to the clipboard
    AddOtp,        ///< open the Wi-Fi setup page for adding an account
    CleanScreen,   ///< the black/red/white pigment cycle that scrubs the glass
    Waveform,      ///< step through the mono refresh waveforms, to compare by eye
    Mode,          ///< how permissions are answered: through a hook, or the window only
    Resync,        ///< ask the Mac to resend the whole snapshot
    Close,         ///< leave the menu
};

/// Number of entries in `MenuItem`.
constexpr uint8_t kMenuItemCount = 7;

/// Entries the menu draws at once: six rows fit above the key bar. With more
/// entries than that the list scrolls, just far enough to keep the highlighted
/// one in view.
constexpr uint8_t kMenuVisibleRows = 6;
static_assert(kMenuItemCount >= kMenuVisibleRows, "the scroll window assumes a list at least as long as the screen");

/// One-time-code accounts the box can hold, and the room for each name.
constexpr uint8_t kMaxOtpAccounts = 16;
constexpr size_t kOtpNameCapacity = 25;
/// A one-time code: at most 8 digits.
constexpr size_t kOtpCodeCapacity = 9;
/// The setup screen's network name, password and address.
constexpr size_t kSetupFieldCapacity = 33;
constexpr size_t kSetupNoteCapacity = 49;

/// Everything the UI renders from. No heap, no dynamic strings.
class State {
public:
    // --- Focused session ------------------------------------------------------

    proto::ClaudeState claudeState() const { return claudeState_; }
    const char* detail() const { return detail_; }
    const char* message() const { return message_; }

    /// Returns true when the value actually changed, so the caller knows
    /// whether the e-ink needs a redraw.
    bool setClaudeState(proto::ClaudeState state, const char* detail);
    bool setMessage(const char* text);

    // --- Sessions -------------------------------------------------------------

    uint8_t sessionCount() const { return sessionCount_; }
    /// Sessions the Mac knows about, which may exceed what fits in one frame.
    uint8_t sessionTotal() const { return sessionTotal_; }
    const proto::SessionEntry& sessionAt(uint8_t index) const { return sessions_[index]; }

    /// Session the `state` and `message` frames describe. Empty when unknown.
    const char* focusId() const { return focusId_; }
    /// Position of the focused session in the list, or -1.
    int focusedIndex() const;
    /// Display name for a session id; the id itself when it is not listed.
    const char* nameFor(const char* id) const;
    /// How many listed sessions are waiting on a permission.
    uint8_t waitingCount() const;
    /// Whether any session is thinking or writing.
    bool anySessionWorking() const;

    /// Whether a decision is waiting: a permission on screen, or any session
    /// asking for one. This is what makes the status LED blink.
    bool actionPending() const;

    /// Replaces the whole list, the focus and the active-permission reconcile.
    ///
    /// The list is level-triggered on purpose: it carries the permission the
    /// Mac considers active, so a `permission_cleared` lost to line noise
    /// cannot strand a dead request on the screen.
    bool setSessions(const proto::InboundFrame& frame);
    bool clearSessions();

    // --- Session list cursor --------------------------------------------------

    uint8_t cursor() const { return cursor_; }
    bool setCursor(uint8_t index);
    /// Moves the cursor by `delta`, clamped to the list. No wrap-around: on a
    /// list this short, stopping at the end is easier to feel than wrapping.
    bool moveCursor(int delta);
    /// Id under the cursor, or an empty string when the list is empty.
    const char* cursorSessionId() const;

    // --- Permission -----------------------------------------------------------

    const ActivePermission& permission() const { return permission_; }
    /// Whether a request is the screen's business right now. A verdict already
    /// sent takes it off the screen without dropping it.
    bool permissionOnScreen() const { return permission_.valid && !permission_.answered; }

    /// Records that a verdict for the request on screen has gone out, so the
    /// device can move on without waiting for the Mac.
    bool markPermissionAnswered(uint32_t nowMs);

    /// Puts an unconfirmed request back, `waitMs` after its verdict went out.
    /// The arming delay starts again with it, so a key still held down cannot
    /// answer it on the way in.
    bool restorePermission(uint32_t nowMs, uint32_t waitMs);

    /// Stores the request as the active one and starts its arming delay.
    bool setPermission(const proto::InboundFrame& frame, uint32_t nowMs);

    /// Drops the active permission. `id` may be null to clear unconditionally;
    /// when given, a mismatch is ignored so a late clear for an old request
    /// cannot dismiss the current one.
    bool clearPermission(const char* id);

    /// Whether a verdict may be sent for `id` right now. This is the device's
    /// half of the approve guard: the id must be the one displayed, and the
    /// arming delay must have elapsed. The Mac checks again independently.
    bool canSendVerdict(const char* id, uint32_t nowMs, uint32_t armingDelayMs) const;

    // --- Link ---------------------------------------------------------------

    bool linkUp() const { return linkUp_; }
    bool setLinkUp(bool up);
    void noteInbound(uint32_t nowMs) { lastInboundMs_ = nowMs; }
    uint32_t lastInboundMs() const { return lastInboundMs_; }

    /// Step of the "connecting" indicator. Only ever drawn on that screen.
    uint8_t loaderFrame() const { return loaderFrame_; }
    bool setLoaderFrame(uint8_t frame);

    // --- The chicken in the title bar -----------------------------------------

    /// How many steps the chicken has taken. It only ever counts up;
    /// `ui::walkPose` turns it into a position on whatever lane the screen has.
    uint32_t walkPhase() const { return walkPhase_; }
    void advanceWalk() { ++walkPhase_; }

    // --- Navigation ---------------------------------------------------------

    Screen screen() const { return screen_; }
    bool setScreen(Screen screen);
    /// Steps through Home -> Sessions -> Message, in either direction. Does
    /// nothing while a non-navigable screen owns the display.
    bool cycleScreen(int delta);

    // --- Clock ----------------------------------------------------------------
    //
    // The board has no clock that survives a power cycle, so one-time codes
    // take the time from the Mac (`time` frames), carried forward on millis().

    bool hasClock() const { return hasClock_; }
    void setClock(uint64_t unixMs, uint32_t nowMs);
    /// Unix seconds now. Only meaningful once `hasClock()`.
    uint64_t unixSeconds(uint32_t nowMs) const;

    // --- One-time codes -------------------------------------------------------

    /// The account names, in list order. Never the secrets: the list is all the
    /// interface needs, and the secrets stay inside the store.
    void setOtpAccounts(uint8_t count, const char* const* names);
    uint8_t otpCount() const { return otpCount_; }
    const char* otpName(uint8_t index) const;
    uint8_t otpCursor() const { return otpCursor_; }
    /// Moves the highlighted account, clamped to the list (no wrap).
    bool moveOtpCursor(int delta);

    /// The code just copied, for the confirmation screen.
    void showOtpCode(const char* name, const char* code, uint16_t secondsLeft,
                     uint32_t expiresAtMs);
    const char* otpCodeName() const { return otpCodeName_; }
    const char* otpCode() const { return otpCode_; }
    uint16_t otpSecondsLeft() const { return otpSecondsLeft_; }
    uint32_t otpExpiresAtMs() const { return otpExpiresAtMs_; }
    bool otpExpired() const { return otpExpired_; }
    /// The copied code's window has ended. Returns true if that is news.
    bool expireOtpCode();

    /// What the setup screen tells the user to join and open, and the QR code
    /// that joins it.
    void setOtpSetup(const char* network, const char* password, const char* address,
                     const otp::QrBitmap& qr);
    /// Forgets all of it once the network is down: the password was only ever
    /// good for that one session.
    void clearOtpSetup();
    const otp::QrBitmap& setupQr() const { return setupQr_; }
    const char* setupNetwork() const { return setupNetwork_; }
    const char* setupPassword() const { return setupPassword_; }
    const char* setupAddress() const { return setupAddress_; }
    /// The last thing that happened on the setup page, e.g. "Added GitHub".
    bool setSetupNote(const char* note);
    const char* setupNote() const { return setupNote_; }

    // --- Menu -----------------------------------------------------------------

    uint8_t menuCursor() const { return menuCursor_; }
    /// The first entry drawn. Always within `kMenuItemCount - kMenuVisibleRows`.
    uint8_t menuFirstRow() const { return menuFirstRow_; }
    static constexpr uint8_t menuItemCount() { return kMenuItemCount; }
    MenuItem menuItem() const { return static_cast<MenuItem>(menuCursor_); }
    void resetMenuCursor() {
        menuCursor_ = 0;
        menuFirstRow_ = 0;
    }
    /// Moves the highlighted entry, clamped to the list (no wrap), and scrolls the
    /// window to keep it in view.
    bool moveMenuCursor(int delta);

    // --- Permission mode ------------------------------------------------------

    /// How the box wants permissions answered. Loaded from flash at boot and sent
    /// to the Mac in a `mode` frame; see protocol/PROTOCOL.md §2.
    proto::PermissionMode permissionMode() const { return permissionMode_; }
    /// Returns whether the mode changed.
    bool setPermissionMode(proto::PermissionMode mode);

    uint16_t page() const { return page_; }
    bool setPage(uint16_t page);
    uint16_t pageCount() const { return pageCount_; }
    void setPageCount(uint16_t count);

    /// Enforces the screens that own the display: `Connecting` while no frame
    /// has arrived, `Permission` while a request is pending.
    bool reconcileScreen();

private:
    proto::ClaudeState claudeState_ = proto::ClaudeState::Disconnected;
    char detail_[proto::kMaxDetailChars] = {0};
    char message_[proto::kMaxMessageChars] = {0};
    ActivePermission permission_;

    proto::SessionEntry sessions_[proto::kMaxSessions];
    uint8_t sessionCount_ = 0;
    uint8_t sessionTotal_ = 0;
    char focusId_[proto::kMaxSessionIdChars] = {0};
    uint8_t cursor_ = 0;

    bool linkUp_ = false;
    uint32_t lastInboundMs_ = 0;
    uint8_t loaderFrame_ = 0;
    uint32_t walkPhase_ = 0;

    Screen screen_ = Screen::Connecting;
    uint8_t menuCursor_ = 0;
    uint8_t menuFirstRow_ = 0;
    proto::PermissionMode permissionMode_ = proto::PermissionMode::Auto;

    bool hasClock_ = false;
    /// The last `time` frame: Unix milliseconds, and millis() when it arrived.
    /// Time since is the unsigned difference of millis(), which stays right
    /// across millis() wrapping round — resynced every minute, it never gets
    /// near the 49.7 days where that would stop being true.
    uint64_t clockUnixMs_ = 0;
    uint32_t clockAnchorMs_ = 0;

    char otpNames_[kMaxOtpAccounts][kOtpNameCapacity] = {};
    uint8_t otpCount_ = 0;
    uint8_t otpCursor_ = 0;
    char otpCodeName_[kOtpNameCapacity] = {0};
    char otpCode_[kOtpCodeCapacity] = {0};
    uint16_t otpSecondsLeft_ = 0;
    uint32_t otpExpiresAtMs_ = 0;
    bool otpExpired_ = false;
    char setupNetwork_[kSetupFieldCapacity] = {0};
    char setupPassword_[kSetupFieldCapacity] = {0};
    char setupAddress_[kSetupFieldCapacity] = {0};
    char setupNote_[kSetupNoteCapacity] = {0};
    otp::QrBitmap setupQr_;
    uint16_t page_ = 0;
    uint16_t pageCount_ = 1;
};

}  // namespace app
