#pragma once

#include "Protocol.h"

namespace settings {

/// What the box remembers across a power cycle, in the chip's NVS flash.
///
/// Not the one-time-code accounts, which have their own store and their own
/// threat model: these are preferences, and losing them costs nothing.
class Store {
public:
    /// Opens the store and reads what it holds. False when flash cannot be
    /// opened; the box then runs on its defaults and cannot save.
    bool begin();

    /// How the box wants permissions answered. The default, `Auto`, answers
    /// through a Claude Code hook and falls back to the window.
    proto::PermissionMode permissionMode() const { return permissionMode_; }

    /// Remembers `mode`. False when it could not be written; the value is kept
    /// for this boot either way.
    bool savePermissionMode(proto::PermissionMode mode);

private:
    bool ready_ = false;
    proto::PermissionMode permissionMode_ = proto::PermissionMode::Auto;
};

}  // namespace settings
