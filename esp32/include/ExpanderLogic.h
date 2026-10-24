#pragma once

#include <stdint.h>

#include "ButtonState.h"

// ---------------------------------------------------------------------------
// MCP23008 I2C I/O expander: register map and the decoding of one read.
//
// Hardware-free, so the rules that turn register bytes into presses — the part
// that must never invent an APPROVE — run in `pio test -e native`. The bus
// access itself lives in ButtonBankExpander.cpp.
// ---------------------------------------------------------------------------

namespace input {
namespace mcp23008 {

// Register addresses (Microchip DS21919, table 1-3), with IOCON.BANK unused:
// the MCP23008 has a single bank.
constexpr uint8_t kIodir = 0x00;    // 1 = input
constexpr uint8_t kIpol = 0x01;     // 1 = invert the GPIO bit
constexpr uint8_t kGpinten = 0x02;  // 1 = interrupt-on-change enabled
constexpr uint8_t kDefval = 0x03;   // compare value, unused with INTCON = 0
constexpr uint8_t kIntcon = 0x04;   // 0 = interrupt when a pin changes at all
constexpr uint8_t kIocon = 0x05;    // SEQOP = 0: the register pointer advances
constexpr uint8_t kGppu = 0x06;     // 1 = 100 kΩ pull-up on
constexpr uint8_t kIntf = 0x07;     // which pin caused the pending interrupt
constexpr uint8_t kIntcap = 0x08;   // port value captured when it happened
constexpr uint8_t kGpio = 0x09;     // port value now

/// Pull-ups on every pin, the spare ones too, so nothing on the chip floats.
/// Doubles as a signature: after a brown-out the chip resets GPPU to 0x00, and
/// a read that no longer shows 0xFF is a chip that forgot its configuration.
constexpr uint8_t kPullUps = 0xFF;

/// One read of GPPU, INTF, INTCAP and GPIO, in a single sequential transfer.
/// Defaults describe "nothing pressed, nothing pending".
struct Sample {
    uint8_t gppu = kPullUps;
    uint8_t intf = 0x00;
    uint8_t intcap = 0xFF;
    uint8_t gpio = 0xFF;
};

/// Whether the chip still holds the configuration this firmware gave it.
///
/// If not, every input may be floating — and an input that floats low reads
/// as pressed. Such a sample must be discarded whole, never decoded.
inline bool configured(const Sample& sample) { return sample.gppu == kPullUps; }

/// Folds one trusted read into the button states.
///
/// Per input, three things, in this order:
///  1. INTF says the pin changed since the previous read, and INTCAP holds the
///     level it changed *to*. That first change is latched by the chip itself,
///     which is what catches a tap that came and went while the loop was blind
///     behind a refresh — its press survives in INTCAP after the key is up.
///  2. The level now, as a second edge: after a tap, this is what brings the
///     edge tracker back to "released", so the next press is not mistaken for
///     a level it already holds.
///  3. The level now, as a sample, for hold, release and click.
///
/// Inputs are pulled up, so a pressed key reads 0.
template <uint8_t Count>
inline void apply(const Sample& sample,
                  const uint8_t (&pins)[Count],
                  ButtonState (&states)[Count],
                  uint32_t nowMs,
                  uint32_t debounceMs) {
    for (uint8_t index = 0; index < Count; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1u << pins[index]);
        ButtonState& state = states[index];
        if ((sample.intf & bit) != 0) {
            state.noteEdge((sample.intcap & bit) == 0, nowMs, debounceMs);
        }
        const bool down = (sample.gpio & bit) == 0;
        state.noteEdge(down, nowMs, debounceMs);
        state.sample(down, nowMs, debounceMs);
    }
}

/// Interrupt-on-change mask for the pins in use.
template <uint8_t Count>
inline uint8_t maskOf(const uint8_t (&pins)[Count]) {
    uint8_t mask = 0;
    for (uint8_t index = 0; index < Count; ++index) {
        mask = static_cast<uint8_t>(mask | (1u << pins[index]));
    }
    return mask;
}

}  // namespace mcp23008
}  // namespace input
