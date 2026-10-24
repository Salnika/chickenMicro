#include "Protocol.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace proto {
namespace {

/// Copies `source` into a fixed buffer, always NUL-terminating.
void copyField(char* destination, size_t capacity, const char* source) {
    if (capacity == 0) return;
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    size_t index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

/// Escapes a string into a JSON literal, quotes included. Returns false when
/// the result would not fit, so a frame is never emitted half-written.
bool writeQuoted(char* out, size_t capacity, size_t& offset, const char* value) {
    auto push = [&](char character) {
        if (offset + 1 >= capacity) return false;
        out[offset++] = character;
        return true;
    };
    if (!push('"')) return false;
    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        switch (character) {
            case '"':
                if (!push('\\') || !push('"')) return false;
                break;
            case '\\':
                if (!push('\\') || !push('\\')) return false;
                break;
            case '\n':
                if (!push('\\') || !push('n')) return false;
                break;
            case '\r':
                if (!push('\\') || !push('r')) return false;
                break;
            case '\t':
                if (!push('\\') || !push('t')) return false;
                break;
            default:
                if (character < 0x20) {
                    char escape[7];
                    snprintf(escape, sizeof(escape), "\\u%04x", character);
                    for (const char* e = escape; *e != '\0'; ++e) {
                        if (!push(*e)) return false;
                    }
                } else if (!push(static_cast<char>(character))) {
                    return false;
                }
                break;
        }
    }
    return push('"');
}

bool writeLiteral(char* out, size_t capacity, size_t& offset, const char* literal) {
    for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
        if (offset + 1 >= capacity) return false;
        out[offset++] = *cursor;
    }
    return true;
}

/// Terminates a frame with a newline and a NUL. Returns the length written.
size_t finish(char* out, size_t capacity, size_t offset) {
    if (offset + 2 > capacity) return 0;
    out[offset++] = '\n';
    out[offset] = '\0';
    return offset;
}

/// Reads an integer field, rejecting JSON booleans and fractional numbers.
bool readInt(JsonVariantConst value, long& out) {
    if (value.isNull() || value.is<bool>() || !value.is<long>()) return false;
    out = value.as<long>();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

bool parseState(const char* value, ClaudeState& out) {
    if (value == nullptr) return false;
    struct Entry {
        const char* name;
        ClaudeState state;
    };
    static const Entry table[] = {
        {"disconnected", ClaudeState::Disconnected},
        {"idle", ClaudeState::Idle},
        {"thinking", ClaudeState::Thinking},
        {"writing", ClaudeState::Writing},
        {"waiting_permission", ClaudeState::WaitingPermission},
        {"done", ClaudeState::Done},
        {"error", ClaudeState::Error},
    };
    for (const Entry& entry : table) {
        if (strcmp(value, entry.name) == 0) {
            out = entry.state;
            return true;
        }
    }
    return false;
}

bool requiresRequestId(Action action) {
    // approve_all quotes the shown request's id so the Mac can gate it exactly
    // like a single approve before it fans the verdict out to the queue.
    return action == Action::Approve || action == Action::Deny ||
           action == Action::ApproveAll;
}

namespace {

/// The shared id charset: `[A-Za-z0-9._:-]`, non-empty, bounded.
bool isValidId(const char* id, size_t capacity) {
    if (id == nullptr || id[0] == '\0') return false;
    size_t length = 0;
    for (const char* cursor = id; *cursor != '\0'; ++cursor, ++length) {
        if (length + 1 >= capacity) return false;
        const char character = *cursor;
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '.' || character == '_' ||
                             character == ':' || character == '-';
        if (!allowed) return false;
    }
    return true;
}

}  // namespace

bool isValidRequestId(const char* id) {
    return isValidId(id, kMaxRequestIdChars);
}

bool isValidSessionId(const char* id) {
    return isValidId(id, kMaxSessionIdChars);
}

const char* toString(ParseResult result) {
    switch (result) {
        case ParseResult::Ok: return "ok";
        case ParseResult::NotJson: return "not JSON";
        case ParseResult::VersionMismatch: return "version mismatch";
        case ParseResult::MissingType: return "missing type";
        case ParseResult::UnknownType: return "unknown type";
        case ParseResult::MissingField: return "missing field";
        case ParseResult::InvalidEnum: return "invalid enum";
        case ParseResult::OutOfRange: return "value out of range";
    }
    return "unknown";
}

const char* toString(ClaudeState state) {
    switch (state) {
        case ClaudeState::Disconnected: return "disconnected";
        case ClaudeState::Idle: return "idle";
        case ClaudeState::Thinking: return "thinking";
        case ClaudeState::Writing: return "writing";
        case ClaudeState::WaitingPermission: return "waiting_permission";
        case ClaudeState::Done: return "done";
        case ClaudeState::Error: return "error";
    }
    return "unknown";
}

const char* toString(Action action) {
    switch (action) {
        case Action::Approve: return "approve";
        case Action::Deny: return "deny";
        case Action::ApproveAll: return "approve_all";
        case Action::Stop: return "stop";
        case Action::Context: return "context";
    }
    return "unknown";
}

const char* toString(Navigation navigation) {
    switch (navigation) {
        case Navigation::Up: return "up";
        case Navigation::Down: return "down";
        case Navigation::Left: return "left";
        case Navigation::Right: return "right";
        case Navigation::Select: return "select";
    }
    return "unknown";
}

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

ParseResult parseInbound(const char* line, size_t length, InboundFrame& out) {
    // Copied from a single zero-initialised instance rather than from a
    // temporary: the frame carries the session table now, and building one on
    // the stack per line would cost the loop task ~1.6 KB it does not have to
    // spare during a paged e-ink render.
    static const InboundFrame kEmpty;
    out = kEmpty;
    if (line == nullptr || length == 0 || length > kMaxLineBytes) return ParseResult::NotJson;

    JsonDocument document;
    if (deserializeJson(document, line, length) != DeserializationError::Ok) {
        return ParseResult::NotJson;
    }
    if (!document.is<JsonObjectConst>()) return ParseResult::NotJson;
    JsonObjectConst object = document.as<JsonObjectConst>();

    long version = 0;
    if (!readInt(object["v"], version) || version != kVersion) {
        return ParseResult::VersionMismatch;
    }

    const char* type = object["type"].is<const char*>() ? object["type"].as<const char*>() : nullptr;
    if (type == nullptr) return ParseResult::MissingType;

    if (strcmp(type, "state") == 0) {
        const char* rawState =
            object["state"].is<const char*>() ? object["state"].as<const char*>() : nullptr;
        if (rawState == nullptr) return ParseResult::MissingField;
        if (!parseState(rawState, out.state)) return ParseResult::InvalidEnum;
        if (object["detail"].is<const char*>()) {
            copyField(out.detail, sizeof(out.detail), object["detail"].as<const char*>());
            out.hasDetail = true;
        }
        out.type = InboundType::State;
        return ParseResult::Ok;
    }

    if (strcmp(type, "message") == 0) {
        if (!object["text"].is<const char*>()) return ParseResult::MissingField;
        copyField(out.text, sizeof(out.text), object["text"].as<const char*>());
        out.type = InboundType::Message;
        return ParseResult::Ok;
    }

    if (strcmp(type, "permission") == 0) {
        const char* id = object["id"].is<const char*>() ? object["id"].as<const char*>() : nullptr;
        if (id == nullptr || !isValidRequestId(id)) return ParseResult::MissingField;
        if (!object["kind"].is<const char*>()) return ParseResult::MissingField;
        if (!object["actions"].is<JsonArrayConst>()) return ParseResult::MissingField;

        copyField(out.id, sizeof(out.id), id);
        copyField(out.kind, sizeof(out.kind), object["kind"].as<const char*>());
        if (object["title"].is<const char*>()) {
            copyField(out.title, sizeof(out.title), object["title"].as<const char*>());
        }
        if (object["content"].is<const char*>()) {
            copyField(out.content, sizeof(out.content), object["content"].as<const char*>());
        }
        if (object["session"].is<const char*>()) {
            const char* session = object["session"].as<const char*>();
            // A malformed session id only costs the header its name, so it is
            // dropped rather than failing a frame carrying a real decision.
            if (isValidSessionId(session)) copyField(out.session, sizeof(out.session), session);
        }
        for (JsonVariantConst entry : object["actions"].as<JsonArrayConst>()) {
            if (!entry.is<const char*>()) continue;
            const char* name = entry.as<const char*>();
            if (strcmp(name, "approve") == 0) out.offersApprove = true;
            if (strcmp(name, "deny") == 0) out.offersDeny = true;
        }
        // A permission offering neither verdict is not actionable.
        if (!out.offersApprove && !out.offersDeny) return ParseResult::InvalidEnum;
        out.type = InboundType::Permission;
        return ParseResult::Ok;
    }

    if (strcmp(type, "sessions") == 0) {
        if (!object["sessions"].is<JsonArrayConst>()) return ParseResult::MissingField;

        for (JsonVariantConst entry : object["sessions"].as<JsonArrayConst>()) {
            if (out.sessionCount >= kMaxSessions) break;
            if (!entry.is<JsonObjectConst>()) return ParseResult::InvalidEnum;
            JsonObjectConst session = entry.as<JsonObjectConst>();

            const char* id = session["id"].is<const char*>() ? session["id"].as<const char*>()
                                                             : nullptr;
            if (!isValidSessionId(id)) return ParseResult::MissingField;
            const char* rawState =
                session["state"].is<const char*>() ? session["state"].as<const char*>() : nullptr;
            if (rawState == nullptr) return ParseResult::MissingField;

            SessionEntry& target = out.sessions[out.sessionCount];
            if (!parseState(rawState, target.state)) return ParseResult::InvalidEnum;
            copyField(target.id, sizeof(target.id), id);
            // The name is cosmetic; falling back to the id keeps the row
            // selectable rather than blank.
            copyField(target.name, sizeof(target.name),
                      session["name"].is<const char*>() ? session["name"].as<const char*>() : id);
            ++out.sessionCount;
        }

        long total = 0;
        out.sessionTotal =
            readInt(object["total"], total) && total >= out.sessionCount && total < 256
                ? static_cast<uint8_t>(total)
                : out.sessionCount;

        if (object["focus"].is<const char*>()) {
            const char* focus = object["focus"].as<const char*>();
            if (isValidSessionId(focus)) copyField(out.focus, sizeof(out.focus), focus);
        }
        if (object["permission"].is<const char*>()) {
            const char* active = object["permission"].as<const char*>();
            if (isValidRequestId(active)) {
                copyField(out.activePermission, sizeof(out.activePermission), active);
            }
        }
        out.type = InboundType::Sessions;
        return ParseResult::Ok;
    }

    if (strcmp(type, "permission_cleared") == 0) {
        if (object["id"].is<const char*>()) {
            copyField(out.id, sizeof(out.id), object["id"].as<const char*>());
        }
        out.type = InboundType::PermissionCleared;
        return ParseResult::Ok;
    }

    if (strcmp(type, "hello") == 0) {
        long delay = 0;
        if (readInt(object["approveDelayMs"], delay) && delay >= 0) {
            out.approveDelayMs = static_cast<uint32_t>(delay);
            out.hasApproveDelay = true;
        }
        out.type = InboundType::Hello;
        return ParseResult::Ok;
    }

    if (strcmp(type, "error") == 0) {
        if (object["message"].is<const char*>()) {
            copyField(out.text, sizeof(out.text), object["message"].as<const char*>());
        }
        out.type = InboundType::Error;
        return ParseResult::Ok;
    }

    if (strcmp(type, "time") == 0) {
        // An integer, strictly: a boolean or a fraction is not a clock.
        const JsonVariantConst value = object["unixMs"];
        if (value.isNull() || value.is<bool>() || !value.is<uint64_t>()) {
            return ParseResult::MissingField;
        }
        const uint64_t unixMs = value.as<uint64_t>();
        if (unixMs < kMinUnixMs || unixMs > kMaxUnixMs) return ParseResult::OutOfRange;
        out.unixMs = unixMs;
        out.type = InboundType::Time;
        return ParseResult::Ok;
    }

    if (strcmp(type, "ping") == 0 || strcmp(type, "pong") == 0) {
        long seq = 0;
        if (!readInt(object["seq"], seq)) return ParseResult::MissingField;
        out.seq = static_cast<int>(seq);
        out.type = strcmp(type, "ping") == 0 ? InboundType::Ping : InboundType::Pong;
        return ParseResult::Ok;
    }

    return ParseResult::UnknownType;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

size_t encodeAction(
    char* out, size_t capacity, Action action, const char* requestId, const char* sessionId) {
    // The id is part of what makes an approve safe; refuse to emit a critical
    // action without a valid one rather than sending something the Mac will
    // reject anyway.
    if (requiresRequestId(action) && !isValidRequestId(requestId)) return 0;

    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"action\",\"action\":")) return 0;
    if (!writeQuoted(out, capacity, offset, toString(action))) return 0;
    if (requiresRequestId(action)) {
        if (!writeLiteral(out, capacity, offset, ",\"requestId\":")) return 0;
        if (!writeQuoted(out, capacity, offset, requestId)) return 0;
    } else if (isValidSessionId(sessionId)) {
        if (!writeLiteral(out, capacity, offset, ",\"sessionId\":")) return 0;
        if (!writeQuoted(out, capacity, offset, sessionId)) return 0;
    }
    if (!writeLiteral(out, capacity, offset, "}")) return 0;
    return finish(out, capacity, offset);
}

size_t encodeFocus(char* out, size_t capacity, const char* sessionId) {
    if (!isValidSessionId(sessionId)) return 0;
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"focus\",\"sessionId\":")) return 0;
    if (!writeQuoted(out, capacity, offset, sessionId)) return 0;
    if (!writeLiteral(out, capacity, offset, "}")) return 0;
    return finish(out, capacity, offset);
}

size_t encodeNavigation(char* out, size_t capacity, Navigation navigation) {
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"navigation\",\"action\":")) return 0;
    if (!writeQuoted(out, capacity, offset, toString(navigation))) return 0;
    if (!writeLiteral(out, capacity, offset, "}")) return 0;
    return finish(out, capacity, offset);
}

size_t encodeHello(char* out, size_t capacity, const char* app, const char* version) {
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"hello\",\"role\":\"device\",\"app\":")) {
        return 0;
    }
    if (!writeQuoted(out, capacity, offset, app)) return 0;
    if (!writeLiteral(out, capacity, offset, ",\"version\":")) return 0;
    if (!writeQuoted(out, capacity, offset, version)) return 0;
    if (!writeLiteral(out, capacity, offset, "}")) return 0;
    return finish(out, capacity, offset);
}

size_t encodeLog(char* out, size_t capacity, LogLevel level, const char* message) {
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"log\",\"level\":")) return 0;
    if (!writeQuoted(out, capacity, offset, toString(level))) return 0;
    if (!writeLiteral(out, capacity, offset, ",\"message\":")) return 0;
    if (!writeQuoted(out, capacity, offset, message)) return 0;
    if (!writeLiteral(out, capacity, offset, "}")) return 0;
    return finish(out, capacity, offset);
}

namespace {
size_t encodeSeqFrame(char* out, size_t capacity, const char* type, int seq) {
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":")) return 0;
    if (!writeQuoted(out, capacity, offset, type)) return 0;
    char tail[32];
    snprintf(tail, sizeof(tail), ",\"seq\":%d}", seq);
    if (!writeLiteral(out, capacity, offset, tail)) return 0;
    return finish(out, capacity, offset);
}
}  // namespace

bool isOtpCode(const char* code) {
    if (code == nullptr) return false;
    size_t length = 0;
    for (; code[length] != '\0'; ++length) {
        if (code[length] < '0' || code[length] > '9') return false;
        if (length >= 8) return false;
    }
    return length >= 6;
}

size_t encodeOtp(char* out, size_t capacity, const char* label, const char* code,
                 uint32_t expiresInMs) {
    if (!isOtpCode(code)) return 0;
    size_t offset = 0;
    if (!writeLiteral(out, capacity, offset, "{\"v\":1,\"type\":\"otp\",\"label\":")) return 0;
    if (!writeQuoted(out, capacity, offset, label != nullptr ? label : "")) return 0;
    if (!writeLiteral(out, capacity, offset, ",\"code\":")) return 0;
    if (!writeQuoted(out, capacity, offset, code)) return 0;
    char tail[32];
    snprintf(tail, sizeof(tail), ",\"expiresInMs\":%lu}", static_cast<unsigned long>(expiresInMs));
    if (!writeLiteral(out, capacity, offset, tail)) return 0;
    return finish(out, capacity, offset);
}

size_t encodePing(char* out, size_t capacity, int seq) {
    return encodeSeqFrame(out, capacity, "ping", seq);
}

size_t encodePong(char* out, size_t capacity, int seq) {
    return encodeSeqFrame(out, capacity, "pong", seq);
}

}  // namespace proto
