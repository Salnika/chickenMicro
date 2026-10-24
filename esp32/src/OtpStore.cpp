#include "OtpStore.h"

#include <Preferences.h>
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

#include "Log.h"

namespace otp {
namespace {

constexpr const char* kNamespace = "otp";

// One account per NVS blob "a<slot>", in an explicit layout rather than the raw
// struct, so a compiler change cannot silently reinterpret stored secrets:
//   [0] layout version  [1..25] name  [26] secret length  [27..90] secret
//   [91] digits  [92..93] period, little-endian  [94] algorithm
constexpr uint8_t kLayoutVersion = 1;
constexpr size_t kRecordBytes = 1 + kNameCapacity + 1 + kMaxSecretBytes + 1 + 2 + 1;

Preferences nvs;

void slotKey(uint8_t slot, char (&key)[5]) {
    snprintf(key, sizeof(key), "a%u", static_cast<unsigned>(slot));
}

size_t hmacMbedtls(Algorithm algorithm, const uint8_t* key, size_t keyLength,
                   const uint8_t* message, size_t messageLength, uint8_t* digest,
                   size_t capacity) {
    mbedtls_md_type_t type = MBEDTLS_MD_SHA1;
    switch (algorithm) {
        case Algorithm::Sha1: type = MBEDTLS_MD_SHA1; break;
        case Algorithm::Sha256: type = MBEDTLS_MD_SHA256; break;
        case Algorithm::Sha512: type = MBEDTLS_MD_SHA512; break;
    }
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
    if (info == nullptr) return 0;
    const size_t size = mbedtls_md_get_size(info);
    if (size > capacity) return 0;
    if (mbedtls_md_hmac(info, key, keyLength, message, messageLength, digest) != 0) return 0;
    return size;
}

/// RFC 6238 appendix B at T = 59, one vector per algorithm. The host tests
/// prove the TOTP logic; this proves mbedTLS is wired to it correctly, on the
/// chip that will actually compute the codes.
bool selfTest() {
    struct Vector {
        Algorithm algorithm;
        const char* key;
        const char* code;
    };
    static const Vector vectors[] = {
        {Algorithm::Sha1, "12345678901234567890", "94287082"},
        {Algorithm::Sha256, "12345678901234567890123456789012", "46119246"},
        {Algorithm::Sha512,
         "1234567890123456789012345678901234567890123456789012345678901234", "90693936"},
    };
    for (const Vector& vector : vectors) {
        Account account;
        account.secretLength = static_cast<uint8_t>(strlen(vector.key));
        memcpy(account.secret, vector.key, account.secretLength);
        account.digits = 8;
        account.period = 30;
        account.algorithm = vector.algorithm;
        uint32_t code = 0;
        if (!totp(account, 59, hmacMbedtls, code)) return false;
        char text[9];
        formatCode(code, 8, text, sizeof(text));
        if (strcmp(text, vector.code) != 0) return false;
    }
    return true;
}

void pack(const Account& account, uint8_t (&record)[kRecordBytes]) {
    memset(record, 0, sizeof(record));
    size_t at = 0;
    record[at++] = kLayoutVersion;
    memcpy(&record[at], account.name, kNameCapacity);
    at += kNameCapacity;
    record[at++] = account.secretLength;
    memcpy(&record[at], account.secret, kMaxSecretBytes);
    at += kMaxSecretBytes;
    record[at++] = account.digits;
    record[at++] = static_cast<uint8_t>(account.period & 0xFF);
    record[at++] = static_cast<uint8_t>(account.period >> 8);
    record[at++] = static_cast<uint8_t>(account.algorithm);
}

/// Rebuilds an account from a record, refusing anything out of range: a record
/// that does not read back exactly as written is not one to derive codes from.
bool unpack(const uint8_t (&record)[kRecordBytes], Account& account) {
    size_t at = 0;
    if (record[at++] != kLayoutVersion) return false;
    memcpy(account.name, &record[at], kNameCapacity);
    at += kNameCapacity;
    if (account.name[kNameCapacity - 1] != '\0' || account.name[0] == '\0') return false;
    account.secretLength = record[at++];
    if (account.secretLength == 0 || account.secretLength > kMaxSecretBytes) return false;
    memcpy(account.secret, &record[at], kMaxSecretBytes);
    at += kMaxSecretBytes;
    account.digits = record[at++];
    account.period = static_cast<uint16_t>(record[at] | (record[at + 1] << 8));
    at += 2;
    const uint8_t algorithm = record[at++];
    if (account.digits < kMinDigits || account.digits > kMaxDigits) return false;
    if (account.period < kMinPeriod || account.period > kMaxPeriod) return false;
    if (algorithm > static_cast<uint8_t>(Algorithm::Sha512)) return false;
    account.algorithm = static_cast<Algorithm>(algorithm);
    return true;
}

char lower(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character + 32) : character;
}

int compareNames(const char* left, const char* right) {
    for (;; ++left, ++right) {
        const char a = lower(*left);
        const char b = lower(*right);
        if (a != b || a == '\0') return a - b;
    }
}

/// Secrets pass through stack buffers on their way in and out of NVS; they are
/// wiped as soon as they are done with.
void wipe(void* memory, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
    while (length-- > 0) *bytes++ = 0;
}

}  // namespace

const char* describe(Store::AddResult result) {
    switch (result) {
        case Store::AddResult::Added: return "Added.";
        case Store::AddResult::Full: return "The box is full (16 accounts). Remove one first.";
        case Store::AddResult::Duplicate: return "An account with that name already exists.";
        case Store::AddResult::Unavailable: return "One-time codes are disabled on this box: see its log.";
        case Store::AddResult::WriteFailed: return "Could not save it to flash. Try again.";
    }
    return "?";
}

bool Store::begin() {
    ready_ = false;
    count_ = 0;
    if (!selfTest()) {
        logMessage(proto::LogLevel::Error,
                   "[otp] HMAC self-test failed against RFC 6238: one-time codes disabled");
        return false;
    }
    if (!nvs.begin(kNamespace, /*readOnly=*/false)) {
        logMessage(proto::LogLevel::Error, "[otp] cannot open NVS: one-time codes disabled");
        return false;
    }
    for (uint8_t slot = 0; slot < app::kMaxOtpAccounts; ++slot) {
        char key[5];
        slotKey(slot, key);
        if (!nvs.isKey(key)) continue;
        uint8_t record[kRecordBytes];
        Account account;
        const size_t got = nvs.getBytes(key, record, sizeof(record));
        const bool valid = got == sizeof(record) && unpack(record, account);
        wipe(record, sizeof(record));
        if (!valid) {
            logf(proto::LogLevel::Warn, "[otp] stored account %u is unreadable; ignored", slot);
            continue;
        }
        entries_[count_].account = account;
        entries_[count_].slot = slot;
        ++count_;
        wipe(&account, sizeof(account));
    }
    sort();
    ready_ = true;
    logf(proto::LogLevel::Info, "[otp] %u account(s) loaded; HMAC self-test passed",
         static_cast<unsigned>(count_));
    return true;
}

const char* Store::name(uint8_t index) const {
    return index < count_ ? entries_[index].account.name : "";
}

uint8_t Store::names(const char* out[], uint8_t capacity) const {
    uint8_t written = 0;
    for (; written < count_ && written < capacity; ++written) {
        out[written] = entries_[written].account.name;
    }
    return written;
}

int Store::freeSlot() const {
    for (uint8_t slot = 0; slot < app::kMaxOtpAccounts; ++slot) {
        bool used = false;
        for (uint8_t index = 0; index < count_ && !used; ++index) {
            used = entries_[index].slot == slot;
        }
        if (!used) return slot;
    }
    return -1;
}

void Store::sort() {
    // Sixteen entries at most: an insertion sort is plenty.
    for (uint8_t index = 1; index < count_; ++index) {
        Entry moving = entries_[index];
        int at = index - 1;
        while (at >= 0 && compareNames(entries_[at].account.name, moving.account.name) > 0) {
            entries_[at + 1] = entries_[at];
            --at;
        }
        entries_[at + 1] = moving;
        wipe(&moving, sizeof(moving));
    }
}

int Store::find(const char* name) const {
    if (name == nullptr) return -1;
    for (uint8_t index = 0; index < count_; ++index) {
        if (compareNames(entries_[index].account.name, name) == 0) return index;
    }
    return -1;
}

Store::AddResult Store::add(const Account& account) {
    if (!ready_) return AddResult::Unavailable;
    if (count_ >= app::kMaxOtpAccounts) return AddResult::Full;
    for (uint8_t index = 0; index < count_; ++index) {
        if (compareNames(entries_[index].account.name, account.name) == 0) {
            return AddResult::Duplicate;
        }
    }
    const int slot = freeSlot();
    if (slot < 0) return AddResult::Full;

    uint8_t record[kRecordBytes];
    pack(account, record);
    char key[5];
    slotKey(static_cast<uint8_t>(slot), key);
    const size_t written = nvs.putBytes(key, record, sizeof(record));
    wipe(record, sizeof(record));
    if (written != sizeof(record)) return AddResult::WriteFailed;

    entries_[count_].account = account;
    entries_[count_].slot = static_cast<uint8_t>(slot);
    ++count_;
    sort();
    logf(proto::LogLevel::Info, "[otp] account \"%s\" added (%u stored)", account.name,
         static_cast<unsigned>(count_));
    return AddResult::Added;
}

bool Store::remove(uint8_t index) {
    if (!ready_ || index >= count_) return false;
    char key[5];
    slotKey(entries_[index].slot, key);
    // NVS erases logically: the bytes stay in flash until the page is recycled.
    // Erasing the whole flash is the only way to be sure they are gone.
    if (!nvs.remove(key)) return false;
    logf(proto::LogLevel::Info, "[otp] account \"%s\" removed", entries_[index].account.name);
    for (uint8_t at = index; at + 1 < count_; ++at) entries_[at] = entries_[at + 1];
    --count_;
    wipe(&entries_[count_], sizeof(Entry));
    return true;
}

bool Store::code(uint8_t index, uint64_t unixSeconds, char* out, size_t capacity,
                 uint16_t& secondsLeft) const {
    if (!ready_ || index >= count_) return false;
    const Account& account = entries_[index].account;
    uint32_t value = 0;
    if (!totp(account, unixSeconds, hmacMbedtls, value)) return false;
    formatCode(value, account.digits, out, capacity);
    secondsLeft = otp::secondsLeft(account, unixSeconds);
    return out[0] != '\0';
}

}  // namespace otp
