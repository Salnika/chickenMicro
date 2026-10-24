#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtpStore.h"
#include "WifiQr.h"

class String;

namespace otp {

/// "Add OTP": a temporary Wi-Fi access point serving one page, where a phone or
/// laptop hands the box an otpauth:// link, a secret key, or an
/// otpauth-migration:// transfer link carrying several accounts.
///
/// It exists only while the setup screen is shown, and joining it takes the
/// password printed on the e-ink, or the QR code next to it — so adding an
/// account needs the box in front of you. One client at a time; a fresh
/// password and form token each time it opens; closed after kOtpSetupTimeoutMs
/// without a request. The page lists account names only: never a secret, never
/// a code.
class Setup {
public:
    explicit Setup(Store& store) : store_(store) {}

    bool start(uint32_t nowMs);
    void stop();
    bool running() const { return running_; }
    /// Serves DNS and HTTP. Call every loop while running.
    void poll();

    const char* network() const { return network_; }
    const char* password() const { return password_; }
    const char* address() const { return "http://192.168.4.1"; }
    /// Joins the network from a phone's camera. Size 0 if it could not be made.
    const QrBitmap& qr() const { return qr_; }

    /// What the last request did, or who joined or left the network, for the
    /// setup screen — once per change.
    bool takeNote(char* out, size_t capacity);
    /// Milliseconds since the last request, or since it opened.
    uint32_t idleMs(uint32_t nowMs) const { return nowMs - lastRequestMs_; }

    // Request handlers, called by the web server.
    void handleRoot();
    void handleAdd();
    void handleDelete();
    void handleOther();

private:
    /// `details`, when given, is HTML already escaped: one <li> per account.
    void sendPage(const char* message, bool success, const String* details = nullptr);
    void addTransfer(const String& uri);
    bool tokenMatches();
    void note(const char* text);

    Store& store_;
    bool running_ = false;
    uint32_t lastRequestMs_ = 0;
    char network_[24] = {0};
    char password_[16] = {0};
    char token_[17] = {0};
    char note_[49] = {0};
    bool notePending_ = false;
    QrBitmap qr_;
    uint32_t seenJoins_ = 0;
    uint32_t seenFailedJoins_ = 0;
    uint32_t seenLeaves_ = 0;
};

}  // namespace otp
