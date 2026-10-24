// ---------------------------------------------------------------------------
// One-shot panel cleaner.
//
// WHY THIS EXISTS
// ---------------
// The firmware runs the GDEY0213Z98 in monochrome (EINK_MODEL_GDEY0213Z98_FAST)
// because that is the only way to get a ~300 ms partial refresh out of it. The
// catch is that the monochrome waveform cannot move the red pigment at all — it
// only drives black<->white transitions. So any red already on the glass, from
// a previous tri-colour session or from the factory test pattern, stays there
// permanently, showing through underneath the UI.
//
// This utility loads the *tri-colour* waveform once and drives the pigment
// through a full black / red / white cycle, which clears the red and restores
// saturation. Then you flash the normal firmware again and stay in mono.
//
// It is a standalone program: it does not speak the wire protocol, does not
// touch the controls, and is excluded from every normal build.
//
// USAGE
// -----
//     cd esp32
//     pio run -e esp32dev_clear_panel --target upload
//     pio device monitor            # watch it, ~45 s
//     pio run -e esp32dev --target upload     # back to the real firmware
//
// Run it when you see red patches under the UI, or once after any tri-colour
// experiment. It is not needed on a panel that has only ever run in mono.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SPI.h>

#include "Config.h"
#include "DisplayModel.h"

#if !EINK_PANEL_IS_3C
#error "ClearPanel needs the 3-colour driver. Build it with `pio run -e esp32dev_clear_panel`, which forces EINK_MODEL_GDEY0213Z98_TRI."
#endif

#include <GxEPD2_3C.h>

namespace {

GxEPD2_3C<EINK_PANEL_CLASS, EINK_PANEL_CLASS::HEIGHT> panel(
    EINK_PANEL_CLASS(config::kEpdCs, config::kEpdDc, config::kEpdRst, config::kEpdBusy));

/// One pass of the pigment cycle. `black` and `colour` are the two RAM planes
/// the SSD1680 exposes under the tri-colour waveform: 0x00 means "set this
/// plane everywhere", 0xFF means "clear it".
void pass(const char* label, uint8_t black, uint8_t colour) {
    const uint32_t started = millis();
    Serial.printf("[clear] %-14s ", label);
    Serial.flush();
    panel.epd2.clearScreen(black, colour);
    Serial.printf("%lu ms\n", static_cast<unsigned long>(millis() - started));
}

}  // namespace

void setup() {
    Serial.begin(config::kSerialBaud);
    delay(400);

    Serial.println();
    Serial.println("[clear] ChickenMicro one-shot panel cleaner");
    Serial.printf("[clear] panel %s\n", EINK_MODEL_NAME);
    Serial.println("[clear] each pass uses the tri-colour waveform and takes ~14 s");

    SPI.begin(config::kEpdSck, -1, config::kEpdMosi, config::kEpdCs);
    // serial_diag_bitrate = 0: GxEPD2 would otherwise re-open Serial itself.
    panel.init(0, true, 50, false);
    panel.setRotation(EINK_ROTATION);
    Serial.printf("[clear] %dx%d\n", panel.width(), panel.height());

    const uint32_t started = millis();

    // Black and red are driven to full before clearing, which both wipes any
    // residual red and re-saturates pigment that has gone muddy while idle.
    pass("all black", 0x00, 0xFF);
    pass("all red", 0xFF, 0x00);
    pass("all white", 0xFF, 0xFF);

    panel.hibernate();

    Serial.printf("[clear] done in %lu ms\n", static_cast<unsigned long>(millis() - started));
    Serial.println("[clear] the panel should now be uniformly white, with no red left");
    Serial.println("[clear] now reflash the firmware: pio run -e esp32dev --target upload");
}

void loop() {
    // Nothing to do. E-ink holds its image with no power, so idling here keeps
    // the cleared panel as it is until the real firmware is flashed.
    delay(1000);
}
