#include "KeyLights.h"

#include <Adafruit_NeoPixel.h>

#include "Config.h"
#include "Log.h"

namespace ui {
namespace {

constexpr bool chainIsPermutation(const uint8_t (&chain)[kKeyCount]) {
    return chain[0] < kKeyCount && chain[1] < kKeyCount && chain[2] < kKeyCount &&
           chain[3] < kKeyCount && chain[0] != chain[1] && chain[0] != chain[2] &&
           chain[0] != chain[3] && chain[1] != chain[2] && chain[1] != chain[3] &&
           chain[2] != chain[3];
}
static_assert(chainIsPermutation(config::kKeyLightChain),
              "kKeyLightChain must name each of the four chain positions 0-3 exactly once");

// GRB at 800 kHz: the WS2812B's own byte order and speed.
Adafruit_NeoPixel chain(kKeyCount, config::kKeyLightPin, NEO_GRB + NEO_KHZ800);

void show(const KeyLightFrame& frame) {
    for (uint8_t key = 0; key < kKeyCount; ++key) {
        const Rgb& colour = frame.keys[key];
        chain.setPixelColor(config::kKeyLightChain[key], colour.r, colour.g, colour.b);
    }
    chain.show();
}

}  // namespace

void KeyLights::begin() {
    if (config::kKeyLightPin < 0) return;
    chain.begin();
    // Dark straight away. WS2812B can power up showing anything, and on the C3
    // the data line is held high through boot by its pull-up, which can clock a
    // stray bit into the first LED; one full write settles all four.
    shown_ = KeyLightFrame();
    show(shown_);
    started_ = true;
    logf(proto::LogLevel::Info, "[lights] %u keycap LEDs on GPIO%d", kKeyCount,
         config::kKeyLightPin);
}

void KeyLights::selfTest() {
    if (!started_ || config::kKeyLightTestStepMs == 0) return;
    for (uint8_t step = 0; step < kKeyLightTestSteps; ++step) {
        show(keyLightsForTest(step));
        delay(config::kKeyLightTestStepMs);
    }
    shown_ = KeyLightFrame();
    show(shown_);
    logMessage(proto::LogLevel::Info,
               "[lights] self-test done: all four, then X red, # amber, v green, <- white");
}

void KeyLights::flash(Key key, uint32_t nowMs) {
    const uint8_t index = static_cast<uint8_t>(key);
    flashing_[index] = true;
    flashUntilMs_[index] = nowMs + config::kKeyLightFlashMs;
}

void KeyLights::update(uint32_t nowMs, const KeyLegend& legend) {
    if (!started_) return;
    for (uint8_t index = 0; index < kKeyCount; ++index) {
        // Signed difference, so the millis() rollover cannot leave a key white.
        if (flashing_[index] && static_cast<int32_t>(nowMs - flashUntilMs_[index]) >= 0) {
            flashing_[index] = false;
        }
    }
    const KeyLightFrame frame = keyLightsFor(legend, flashing_);
    if (frame == shown_) return;
    show(frame);
    shown_ = frame;
}

}  // namespace ui
