#pragma once

#include <stdint.h>

#include "Controls.h"
#include "Display.h"
#include "KeyLights.h"
#include "Led.h"
#include "OtpSetup.h"
#include "OtpStore.h"
#include "SerialProtocol.h"
#include "State.h"

namespace app {

/// The firmware's top level: owns the state, the display, the controls and the
/// link, and maps one onto the other.
///
/// `main.cpp` does nothing but construct this and call `setup`/`loop`.
class App final : public wire::FrameHandler {
public:
    App();

    void setup();
    void loop();

    // wire::FrameHandler
    void onHello(const proto::InboundFrame& frame) override;
    void onState(const proto::InboundFrame& frame) override;
    void onMessage(const proto::InboundFrame& frame) override;
    void onSessions(const proto::InboundFrame& frame) override;
    void onPermission(const proto::InboundFrame& frame) override;
    void onPermissionCleared(const proto::InboundFrame& frame) override;
    void onError(const proto::InboundFrame& frame) override;
    void onPing(int seq) override;
    void onPong(int seq) override;
    void onTime(const proto::InboundFrame& frame) override;

private:
    void handleControls(uint32_t nowMs);
    void handleNavigation(input::Direction direction, uint32_t nowMs);
    /// The return key and the joystick click: open, go back, turn the page.
    void handleEnter();

    // --- Menu -----------------------------------------------------------------
    void toggleMenu();
    void openMenu();
    void closeMenu();
    void handleMenuControls(const input::ControlEvents& events);
    void activateMenuItem();

    // --- One-time codes ---------------------------------------------------------
    void openOtpList();
    void openOtpSetup();
    /// Back to the menu from any one-time-code screen, closing Wi-Fi if open.
    void backToMenu();
    /// Wi-Fi off, and the screen's copy of its password and QR code gone.
    void closeOtpSetup();
    void handleOtpControls(const input::ControlEvents& events, uint32_t nowMs);
    /// Makes the code for account `index` and hands it to the Mac's clipboard.
    /// Only ever called on a physical press.
    void copyOtp(uint8_t index, uint32_t nowMs);
    void refreshOtpAccounts();
    void tickOtp(uint32_t nowMs);
    /// Brings back a request the Mac never confirmed a verdict for.
    void tickVerdict(uint32_t nowMs);
    void sendVerdict(proto::Action action, uint32_t nowMs);
    /// Approves every queued request at once, anchored on the one on screen.
    void sendApproveAll(uint32_t nowMs);
    void sendStop();
    /// Asks the Mac to follow another session, if there is one to follow.
    void focusSession(const char* sessionId);
    /// Session the keys act on, which is the highlighted row on the session
    /// list and the followed session everywhere else.
    const char* targetSessionId() const;

    void updateLinkState(uint32_t nowMs);
    void tickConnecting(uint32_t nowMs);
    /// Moves the title-bar chicken one step, when it is allowed to walk.
    void tickChicken(uint32_t nowMs);
    void refreshPageCount();
    void markDirty() { dirty_ = true; }
    /// Something the user can see changed: redraw it, and count it as activity
    /// for the chicken.
    void markChanged();

    /// Installed as the display's busy callback, so the buttons keep being read
    /// while a refresh holds up the loop.
    static void sampleWhileBusy(const void* context);

    State state_;
    ui::Display display_;
    ui::Led led_;
    ui::KeyLights keyLights_;
    otp::Store otpStore_;
    otp::Setup otpSetup_{otpStore_};
    input::Controls controls_;
    wire::SerialProtocol link_;

    /// Arming delay actually in force; the Mac may override the default via
    /// `hello.approveDelayMs`.
    uint32_t armingDelayMs_;

    bool dirty_ = true;
    int pingSeq_ = 0;
    uint32_t lastPingMs_ = 0;

    /// When the device last started waiting for the Mac, so the connecting
    /// indicator can stop stepping instead of refreshing the panel forever.
    uint32_t waitingSinceMs_ = 0;
    uint32_t lastLoaderStepMs_ = 0;

    /// Last key press or change on screen; the chicken walks for a while after.
    uint32_t lastActivityMs_ = 0;
    uint32_t lastChickenStepMs_ = 0;

    /// The screen the menu was opened from, restored when it closes.
    Screen screenBeforeMenu_ = Screen::Home;
};

}  // namespace app
