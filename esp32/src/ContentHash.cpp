#include "ContentHash.h"

namespace ui {

uint32_t hashByte(uint32_t seed, uint8_t value) {
    return (seed ^ value) * 16777619u;
}

uint32_t hashString(uint32_t seed, const char* text) {
    uint32_t hash = seed;
    if (text == nullptr) return hash;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        hash ^= static_cast<uint8_t>(*cursor);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t contentHashOf(const app::State& state, Waveform waveform) {
    uint32_t hash = 2166136261u;
    hash = hashByte(hash, static_cast<uint8_t>(state.screen()));
    hash = hashByte(hash, static_cast<uint8_t>(state.claudeState()));
    hash = hashByte(hash, static_cast<uint8_t>(state.page()));
    hash = hashByte(hash, state.linkUp() ? 1u : 0u);
    hash = hashByte(hash, state.loaderFrame());
    hash = hashByte(hash, state.cursor());
    hash = hashByte(hash, state.menuCursor());
    // The window the menu has scrolled to: one cursor row can sit in two windows,
    // and a menu that moved with the cursor unchanged must still be redrawn.
    hash = hashByte(hash, state.menuFirstRow());
    // Part of what the menu shows, so switching waveform redraws its label.
    hash = hashByte(hash, static_cast<uint8_t>(waveform));
    // Likewise the answer mode: an entry that flips a value has to show it.
    hash = hashByte(hash, static_cast<uint8_t>(state.permissionMode()));
    hash = hashByte(hash, state.sessionTotal());
    hash = hashString(hash, state.focusId());
    hash = hashString(hash, state.detail());
    hash = hashString(hash, state.message());
    for (uint8_t index = 0; index < state.sessionCount(); ++index) {
        const proto::SessionEntry& session = state.sessionAt(index);
        hash = hashString(hash, session.id);
        hash = hashString(hash, session.name);
        hash = hashByte(hash, static_cast<uint8_t>(session.state));
    }
    // One-time codes: every field their three screens draw.
    hash = hashByte(hash, state.hasClock() ? 1u : 0u);
    hash = hashByte(hash, state.otpCount());
    hash = hashByte(hash, state.otpCursor());
    for (uint8_t index = 0; index < state.otpCount(); ++index) {
        hash = hashString(hash, state.otpName(index));
    }
    hash = hashString(hash, state.otpCodeName());
    hash = hashString(hash, state.otpCode());
    hash = hashByte(hash, state.otpExpired() ? 1u : 0u);
    hash = hashByte(hash, static_cast<uint8_t>(state.otpSecondsLeft()));
    hash = hashString(hash, state.setupNetwork());
    hash = hashString(hash, state.setupPassword());
    hash = hashString(hash, state.setupNote());
    if (state.permission().valid) {
        hash = hashString(hash, state.permission().id);
        hash = hashString(hash, state.permission().kind);
        hash = hashString(hash, state.permission().content);
        hash = hashString(hash, state.permission().title);
        hash = hashString(hash, state.permission().session);
    }
    return hash;
}

}  // namespace ui
