#include "Totp.h"

#include <string.h>

namespace otp {
namespace {

char lower(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character + 32) : character;
}

bool equalsIgnoreCase(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (lower(*left) != lower(*right)) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool startsWithIgnoreCase(const char* text, const char* prefix) {
    while (*prefix != '\0') {
        if (lower(*text) != lower(*prefix)) return false;
        ++text;
        ++prefix;
    }
    return true;
}

int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

/// Percent-decodes `length` bytes of `text` into `out`. A malformed escape is
/// kept as typed rather than guessed at.
void percentDecode(const char* text, size_t length, char* out, size_t capacity) {
    size_t written = 0;
    for (size_t index = 0; index < length && written + 1 < capacity; ++index) {
        if (text[index] == '%' && index + 2 < length) {
            const int high = hexValue(text[index + 1]);
            const int low = hexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                out[written++] = static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        out[written++] = text[index];
    }
    out[written] = '\0';
}

/// Copies `text` into an account name: surrounding spaces trimmed, anything
/// the panel's font cannot draw replaced, clipped to fit.
///
/// The fonts cover 0x20-0x7E only. A UTF-8 character becomes a single '?':
/// its lead byte does, and its continuation bytes are dropped.
bool setName(const char* text, char* out) {
    while (*text == ' ') ++text;
    size_t written = 0;
    for (const char* cursor = text; *cursor != '\0' && written + 1 < kNameCapacity; ++cursor) {
        const unsigned char byte = static_cast<unsigned char>(*cursor);
        if (byte >= 0x80 && byte < 0xC0) continue;   // UTF-8 continuation
        out[written++] = (byte >= 0x20 && byte <= 0x7E) ? static_cast<char>(byte) : '?';
    }
    while (written > 0 && out[written - 1] == ' ') --written;
    out[written] = '\0';
    return written > 0;
}

/// The name shown for a label and an issuer. The label is usually
/// "Issuer:account" already; when it is only the account, the issuer goes in
/// front so two services' "alice" accounts stay apart in the list.
bool composeName(const char* label, const char* issuer, char* out) {
    char name[160];
    if (strchr(label, ':') == nullptr && issuer[0] != '\0') {
        strncpy(name, issuer, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (label[0] != '\0') {
            strncat(name, ":", sizeof(name) - strlen(name) - 1);
            strncat(name, label, sizeof(name) - strlen(name) - 1);
        }
    } else {
        strncpy(name, label, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    return setName(name, out);
}

void wipeBytes(void* memory, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
    while (length-- > 0) *bytes++ = 0;
}

// --- Transfer links: base64 and the protobuf inside -------------------------

int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+' || character == '-') return 62;   // standard or URL-safe
    if (character == '/' || character == '_') return 63;
    return -1;
}

/// Decodes the `data` parameter: percent-escapes undone on the fly, then
/// base64 in either alphabet, padding optional. False on anything else, or if
/// it would not fit.
bool decodeMigrationData(const char* text, size_t length, uint8_t* out, size_t capacity,
                         size_t& written) {
    uint32_t buffer = 0;
    int bits = 0;
    int symbols = 0;
    bool padding = false;
    written = 0;
    for (size_t index = 0; index < length; ++index) {
        char character = text[index];
        if (character == '%') {
            if (index + 2 >= length) return false;
            const int high = hexValue(text[index + 1]);
            const int low = hexValue(text[index + 2]);
            if (high < 0 || low < 0) return false;
            character = static_cast<char>((high << 4) | low);
            index += 2;
        }
        if (character == '=') {
            padding = true;
            continue;
        }
        const int value = base64Value(character);
        if (value < 0 || padding) return false;
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        ++symbols;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity) return false;
            out[written++] = static_cast<uint8_t>(buffer >> bits);
        }
    }
    // A lone symbol in the last group carries no whole byte: cut short.
    return symbols % 4 != 1 && written > 0;
}

/// A read position in a protobuf message.
struct Wire {
    const uint8_t* data;
    size_t length;
    size_t offset;
};

bool readVarint(Wire& wire, uint64_t& out) {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (wire.offset >= wire.length) return false;
        const uint8_t byte = wire.data[wire.offset++];
        out |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

bool readKey(Wire& wire, uint32_t& field, uint8_t& type) {
    uint64_t key = 0;
    if (!readVarint(wire, key) || (key >> 3) == 0 || (key >> 3) > 0x1FFFFFFF) return false;
    field = static_cast<uint32_t>(key >> 3);
    type = static_cast<uint8_t>(key & 7);
    return true;
}

bool readLengthDelimited(Wire& wire, const uint8_t*& start, size_t& length) {
    uint64_t declared = 0;
    if (!readVarint(wire, declared) || declared > wire.length - wire.offset) return false;
    start = wire.data + wire.offset;
    length = static_cast<size_t>(declared);
    wire.offset += length;
    return true;
}

/// Steps over a field this reader has no use for. Groups (types 3 and 4) are
/// long deprecated and never in this format: refused.
bool skipField(Wire& wire, uint8_t type) {
    uint64_t ignored = 0;
    const uint8_t* start = nullptr;
    size_t length = 0;
    switch (type) {
        case 0: return readVarint(wire, ignored);
        case 1: length = 8; break;
        case 2: return readLengthDelimited(wire, start, length);
        case 5: length = 4; break;
        default: return false;
    }
    if (length > wire.length - wire.offset) return false;
    wire.offset += length;
    return true;
}

void copyText(const uint8_t* start, size_t length, char* out, size_t capacity) {
    const size_t kept = length < capacity - 1 ? length : capacity - 1;
    memcpy(out, start, kept);
    out[kept] = '\0';
}

/// One OtpParameters message:
///   1 secret (bytes)   2 name   3 issuer   4 algorithm   5 digits   6 type
/// False if it is not well formed. Otherwise `error` says whether the account
/// can be added, and `out.name` is set either way.
bool readParameters(const uint8_t* data, size_t length, Account& out, ParseError& error) {
    Wire wire{data, length, 0};
    const uint8_t* secret = nullptr;
    size_t secretLength = 0;
    char label[128] = {0};
    char issuer[64] = {0};
    uint64_t algorithm = 0;
    uint64_t digits = 0;
    uint64_t type = 0;

    while (wire.offset < wire.length) {
        uint32_t field = 0;
        uint8_t kind = 0;
        if (!readKey(wire, field, kind)) return false;
        const uint8_t* start = nullptr;
        size_t size = 0;
        if (kind == 2 && field >= 1 && field <= 3) {
            if (!readLengthDelimited(wire, start, size)) return false;
            if (field == 1) {
                secret = start;
                secretLength = size;
            } else {
                copyText(start, size, field == 2 ? label : issuer,
                         field == 2 ? sizeof(label) : sizeof(issuer));
            }
        } else if (kind == 0 && field >= 4 && field <= 6) {
            uint64_t& target = field == 4 ? algorithm : (field == 5 ? digits : type);
            if (!readVarint(wire, target)) return false;
        } else if (!skipField(wire, kind)) {   // 7 counter, and anything newer
            return false;
        }
    }

    Account account;
    const bool named = composeName(label, issuer, account.name);
    // Enum values: type 1 HOTP, 2 TOTP; algorithm 1 SHA1, 2 SHA256, 3 SHA512,
    // 4 MD5; digits 1 six, 2 eight. 0 is "unspecified", which means the usual.
    error = ParseError::None;
    if (type != 0 && type != 2) {
        error = ParseError::NotTotp;
    } else if (algorithm > 3) {
        error = ParseError::BadAlgorithm;
    } else if (digits > 2) {
        error = ParseError::BadDigits;
    } else if (secret == nullptr || secretLength == 0) {
        error = ParseError::MissingSecret;
    } else if (secretLength > kMaxSecretBytes) {
        error = ParseError::SecretTooLong;
    } else if (!named) {
        error = ParseError::MissingName;
    }
    if (error == ParseError::None) {
        account.algorithm = algorithm == 2   ? Algorithm::Sha256
                            : algorithm == 3 ? Algorithm::Sha512
                                             : Algorithm::Sha1;
        account.digits = digits == 2 ? 8 : 6;
        account.period = 30;
        memcpy(account.secret, secret, secretLength);
        account.secretLength = static_cast<uint8_t>(secretLength);
    }
    out = account;
    wipeBytes(&account, sizeof(account));
    return true;
}

bool setSecret(const char* base32, Account& out, ParseError& error) {
    size_t length = 0;
    uint8_t decoded[kMaxSecretBytes + 1];
    // One byte of headroom tells "too long" apart from "not base32".
    if (!decodeBase32(base32, decoded, sizeof(decoded), length)) {
        size_t probe = 0;
        uint8_t wide[256];
        error = decodeBase32(base32, wide, sizeof(wide), probe) ? ParseError::SecretTooLong
                                                                 : ParseError::BadSecret;
        return false;
    }
    if (length > kMaxSecretBytes) {
        error = ParseError::SecretTooLong;
        return false;
    }
    memcpy(out.secret, decoded, length);
    out.secretLength = static_cast<uint8_t>(length);
    return true;
}

bool readNumber(const char* text, uint32_t& out) {
    if (*text == '\0') return false;
    uint32_t value = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') return false;
        value = value * 10u + static_cast<uint32_t>(*cursor - '0');
        if (value > 100000u) return false;
    }
    out = value;
    return true;
}

size_t digestSize(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::Sha1: return 20;
        case Algorithm::Sha256: return 32;
        case Algorithm::Sha512: return 64;
    }
    return 0;
}

}  // namespace

const char* describe(ParseError error) {
    switch (error) {
        case ParseError::None: return "ok";
        case ParseError::NotOtpauth:
            return "That is not an otpauth:// or otpauth-migration:// link.";
        case ParseError::NotTotp: return "Only time-based (TOTP) codes are supported, not HOTP.";
        case ParseError::MissingName: return "A name is required, so you can find it in the list.";
        case ParseError::MissingSecret: return "The secret key is missing.";
        case ParseError::BadSecret:
            return "The secret key is not valid base32 (letters A-Z and digits 2-7).";
        case ParseError::SecretTooLong: return "The secret key is too long (64 bytes at most).";
        case ParseError::BadDigits: return "Codes must be 6, 7 or 8 digits.";
        case ParseError::BadPeriod: return "The period must be between 10 and 300 seconds.";
        case ParseError::BadAlgorithm: return "The algorithm must be SHA1, SHA256 or SHA512.";
        case ParseError::BadTransfer:
            return "That transfer link is damaged or cut short. Copy it from the QR code again.";
    }
    return "invalid";
}

const char* toString(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::Sha1: return "SHA1";
        case Algorithm::Sha256: return "SHA256";
        case Algorithm::Sha512: return "SHA512";
    }
    return "?";
}

bool parseAlgorithm(const char* text, Algorithm& out) {
    if (equalsIgnoreCase(text, "SHA1")) out = Algorithm::Sha1;
    else if (equalsIgnoreCase(text, "SHA256")) out = Algorithm::Sha256;
    else if (equalsIgnoreCase(text, "SHA512")) out = Algorithm::Sha512;
    else return false;
    return true;
}

bool decodeBase32(const char* text, uint8_t* out, size_t capacity, size_t& length) {
    uint32_t buffer = 0;
    int bits = 0;
    size_t written = 0;
    bool padding = false;

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const char character = *cursor;
        if (character == ' ' || character == '-' || character == '\t' || character == '\r' ||
            character == '\n') {
            continue;
        }
        if (character == '=') {
            padding = true;
            continue;
        }
        // Padding ends the data: nothing but more padding may follow it.
        if (padding) return false;

        int value;
        const char folded = lower(character);
        if (folded >= 'a' && folded <= 'z') value = folded - 'a';
        else if (character >= '2' && character <= '7') value = character - '2' + 26;
        else return false;

        buffer = (buffer << 5) | static_cast<uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity) return false;
            out[written++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
        }
    }
    length = written;
    return written > 0;
}

ParseError parseOtpauthUri(const char* uri, Account& out) {
    while (*uri == ' ' || *uri == '\t' || *uri == '\r' || *uri == '\n') ++uri;
    if (!startsWithIgnoreCase(uri, "otpauth://")) return ParseError::NotOtpauth;
    const char* cursor = uri + strlen("otpauth://");

    if (startsWithIgnoreCase(cursor, "hotp/")) return ParseError::NotTotp;
    if (!startsWithIgnoreCase(cursor, "totp/")) return ParseError::NotOtpauth;
    cursor += strlen("totp/");

    const char* query = strchr(cursor, '?');
    const size_t labelLength = query != nullptr ? static_cast<size_t>(query - cursor) : strlen(cursor);
    char label[128];
    percentDecode(cursor, labelLength, label, sizeof(label));

    Account account;
    char secret[256] = {0};
    char issuer[64] = {0};
    bool haveSecret = false;

    if (query != nullptr) {
        const char* parameter = query + 1;
        while (*parameter != '\0') {
            const char* end = strchr(parameter, '&');
            const size_t length = end != nullptr ? static_cast<size_t>(end - parameter) : strlen(parameter);
            const char* equals = static_cast<const char*>(memchr(parameter, '=', length));
            if (equals != nullptr) {
                char key[16];
                char value[256];
                percentDecode(parameter, static_cast<size_t>(equals - parameter), key, sizeof(key));
                percentDecode(equals + 1, length - static_cast<size_t>(equals - parameter) - 1, value,
                              sizeof(value));
                if (equalsIgnoreCase(key, "secret")) {
                    strncpy(secret, value, sizeof(secret) - 1);
                    haveSecret = value[0] != '\0';
                } else if (equalsIgnoreCase(key, "issuer")) {
                    strncpy(issuer, value, sizeof(issuer) - 1);
                } else if (equalsIgnoreCase(key, "algorithm")) {
                    if (!parseAlgorithm(value, account.algorithm)) return ParseError::BadAlgorithm;
                } else if (equalsIgnoreCase(key, "digits")) {
                    uint32_t digits = 0;
                    if (!readNumber(value, digits) || digits < kMinDigits || digits > kMaxDigits) {
                        return ParseError::BadDigits;
                    }
                    account.digits = static_cast<uint8_t>(digits);
                } else if (equalsIgnoreCase(key, "period")) {
                    uint32_t period = 0;
                    if (!readNumber(value, period) || period < kMinPeriod || period > kMaxPeriod) {
                        return ParseError::BadPeriod;
                    }
                    account.period = static_cast<uint16_t>(period);
                }
                // Anything else (image=, counter=...) is not needed for TOTP.
            }
            if (end == nullptr) break;
            parameter = end + 1;
        }
    }

    if (!composeName(label, issuer, account.name)) return ParseError::MissingName;

    if (!haveSecret) return ParseError::MissingSecret;
    ParseError error = ParseError::None;
    if (!setSecret(secret, account, error)) return error;

    out = account;
    return ParseError::None;
}

bool isMigrationLink(const char* uri) {
    while (*uri == ' ' || *uri == '\t' || *uri == '\r' || *uri == '\n') ++uri;
    return startsWithIgnoreCase(uri, "otpauth-migration:");
}

ParseError MigrationReader::open(const char* uri) {
    wipe();
    while (*uri == ' ' || *uri == '\t' || *uri == '\r' || *uri == '\n') ++uri;
    if (!startsWithIgnoreCase(uri, "otpauth-migration://offline")) return ParseError::NotOtpauth;
    const char* query = strchr(uri, '?');
    if (query == nullptr) return ParseError::BadTransfer;

    // The payload is the `data` parameter; nothing else in the link matters.
    const char* data = nullptr;
    size_t dataLength = 0;
    for (const char* parameter = query + 1; *parameter != '\0';) {
        const char* end = strchr(parameter, '&');
        const size_t length = end != nullptr ? static_cast<size_t>(end - parameter) : strlen(parameter);
        if (length > 5 && startsWithIgnoreCase(parameter, "data=")) {
            data = parameter + 5;
            dataLength = length - 5;
        }
        if (end == nullptr) break;
        parameter = end + 1;
    }
    // Trailing whitespace from a paste is not part of it.
    while (dataLength > 0 && (data[dataLength - 1] == ' ' || data[dataLength - 1] == '\r' ||
                              data[dataLength - 1] == '\n' || data[dataLength - 1] == '\t')) {
        --dataLength;
    }
    if (data == nullptr ||
        !decodeMigrationData(data, dataLength, payload_, sizeof(payload_), length_)) {
        wipe();
        return ParseError::BadTransfer;
    }

    // MigrationPayload: 1 account (repeated), 2 version, 3 batch size,
    // 4 batch index, 5 batch id. Every field is walked, and every account
    // read, before any is handed out.
    Wire wire{payload_, length_, 0};
    uint16_t count = 0;
    uint64_t batchSize = 0;
    uint64_t batchIndex = 0;
    while (wire.offset < wire.length) {
        uint32_t field = 0;
        uint8_t kind = 0;
        bool good = readKey(wire, field, kind);
        if (good && field == 1 && kind == 2) {
            const uint8_t* start = nullptr;
            size_t size = 0;
            Account scratch;
            ParseError ignored = ParseError::None;
            good = readLengthDelimited(wire, start, size) &&
                   readParameters(start, size, scratch, ignored) && count < 0xFFFF;
            wipeBytes(&scratch, sizeof(scratch));
            ++count;
        } else if (good && (field == 3 || field == 4) && kind == 0) {
            good = readVarint(wire, field == 3 ? batchSize : batchIndex);
        } else if (good) {
            good = skipField(wire, kind);
        }
        if (!good) {
            wipe();
            return ParseError::BadTransfer;
        }
    }
    if (count == 0) {
        wipe();
        return ParseError::BadTransfer;
    }
    count_ = count;
    batchSize_ = static_cast<uint8_t>(batchSize > 255 ? 255 : batchSize);
    batchIndex_ = static_cast<uint8_t>(batchIndex > 255 ? 255 : batchIndex);
    return ParseError::None;
}

bool MigrationReader::next(Account& out, ParseError& error) {
    Wire wire{payload_, length_, offset_};
    while (wire.offset < wire.length) {
        uint32_t field = 0;
        uint8_t kind = 0;
        if (!readKey(wire, field, kind)) break;
        if (field == 1 && kind == 2) {
            const uint8_t* start = nullptr;
            size_t size = 0;
            if (!readLengthDelimited(wire, start, size) ||
                !readParameters(start, size, out, error)) {
                break;
            }
            offset_ = wire.offset;
            return true;
        }
        if (!skipField(wire, kind)) break;
    }
    offset_ = length_;
    return false;
}

void MigrationReader::wipe() {
    wipeBytes(payload_, sizeof(payload_));
    length_ = 0;
    offset_ = 0;
    count_ = 0;
    batchIndex_ = 0;
    batchSize_ = 0;
}

bool renameAccount(Account& account, const char* name) {
    char folded[kNameCapacity];
    if (name == nullptr || !setName(name, folded)) return false;
    memcpy(account.name, folded, sizeof(folded));
    return true;
}

ParseError makeAccount(const char* name,
                       const char* base32Secret,
                       uint8_t digits,
                       uint16_t period,
                       Algorithm algorithm,
                       Account& out) {
    Account account;
    if (name == nullptr || !setName(name, account.name)) return ParseError::MissingName;
    if (base32Secret == nullptr || base32Secret[0] == '\0') return ParseError::MissingSecret;
    if (digits < kMinDigits || digits > kMaxDigits) return ParseError::BadDigits;
    if (period < kMinPeriod || period > kMaxPeriod) return ParseError::BadPeriod;
    account.digits = digits;
    account.period = period;
    account.algorithm = algorithm;
    ParseError error = ParseError::None;
    if (!setSecret(base32Secret, account, error)) return error;
    out = account;
    return ParseError::None;
}

bool hotp(const Account& account, uint64_t counter, HmacFunction hmac, uint32_t& code) {
    if (account.secretLength == 0 || account.digits < kMinDigits || account.digits > kMaxDigits) {
        return false;
    }
    uint8_t message[8];
    for (int index = 7; index >= 0; --index) {
        message[index] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }
    uint8_t digest[64];
    const size_t size = hmac(account.algorithm, account.secret, account.secretLength, message,
                             sizeof(message), digest, sizeof(digest));
    if (size == 0 || size != digestSize(account.algorithm)) return false;

    // RFC 4226 §5.3 dynamic truncation, off the digest's last byte — which is
    // what RFC 6238's reference code does for SHA-256 and SHA-512 too.
    const uint8_t offset = digest[size - 1] & 0x0F;
    const uint32_t binary = (static_cast<uint32_t>(digest[offset] & 0x7F) << 24) |
                            (static_cast<uint32_t>(digest[offset + 1]) << 16) |
                            (static_cast<uint32_t>(digest[offset + 2]) << 8) |
                            static_cast<uint32_t>(digest[offset + 3]);
    uint32_t modulus = 1;
    for (uint8_t digit = 0; digit < account.digits; ++digit) modulus *= 10u;
    code = binary % modulus;
    return true;
}

bool totp(const Account& account, uint64_t unixSeconds, HmacFunction hmac, uint32_t& code) {
    if (account.period == 0) return false;
    return hotp(account, unixSeconds / account.period, hmac, code);
}

uint16_t secondsLeft(const Account& account, uint64_t unixSeconds) {
    if (account.period == 0) return 0;
    return static_cast<uint16_t>(account.period - (unixSeconds % account.period));
}

void formatCode(uint32_t code, uint8_t digits, char* out, size_t capacity) {
    if (capacity == 0) return;
    if (digits > kMaxDigits) digits = kMaxDigits;
    if (static_cast<size_t>(digits) + 1 > capacity) {
        out[0] = '\0';
        return;
    }
    for (int index = digits - 1; index >= 0; --index) {
        out[index] = static_cast<char>('0' + code % 10u);
        code /= 10u;
    }
    out[digits] = '\0';
}

}  // namespace otp
