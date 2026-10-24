#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Time-based one-time passwords (RFC 6238), with no hardware in it.
//
// The box keeps each account's secret and turns it into a 6-8 digit code on a
// physical press. Only the code ever leaves the device: the secret is never
// logged, never shown, and never sent over the wire.
//
// Everything here is pure — base32, the otpauth:// link format, Google
// Authenticator's transfer links, the HOTP truncation — so it runs in
// `pio test -e native` against the RFC's own test vectors. The HMAC is handed in: mbedTLS on the device, a reference
// implementation in the tests.
// ---------------------------------------------------------------------------

namespace otp {

enum class Algorithm : uint8_t {
    Sha1,
    Sha256,
    Sha512,
};

/// Visible characters in an account's name, plus the terminating NUL.
constexpr size_t kNameCapacity = 25;
/// Largest decoded secret accepted. Services use 10-32 bytes; 64 is the
/// SHA-512 block size, past which HMAC would hash the key anyway.
constexpr size_t kMaxSecretBytes = 64;

constexpr uint8_t kMinDigits = 6;
constexpr uint8_t kMaxDigits = 8;
constexpr uint16_t kMinPeriod = 10;
constexpr uint16_t kMaxPeriod = 300;

/// One account: what the list shows, and what the code is made from.
struct Account {
    char name[kNameCapacity] = {0};
    uint8_t secret[kMaxSecretBytes] = {0};
    uint8_t secretLength = 0;
    uint8_t digits = 6;
    uint16_t period = 30;
    Algorithm algorithm = Algorithm::Sha1;
};

enum class ParseError : uint8_t {
    None,
    NotOtpauth,
    NotTotp,
    MissingName,
    MissingSecret,
    BadSecret,
    SecretTooLong,
    BadDigits,
    BadPeriod,
    BadAlgorithm,
    BadTransfer,
};

/// A sentence for the setup page, saying what to fix.
const char* describe(ParseError error);

const char* toString(Algorithm algorithm);
/// "SHA1", "SHA256" or "SHA512", any case. False for anything else.
bool parseAlgorithm(const char* text, Algorithm& out);

/// Decodes RFC 4648 base32, the way services print secrets: any case, with
/// spaces, dashes and trailing '=' padding ignored. False on any other
/// character, on an empty result, or when the result would not fit.
bool decodeBase32(const char* text, uint8_t* out, size_t capacity, size_t& length);

/// Reads an `otpauth://totp/...` link — what a setup QR code contains — into
/// `out`. The name is the link's label ("Issuer:account"), with the issuer
/// parameter put in front when the label lacks one, folded to printable ASCII
/// and clipped to fit.
ParseError parseOtpauthUri(const char* uri, Account& out);

/// True for an `otpauth-migration://` link, which MigrationReader reads.
bool isMigrationLink(const char* uri);

/// Longest transfer link accepted, and the most it can carry once decoded.
/// One of Google Authenticator's QR codes holds about ten accounts, 1.5 KB.
constexpr size_t kMaxMigrationLinkLength = 4096;
constexpr size_t kMaxMigrationPayload = 3072;

/// Reads an `otpauth-migration://offline?data=...` link: what Google
/// Authenticator's "Transfer accounts" QR codes hold — several accounts at
/// once, as a base64 protobuf — and what other apps export to move to it.
///
/// The format has no period: every account in it is a 30-second one.
class MigrationReader {
public:
    /// Decodes the link and checks all of it, so a damaged one is refused
    /// whole rather than half added: NotOtpauth or BadTransfer.
    ParseError open(const char* uri);

    /// The accounts in the link, the ones that cannot be added included.
    uint16_t count() const { return count_; }
    /// Google splits a long list over several QR codes: this link is number
    /// `batchIndex() + 1` of `batchSize()`. A size of 0 or 1 means it is alone.
    uint8_t batchIndex() const { return batchIndex_; }
    uint8_t batchSize() const { return batchSize_; }

    /// The next account, in the link's order. `error` says why this one
    /// cannot be added — a counter-based code, MD5 — while `out.name` still
    /// names it, so the page can say which. False once all have been read.
    bool next(Account& out, ParseError& error);

    /// The payload is every secret in the link: gone once it has been read.
    void wipe();

private:
    uint8_t payload_[kMaxMigrationPayload] = {0};
    size_t length_ = 0;
    size_t offset_ = 0;
    uint16_t count_ = 0;
    uint8_t batchIndex_ = 0;
    uint8_t batchSize_ = 0;
};

/// Builds an account from fields typed by hand: the secret a service shows
/// under "can't scan the code?".
ParseError makeAccount(const char* name,
                       const char* base32Secret,
                       uint8_t digits,
                       uint16_t period,
                       Algorithm algorithm,
                       Account& out);

/// Replaces an account's name with `name`, folded and clipped like a parsed
/// one. False, leaving the account alone, if nothing printable is left.
bool renameAccount(Account& account, const char* name);

/// HMAC of `message` under `key`. Writes the digest and returns its length (20,
/// 32 or 64 bytes), or 0 on failure.
using HmacFunction = size_t (*)(Algorithm algorithm,
                                const uint8_t* key,
                                size_t keyLength,
                                const uint8_t* message,
                                size_t messageLength,
                                uint8_t* digest,
                                size_t digestCapacity);

/// RFC 4226 HOTP value for `counter`. False if the HMAC failed.
bool hotp(const Account& account, uint64_t counter, HmacFunction hmac, uint32_t& code);

/// RFC 6238 TOTP value at `unixSeconds` (T0 = 0). False if the HMAC failed.
bool totp(const Account& account, uint64_t unixSeconds, HmacFunction hmac, uint32_t& code);

/// Seconds the code for `unixSeconds` remains valid: 1..period.
uint16_t secondsLeft(const Account& account, uint64_t unixSeconds);

/// `code` zero-padded to `digits`, e.g. 7081804 at 8 digits is "07081804".
/// `out` needs room for 9 bytes.
void formatCode(uint32_t code, uint8_t digits, char* out, size_t capacity);

}  // namespace otp
