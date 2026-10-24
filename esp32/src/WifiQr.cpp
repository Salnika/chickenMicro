#include "WifiQr.h"

#include <string.h>

#include "qrcodegen.h"

namespace otp {
namespace {

bool append(char* out, size_t capacity, size_t& length, char character) {
    if (length + 1 >= capacity) return false;
    out[length++] = character;
    out[length] = '\0';
    return true;
}

bool appendText(char* out, size_t capacity, size_t& length, const char* text) {
    for (; *text != '\0'; ++text) {
        if (!append(out, capacity, length, *text)) return false;
    }
    return true;
}

/// The format reserves these; a backslash makes them literal.
bool appendEscaped(char* out, size_t capacity, size_t& length, const char* value) {
    for (; *value != '\0'; ++value) {
        if (strchr("\\;,:\"", *value) != nullptr && !append(out, capacity, length, '\\')) {
            return false;
        }
        if (!append(out, capacity, length, *value)) return false;
    }
    return true;
}

}  // namespace

bool wifiPayload(const char* ssid, const char* password, char* out, size_t capacity) {
    if (capacity == 0) return false;
    size_t length = 0;
    out[0] = '\0';
    // T:WPA covers WPA2, which is what the setup access point runs.
    return appendText(out, capacity, length, "WIFI:T:WPA;S:") &&
           appendEscaped(out, capacity, length, ssid) &&
           appendText(out, capacity, length, ";P:") &&
           appendEscaped(out, capacity, length, password) &&
           appendText(out, capacity, length, ";;");
}

bool encodeQr(const char* text, QrBitmap& out) {
    constexpr int kMaxVersion = 4;
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];
    uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];
    out = QrBitmap();
    if (!qrcodegen_encodeText(text, scratch, code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              kMaxVersion, qrcodegen_Mask_AUTO, /*boostEcl=*/true)) {
        return false;
    }
    const int size = qrcodegen_getSize(code);
    if (size <= 0 || size > kQrMaxModules) return false;
    out.size = static_cast<uint8_t>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!qrcodegen_getModule(code, x, y)) continue;
            const uint16_t index = static_cast<uint16_t>(y * size + x);
            out.bits[index / 8] = static_cast<uint8_t>(out.bits[index / 8] | (1u << (index % 8)));
        }
    }
    // The buffers held the payload, password included.
    memset(code, 0, sizeof(code));
    memset(scratch, 0, sizeof(scratch));
    return true;
}

}  // namespace otp
