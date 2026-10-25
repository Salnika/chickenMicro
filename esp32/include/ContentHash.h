#pragma once

#include <stdint.h>

#include "State.h"
#include "Waveform.h"

namespace ui {

/// FNV-1a style folding of one byte into a running hash.
uint32_t hashByte(uint32_t seed, uint8_t value);

/// Folds every byte of `text` into a running hash. A null string folds nothing.
uint32_t hashString(uint32_t seed, const char* text);

/// A fingerprint of everything the panel's *content* depends on.
///
/// The display redraws only when this changes, because an e-ink refresh is slow
/// and visible. That makes it the one place where a field that is drawn but not
/// hashed becomes a bug: the screen simply never updates when it changes, and
/// shows the old value until something else happens to move the hash. The menu's
/// "Answer via" entry did exactly that.
///
/// So anything a screen draws from `State` belongs here. `waveform` is the one
/// input that is not in `State`: it lives in the panel driver.
uint32_t contentHashOf(const app::State& state, Waveform waveform);

}  // namespace ui
