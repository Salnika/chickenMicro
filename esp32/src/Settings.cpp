#include "Settings.h"

#include <Preferences.h>

#include "Log.h"

namespace settings {
namespace {

// NVS namespace and key names are limited to 15 characters.
constexpr const char* kNamespace = "chickenmicro";
constexpr const char* kModeKey = "mode";

Preferences nvs;

}  // namespace

bool Store::begin() {
    ready_ = false;
    permissionMode_ = proto::PermissionMode::Auto;
    if (!nvs.begin(kNamespace, /*readOnly=*/false)) {
        logMessage(proto::LogLevel::Warn, "[settings] cannot open NVS: settings will not be kept");
        return false;
    }
    ready_ = true;
    // A byte nobody wrote, or one that is not a mode, reads as the default.
    permissionMode_ = proto::permissionModeFromByte(
        nvs.getUChar(kModeKey, static_cast<uint8_t>(proto::PermissionMode::Auto)));
    logf(proto::LogLevel::Info, "[settings] answer mode %s",
         proto::permissionModeName(permissionMode_));
    return true;
}

bool Store::savePermissionMode(proto::PermissionMode mode) {
    permissionMode_ = mode;
    if (!ready_) return false;
    return nvs.putUChar(kModeKey, static_cast<uint8_t>(mode)) == 1;
}

}  // namespace settings
