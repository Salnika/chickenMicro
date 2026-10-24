#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ButtonState.h"
#include "Config.h"
#include "ExpanderLogic.h"

namespace input {

/// The five front inputs: the four keycaps left to right, then the joystick's
/// click.
enum class ButtonId : uint8_t {
    Deny,
    Stop,
    Approve,
    Action,
    Joystick,
};
constexpr uint8_t kButtonCount = 5;

/// The five inputs, however they reach the MCU.
///
/// Two wirings behind one interface, picked per board in Config.h:
///  - **GPIO** — one pin per input, each watched by its own interrupt
///    (ESP32 DevKit, ESP32-S3). ButtonBankGpio.cpp.
///  - **MCP23008** — all five on one I2C expander, for the ESP32-C3 SuperMini,
///    which has too few pins to give each its own. ButtonBankExpander.cpp.
///
/// Either way a press is caught in hardware the instant it happens — by a GPIO
/// interrupt, or by the expander's interrupt-capture register — so a tap that
/// lands during a blocking e-ink refresh is still delivered afterwards.
class ButtonBank {
public:
    /// Configures the hardware. Returns false if it did not answer — the
    /// expander missing from the bus. The inputs then read as released, never
    /// pressed, and the bank keeps retrying on its own.
    bool begin(uint32_t nowMs);

    /// Brings every input up to date. Cheap: call it every loop, and from the
    /// display's busy callback while a refresh holds the loop up.
    void sample(uint32_t nowMs);

    ButtonState& operator[](ButtonId id) { return states_[static_cast<uint8_t>(id)]; }

    /// One line for the boot log: which physical input carries which key. It is
    /// the one thing that cannot be checked from outside the box.
    void describe(char* out, size_t capacity) const;

private:
    ButtonState states_[kButtonCount];

#if CM_INPUTS_ON_MCP23008
    bool configureExpander();
    bool readExpander(mcp23008::Sample& out);
    void resync(uint32_t nowMs, uint8_t gpio);

    bool present_ = false;
    uint32_t lastReadMs_ = 0;
    uint32_t lastRetryMs_ = 0;
#else
    /// One pin's interrupt-side state. Written in the ISR, read on the loop:
    /// aligned 32-bit words, single writer, so no lock is needed.
    struct PinWatch {
        uint8_t gpio = 0;
        volatile bool isrDown = false;
        volatile uint32_t lastIsrMs = 0;
        volatile uint32_t pressAtMs = 0;
        /// Presses seen by the ISR. A counter rather than a flag: the loop
        /// compares it with the last value it handled, so a press landing
        /// between its read and its reset can never be wiped out.
        volatile uint32_t pressCount = 0;
        uint32_t handledCount = 0;
    };

    static void isr(void* arg);

    PinWatch pins_[kButtonCount];
#endif
};

}  // namespace input
