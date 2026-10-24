// The five inputs on an MCP23008 I2C expander — the ESP32-C3 SuperMini, whose
// thirteen GPIO are all spoken for by the panel, the joystick and the bus.
//
// No INT wire. The expander latches the first change on any pin in INTF and
// INTCAP until they are read, so reading it every few milliseconds loses
// nothing — including a tap that starts and ends while the loop is stuck behind
// an e-ink refresh, which is exactly what the GPIO build uses interrupts for.
// The busy callback keeps reading through a refresh, too.
//
// The one thing this file must never do is invent a press. The inputs are
// pulled up, so a pressed key reads 0 — and a bus that returns zeros would read
// as every key down at once, APPROVE included. So a read is used only when the
// transfer succeeded *and* the chip still shows the pull-up configuration this
// firmware gave it; anything else is dropped whole, and the keys read as
// released until the chip answers properly again.

#include "ButtonBank.h"

#if CM_INPUTS_ON_MCP23008

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>

#include "Log.h"

namespace input {
namespace {

/// The expander pin behind each input, in ButtonId order.
constexpr uint8_t kPins[kButtonCount] = {
    config::kExpanderDeny,
    config::kExpanderStop,
    config::kExpanderApprove,
    config::kExpanderAction,
    config::kExpanderJoystick,
};

bool writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(config::kExpanderAddress);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

}  // namespace

bool ButtonBank::configureExpander() {
    using namespace mcp23008;
    // IOCON first, because it decides how the rest is addressed: SEQOP = 0 keeps
    // the register pointer advancing, so one read walks GPPU, INTF, INTCAP and
    // GPIO. INT is left push-pull and unconnected.
    return writeRegister(kIocon, 0x00) &&
           writeRegister(kIodir, 0xFF) &&     // all inputs
           writeRegister(kIpol, 0x00) &&      // no inversion: pressed reads 0
           writeRegister(kGppu, kPullUps) &&  // pull-ups everywhere
           writeRegister(kDefval, 0x00) &&
           writeRegister(kIntcon, 0x00) &&    // capture on any change
           writeRegister(kGpinten, maskOf(kPins));
}

bool ButtonBank::readExpander(mcp23008::Sample& out) {
    Wire.beginTransmission(config::kExpanderAddress);
    Wire.write(mcp23008::kGppu);
    // Repeated start: the pointer is set and read back in one transaction.
    if (Wire.endTransmission(false) != 0) return false;
    const size_t got = Wire.requestFrom(static_cast<uint16_t>(config::kExpanderAddress),
                                        static_cast<size_t>(4), true);
    if (got != 4) return false;
    // Reading INTCAP also releases the chip's capture, arming it for the next
    // change.
    out.gppu = static_cast<uint8_t>(Wire.read());
    out.intf = static_cast<uint8_t>(Wire.read());
    out.intcap = static_cast<uint8_t>(Wire.read());
    out.gpio = static_cast<uint8_t>(Wire.read());
    return true;
}

void ButtonBank::resync(uint32_t nowMs, uint8_t gpio) {
    // Start every key from the level it is at now, so one held across a
    // (re)connection is not reported as a fresh press.
    for (uint8_t index = 0; index < kButtonCount; ++index) {
        states_[index].begin((gpio & (1u << kPins[index])) == 0, nowMs);
    }
}

bool ButtonBank::begin(uint32_t nowMs) {
    Wire.begin(config::kI2cSda, config::kI2cScl, config::kI2cClockHz);
    // A missing or wedged expander must cost a few milliseconds per retry, not
    // Wire's default 50.
    Wire.setTimeOut(config::kI2cTimeoutMs);

    present_ = false;
    lastRetryMs_ = nowMs;
    resync(nowMs, 0xFF);   // released until proven otherwise

    mcp23008::Sample sample;
    if (configureExpander() && readExpander(sample) && mcp23008::configured(sample)) {
        present_ = true;
        lastReadMs_ = nowMs;
        resync(nowMs, sample.gpio);
        return true;
    }

    logf(proto::LogLevel::Error,
         "[input] no MCP23008 at 0x%02X (SDA=GPIO%u, SCL=GPIO%u): buttons read as released "
         "until it answers. Check VDD on 3V3, RESET tied to 3V3, A0-A2 to GND.",
         config::kExpanderAddress, config::kI2cSda, config::kI2cScl);
    return false;
}

void ButtonBank::sample(uint32_t nowMs) {
    if (!present_) {
        // Retry about once a second, so a loose wire or a late-powered expander
        // recovers without a reboot.
        if (nowMs - lastRetryMs_ < config::kExpanderRetryMs) return;
        lastRetryMs_ = nowMs;
        mcp23008::Sample sample;
        if (!configureExpander() || !readExpander(sample) || !mcp23008::configured(sample)) {
            return;
        }
        present_ = true;
        lastReadMs_ = nowMs;
        resync(nowMs, sample.gpio);
        logf(proto::LogLevel::Info, "[input] MCP23008 answering at 0x%02X",
             config::kExpanderAddress);
        return;
    }

    // Every few milliseconds is plenty: the chip holds the first change between
    // reads, and the busy callback would otherwise hit the bus every millisecond.
    if (nowMs - lastReadMs_ < config::kExpanderPollMs) return;
    lastReadMs_ = nowMs;

    mcp23008::Sample sample;
    if (!readExpander(sample) || !mcp23008::configured(sample)) {
        // Dropped whole — see the top of this file. Everything reads released
        // from here on, so no key is left looking held for a hold to fire on.
        present_ = false;
        lastRetryMs_ = nowMs;
        resync(nowMs, 0xFF);
        logMessage(proto::LogLevel::Warn,
                   "[input] lost the MCP23008 (bus error or chip reset); "
                   "buttons read as released until it answers");
        return;
    }
    mcp23008::apply(sample, kPins, states_, nowMs, config::kButtonDebounceMs);
}

void ButtonBank::describe(char* out, size_t capacity) const {
    snprintf(out, capacity,
             "keys on MCP23008 0x%02X: X=GP%u deny, #=GP%u stop, v=GP%u approve, "
             "<-=GP%u action, joystick click=GP%u%s",
             config::kExpanderAddress, kPins[0], kPins[1], kPins[2], kPins[3], kPins[4],
             present_ ? "" : " (NOT ANSWERING)");
}

}  // namespace input

#endif  // CM_INPUTS_ON_MCP23008
