#pragma once

#include <stddef.h>
#include <stdint.h>

#include "State.h"
#include "Totp.h"

namespace otp {

/// The one-time-code accounts, kept in the ESP32's NVS flash so they survive a
/// power cycle.
///
/// The secrets never leave this class. The rest of the firmware sees names and
/// codes only, and a code is only ever computed on behalf of a physical press:
/// nothing arriving over the wire can ask for one.
///
/// Threat model, stated plainly: NVS is not encrypted on this build. Anyone who
/// holds the board and can run esptool can read the flash, secrets included —
/// the same as a phone authenticator app on an unlocked phone. What it does
/// protect against is software on the Mac: that can never read a secret, nor
/// get a code without someone pressing the key. docs/otp.md.
class Store {
public:
    enum class AddResult : uint8_t {
        Added,
        Full,
        Duplicate,
        Unavailable,
        WriteFailed,
    };

    /// Checks the HMAC against RFC 6238's vectors, then opens NVS and loads the
    /// accounts. False, with codes disabled, if either fails.
    bool begin();
    bool ready() const { return ready_; }

    uint8_t count() const { return count_; }
    /// The account's name, "" past the end. Accounts are kept sorted by name.
    const char* name(uint8_t index) const;
    /// Every name, in list order, for State::setOtpAccounts.
    uint8_t names(const char* out[], uint8_t capacity) const;

    /// The index of the account called `name` (any case), or -1.
    int find(const char* name) const;

    AddResult add(const Account& account);
    bool remove(uint8_t index);

    /// The code at `unixSeconds` for account `index`, zero-padded into `out`,
    /// and how many seconds it has left.
    bool code(uint8_t index, uint64_t unixSeconds, char* out, size_t capacity,
              uint16_t& secondsLeft) const;

private:
    struct Entry {
        Account account;
        uint8_t slot = 0;   // NVS key "a<slot>"
    };

    void sort();
    int freeSlot() const;

    Entry entries_[app::kMaxOtpAccounts];
    uint8_t count_ = 0;
    bool ready_ = false;
};

const char* describe(Store::AddResult result);

}  // namespace otp
