#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// The "join this Wi-Fi" QR code the setup screen shows.
//
// Phones' cameras (iOS 11+, Android 10+) read the de-facto `WIFI:` format and
// offer to join the network — no typing, and so no mistyped password. It also
// updates a password the phone saved from an earlier session, which a fresh
// one every time would otherwise keep colliding with.
//
// Pure: the payload builder and the encoder run in `pio test -e native`, and
// the preview draws the real code, so its scannability can be checked on the
// host. The encoding itself is Project Nayuki's qrcodegen (MIT).
// ---------------------------------------------------------------------------

namespace otp {

/// Version 4 at most: 33 x 33 modules, which still fits the panel at three
/// pixels a module. A Wi-Fi payload of this box's size needs version 3.
constexpr uint8_t kQrMaxModules = 33;

/// A QR code as a square of modules, row by row, one bit each. Size 0 = none.
struct QrBitmap {
    uint8_t size = 0;
    uint8_t bits[(kQrMaxModules * kQrMaxModules + 7) / 8] = {0};

    bool module(uint8_t x, uint8_t y) const {
        const uint16_t index = static_cast<uint16_t>(y) * size + x;
        return (bits[index / 8] >> (index % 8)) & 1u;
    }
};

/// `WIFI:T:WPA;S:<ssid>;P:<password>;;`, with the format's special characters
/// (\ ; , : ") escaped. False if it does not fit `capacity`.
bool wifiPayload(const char* ssid, const char* password, char* out, size_t capacity);

/// Encodes `text` at low error correction — the panel is clean and flat, and
/// the lowest level keeps the modules largest. False if it needs more than
/// version 4.
bool encodeQr(const char* text, QrBitmap& out);

}  // namespace otp
