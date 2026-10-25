#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Wire protocol, device side. Mirrors mac/Sources/ClaudeRemoteCore/Protocol/
// and protocol/PROTOCOL.md — change all three together.
//
// Deliberately free of Arduino headers so the parser can be unit-tested on the
// host with `pio test -e native`.
// ---------------------------------------------------------------------------

namespace proto {

/// Protocol version carried in every frame as `v`.
constexpr int kVersion = 1;

/// Longest accepted NDJSON line, in bytes. A longer line is dropped whole:
/// its tail is never reparsed as a fresh frame.
constexpr size_t kMaxLineBytes = 768;

// Fixed field capacities. No heap allocation anywhere in the firmware.
constexpr size_t kMaxMessageChars = 513;   // 512 + NUL
constexpr size_t kMaxDetailChars = 97;
constexpr size_t kMaxRequestIdChars = 65;
constexpr size_t kMaxKindChars = 25;
constexpr size_t kMaxTitleChars = 97;
constexpr size_t kMaxContentChars = 513;

/// Sessions the device tracks at once. Several Claude Desktop windows — or
/// several copies of the app — are each a session; the Mac sends the list and
/// says which one the `state` and `message` frames describe.
constexpr size_t kMaxSessions = 8;
constexpr size_t kMaxSessionIdChars = 17;
constexpr size_t kMaxSessionNameChars = 31;

/// Normalised Claude state. See protocol/PROTOCOL.md §3.
enum class ClaudeState : uint8_t {
    Disconnected,
    Idle,
    Thinking,
    Writing,
    WaitingPermission,
    Done,
    Error,
};

/// Actions the device can originate. See protocol/PROTOCOL.md §4.
enum class Action : uint8_t {
    Approve,
    Deny,
    ApproveAll,   ///< approve every queued request; quotes the shown one's id
    Stop,
    Context,
};

/// How the box wants permissions answered, sent to the Mac in a `mode` frame.
/// See protocol/PROTOCOL.md §2 and §10.
enum class PermissionMode : uint8_t {
    Auto,   ///< through a Claude Code hook when one is listening, else the interface
    Ui,     ///< by pressing the buttons in Claude's window only
};

/// The `value` a `mode` frame carries.
constexpr const char* permissionModeName(PermissionMode mode) {
    return mode == PermissionMode::Ui ? "ui" : "auto";
}

enum class Navigation : uint8_t {
    Up,
    Down,
    Left,
    Right,
    Select,
};

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error,
};

/// Frame kinds the device accepts from the Mac.
enum class InboundType : uint8_t {
    Unknown,
    Hello,
    State,
    Message,
    Sessions,
    Permission,
    PermissionCleared,
    Ping,
    Pong,
    Error,
    /// The Mac's clock, for one-time codes: the board has no clock of its own
    /// that survives a power cycle.
    Time,
};

/// A clock earlier than 2023-11-14 or later than 2100 is not one to derive
/// one-time codes from: refused rather than trusted.
constexpr uint64_t kMinUnixMs = 1700000000000ull;
constexpr uint64_t kMaxUnixMs = 4102444800000ull;

/// One Claude Desktop session, as listed in a `sessions` frame.
struct SessionEntry {
    char id[kMaxSessionIdChars] = {0};
    char name[kMaxSessionNameChars] = {0};
    ClaudeState state = ClaudeState::Idle;
};

/// Why a line was refused. Every rejection is logged, never silently ignored:
/// serial noise must never become an action.
enum class ParseResult : uint8_t {
    Ok,
    NotJson,
    VersionMismatch,
    MissingType,
    UnknownType,
    MissingField,
    InvalidEnum,
    OutOfRange,
};

/// A decoded inbound frame. One struct for every kind keeps the parser
/// allocation-free; only the fields relevant to `type` are populated.
struct InboundFrame {
    InboundType type = InboundType::Unknown;

    // state
    ClaudeState state = ClaudeState::Disconnected;
    char detail[kMaxDetailChars] = {0};
    bool hasDetail = false;

    // message / error
    char text[kMaxMessageChars] = {0};

    // permission
    char id[kMaxRequestIdChars] = {0};
    char kind[kMaxKindChars] = {0};
    char title[kMaxTitleChars] = {0};
    char content[kMaxContentChars] = {0};
    bool offersApprove = false;
    bool offersDeny = false;

    // sessions
    SessionEntry sessions[kMaxSessions];
    uint8_t sessionCount = 0;
    /// Total sessions on the Mac, which may exceed what fits in one frame.
    uint8_t sessionTotal = 0;
    /// Session the `state` / `message` frames describe. Empty when unknown.
    char focus[kMaxSessionIdChars] = {0};
    /// Permission the Mac considers active, or empty for none. The device
    /// reconciles against it, so a lost `permission_cleared` cannot strand a
    /// dead request on screen.
    char activePermission[kMaxRequestIdChars] = {0};
    /// Session that raised the permission, for `permission` frames.
    char session[kMaxSessionIdChars] = {0};

    // hello
    uint32_t approveDelayMs = 0;
    bool hasApproveDelay = false;

    // ping / pong
    int seq = 0;

    // time
    uint64_t unixMs = 0;
};

/// Strictly decodes one NDJSON line. Anything not matching the protocol
/// exactly is refused with a reason, never coerced.
ParseResult parseInbound(const char* line, size_t length, InboundFrame& out);

const char* toString(ParseResult result);
const char* toString(ClaudeState state);
const char* toString(Action action);
const char* toString(Navigation navigation);
const char* toString(LogLevel level);

/// Parses a `state` value. Returns false for anything outside the enum, so the
/// device keeps its previous state rather than inventing one.
bool parseState(const char* value, ClaudeState& out);

/// True when the action authorises or refuses a specific request and therefore
/// must quote a `requestId`.
bool requiresRequestId(Action action);

/// Validates a request id against the protocol's charset and length.
bool isValidRequestId(const char* id);

/// Same charset as a request id, bounded by the shorter session-id capacity.
bool isValidSessionId(const char* id);

// ---------------------------------------------------------------------------
// Encoding (device -> Mac)
// ---------------------------------------------------------------------------

/// Writes `{"v":1,"type":"action",...}\n` into `out`. Returns the length
/// written, or 0 if it would not fit. `requestId` may be null for actions that
/// do not need one.
///
/// `sessionId` names the session an action applies to. It is only meaningful
/// for `stop` and `context`: `approve` and `deny` are bound by `requestId`,
/// which the Mac already resolves to exactly one session — adding a second,
/// possibly disagreeing, address to a verdict could only ever weaken that.
size_t encodeAction(
    char* out, size_t capacity, Action action, const char* requestId,
    const char* sessionId = nullptr);

/// Asks the Mac to make `sessionId` the session it reports on.
size_t encodeFocus(char* out, size_t capacity, const char* sessionId);

size_t encodeNavigation(char* out, size_t capacity, Navigation navigation);

size_t encodeHello(char* out, size_t capacity, const char* app, const char* version);

size_t encodeLog(char* out, size_t capacity, LogLevel level, const char* message);

size_t encodePing(char* out, size_t capacity, int seq);

size_t encodePong(char* out, size_t capacity, int seq);

/// True for 6 to 8 ASCII digits and nothing else — the only thing an `otp`
/// frame may carry.
bool isOtpCode(const char* code);

/// Writes `{"v":1,"type":"otp","label":...,"code":...,"expiresInMs":...}\n`.
///
/// The Mac puts `code` on its clipboard, so the frame is refused unless `code`
/// is a one-time code: a frame able to carry arbitrary text would let the box
/// put anything on the clipboard for the user to paste. Returns 0 in that case,
/// or if it does not fit.
size_t encodeOtp(char* out, size_t capacity, const char* label, const char* code,
                 uint32_t expiresInMs);

}  // namespace proto
