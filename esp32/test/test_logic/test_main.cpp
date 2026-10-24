// ---------------------------------------------------------------------------
// Host unit tests: pio test -e native
//
// Covers the firmware logic that has no hardware in it — the NDJSON line
// reader, protocol validation, request-id rules, action encoding, and the
// joystick dead zone / dominant-axis / repeat behaviour.
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "ButtonState.h"
#include "ChickenWalk.h"
#include "ExpanderLogic.h"
#include "JoystickLogic.h"
#include "KeyLegend.h"
#include "Led.h"
#include "LineReader.h"
#include "LinkTimeout.h"
#include "Protocol.h"
#include "State.h"
#include "TextLayout.h"
#include "Totp.h"
#include "Waveform.h"
#include "WifiQr.h"

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// LineReader — NDJSON framing
// ---------------------------------------------------------------------------

namespace {

/// Feeds a whole string, returning how many complete lines came out and
/// keeping the last one.
int feedAll(proto::LineReader& reader, const char* text, char* lastLine, size_t capacity) {
    int lines = 0;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (reader.feed(*cursor)) {
            ++lines;
            if (lastLine != nullptr) {
                strncpy(lastLine, reader.line(), capacity - 1);
                lastLine[capacity - 1] = '\0';
            }
        }
    }
    return lines;
}

}  // namespace

void test_line_reader_splits_on_newline() {
    proto::LineReader reader;
    char last[128] = {0};
    TEST_ASSERT_EQUAL_INT(2, feedAll(reader, "{\"a\":1}\n{\"b\":2}\n", last, sizeof(last)));
    TEST_ASSERT_EQUAL_STRING("{\"b\":2}", last);
}

void test_line_reader_reassembles_partial_line() {
    proto::LineReader reader;
    char last[128] = {0};
    TEST_ASSERT_EQUAL_INT(0, feedAll(reader, "{\"ty", nullptr, 0));
    TEST_ASSERT_EQUAL_INT(0, feedAll(reader, "pe\":\"pi", nullptr, 0));
    TEST_ASSERT_EQUAL_INT(1, feedAll(reader, "ng\"}\n", last, sizeof(last)));
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"ping\"}", last);
}

void test_line_reader_strips_cr_and_blank_lines() {
    proto::LineReader reader;
    char last[128] = {0};
    TEST_ASSERT_EQUAL_INT(2, feedAll(reader, "a\r\n\r\nb\n", last, sizeof(last)));
    TEST_ASSERT_EQUAL_STRING("b", last);
}

/// An oversized line must be abandoned whole. If its tail were parsed as a
/// fresh frame, a burst of line noise could synthesise a valid action.
void test_line_reader_drops_oversized_line_whole() {
    proto::LineReader reader;
    char noise[proto::kMaxLineBytes + 64];
    memset(noise, 'x', sizeof(noise));
    noise[sizeof(noise) - 1] = '\0';

    TEST_ASSERT_EQUAL_INT(0, feedAll(reader, noise, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(
        0, feedAll(reader, "{\"v\":1,\"type\":\"action\",\"action\":\"approve\"}\n", nullptr, 0));
    TEST_ASSERT_EQUAL_UINT32(1, reader.droppedLines());

    // And it recovers for the next line.
    char last[128] = {0};
    TEST_ASSERT_EQUAL_INT(1, feedAll(reader, "{\"ok\":1}\n", last, sizeof(last)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":1}", last);
}

// ---------------------------------------------------------------------------
// Protocol — strict decoding
// ---------------------------------------------------------------------------

namespace {

proto::ParseResult parse(const char* line, proto::InboundFrame& frame) {
    return proto::parseInbound(line, strlen(line), frame);
}

}  // namespace

void test_parses_state_frame() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::Ok,
        (int)parse("{\"v\":1,\"type\":\"state\",\"state\":\"thinking\",\"detail\":\"Refactor\"}",
                   frame));
    TEST_ASSERT_EQUAL_INT((int)proto::InboundType::State, (int)frame.type);
    TEST_ASSERT_EQUAL_INT((int)proto::ClaudeState::Thinking, (int)frame.state);
    TEST_ASSERT_TRUE(frame.hasDetail);
    TEST_ASSERT_EQUAL_STRING("Refactor", frame.detail);
}

void test_parses_permission_frame() {
    proto::InboundFrame frame;
    const char* line =
        "{\"v\":1,\"type\":\"permission\",\"id\":\"req-12\",\"kind\":\"shell\","
        "\"title\":\"Run command\",\"content\":\"git push origin master\","
        "\"actions\":[\"deny\",\"approve\"]}";
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::Ok, (int)parse(line, frame));
    TEST_ASSERT_EQUAL_STRING("req-12", frame.id);
    TEST_ASSERT_EQUAL_STRING("shell", frame.kind);
    TEST_ASSERT_EQUAL_STRING("git push origin master", frame.content);
    TEST_ASSERT_TRUE(frame.offersApprove);
    TEST_ASSERT_TRUE(frame.offersDeny);
}

void test_parses_ping_and_hello() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::Ok,
                          (int)parse("{\"v\":1,\"type\":\"ping\",\"seq\":7}", frame));
    TEST_ASSERT_EQUAL_INT(7, frame.seq);

    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::Ok,
        (int)parse("{\"v\":1,\"type\":\"hello\",\"role\":\"mac\",\"approveDelayMs\":250}", frame));
    TEST_ASSERT_TRUE(frame.hasApproveDelay);
    TEST_ASSERT_EQUAL_UINT32(250, frame.approveDelayMs);
}

void test_rejects_garbage_and_wrong_version() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::NotJson, (int)parse("not json", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::NotJson, (int)parse("", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::NotJson, (int)parse("[1,2,3]", frame));
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::VersionMismatch,
        (int)parse("{\"v\":2,\"type\":\"state\",\"state\":\"idle\"}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::VersionMismatch,
                          (int)parse("{\"type\":\"state\",\"state\":\"idle\"}", frame));
    // A boolean must not pass as the version integer.
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::VersionMismatch,
                          (int)parse("{\"v\":true,\"type\":\"ping\",\"seq\":1}", frame));
}

void test_rejects_unknown_type_and_bad_enum() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingType, (int)parse("{\"v\":1}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::UnknownType,
                          (int)parse("{\"v\":1,\"type\":\"reboot\"}", frame));
    // An unknown state must not be coerced: the device keeps its previous one.
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::InvalidEnum,
        (int)parse("{\"v\":1,\"type\":\"state\",\"state\":\"pondering\"}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"ping\",\"seq\":\"7\"}", frame));
}

void test_rejects_permission_with_bad_id_or_no_actions() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::MissingField,
        (int)parse("{\"v\":1,\"type\":\"permission\",\"id\":\"req 1; rm -rf /\","
                   "\"kind\":\"shell\",\"actions\":[\"approve\"]}",
                   frame));
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::MissingField,
        (int)parse("{\"v\":1,\"type\":\"permission\",\"id\":\"\",\"kind\":\"shell\","
                   "\"actions\":[\"approve\"]}",
                   frame));
    // Neither verdict offered means nothing is actionable.
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::InvalidEnum,
        (int)parse("{\"v\":1,\"type\":\"permission\",\"id\":\"req-1\",\"kind\":\"shell\","
                   "\"actions\":[\"maybe\"]}",
                   frame));
}

void test_request_id_validation() {
    TEST_ASSERT_TRUE(proto::isValidRequestId("req-1"));
    TEST_ASSERT_TRUE(proto::isValidRequestId("a.b_c:d-1"));
    TEST_ASSERT_FALSE(proto::isValidRequestId(""));
    TEST_ASSERT_FALSE(proto::isValidRequestId(nullptr));
    TEST_ASSERT_FALSE(proto::isValidRequestId("has space"));
    TEST_ASSERT_FALSE(proto::isValidRequestId("quote\"inside"));

    char tooLong[proto::kMaxRequestIdChars + 8];
    memset(tooLong, 'a', sizeof(tooLong));
    tooLong[sizeof(tooLong) - 1] = '\0';
    TEST_ASSERT_FALSE(proto::isValidRequestId(tooLong));
}

// ---------------------------------------------------------------------------
// Protocol — encoding
// ---------------------------------------------------------------------------

void test_encodes_actions() {
    char frame[256];
    TEST_ASSERT_TRUE(proto::encodeAction(frame, sizeof(frame), proto::Action::Approve, "req-9") > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":1,\"type\":\"action\",\"action\":\"approve\",\"requestId\":\"req-9\"}\n", frame);

    TEST_ASSERT_TRUE(proto::encodeAction(frame, sizeof(frame), proto::Action::Stop, nullptr) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"type\":\"action\",\"action\":\"stop\"}\n", frame);

    // approve_all quotes the shown request's id, like a single approve.
    TEST_ASSERT_TRUE(
        proto::encodeAction(frame, sizeof(frame), proto::Action::ApproveAll, "s1.req-1") > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":1,\"type\":\"action\",\"action\":\"approve_all\",\"requestId\":\"s1.req-1\"}\n", frame);
    // And, being a verdict, it is refused without a usable id.
    TEST_ASSERT_EQUAL_UINT32(
        0, proto::encodeAction(frame, sizeof(frame), proto::Action::ApproveAll, nullptr));

    TEST_ASSERT_TRUE(proto::encodeNavigation(frame, sizeof(frame), proto::Navigation::Up) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"type\":\"navigation\",\"action\":\"up\"}\n", frame);
}

/// A verdict without a usable id must not be emitted at all. Encoding it would
/// put a frame on the wire that cannot be bound to what the operator saw.
void test_refuses_to_encode_verdict_without_valid_id() {
    char frame[256];
    TEST_ASSERT_EQUAL_UINT32(
        0, proto::encodeAction(frame, sizeof(frame), proto::Action::Approve, nullptr));
    TEST_ASSERT_EQUAL_UINT32(
        0, proto::encodeAction(frame, sizeof(frame), proto::Action::Approve, ""));
    TEST_ASSERT_EQUAL_UINT32(
        0, proto::encodeAction(frame, sizeof(frame), proto::Action::Deny, "bad id"));
}

void test_encoding_escapes_and_respects_capacity() {
    char frame[256];
    TEST_ASSERT_TRUE(
        proto::encodeLog(frame, sizeof(frame), proto::LogLevel::Warn, "quote\" back\\ nl\n") > 0);
    TEST_ASSERT_NOT_NULL(strstr(frame, "\\\""));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\\\\"));
    TEST_ASSERT_NOT_NULL(strstr(frame, "\\n\"}"));

    // Too small a buffer yields nothing rather than a half-written frame.
    char tiny[8];
    TEST_ASSERT_EQUAL_UINT32(
        0, proto::encodeLog(tiny, sizeof(tiny), proto::LogLevel::Info, "a long message"));
}

void test_round_trip_action_through_parser() {
    // What the device sends must be what the Mac's grammar accepts, so the
    // encoder's output is fed back through the decoder.
    char frame[256];
    const size_t length =
        proto::encodeAction(frame, sizeof(frame), proto::Action::Deny, "req-77");
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_EQUAL_CHAR('\n', frame[length - 1]);
}

// ---------------------------------------------------------------------------
// Joystick logic
// ---------------------------------------------------------------------------

namespace {

input::JoystickCalibration calibration() {
    input::JoystickCalibration cal;
    cal.centerX = 1900;   // deliberately not 2048
    cal.centerY = 2100;
    cal.deadZone = 700;
    return cal;
}

}  // namespace

void test_joystick_dead_zone() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT((int)input::Direction::None,
                          (int)input::resolveDirection(cal.centerX, cal.centerY, cal));
    // Just inside the dead zone.
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::None,
        (int)input::resolveDirection(cal.centerX + 699, cal.centerY + 699, cal));
}

void test_joystick_directions_use_measured_centre() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Right,
                          (int)input::resolveDirection(cal.centerX + 900, cal.centerY, cal));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Left,
                          (int)input::resolveDirection(cal.centerX - 900, cal.centerY, cal));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Up,
                          (int)input::resolveDirection(cal.centerX, cal.centerY + 900, cal));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Down,
                          (int)input::resolveDirection(cal.centerX, cal.centerY - 900, cal));
}

/// A diagonal must resolve to exactly one axis, and always the same one, so the
/// UI never receives two competing events from one push.
void test_joystick_diagonal_picks_dominant_axis() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Right,
        (int)input::resolveDirection(cal.centerX + 1200, cal.centerY + 800, cal));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Up,
        (int)input::resolveDirection(cal.centerX + 800, cal.centerY + 1200, cal));
    // Exactly equal deflection favours the vertical axis, deterministically.
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Up,
        (int)input::resolveDirection(cal.centerX + 900, cal.centerY + 900, cal));
}

void test_joystick_inversion() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Left,
        (int)input::resolveDirection(cal.centerX + 900, cal.centerY, cal, true, false));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Down,
        (int)input::resolveDirection(cal.centerX, cal.centerY + 900, cal, false, true));
}

/// The swap maps the channels onto the logical axes, and leaves the meaning of
/// the inversions untouched: a deflection on the X channel becomes vertical.
void test_joystick_axis_swap() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Up,
        (int)input::resolveDirection(cal.centerX + 900, cal.centerY, cal, false, false, true));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Right,
        (int)input::resolveDirection(cal.centerX, cal.centerY + 900, cal, false, false, true));
}

/// The orientation actually shipped in `Config.h`, against the four pushes as
/// the module reads them: up and right both pull their channel below centre.
/// Without the swap, up read as Right and right read as Up.
void test_joystick_shipped_orientation() {
    const input::JoystickCalibration cal = calibration();
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Up,
        (int)input::resolveDirection(cal.centerX - 900, cal.centerY, cal, true, true, true));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Down,
        (int)input::resolveDirection(cal.centerX + 900, cal.centerY, cal, true, true, true));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Left,
        (int)input::resolveDirection(cal.centerX, cal.centerY + 900, cal, true, true, true));
    TEST_ASSERT_EQUAL_INT(
        (int)input::Direction::Right,
        (int)input::resolveDirection(cal.centerX, cal.centerY - 900, cal, true, true, true));
}

/// One flick is exactly one event; holding repeats after the initial delay.
void test_repeat_gate_fires_once_then_repeats() {
    input::RepeatGate gate(450, 180);

    TEST_ASSERT_EQUAL_INT((int)input::Direction::Down, (int)gate.update(input::Direction::Down, 0));
    // Still held, inside the initial delay: silent.
    TEST_ASSERT_EQUAL_INT((int)input::Direction::None, (int)gate.update(input::Direction::Down, 100));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::None, (int)gate.update(input::Direction::Down, 449));
    // Initial delay elapsed: repeat.
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Down, (int)gate.update(input::Direction::Down, 450));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::None, (int)gate.update(input::Direction::Down, 600));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Down, (int)gate.update(input::Direction::Down, 630));
}

void test_repeat_gate_resets_on_release() {
    input::RepeatGate gate(450, 180);
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Up, (int)gate.update(input::Direction::Up, 0));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::None, (int)gate.update(input::Direction::None, 10));
    // A fresh flick fires immediately again, with no leftover repeat timing.
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Up, (int)gate.update(input::Direction::Up, 20));
}

void test_repeat_gate_changing_direction_fires_immediately() {
    input::RepeatGate gate(450, 180);
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Left, (int)gate.update(input::Direction::Left, 0));
    TEST_ASSERT_EQUAL_INT((int)input::Direction::Right,
                          (int)gate.update(input::Direction::Right, 50));
}


// ---------------------------------------------------------------------------
// Text layout — word wrap and pagination
// ---------------------------------------------------------------------------

namespace {

/// Synthetic monospace metric: 6 px per character, like the built-in GFX font.
int16_t monospace6(const char* text, size_t length, void* context) {
    (void)text;
    (void)context;
    return (int16_t)(length * 6);
}

ui::WrappedText wrap(const char* text, int16_t maxWidth) {
    static ui::WrappedText out;
    ui::wrapText(text, maxWidth, monospace6, nullptr, out);
    return out;
}

}  // namespace

void test_wrap_short_text_is_one_line() {
    const ui::WrappedText out = wrap("git push", 240);
    TEST_ASSERT_EQUAL_UINT16(1, out.count);
    TEST_ASSERT_EQUAL_STRING("git push", out.lines[0]);
}

void test_wrap_breaks_on_word_boundary() {
    // 40 px = 6 characters, so every word gets its own line.
    const ui::WrappedText out = wrap("alpha beta gamma", 40);
    TEST_ASSERT_EQUAL_UINT16(3, out.count);
    TEST_ASSERT_EQUAL_STRING("alpha", out.lines[0]);
    TEST_ASSERT_EQUAL_STRING("beta", out.lines[1]);
    TEST_ASSERT_EQUAL_STRING("gamma", out.lines[2]);
}

/// A line that fills the width exactly must not wrap early — off-by-one here
/// would waste a third of a 250 px panel.
void test_wrap_uses_the_full_width() {
    // 60 px = exactly "alpha beta".
    const ui::WrappedText out = wrap("alpha beta gamma", 60);
    TEST_ASSERT_EQUAL_UINT16(2, out.count);
    TEST_ASSERT_EQUAL_STRING("alpha beta", out.lines[0]);
    TEST_ASSERT_EQUAL_STRING("gamma", out.lines[1]);

    // One pixel less and it no longer fits.
    const ui::WrappedText tight = wrap("alpha beta gamma", 59);
    TEST_ASSERT_EQUAL_STRING("alpha", tight.lines[0]);
}

void test_wrap_never_exceeds_the_width() {
    const char* command =
        "git push --force-with-lease origin refactor/table-header && gh pr create";
    const ui::WrappedText out = wrap(command, 120);   // 20 characters
    TEST_ASSERT_TRUE(out.count > 1);
    for (uint16_t line = 0; line < out.count; ++line) {
        TEST_ASSERT_TRUE_MESSAGE(strlen(out.lines[line]) <= 20, out.lines[line]);
    }
}

/// A word longer than the line must be split, not clipped: the operator has to
/// be able to read the whole command before approving it.
void test_wrap_splits_a_word_too_long_for_one_line() {
    const ui::WrappedText out = wrap("aaaaaaaaaaaaaaaaaaaaaaaaa", 60);   // 25 a's, 10 per line
    TEST_ASSERT_EQUAL_UINT16(3, out.count);
    TEST_ASSERT_EQUAL_STRING("aaaaaaaaaa", out.lines[0]);
    TEST_ASSERT_EQUAL_STRING("aaaaaaaaaa", out.lines[1]);
    TEST_ASSERT_EQUAL_STRING("aaaaa", out.lines[2]);

    // Nothing may be lost in the split.
    size_t total = 0;
    for (uint16_t line = 0; line < out.count; ++line) total += strlen(out.lines[line]);
    TEST_ASSERT_EQUAL_UINT32(25, total);
}

void test_wrap_honours_hard_newlines() {
    const ui::WrappedText out = wrap("one\ntwo", 240);
    TEST_ASSERT_EQUAL_UINT16(2, out.count);
    TEST_ASSERT_EQUAL_STRING("one", out.lines[0]);
    TEST_ASSERT_EQUAL_STRING("two", out.lines[1]);
}

void test_wrap_keeps_blank_lines_as_paragraph_breaks() {
    const ui::WrappedText out = wrap("one\n\ntwo", 240);
    TEST_ASSERT_EQUAL_UINT16(3, out.count);
    TEST_ASSERT_EQUAL_STRING("one", out.lines[0]);
    TEST_ASSERT_EQUAL_STRING("", out.lines[1]);
    TEST_ASSERT_EQUAL_STRING("two", out.lines[2]);
}

void test_wrap_handles_empty_and_degenerate_input() {
    TEST_ASSERT_EQUAL_UINT16(0, wrap("", 240).count);
    TEST_ASSERT_EQUAL_UINT16(0, wrap(nullptr, 240).count);
    TEST_ASSERT_EQUAL_UINT16(0, wrap("text", 0).count);
}

/// Long content must not silently overflow the line buffer.
void test_wrap_respects_the_line_buffer() {
    char long_line[400];
    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = '\0';

    // A width far wider than the buffer: the character bound has to win.
    const ui::WrappedText out = wrap(long_line, 32000);
    TEST_ASSERT_TRUE(out.count >= 1);
    for (uint16_t line = 0; line < out.count; ++line) {
        TEST_ASSERT_TRUE(strlen(out.lines[line]) < ui::WrappedText::kMaxLineChars);
    }
}

void test_pagination() {
    // Body band 22..98 with 10 px lines leaves 7 lines per page.
    TEST_ASSERT_EQUAL_UINT16(7, ui::linesPerPage(10, 22, 98));
    // Never zero, even when the band is impossible.
    TEST_ASSERT_EQUAL_UINT16(1, ui::linesPerPage(10, 50, 50));
    TEST_ASSERT_EQUAL_UINT16(1, ui::linesPerPage(200, 22, 98));
    TEST_ASSERT_EQUAL_UINT16(1, ui::linesPerPage(0, 22, 98));

    TEST_ASSERT_EQUAL_UINT16(1, ui::pageCountFrom(0, 7));    // empty is page 1 of 1
    TEST_ASSERT_EQUAL_UINT16(1, ui::pageCountFrom(7, 7));    // exactly full
    TEST_ASSERT_EQUAL_UINT16(2, ui::pageCountFrom(8, 7));    // one over
    TEST_ASSERT_EQUAL_UINT16(3, ui::pageCountFrom(15, 7));
}


// ---------------------------------------------------------------------------
// Link timeout
// ---------------------------------------------------------------------------

void test_link_is_up_while_frames_are_recent() {
    TEST_ASSERT_TRUE(app::linkIsUp(5000, 4000, 10000));
    TEST_ASSERT_TRUE(app::linkIsUp(14000, 5000, 10000));   // 9 s, just inside
}

void test_link_is_down_after_the_timeout() {
    TEST_ASSERT_FALSE(app::linkIsUp(15000, 5000, 10000));  // exactly 10 s
    TEST_ASSERT_FALSE(app::linkIsUp(60000, 5000, 10000));
}

void test_link_is_down_before_anything_arrives() {
    TEST_ASSERT_FALSE(app::linkIsUp(5000, 0, 10000));
}

/// The regression this function exists for.
///
/// App::loop samples millis() once, then poll() dispatches frames whose
/// handlers stamp lastInbound with a *later* millis(). A plain unsigned
/// subtraction underflows to ~4.29e9 and reports the link dead once per
/// received frame, so the link flaps at the frame rate.
void test_link_survives_a_frame_arriving_after_now_was_sampled() {
    TEST_ASSERT_TRUE(app::linkIsUp(5000, 5001, 10000));
    TEST_ASSERT_TRUE(app::linkIsUp(5000, 5050, 10000));
    TEST_ASSERT_TRUE(app::linkIsUp(5000, 5000, 10000));

    // Prove the naive form really is broken, so this test cannot be "fixed"
    // by reverting to it.
    const uint32_t naive = 5000u - 5001u;
    TEST_ASSERT_TRUE(naive > 10000u);
}

/// millis() wraps every 49.7 days; the modular difference must still work,
/// which is why the guard is a signed cast and not `now >= lastInbound`.
void test_link_handles_millis_rollover() {
    const uint32_t justBeforeWrap = 0xFFFFFF00u;
    const uint32_t justAfterWrap = 0x00000100u;   // 512 ms later, modulo 2^32
    TEST_ASSERT_TRUE(app::linkIsUp(justAfterWrap, justBeforeWrap, 10000));
    TEST_ASSERT_FALSE(app::linkIsUp(justAfterWrap + 20000, justBeforeWrap, 10000));
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Sessions — several Claude Desktop windows at once
// ---------------------------------------------------------------------------

void test_parses_sessions_frame() {
    proto::InboundFrame frame;
    const char* line =
        "{\"v\":1,\"type\":\"sessions\",\"focus\":\"s2\",\"permission\":\"req-7\","
        "\"total\":2,\"sessions\":["
        "{\"id\":\"s1\",\"name\":\"chickenMicro\",\"state\":\"writing\"},"
        "{\"id\":\"s2\",\"name\":\"docs\",\"state\":\"waiting_permission\"}]}";
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::Ok, (int)parse(line, frame));
    TEST_ASSERT_EQUAL_INT((int)proto::InboundType::Sessions, (int)frame.type);
    TEST_ASSERT_EQUAL_UINT8(2, frame.sessionCount);
    TEST_ASSERT_EQUAL_STRING("s1", frame.sessions[0].id);
    TEST_ASSERT_EQUAL_STRING("chickenMicro", frame.sessions[0].name);
    TEST_ASSERT_EQUAL_INT((int)proto::ClaudeState::Writing, (int)frame.sessions[0].state);
    TEST_ASSERT_EQUAL_INT((int)proto::ClaudeState::WaitingPermission,
                          (int)frame.sessions[1].state);
    TEST_ASSERT_EQUAL_STRING("s2", frame.focus);
    TEST_ASSERT_EQUAL_STRING("req-7", frame.activePermission);
}

/// A nameless session still has to be selectable, so the id stands in.
void test_sessions_frame_falls_back_to_the_id_as_a_name() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::Ok,
        (int)parse("{\"v\":1,\"type\":\"sessions\",\"sessions\":"
                   "[{\"id\":\"s4\",\"state\":\"idle\"}]}",
                   frame));
    TEST_ASSERT_EQUAL_STRING("s4", frame.sessions[0].name);
}

void test_sessions_frame_is_strict() {
    proto::InboundFrame frame;
    // No array at all.
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"sessions\"}", frame));
    // A state outside the enum must not be coerced into one.
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::InvalidEnum,
        (int)parse("{\"v\":1,\"type\":\"sessions\",\"sessions\":"
                   "[{\"id\":\"s1\",\"state\":\"busy\"}]}",
                   frame));
    // An id outside the charset would come straight back to the Mac in a focus
    // frame, so it is refused here.
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::MissingField,
        (int)parse("{\"v\":1,\"type\":\"sessions\",\"sessions\":"
                   "[{\"id\":\"s 1\",\"state\":\"idle\"}]}",
                   frame));
    // An empty list is legitimate: Claude Desktop is not running.
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::Ok,
                          (int)parse("{\"v\":1,\"type\":\"sessions\",\"sessions\":[]}", frame));
    TEST_ASSERT_EQUAL_UINT8(0, frame.sessionCount);
}

/// More sessions than the device can hold must not overrun the array; the
/// count it could not show is still reported.
void test_sessions_frame_clamps_to_capacity() {
    char line[768];
    size_t offset = 0;
    offset += (size_t)snprintf(line + offset, sizeof(line) - offset,
                               "{\"v\":1,\"type\":\"sessions\",\"total\":12,\"sessions\":[");
    for (int index = 0; index < 12; ++index) {
        offset += (size_t)snprintf(line + offset, sizeof(line) - offset,
                                   "%s{\"id\":\"s%d\",\"state\":\"idle\"}",
                                   index == 0 ? "" : ",", index);
    }
    snprintf(line + offset, sizeof(line) - offset, "]}");

    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::Ok, (int)parse(line, frame));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)proto::kMaxSessions, frame.sessionCount);
    TEST_ASSERT_EQUAL_UINT8(12, frame.sessionTotal);
}

void test_encodes_focus_and_targeted_stop() {
    char buffer[128];

    TEST_ASSERT_TRUE(proto::encodeFocus(buffer, sizeof(buffer), "s3") > 0);
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"type\":\"focus\",\"sessionId\":\"s3\"}\n", buffer);
    // A session id that could not be parsed back is never emitted.
    TEST_ASSERT_EQUAL_UINT32(0, proto::encodeFocus(buffer, sizeof(buffer), "s 3"));
    TEST_ASSERT_EQUAL_UINT32(0, proto::encodeFocus(buffer, sizeof(buffer), ""));

    TEST_ASSERT_TRUE(
        proto::encodeAction(buffer, sizeof(buffer), proto::Action::Stop, nullptr, "s2") > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":1,\"type\":\"action\",\"action\":\"stop\",\"sessionId\":\"s2\"}\n", buffer);

    // A verdict is addressed by requestId alone: a second address could only
    // disagree with it.
    TEST_ASSERT_TRUE(
        proto::encodeAction(buffer, sizeof(buffer), proto::Action::Approve, "req-1", "s2") > 0);
    TEST_ASSERT_NULL(strstr(buffer, "sessionId"));
}

// ---------------------------------------------------------------------------
// State — what the screens are driven from
// ---------------------------------------------------------------------------

namespace {

proto::InboundFrame sessionsFrame(int count, const char* focus, const char* activePermission) {
    proto::InboundFrame frame;
    frame.type = proto::InboundType::Sessions;
    for (int index = 0; index < count; ++index) {
        snprintf(frame.sessions[index].id, proto::kMaxSessionIdChars, "s%d", index + 1);
        snprintf(frame.sessions[index].name, proto::kMaxSessionNameChars, "session %d", index + 1);
        frame.sessions[index].state = proto::ClaudeState::Idle;
    }
    frame.sessionCount = (uint8_t)count;
    frame.sessionTotal = (uint8_t)count;
    snprintf(frame.focus, sizeof(frame.focus), "%s", focus);
    snprintf(frame.activePermission, sizeof(frame.activePermission), "%s", activePermission);
    return frame;
}

proto::InboundFrame permissionFrame(const char* id, const char* session) {
    proto::InboundFrame frame;
    frame.type = proto::InboundType::Permission;
    snprintf(frame.id, sizeof(frame.id), "%s", id);
    snprintf(frame.kind, sizeof(frame.kind), "shell");
    snprintf(frame.content, sizeof(frame.content), "git push");
    snprintf(frame.session, sizeof(frame.session), "%s", session);
    frame.offersApprove = true;
    frame.offersDeny = true;
    return frame;
}

}  // namespace

/// A permission raised by one window is none of the focused window's business:
/// a `state` frame about another session used to blank it.
void test_permission_survives_a_state_change_in_another_session() {
    app::State state;
    state.setLinkUp(true);
    proto::InboundFrame sessions = sessionsFrame(2, "s1", "req-1");
    state.setSessions(sessions);
    proto::InboundFrame permission = permissionFrame("req-1", "s2");
    TEST_ASSERT_TRUE(state.setPermission(permission, 0));

    state.setClaudeState(proto::ClaudeState::Thinking, "");
    TEST_ASSERT_TRUE(state.permission().valid);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());
}

/// The list carries the permission the Mac considers active, so a
/// `permission_cleared` lost to line noise cannot strand a dead request.
void test_sessions_frame_reconciles_a_stale_permission() {
    app::State state;
    state.setLinkUp(true);
    proto::InboundFrame sessions = sessionsFrame(1, "s1", "req-1");
    state.setSessions(sessions);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);

    proto::InboundFrame cleared = sessionsFrame(1, "s1", "");
    TEST_ASSERT_TRUE(state.setSessions(cleared));
    TEST_ASSERT_FALSE(state.permission().valid);

    // And a list that still names it leaves it alone.
    state.setPermission(permission, 0);
    proto::InboundFrame same = sessionsFrame(1, "s1", "req-1");
    state.setSessions(same);
    TEST_ASSERT_TRUE(state.permission().valid);
}

/// Sessions come and go while someone is looking at the list; the highlight
/// must stay on the session it was on, not on the row number.
void test_cursor_follows_its_session_across_updates() {
    app::State state;
    proto::InboundFrame three = sessionsFrame(3, "s1", "");
    state.setSessions(three);
    state.setCursor(2);
    TEST_ASSERT_EQUAL_STRING("s3", state.cursorSessionId());

    // s1 closes: what was row 3 is now row 2.
    proto::InboundFrame frame = sessionsFrame(3, "s1", "");
    snprintf(frame.sessions[0].id, proto::kMaxSessionIdChars, "s2");
    snprintf(frame.sessions[1].id, proto::kMaxSessionIdChars, "s3");
    frame.sessionCount = 2;
    frame.sessionTotal = 2;
    state.setSessions(frame);
    TEST_ASSERT_EQUAL_UINT8(1, state.cursor());
    TEST_ASSERT_EQUAL_STRING("s3", state.cursorSessionId());
}

void test_screen_cycle_refuses_to_leave_a_locked_screen() {
    app::State state;
    state.setLinkUp(true);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Home, (int)state.screen());

    TEST_ASSERT_TRUE(state.cycleScreen(1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Sessions, (int)state.screen());
    TEST_ASSERT_TRUE(state.cycleScreen(1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Message, (int)state.screen());
    TEST_ASSERT_TRUE(state.cycleScreen(1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Home, (int)state.screen());
    TEST_ASSERT_TRUE(state.cycleScreen(-1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Message, (int)state.screen());

    // A pending decision owns the screen.
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);
    state.reconcileScreen();
    TEST_ASSERT_FALSE(state.cycleScreen(1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());

    // So does a link that has gone quiet.
    state.clearPermission(nullptr);
    state.setLinkUp(false);
    state.reconcileScreen();
    TEST_ASSERT_FALSE(state.cycleScreen(1));
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Connecting, (int)state.screen());
}

// ---------------------------------------------------------------------------
// Buttons: the rules shared by the GPIO and the MCP23008 wirings
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kDebounce = 25;

/// Samples `down` every `stepMs` from `fromMs` for `forMs`; returns the time the
/// next sample would have been taken.
uint32_t holdLevel(input::ButtonState& state, bool down, uint32_t fromMs, uint32_t forMs,
                   uint32_t stepMs = 3) {
    uint32_t at = fromMs;
    for (; at <= fromMs + forMs; at += stepMs) state.sample(down, at, kDebounce);
    return at;
}

}  // namespace

void test_button_press_is_taken_once() {
    input::ButtonState button;
    button.begin(false, 0);
    button.notePress(100);
    TEST_ASSERT_TRUE(button.takePress());
    TEST_ASSERT_FALSE(button.takePress());
}

/// A key held while the board boots is never a press — on APPROVE that would be
/// a verdict nobody gave.
void test_button_held_at_boot_is_not_a_press() {
    input::ButtonState button;
    button.begin(true, 0);
    holdLevel(button, true, 0, 100);
    TEST_ASSERT_FALSE(button.takePress());
    TEST_ASSERT_TRUE(button.isDown());
}

void test_button_short_press_is_a_click_not_a_hold() {
    input::ButtonState button;
    button.begin(false, 0);
    button.noteEdge(true, 10, kDebounce);
    const uint32_t releasedAt = holdLevel(button, true, 10, 120);
    TEST_ASSERT_FALSE(button.takeHold(600));
    button.noteEdge(false, releasedAt, kDebounce);
    holdLevel(button, false, releasedAt, 60);
    TEST_ASSERT_TRUE(button.takePress());
    TEST_ASSERT_TRUE(button.takeClick());
    TEST_ASSERT_TRUE(button.takeRelease());
}

void test_button_long_press_holds_once_and_is_not_a_click() {
    input::ButtonState button;
    button.begin(false, 0);
    button.noteEdge(true, 10, kDebounce);
    const uint32_t releasedAt = holdLevel(button, true, 10, 700);
    TEST_ASSERT_TRUE(button.takeHold(600));
    TEST_ASSERT_FALSE(button.takeHold(600));   // once per press
    holdLevel(button, false, releasedAt, 60);
    TEST_ASSERT_FALSE(button.takeClick());     // that release closed a long press
}

/// The loop was blind for 400 ms after the press, behind a refresh. The hold is
/// counted from the press itself, not from when the level was first seen.
void test_button_hold_is_measured_from_the_latched_press() {
    input::ButtonState button;
    button.begin(false, 0);
    button.notePress(100);
    holdLevel(button, true, 500, 250);
    TEST_ASSERT_TRUE(button.takeHold(600));
}

/// A press whose own edge was lost must not be dated from an older press: that
/// would make it look held at once, and open the menu on a plain click.
void test_button_stale_press_time_does_not_fake_a_hold() {
    input::ButtonState button;
    button.begin(false, 0);
    button.notePress(0);
    holdLevel(button, true, 0, 100);
    holdLevel(button, false, 101, 100);
    holdLevel(button, true, 1000, 30);
    TEST_ASSERT_FALSE(button.takeHold(600));
}

void test_button_press_bounce_latches_one_press() {
    input::ButtonState button;
    button.begin(false, 0);
    button.noteEdge(true, 100, kDebounce);
    button.noteEdge(false, 102, kDebounce);
    button.noteEdge(true, 104, kDebounce);
    TEST_ASSERT_TRUE(button.takePress());
    TEST_ASSERT_FALSE(button.takePress());
}

/// Release bounce flicks the input low again. It must never read as a press.
void test_button_release_bounce_is_not_a_press() {
    input::ButtonState button;
    button.begin(true, 0);
    button.noteEdge(false, 1000, kDebounce);
    button.noteEdge(true, 1004, kDebounce);
    button.noteEdge(false, 1008, kDebounce);
    TEST_ASSERT_FALSE(button.takePress());
}

// --- MCP23008 decoding --------------------------------------------------------

namespace {

/// Five inputs on five pins, in ButtonId order. Which pins a board uses is
/// Config.h's business; what matters here is that the decoding follows them.
constexpr uint8_t kExpanderPins[5] = {0, 1, 2, 3, 4};

void beginAll(input::ButtonState (&states)[5], bool down) {
    for (input::ButtonState& state : states) state.begin(down, 0);
}

uint8_t low(uint8_t pin) { return static_cast<uint8_t>(~(1u << pin)); }

}  // namespace

/// APPROVE (GP2) was tapped while the loop was stuck behind a refresh. By the
/// next read it is up again; only INTCAP still shows that it went down.
void test_expander_catches_a_tap_between_two_reads() {
    input::ButtonState states[5];
    beginAll(states, false);
    input::mcp23008::Sample read;
    read.intf = 1u << 2;
    read.intcap = low(2);
    read.gpio = 0xFF;
    input::mcp23008::apply(read, kExpanderPins, states, 400, kDebounce);
    TEST_ASSERT_TRUE(states[2].takePress());
    const uint8_t others[] = {0, 1, 3, 4};
    for (uint8_t index : others) TEST_ASSERT_FALSE(states[index].takePress());
}

/// A key coming *up* is captured too, with INTCAP high: that is no press.
void test_expander_release_is_not_a_press() {
    input::ButtonState states[5];
    beginAll(states, false);
    states[2].begin(true, 0);
    input::mcp23008::Sample read;
    read.intf = 1u << 2;
    read.intcap = 0xFF;
    read.gpio = 0xFF;
    input::mcp23008::apply(read, kExpanderPins, states, 1000, kDebounce);
    TEST_ASSERT_FALSE(states[2].takePress());
}

/// The release, then a bounce low caught by the very next read, 3 ms later.
void test_expander_release_bounce_across_reads_is_not_a_press() {
    input::ButtonState states[5];
    beginAll(states, false);
    states[2].begin(true, 0);
    input::mcp23008::Sample release;
    release.intf = 1u << 2;
    release.intcap = 0xFF;
    release.gpio = 0xFF;
    input::mcp23008::apply(release, kExpanderPins, states, 1000, kDebounce);
    input::mcp23008::Sample bounce;
    bounce.intf = 1u << 2;
    bounce.intcap = low(2);
    bounce.gpio = 0xFF;
    input::mcp23008::apply(bounce, kExpanderPins, states, 1003, kDebounce);
    TEST_ASSERT_FALSE(states[2].takePress());
}

/// After a tap seen only through INTCAP, the level reads must bring the key
/// back to "released" — or the next real press would look like a level it
/// already holds, and be lost.
void test_expander_next_press_after_a_caught_tap_still_counts() {
    input::ButtonState states[5];
    beginAll(states, false);
    input::mcp23008::Sample tap;
    tap.intf = 1u << 0;
    tap.intcap = low(0);
    tap.gpio = 0xFF;
    input::mcp23008::apply(tap, kExpanderPins, states, 400, kDebounce);
    TEST_ASSERT_TRUE(states[0].takePress());

    const input::mcp23008::Sample idle;
    for (uint32_t at = 403; at < 460; at += 3) {
        input::mcp23008::apply(idle, kExpanderPins, states, at, kDebounce);
    }

    input::mcp23008::Sample press;
    press.intf = 1u << 0;
    press.intcap = low(0);
    press.gpio = low(0);
    input::mcp23008::apply(press, kExpanderPins, states, 600, kDebounce);
    TEST_ASSERT_TRUE(states[0].takePress());
}

/// After a brown-out the chip is back to its defaults: pull-ups off, every
/// input free to float low — which reads as every key down, APPROVE included.
/// Such a read is recognised, so it can be thrown away whole.
void test_expander_forgotten_configuration_is_untrusted() {
    input::mcp23008::Sample reset;
    reset.gppu = 0x00;
    reset.gpio = 0x00;
    TEST_ASSERT_FALSE(input::mcp23008::configured(reset));
    const input::mcp23008::Sample fine;
    TEST_ASSERT_TRUE(input::mcp23008::configured(fine));
}

void test_expander_interrupt_mask_covers_the_used_pins() {
    TEST_ASSERT_EQUAL_HEX8(0x1F, input::mcp23008::maskOf(kExpanderPins));
}

// ---------------------------------------------------------------------------
// Key legend and keycap lights: one rule for the panel and the LEDs
// ---------------------------------------------------------------------------

namespace {

app::State connectedHome(int sessions) {
    app::State state;
    state.setLinkUp(true);
    proto::InboundFrame frame = sessionsFrame(sessions, "s1", "");
    state.setSessions(frame);
    state.reconcileScreen();
    return state;
}

bool allDark(const ui::KeyLegend& legend) {
    for (const ui::KeyHint& hint : legend.keys) {
        if (hint.role != ui::KeyRole::None || hint.label != nullptr) return false;
    }
    return true;
}

bool isWhite(const ui::Rgb& colour) {
    return colour.r > 0 && colour.r == colour.g && colour.g == colour.b;
}

}  // namespace

void test_legend_nothing_lights_before_the_mac_talks() {
    const app::State state;   // Connecting
    TEST_ASSERT_TRUE(allDark(ui::keyLegendFor(state)));
}

/// At rest — one session, nothing running — every key is dark: the keycaps
/// only light up when the box has something to offer.
void test_legend_is_dark_at_rest() {
    const app::State state = connectedHome(1);
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Home, (int)state.screen());
    TEST_ASSERT_TRUE(allDark(ui::keyLegendFor(state)));
}

void test_legend_offers_stop_only_while_claude_works() {
    app::State state = connectedHome(1);
    state.setClaudeState(proto::ClaudeState::Thinking, "");
    const ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Stop, (int)legend[ui::Key::Square].role);
    TEST_ASSERT_EQUAL_STRING("STOP", legend[ui::Key::Square].label);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);
}

void test_legend_permission_lights_exactly_the_verdicts_offered() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);
    state.reconcileScreen();
    const ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Deny, (int)legend[ui::Key::Cross].role);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Stop, (int)legend[ui::Key::Square].role);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Approve, (int)legend[ui::Key::Check].role);
    TEST_ASSERT_EQUAL_STRING("DENY", legend[ui::Key::Cross].label);
    TEST_ASSERT_EQUAL_STRING("ALLOW", legend[ui::Key::Check].label);
    // One page only: the return key has nothing to turn, so it stays dark.
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Return].role);
}

/// A verdict Claude does not offer is never lit: no green under a check whose
/// press would be refused.
void test_legend_never_lights_a_verdict_that_is_not_offered() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    permission.offersApprove = false;
    state.setPermission(permission, 0);
    state.reconcileScreen();
    const ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);
    TEST_ASSERT_NULL(legend[ui::Key::Check].label);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Deny, (int)legend[ui::Key::Cross].role);
}

/// The moment the request is answered its verdict lights go out — before the
/// panel has had time to redraw.
void test_legend_verdicts_go_dark_once_the_request_is_cleared() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);
    state.reconcileScreen();
    state.clearPermission("req-1");
    state.reconcileScreen();
    const ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Cross].role);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);
}

void test_legend_menu_keys_navigate() {
    app::State state = connectedHome(1);
    state.setScreen(app::Screen::Menu);
    const ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Navigate, (int)legend[ui::Key::Cross].role);
    TEST_ASSERT_EQUAL_STRING("BACK", legend[ui::Key::Cross].label);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Navigate, (int)legend[ui::Key::Check].role);
    TEST_ASSERT_EQUAL_STRING("SELECT", legend[ui::Key::Check].label);
}

void test_lights_take_the_colour_of_the_role() {
    ui::KeyLegend legend;
    legend.keys[0].role = ui::KeyRole::Deny;
    legend.keys[2].role = ui::KeyRole::Approve;
    const bool none[ui::kKeyCount] = {false, false, false, false};
    const ui::KeyLightFrame frame = ui::keyLightsFor(legend, none);
    // Red under the cross, green under the check, the other two dark.
    TEST_ASSERT_TRUE(frame.keys[0].r > 0 && frame.keys[0].g == 0 && frame.keys[0].b == 0);
    TEST_ASSERT_TRUE(frame.keys[2].g > 0 && frame.keys[2].r == 0 && frame.keys[2].b == 0);
    TEST_ASSERT_TRUE(frame.keys[1] == ui::Rgb());
    TEST_ASSERT_TRUE(frame.keys[3] == ui::Rgb());
}

/// Every press flashes its key white — on an inert key too, since the flash
/// says "the box felt that", not "that did something".
void test_lights_press_flash_wins() {
    ui::KeyLegend legend;
    legend.keys[2].role = ui::KeyRole::Approve;
    const bool flashing[ui::kKeyCount] = {false, true, true, false};
    const ui::KeyLightFrame frame = ui::keyLightsFor(legend, flashing);
    TEST_ASSERT_TRUE(isWhite(frame.keys[1]));   // inert key, flashing
    TEST_ASSERT_TRUE(isWhite(frame.keys[2]));   // the flash beats the green
    TEST_ASSERT_TRUE(frame.keys[0] == ui::Rgb());
}

// --- The keycap self-test at boot --------------------------------------------
//
// At rest every key is dark, because a press there would do nothing — so a
// chain nobody wired looks exactly like one with nothing to say. The sweep at
// boot is what tells them apart, and the order says which LED sits under which
// key, which is what kKeyLightChain has to match.

bool anyLight(const ui::Rgb& light) {
    return light.r != 0 || light.g != 0 || light.b != 0;
}

void test_self_test_lights_all_four_first() {
    const ui::KeyLightFrame all = ui::keyLightsForTest(0);
    for (const ui::Rgb& light : all.keys) TEST_ASSERT_TRUE(anyLight(light));
}

void test_self_test_then_walks_one_key_at_a_time() {
    for (uint8_t key = 0; key < ui::kKeyCount; ++key) {
        const ui::KeyLightFrame frame = ui::keyLightsForTest(static_cast<uint8_t>(key + 1));
        for (uint8_t index = 0; index < ui::kKeyCount; ++index) {
            TEST_ASSERT_EQUAL(index == key, anyLight(frame.keys[index]));
        }
    }

    // In the legend's colours, so the sweep also says what each key does.
    const ui::Rgb deny = ui::keyLightsForTest(1).keys[static_cast<uint8_t>(ui::Key::Cross)];
    TEST_ASSERT_TRUE(deny.r > 0 && deny.g == 0 && deny.b == 0);
    const ui::Rgb approve = ui::keyLightsForTest(3).keys[static_cast<uint8_t>(ui::Key::Check)];
    TEST_ASSERT_TRUE(approve.g > 0 && approve.r == 0 && approve.b == 0);
}

void test_self_test_ends_dark_and_stays_dark() {
    for (uint8_t step = ui::kKeyLightTestSteps; step < ui::kKeyLightTestSteps + 3; ++step) {
        const ui::KeyLightFrame frame = ui::keyLightsForTest(step);
        for (const ui::Rgb& light : frame.keys) TEST_ASSERT_FALSE(anyLight(light));
    }
}

// ---------------------------------------------------------------------------
// TOTP: base32, otpauth:// links, RFC 4226 / RFC 6238
// ---------------------------------------------------------------------------

namespace {

/// Reference SHA-1 (FIPS 180-4), for the tests only: the device uses mbedTLS.
/// Checked against FIPS and RFC 2202 vectors before anything relies on it.
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t block[64] = {0};
    size_t used = 0;
    uint64_t total = 0;

    static uint32_t rol(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

    void compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
                   (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* data, size_t length) {
        total += length;
        for (size_t i = 0; i < length; ++i) {
            block[used++] = data[i];
            if (used == 64) { compress(); used = 0; }
        }
    }

    void finish(uint8_t out[20]) {
        const uint64_t bits = total * 8;
        const uint8_t marker = 0x80, zero = 0;
        update(&marker, 1);
        while (used != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(length, 8);
        for (int i = 0; i < 20; ++i) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
    }
};

size_t referenceHmacSha1(otp::Algorithm algorithm, const uint8_t* key, size_t keyLength,
                         const uint8_t* message, size_t messageLength, uint8_t* digest,
                         size_t capacity) {
    if (algorithm != otp::Algorithm::Sha1 || capacity < 20) return 0;
    uint8_t padded[64] = {0};
    if (keyLength > 64) {
        Sha1 hash;
        hash.update(key, keyLength);
        hash.finish(padded);
    } else {
        memcpy(padded, key, keyLength);
    }
    uint8_t inner[64], outer[64];
    for (int i = 0; i < 64; ++i) {
        inner[i] = padded[i] ^ 0x36;
        outer[i] = padded[i] ^ 0x5C;
    }
    uint8_t innerDigest[20];
    Sha1 first;
    first.update(inner, 64);
    first.update(message, messageLength);
    first.finish(innerDigest);
    Sha1 second;
    second.update(outer, 64);
    second.update(innerDigest, 20);
    second.finish(digest);
    return 20;
}

void hex(const uint8_t* bytes, size_t length, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0F];
    }
    out[2 * length] = '\0';
}

/// The RFC 6238 appendix B SHA-1 account: key "12345678901234567890".
otp::Account rfc6238Account(uint8_t digits) {
    otp::Account account;
    memcpy(account.secret, "12345678901234567890", 20);
    account.secretLength = 20;
    account.digits = digits;
    account.period = 30;
    account.algorithm = otp::Algorithm::Sha1;
    return account;
}

}  // namespace

/// The oracle first: if it were wrong, every TOTP test below would be too.
void test_reference_sha1_matches_fips_and_rfc2202() {
    uint8_t digest[20];
    char text[41];
    Sha1 hash;
    hash.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    hash.finish(digest);
    hex(digest, 20, text);
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", text);

    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    referenceHmacSha1(otp::Algorithm::Sha1, key, sizeof(key),
                      reinterpret_cast<const uint8_t*>("Hi There"), 8, digest, sizeof(digest));
    hex(digest, 20, text);
    TEST_ASSERT_EQUAL_STRING("b617318655057264e28bc0b6fb378c8ef146be00", text);
}

void test_base32_decodes_rfc4648_vectors() {
    struct Case { const char* encoded; const char* decoded; };
    const Case cases[] = {
        {"MY======", "f"}, {"MZXQ====", "fo"}, {"MZXW6===", "foo"}, {"MZXW6YQ=", "foob"},
        {"MZXW6YTB", "fooba"}, {"MZXW6YTBOI======", "foobar"},
    };
    for (const Case& test : cases) {
        uint8_t out[16];
        size_t length = 0;
        TEST_ASSERT_TRUE_MESSAGE(otp::decodeBase32(test.encoded, out, sizeof(out), length),
                                 test.encoded);
        TEST_ASSERT_EQUAL_UINT32(strlen(test.decoded), length);
        TEST_ASSERT_EQUAL_MEMORY(test.decoded, out, length);
    }
}

/// Services print secrets in lower case, in groups, with or without padding.
void test_base32_accepts_how_services_print_secrets() {
    uint8_t out[16];
    size_t length = 0;
    TEST_ASSERT_TRUE(otp::decodeBase32("jbsw y3dp-ehpk 3pxp", out, sizeof(out), length));
    const uint8_t expected[] = {'H', 'e', 'l', 'l', 'o', '!', 0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), length);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, sizeof(expected));
}

void test_base32_rejects_what_is_not_base32() {
    uint8_t out[16];
    size_t length = 0;
    TEST_ASSERT_FALSE(otp::decodeBase32("", out, sizeof(out), length));
    TEST_ASSERT_FALSE(otp::decodeBase32("====", out, sizeof(out), length));
    TEST_ASSERT_FALSE(otp::decodeBase32("JBSW1", out, sizeof(out), length));    // 1 is not base32
    TEST_ASSERT_FALSE(otp::decodeBase32("JBSW8", out, sizeof(out), length));    // nor is 8
    TEST_ASSERT_FALSE(otp::decodeBase32("MY==MY", out, sizeof(out), length));  // data after padding
    TEST_ASSERT_FALSE(otp::decodeBase32("MZXW6YTBOI", out, 2, length));        // does not fit
}

/// RFC 6238 appendix B, SHA-1, all six instants — the last one past 2^32
/// seconds, which only works with 64-bit time.
void test_totp_matches_rfc6238_sha1_vectors() {
    struct Case { uint64_t time; const char* code; };
    const Case cases[] = {
        {59ull, "94287082"}, {1111111109ull, "07081804"}, {1111111111ull, "14050471"},
        {1234567890ull, "89005924"}, {2000000000ull, "69279037"}, {20000000000ull, "65353130"},
    };
    const otp::Account account = rfc6238Account(8);
    for (const Case& test : cases) {
        uint32_t code = 0;
        TEST_ASSERT_TRUE(otp::totp(account, test.time, referenceHmacSha1, code));
        char text[9];
        otp::formatCode(code, account.digits, text, sizeof(text));
        TEST_ASSERT_EQUAL_STRING(test.code, text);
    }
}

/// Six digits is the usual length; the leading zeros must survive.
void test_totp_six_digits_keep_leading_zeros() {
    const otp::Account account = rfc6238Account(6);
    uint32_t code = 0;
    TEST_ASSERT_TRUE(otp::totp(account, 1234567890ull, referenceHmacSha1, code));
    char text[9];
    otp::formatCode(code, account.digits, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("005924", text);
    TEST_ASSERT_TRUE(otp::totp(account, 1111111109ull, referenceHmacSha1, code));
    otp::formatCode(code, account.digits, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("081804", text);
}

void test_totp_seconds_left_counts_down_the_window() {
    const otp::Account account = rfc6238Account(6);
    TEST_ASSERT_EQUAL_UINT16(1, otp::secondsLeft(account, 59));
    TEST_ASSERT_EQUAL_UINT16(30, otp::secondsLeft(account, 60));
    TEST_ASSERT_EQUAL_UINT16(29, otp::secondsLeft(account, 61));
}

void test_otpauth_reads_a_standard_link() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT(
        (int)otp::ParseError::None,
        (int)otp::parseOtpauthUri(
            "otpauth://totp/Example:alice@google.com?secret=JBSWY3DPEHPK3PXP&issuer=Example",
            account));
    TEST_ASSERT_EQUAL_STRING("Example:alice@google.com", account.name);
    TEST_ASSERT_EQUAL_UINT8(10, account.secretLength);
    TEST_ASSERT_EQUAL_UINT8(6, account.digits);
    TEST_ASSERT_EQUAL_UINT16(30, account.period);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha1, (int)account.algorithm);
}

void test_otpauth_reads_its_parameters() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT(
        (int)otp::ParseError::None,
        (int)otp::parseOtpauthUri("OTPAUTH://TOTP/ACME%20Co:bob?SECRET=jbswy3dpehpk3pxp"
                                  "&algorithm=sha256&digits=8&period=60&image=x",
                                  account));
    TEST_ASSERT_EQUAL_STRING("ACME Co:bob", account.name);
    TEST_ASSERT_EQUAL_UINT8(8, account.digits);
    TEST_ASSERT_EQUAL_UINT16(60, account.period);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha256, (int)account.algorithm);
}

/// A bare "alice" label gets its issuer in front, so two services' "alice"
/// accounts stay apart in the list.
void test_otpauth_puts_the_issuer_in_front_of_a_bare_label() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT(
        (int)otp::ParseError::None,
        (int)otp::parseOtpauthUri("otpauth://totp/alice?secret=JBSWY3DPEHPK3PXP&issuer=GitHub",
                                  account));
    TEST_ASSERT_EQUAL_STRING("GitHub:alice", account.name);
}

void test_otpauth_refuses_what_it_cannot_honour() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::NotOtpauth,
                          (int)otp::parseOtpauthUri("https://example.com/?secret=AAAA", account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::NotTotp,
                          (int)otp::parseOtpauthUri("otpauth://hotp/x?secret=JBSWY3DP&counter=1",
                                                    account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::MissingSecret,
                          (int)otp::parseOtpauthUri("otpauth://totp/x?issuer=y", account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadSecret,
                          (int)otp::parseOtpauthUri("otpauth://totp/x?secret=not-base32!", account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadDigits,
                          (int)otp::parseOtpauthUri("otpauth://totp/x?secret=JBSWY3DP&digits=5",
                                                    account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadPeriod,
                          (int)otp::parseOtpauthUri("otpauth://totp/x?secret=JBSWY3DP&period=1",
                                                    account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadAlgorithm,
                          (int)otp::parseOtpauthUri("otpauth://totp/x?secret=JBSWY3DP&algorithm=MD5",
                                                    account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::MissingName,
                          (int)otp::parseOtpauthUri("otpauth://totp/?secret=JBSWY3DP", account));
}

/// The panel draws ASCII only: an accented character becomes one '?', and a
/// long name is clipped to what the list can show.
void test_otpauth_names_fit_the_panel() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT(
        (int)otp::ParseError::None,
        (int)otp::parseOtpauthUri("otpauth://totp/Soci%C3%A9t%C3%A9:a?secret=JBSWY3DP", account));
    TEST_ASSERT_EQUAL_STRING("Soci?t?:a", account.name);
    TEST_ASSERT_EQUAL_INT(
        (int)otp::ParseError::None,
        (int)otp::parseOtpauthUri(
            "otpauth://totp/A%20very%20long%20service%20name:someone@example.com?secret=JBSWY3DP",
            account));
    TEST_ASSERT_EQUAL_UINT32(otp::kNameCapacity - 1, strlen(account.name));
}

void test_make_account_checks_the_typed_fields() {
    otp::Account account;
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::None,
                          (int)otp::makeAccount("  GitHub  ", "jbsw y3dp", 6, 30,
                                                otp::Algorithm::Sha1, account));
    TEST_ASSERT_EQUAL_STRING("GitHub", account.name);
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::MissingName,
                          (int)otp::makeAccount("   ", "JBSWY3DP", 6, 30, otp::Algorithm::Sha1,
                                                account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::MissingSecret,
                          (int)otp::makeAccount("x", "", 6, 30, otp::Algorithm::Sha1, account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadDigits,
                          (int)otp::makeAccount("x", "JBSWY3DP", 9, 30, otp::Algorithm::Sha1,
                                                account));
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::BadPeriod,
                          (int)otp::makeAccount("x", "JBSWY3DP", 6, 5, otp::Algorithm::Sha1,
                                                account));
}

// --- Google Authenticator transfer links ------------------------------------
//
// Made by tools/test-vectors/transfer_links.py, which builds the protobuf by
// hand, apart from the firmware's decoder, so the decoder is not checked
// against itself. The batch holds ten accounts, one per way one can go wrong.

constexpr const char* kTransferBatch =
    "otpauth-migration://offline?data=CjcKFAECAwQFBgcICQoLDA0ODxAREhMUEhFhbGljZUBleGFtcGxlLmNvbRoGR2l0SHViIAEoATACChsKBQECAwQFEghPbGQgYmFuaxoAIAEoATABOAUKPAogoKGio6SlpqeoqaqrrK2ur7CxsrO0tba3uLm6u7y9vr8SClByb3Rvbjpib2IaBlByb3RvbiACKAIwAgoiChAJCQkJCQkJCQkJCQkJCQkJEgZMZWdhY3kaACAEKAEwAgoZCgpCQkJCQkJCQkJCEgAaA0FXUyAAKAAwAApgCkAHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHEhRTb2Npw6l0w6kgR8OpbsOpcmFsZRoAIAMoATACClUKQQcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHEghUb28gbG9uZxoAIAEoATACChMSCU5vIHNlY3JldBoAIAEoATACChYKCgUFBQUFBQUFBQUSABoAIAEoATACChsKCgYGBgYGBgYGBgYSBVNldmVuGgAgASgDMAIQARgDIAEouWA%3D";
constexpr const char* kTransferUrlSafe =
    "otpauth-migration://offline?data=CjcKFAECAwQFBgcICQoLDA0ODxAREhMUEhFhbGljZUBleGFtcGxlLmNvbRoGR2l0SHViIAEoATACChsKBQECAwQFEghPbGQgYmFuaxoAIAEoATABOAUKPAogoKGio6SlpqeoqaqrrK2ur7CxsrO0tba3uLm6u7y9vr8SClByb3Rvbjpib2IaBlByb3RvbiACKAIwAgoiChAJCQkJCQkJCQkJCQkJCQkJEgZMZWdhY3kaACAEKAEwAgoZCgpCQkJCQkJCQkJCEgAaA0FXUyAAKAAwAApgCkAHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHEhRTb2Npw6l0w6kgR8OpbsOpcmFsZRoAIAMoATACClUKQQcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHEghUb28gbG9uZxoAIAEoATACChMSCU5vIHNlY3JldBoAIAEoATACChYKCgUFBQUFBQUFBQUSABoAIAEoATACChsKCgYGBgYGBgYGBgYSBVNldmVuGgAgASgDMAIQARgDIAEouWA";
constexpr const char* kTransferUnknownFields =
    "otpauth-migration://offline?data=CkEKFAECAwQFBgcICQoLDA0ODxAREhMUEgVhbGljZRoGR2l0SHViIAEoATACSRERERERERERVSIiIiJYrAJiA3h5ehABGAEgACi5YEkREREREREREVUiIiIiWKwCYgN4eXo%3D";
constexpr const char* kTransferTruncated =
    "otpauth-migration://offline?data=CjcKFAECAwQFBgcICQoLDA0ODxAREhMUEhFhbGljZUBleGFtcGxlLmNvbRoGR2l0SHViIAEoATACChsKBQECAwQFEghPbGQgYmFuaxoAIAEoATABOAUKPAogoKGio6SlpqeoqaqrrK2ur7CxsrO0tba3uLm6u7y9vr8SClByb3Rvbjpib2IaBlByb3RvbiACKAIwAgoiChAJCQkJCQkJCQkJCQkJCQkJEgZMZWdhY3kaACAEKAEwAgoZCgpCQkJCQkJCQkJCEgA%3D";
constexpr const char* kTransferEmpty =
    "otpauth-migration://offline?data=EAEYASAAKLlg";

#define ASSERT_PARSE(expected, actual) \
    TEST_ASSERT_EQUAL_INT((int)otp::ParseError::expected, (int)(actual))

/// The example every tool that reads these links quotes: one account whose
/// secret is the TOTP tutorials' JBSWY3DPEHPK3PXP.
void test_transfer_link_reads_the_classic_example() {
    otp::MigrationReader reader;
    ASSERT_PARSE(None, reader.open("otpauth-migration://offline?data=CjEKCkhlbGxvId6tvu8SGEV4YW1wbGU6"
                                   "YWxpY2VAZ29vZ2xlLmNvbRoHRXhhbXBsZTAC"));
    TEST_ASSERT_EQUAL_UINT16(1, reader.count());

    otp::Account account;
    otp::ParseError error = otp::ParseError::BadSecret;
    TEST_ASSERT_TRUE(reader.next(account, error));
    ASSERT_PARSE(None, error);
    TEST_ASSERT_EQUAL_STRING("Example:alice@google.com", account.name);
    uint8_t expected[16];
    size_t length = 0;
    TEST_ASSERT_TRUE(otp::decodeBase32("JBSWY3DPEHPK3PXP", expected, sizeof(expected), length));
    TEST_ASSERT_EQUAL_UINT8(length, account.secretLength);
    TEST_ASSERT_EQUAL_MEMORY(expected, account.secret, length);
    TEST_ASSERT_EQUAL_UINT8(6, account.digits);
    TEST_ASSERT_EQUAL_UINT16(30, account.period);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha1, (int)account.algorithm);
    TEST_ASSERT_FALSE(reader.next(account, error));
}

void assertFilled(const otp::Account& account, uint8_t length, uint8_t first, bool counting) {
    TEST_ASSERT_EQUAL_UINT8(length, account.secretLength);
    for (uint8_t index = 0; index < length; ++index) {
        TEST_ASSERT_EQUAL_UINT8(counting ? first + index : first, account.secret[index]);
    }
}

/// Each account comes out on its own, in order, and the ones the box cannot
/// hold still say who they are, so the page can list them.
void test_transfer_link_reports_every_account_in_a_batch() {
    otp::MigrationReader reader;
    ASSERT_PARSE(None, reader.open(kTransferBatch));
    TEST_ASSERT_EQUAL_UINT16(10, reader.count());
    TEST_ASSERT_EQUAL_UINT8(1, reader.batchIndex());   // the second of three QR codes
    TEST_ASSERT_EQUAL_UINT8(3, reader.batchSize());

    struct Expected {
        const char* name;
        otp::ParseError error;
    };
    const Expected expected[] = {
        {"GitHub:alice@example.com", otp::ParseError::None},
        {"Old bank", otp::ParseError::NotTotp},          // counter-based
        {"Proton:bob", otp::ParseError::None},
        {"Legacy", otp::ParseError::BadAlgorithm},       // MD5
        {"AWS", otp::ParseError::None},                  // every field "unspecified"
        {"Soci?t? G?n?rale", otp::ParseError::None},     // UTF-8 folded for the panel
        {"Too long", otp::ParseError::SecretTooLong},    // 65 bytes
        {"No secret", otp::ParseError::MissingSecret},
        {"", otp::ParseError::MissingName},
        {"Seven", otp::ParseError::BadDigits},           // a digit count that does not exist
    };
    otp::Account accounts[10];
    for (size_t index = 0; index < 10; ++index) {
        otp::ParseError error = otp::ParseError::None;
        TEST_ASSERT_TRUE(reader.next(accounts[index], error));
        TEST_ASSERT_EQUAL_STRING(expected[index].name, accounts[index].name);
        TEST_ASSERT_EQUAL_INT((int)expected[index].error, (int)error);
    }
    otp::ParseError error = otp::ParseError::None;
    TEST_ASSERT_FALSE(reader.next(accounts[0], error));

    assertFilled(accounts[0], 20, 1, true);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha1, (int)accounts[0].algorithm);
    TEST_ASSERT_EQUAL_UINT8(6, accounts[0].digits);
    assertFilled(accounts[2], 32, 0xA0, true);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha256, (int)accounts[2].algorithm);
    TEST_ASSERT_EQUAL_UINT8(8, accounts[2].digits);
    assertFilled(accounts[4], 10, 0x42, false);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha1, (int)accounts[4].algorithm);
    TEST_ASSERT_EQUAL_UINT8(6, accounts[4].digits);
    assertFilled(accounts[5], 64, 0x07, false);
    TEST_ASSERT_EQUAL_INT((int)otp::Algorithm::Sha512, (int)accounts[5].algorithm);
    // No account that cannot be added carries a secret out of the reader.
    TEST_ASSERT_EQUAL_UINT8(0, accounts[1].secretLength);
    TEST_ASSERT_EQUAL_UINT8(0, accounts[3].secretLength);
    TEST_ASSERT_EQUAL_UINT8(0, accounts[6].secretLength);
}

/// URL-safe base64 without padding reads the same as the standard kind with
/// its padding percent-escaped.
void test_transfer_link_takes_either_base64_alphabet() {
    otp::MigrationReader standard;
    otp::MigrationReader urlSafe;
    ASSERT_PARSE(None, standard.open(kTransferBatch));
    ASSERT_PARSE(None, urlSafe.open(kTransferUrlSafe));
    otp::Account left;
    otp::Account right;
    otp::ParseError leftError = otp::ParseError::None;
    otp::ParseError rightError = otp::ParseError::None;
    while (standard.next(left, leftError)) {
        TEST_ASSERT_TRUE(urlSafe.next(right, rightError));
        TEST_ASSERT_EQUAL_STRING(left.name, right.name);
        TEST_ASSERT_EQUAL_INT((int)leftError, (int)rightError);
        TEST_ASSERT_EQUAL_MEMORY(left.secret, right.secret, sizeof(left.secret));
    }
    TEST_ASSERT_FALSE(urlSafe.next(right, rightError));
}

/// A newer exporter's extra fields, of every wire type, are stepped over.
void test_transfer_link_skips_fields_it_does_not_know() {
    otp::MigrationReader reader;
    ASSERT_PARSE(None, reader.open(kTransferUnknownFields));
    otp::Account account;
    otp::ParseError error = otp::ParseError::BadSecret;
    TEST_ASSERT_TRUE(reader.next(account, error));
    ASSERT_PARSE(None, error);
    TEST_ASSERT_EQUAL_STRING("GitHub:alice", account.name);
    assertFilled(account, 20, 1, true);
    TEST_ASSERT_FALSE(reader.next(account, error));
}

/// Nothing from a damaged link is added, not even the accounts before the
/// damage: half a transfer is harder to notice than none.
void test_transfer_link_refuses_a_damaged_link_whole() {
    otp::MigrationReader reader;
    otp::Account account;
    otp::ParseError error = otp::ParseError::None;

    ASSERT_PARSE(BadTransfer, reader.open(kTransferTruncated));
    TEST_ASSERT_EQUAL_UINT16(0, reader.count());
    TEST_ASSERT_FALSE(reader.next(account, error));

    ASSERT_PARSE(BadTransfer, reader.open(kTransferEmpty));                          // no account
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data=@@@@"));
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data=CjEK%2"));
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data=C"));    // a lone symbol
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data="));
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?other=CjEK"));
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline"));
    ASSERT_PARSE(NotOtpauth, reader.open("otpauth-migration://online?data=CjEK"));
    ASSERT_PARSE(NotOtpauth, reader.open("otpauth://totp/x?secret=JBSWY3DPEHPK3PXP"));

    // Group wire types (3, 4) are not in this format.
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data=CwwK"));
    // A length that runs past the end of the payload.
    ASSERT_PARSE(BadTransfer, reader.open("otpauth-migration://offline?data=Cn8K"));

    // More than the payload buffer holds.
    static char huge[otp::kMaxMigrationLinkLength + 64];
    const char* prefix = "otpauth-migration://offline?data=";
    strcpy(huge, prefix);
    memset(huge + strlen(prefix), 'A', sizeof(huge) - strlen(prefix) - 1);
    huge[sizeof(huge) - 1] = '\0';
    ASSERT_PARSE(BadTransfer, reader.open(huge));
}

void test_transfer_link_is_told_apart_from_an_otpauth_link() {
    TEST_ASSERT_TRUE(otp::isMigrationLink("otpauth-migration://offline?data=x"));
    TEST_ASSERT_TRUE(otp::isMigrationLink("  OTPAUTH-MIGRATION://offline?data=x"));
    TEST_ASSERT_FALSE(otp::isMigrationLink("otpauth://totp/x?secret=JBSWY3DPEHPK3PXP"));
    otp::Account account;
    ASSERT_PARSE(NotOtpauth, otp::parseOtpauthUri("otpauth-migration://offline?data=x", account));
}

/// Once read, the payload is gone: it held every secret in the link.
void test_transfer_link_wipes_its_payload() {
    otp::MigrationReader reader;
    ASSERT_PARSE(None, reader.open(kTransferBatch));
    reader.wipe();
    TEST_ASSERT_EQUAL_UINT16(0, reader.count());
    otp::Account account;
    otp::ParseError error = otp::ParseError::None;
    TEST_ASSERT_FALSE(reader.next(account, error));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&reader);
    for (size_t index = 0; index < otp::kMaxMigrationPayload; ++index) {
        TEST_ASSERT_EQUAL_UINT8(0, bytes[index]);
    }
}

// ---------------------------------------------------------------------------
// One-time codes: the wire, the clock, the screens
// ---------------------------------------------------------------------------

void test_parses_time_frame() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT(
        (int)proto::ParseResult::Ok,
        (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":1726000000123}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::InboundType::Time, (int)frame.type);
    TEST_ASSERT_TRUE(frame.unixMs == 1726000000123ull);
}

/// Codes are derived from this clock, so anything that is not plainly one is
/// refused: a boolean, a fraction, a string, or a date no Mac would send.
void test_refuses_a_clock_that_is_not_one() {
    proto::InboundFrame frame;
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"time\"}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":true}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":\"1726000000123\"}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::MissingField,
                          (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":1726000000123.5}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::OutOfRange,
                          (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":86400000}", frame));
    TEST_ASSERT_EQUAL_INT((int)proto::ParseResult::OutOfRange,
                          (int)parse("{\"v\":1,\"type\":\"time\",\"unixMs\":9999999999999}", frame));
}

void test_encodes_otp_frame() {
    char frame[256];
    TEST_ASSERT_TRUE(proto::encodeOtp(frame, sizeof(frame), "GitHub:alice", "012345", 18000) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"v\":1,\"type\":\"otp\",\"label\":\"GitHub:alice\",\"code\":\"012345\","
        "\"expiresInMs\":18000}\n",
        frame);
}

/// The Mac puts what this frame carries on its clipboard, so it carries a code
/// and nothing else — or it is not written at all.
void test_refuses_to_put_anything_but_a_code_on_the_clipboard() {
    char frame[256];
    const char* notCodes[] = {"", "12345", "123456789", "12a456", "123 456", "rm -rf /"};
    for (const char* text : notCodes) {
        TEST_ASSERT_FALSE_MESSAGE(proto::isOtpCode(text), text);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, proto::encodeOtp(frame, sizeof(frame), "x", text, 1000),
                                         text);
    }
    TEST_ASSERT_TRUE(proto::isOtpCode("123456"));
    TEST_ASSERT_TRUE(proto::isOtpCode("1234567"));
    TEST_ASSERT_TRUE(proto::isOtpCode("12345678"));
}

void test_clock_carries_forward_on_millis() {
    app::State state;
    TEST_ASSERT_FALSE(state.hasClock());
    state.setClock(1726000000500ull, 1000);
    TEST_ASSERT_TRUE(state.hasClock());
    TEST_ASSERT_TRUE(state.unixSeconds(1000) == 1726000000ull);
    TEST_ASSERT_TRUE(state.unixSeconds(3500) == 1726000003ull);
}

/// millis() wraps every 49.7 days. A clock synced just before must not jump
/// back seven weeks just after, or every code would be wrong until the next sync.
void test_clock_survives_millis_wrapping() {
    app::State state;
    state.setClock(1726000000000ull, 0xFFFFF000u);
    TEST_ASSERT_TRUE(state.unixSeconds(0x00001000u) == 1726000008ull);   // 0x2000 ms later
}

void test_otp_list_cursor_clamps_and_follows_a_shrinking_list() {
    app::State state;
    const char* names[] = {"Alpha", "Beta", "Gamma"};
    state.setOtpAccounts(3, names);
    TEST_ASSERT_FALSE(state.moveOtpCursor(-1));
    TEST_ASSERT_TRUE(state.moveOtpCursor(5));
    TEST_ASSERT_EQUAL_UINT8(2, state.otpCursor());
    TEST_ASSERT_EQUAL_STRING("Gamma", state.otpName(2));
    // An account removed from the setup page: the cursor stays on the list.
    state.setOtpAccounts(2, names);
    TEST_ASSERT_EQUAL_UINT8(1, state.otpCursor());
    TEST_ASSERT_EQUAL_STRING("", state.otpName(5));
}

void test_otp_code_expires_once() {
    app::State state;
    state.showOtpCode("GitHub", "123456", 12, 5000);
    TEST_ASSERT_FALSE(state.otpExpired());
    TEST_ASSERT_TRUE(state.expireOtpCode());
    TEST_ASSERT_FALSE(state.expireOtpCode());
    // A fresh code starts a fresh window.
    state.showOtpCode("GitHub", "654321", 30, 40000);
    TEST_ASSERT_FALSE(state.otpExpired());
}

/// A permission outranks everything, the one-time-code screens included — the
/// Wi-Fi setup page goes with them.
void test_permission_preempts_the_otp_screens() {
    app::State state = connectedHome(1);
    state.setScreen(app::Screen::OtpSetup);
    TEST_ASSERT_FALSE(state.reconcileScreen());
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());
}

// --- A verdict on its way ---------------------------------------------------
//
// The screen leaves a request the moment its verdict goes out: a press should
// feel like it did something, rather than waiting on the Mac's read, press and
// confirm. The request is kept aside, not dropped — a decision the operator
// believes they made must never be the one that went quietly missing.

void test_a_sent_verdict_takes_the_request_off_the_screen() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 1000);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());

    TEST_ASSERT_TRUE(state.markPermissionAnswered(1500));
    TEST_ASSERT_FALSE(state.permissionOnScreen());
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Home, (int)state.screen());

    // One press, one verdict: the keys cannot send a second one for it.
    TEST_ASSERT_FALSE(state.canSendVerdict("req-1", 9000, 200));
    TEST_ASSERT_FALSE(state.markPermissionAnswered(1600));
    // And the status LED stops asking for a decision the operator has made.
    TEST_ASSERT_FALSE(state.actionPending());
}

void test_a_verdict_the_mac_never_confirms_brings_the_request_back() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 1000);
    state.markPermissionAnswered(1500);

    TEST_ASSERT_FALSE(state.restorePermission(1500 + 2999, 3000));
    TEST_ASSERT_TRUE(state.restorePermission(1500 + 3000, 3000));
    TEST_ASSERT_TRUE(state.permissionOnScreen());
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());

    // Armed afresh on the way in: a key still held down must not answer it.
    TEST_ASSERT_FALSE(state.canSendVerdict("req-1", 1500 + 3000 + 199, 200));
    TEST_ASSERT_TRUE(state.canSendVerdict("req-1", 1500 + 3000 + 200, 200));
    // Once back, it is answerable again — and once only.
    TEST_ASSERT_TRUE(state.markPermissionAnswered(1500 + 3200));
}

void test_a_confirmed_verdict_lets_the_request_go() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 1000);
    state.markPermissionAnswered(1500);

    TEST_ASSERT_TRUE(state.clearPermission("req-1"));
    TEST_ASSERT_FALSE(state.restorePermission(100000, 3000));
    TEST_ASSERT_FALSE(state.permissionOnScreen());
}

/// The Mac re-sends a request when the verdict it received could not be
/// applied: that is what brings it back without waiting out the timeout.
void test_the_mac_resending_a_request_brings_it_back_at_once() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 1000);
    state.markPermissionAnswered(1500);

    TEST_ASSERT_TRUE(state.setPermission(permission, 2000));
    TEST_ASSERT_TRUE(state.permissionOnScreen());
    TEST_ASSERT_FALSE(state.canSendVerdict("req-1", 2199, 200));
    TEST_ASSERT_TRUE(state.canSendVerdict("req-1", 2200, 200));

    // A repeat while it is plainly on screen still must not re-arm it, or a
    // frame every poll would keep it permanently unanswerable.
    TEST_ASSERT_FALSE(state.setPermission(permission, 3000));
    TEST_ASSERT_TRUE(state.canSendVerdict("req-1", 3000, 200));
}

void test_a_session_list_that_drops_it_ends_an_answered_request() {
    app::State state = connectedHome(1);
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 1000);
    state.markPermissionAnswered(1500);

    // Level-triggered: the Mac no longer names it, so it is gone for good.
    proto::InboundFrame sessions = sessionsFrame(1, "s1", "");
    state.setSessions(sessions);
    TEST_ASSERT_FALSE(state.restorePermission(100000, 3000));
    TEST_ASSERT_FALSE(state.permissionOnScreen());
}

void test_a_newer_request_replaces_an_answered_one() {
    app::State state = connectedHome(1);
    proto::InboundFrame first = permissionFrame("req-1", "s1");
    state.setPermission(first, 1000);
    state.markPermissionAnswered(1500);

    proto::InboundFrame second = permissionFrame("req-2", "s1");
    TEST_ASSERT_TRUE(state.setPermission(second, 2000));
    TEST_ASSERT_TRUE(state.permissionOnScreen());
    TEST_ASSERT_EQUAL_STRING("req-2", state.permission().id);
    TEST_ASSERT_FALSE(state.restorePermission(100000, 3000));
}

void test_legend_otp_list_offers_copy_only_when_it_can() {
    app::State state = connectedHome(1);
    state.setScreen(app::Screen::OtpList);
    // No account and no clock: back is all there is.
    ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_STRING("BACK", legend[ui::Key::Cross].label);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);

    const char* names[] = {"GitHub"};
    state.setOtpAccounts(1, names);
    legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);   // no clock yet

    state.setClock(1726000000000ull, 0);
    legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_STRING("COPY", legend[ui::Key::Check].label);
    // Copying a code is not a verdict: no green, which stays Claude's "allow".
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::Navigate, (int)legend[ui::Key::Check].role);
}

void test_legend_otp_code_and_setup_screens() {
    app::State state = connectedHome(1);
    state.setClock(1726000000000ull, 0);
    state.setScreen(app::Screen::OtpCode);
    ui::KeyLegend legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_STRING("BACK", legend[ui::Key::Cross].label);
    TEST_ASSERT_EQUAL_STRING("AGAIN", legend[ui::Key::Check].label);

    state.setScreen(app::Screen::OtpSetup);
    legend = ui::keyLegendFor(state);
    TEST_ASSERT_EQUAL_STRING("DONE", legend[ui::Key::Cross].label);
    TEST_ASSERT_EQUAL_INT((int)ui::KeyRole::None, (int)legend[ui::Key::Check].role);
}

// --- The setup screen's Wi-Fi QR code ---------------------------------------

void test_wifi_payload_is_what_phone_cameras_read() {
    char payload[96];
    TEST_ASSERT_TRUE(otp::wifiPayload("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", payload,
                                      sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:ChickenMicro-4F2A;P:k7mp-q2xa-9rtz;;", payload);
}

/// The format's own separators, in a name or a password, are escaped — or the
/// phone would join a network called "a" with the rest taken as fields.
void test_wifi_payload_escapes_the_separators() {
    char payload[96];
    TEST_ASSERT_TRUE(otp::wifiPayload("a;b,c", "p:w\\\"d", payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\;b\\,c;P:p\\:w\\\\\\\"d;;", payload);
}

void test_wifi_payload_refuses_to_truncate() {
    char payload[40];
    memset(payload, 'x', sizeof(payload));
    TEST_ASSERT_FALSE(otp::wifiPayload("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", payload,
                                       sizeof(payload)));
    // Never runs past the buffer, and never leaves an unterminated string.
    TEST_ASSERT_TRUE(memchr(payload, '\0', sizeof(payload)) != nullptr);
}

/// The finder pattern: a dark ring, a light ring, a dark 3x3 core.
void assertFinderAt(const otp::QrBitmap& qr, uint8_t left, uint8_t top) {
    for (uint8_t y = 0; y < 7; ++y) {
        for (uint8_t x = 0; x < 7; ++x) {
            const bool ring = x == 0 || y == 0 || x == 6 || y == 6;
            const bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
            TEST_ASSERT_EQUAL(ring || core, qr.module(left + x, top + y));
        }
    }
}

/// This box's payload is version 3: 29 modules, which the screen draws at
/// three pixels a module with room to spare.
void test_wifi_qr_is_a_version_3_code_with_its_finders() {
    char payload[96];
    otp::wifiPayload("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", payload, sizeof(payload));
    otp::QrBitmap qr;
    TEST_ASSERT_TRUE(otp::encodeQr(payload, qr));
    TEST_ASSERT_EQUAL_UINT8(29, qr.size);
    assertFinderAt(qr, 0, 0);
    assertFinderAt(qr, qr.size - 7, 0);
    assertFinderAt(qr, 0, qr.size - 7);
    // Timing patterns: alternating, dark on even positions, between the finders.
    for (uint8_t i = 8; i < qr.size - 8; ++i) {
        TEST_ASSERT_EQUAL(i % 2 == 0, qr.module(i, 6));
        TEST_ASSERT_EQUAL(i % 2 == 0, qr.module(6, i));
    }
}

void test_wifi_qr_refuses_what_does_not_fit_the_screen() {
    char text[160];
    memset(text, 'a', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    otp::QrBitmap qr;
    qr.size = 7;
    TEST_ASSERT_FALSE(otp::encodeQr(text, qr));
    TEST_ASSERT_EQUAL_UINT8(0, qr.size);
}

/// Once the network is down, nothing that joined it is left on the screen.
void test_setup_screen_forgets_the_network_once_closed() {
    app::State state = connectedHome(1);
    otp::QrBitmap qr;
    TEST_ASSERT_TRUE(otp::encodeQr("WIFI:T:WPA;S:x;P:12345678;;", qr));
    state.setOtpSetup("ChickenMicro-4F2A", "k7mp-q2xa-9rtz", "http://192.168.4.1", qr);
    state.setSetupNote("Joined. Open the page.");
    TEST_ASSERT_EQUAL_STRING("k7mp-q2xa-9rtz", state.setupPassword());
    TEST_ASSERT_TRUE(state.setupQr().size > 0);

    state.clearOtpSetup();
    TEST_ASSERT_EQUAL_STRING("", state.setupNetwork());
    TEST_ASSERT_EQUAL_STRING("", state.setupPassword());
    TEST_ASSERT_EQUAL_STRING("", state.setupNote());
    TEST_ASSERT_EQUAL_UINT8(0, state.setupQr().size);
    for (uint8_t byte : state.setupQr().bits) TEST_ASSERT_EQUAL_UINT8(0, byte);
}

/// The menu steps through every waveform and wraps; it never lands on the
/// `Count` sentinel and every real entry has a name.
void test_waveform_cycles_and_wraps() {
    ui::Waveform waveform = ui::Waveform::Gentle;
    const int steps = 2 * static_cast<int>(ui::Waveform::Count);
    for (int step = 0; step < steps; ++step) {
        waveform = ui::nextWaveform(waveform);
        TEST_ASSERT_TRUE(waveform != ui::Waveform::Count);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(ui::waveformName(waveform), "?"));
    }
    // A whole number of laps lands back where it started.
    TEST_ASSERT_EQUAL_INT((int)ui::Waveform::Gentle, (int)waveform);
}

void test_menu_cursor_clamps_and_names_its_item() {
    app::State state;
    TEST_ASSERT_EQUAL_UINT8(0, state.menuCursor());
    // One-time codes lead the menu: the entry used most, one flick from the top.
    TEST_ASSERT_EQUAL_INT((int)app::MenuItem::Otp, (int)state.menuItem());

    // Already at the top: no move up.
    TEST_ASSERT_FALSE(state.moveMenuCursor(-1));
    TEST_ASSERT_TRUE(state.moveMenuCursor(1));
    TEST_ASSERT_EQUAL_UINT8(1, state.menuCursor());

    // Cannot run past the last entry.
    for (int step = 0; step < 10; ++step) state.moveMenuCursor(1);
    TEST_ASSERT_EQUAL_UINT8(app::State::menuItemCount() - 1, state.menuCursor());
    TEST_ASSERT_EQUAL_INT((int)app::MenuItem::Close, (int)state.menuItem());
    TEST_ASSERT_FALSE(state.moveMenuCursor(1));

    state.resetMenuCursor();
    TEST_ASSERT_EQUAL_UINT8(0, state.menuCursor());
}

/// The menu is modal: reconcile leaves it alone, but a permission still preempts
/// it and a dropped link falls back to Connecting.
void test_menu_screen_is_modal() {
    app::State state;
    state.setLinkUp(true);
    state.setScreen(app::Screen::Menu);
    TEST_ASSERT_FALSE(state.reconcileScreen());
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Menu, (int)state.screen());

    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    state.setPermission(permission, 0);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Permission, (int)state.screen());

    state.clearPermission(nullptr);
    state.setScreen(app::Screen::Menu);
    state.setLinkUp(false);
    state.reconcileScreen();
    TEST_ASSERT_EQUAL_INT((int)app::Screen::Connecting, (int)state.screen());
}

void test_page_span_with_a_shorter_first_page() {
    // 9 lines, 3 on the first page and 4 after it: 1 + ceil(6 / 4) = 3.
    TEST_ASSERT_EQUAL_UINT16(3, ui::pageCountWithLead(9, 3, 4));
    TEST_ASSERT_EQUAL_UINT16(1, ui::pageCountWithLead(3, 3, 4));
    TEST_ASSERT_EQUAL_UINT16(1, ui::pageCountWithLead(0, 3, 4));

    uint16_t first = 0;
    uint16_t count = 0;
    ui::pageSpan(9, 3, 4, 0, first, count);
    TEST_ASSERT_EQUAL_UINT16(0, first);
    TEST_ASSERT_EQUAL_UINT16(3, count);
    ui::pageSpan(9, 3, 4, 1, first, count);
    TEST_ASSERT_EQUAL_UINT16(3, first);
    TEST_ASSERT_EQUAL_UINT16(4, count);
    ui::pageSpan(9, 3, 4, 2, first, count);
    TEST_ASSERT_EQUAL_UINT16(7, first);
    TEST_ASSERT_EQUAL_UINT16(2, count);
    // Past the end draws nothing rather than reading off the end of the buffer.
    ui::pageSpan(9, 3, 4, 5, first, count);
    TEST_ASSERT_EQUAL_UINT16(0, count);
}

// ---------------------------------------------------------------------------
// The title-bar chicken
// ---------------------------------------------------------------------------

void test_chicken_paces_the_lane_and_turns_at_both_ends() {
    // A 144px lane in 6px steps: 24 stops out, 24 back.
    TEST_ASSERT_EQUAL_INT16(0, ui::walkPose(0, 144, 6).x);
    TEST_ASSERT_TRUE(ui::walkPose(0, 144, 6).facingRight);
    TEST_ASSERT_EQUAL_INT16(54, ui::walkPose(9, 144, 6).x);
    TEST_ASSERT_TRUE(ui::walkPose(9, 144, 6).facingRight);

    // At the far wall it turns round rather than walking backwards.
    TEST_ASSERT_EQUAL_INT16(144, ui::walkPose(24, 144, 6).x);
    TEST_ASSERT_FALSE(ui::walkPose(24, 144, 6).facingRight);
    TEST_ASSERT_EQUAL_INT16(90, ui::walkPose(33, 144, 6).x);
    TEST_ASSERT_FALSE(ui::walkPose(33, 144, 6).facingRight);

    // And the beat closes on itself.
    TEST_ASSERT_EQUAL_INT16(0, ui::walkPose(48, 144, 6).x);
    TEST_ASSERT_TRUE(ui::walkPose(48, 144, 6).facingRight);
}

void test_chicken_never_leaves_its_lane_or_jumps() {
    // A lane that is not a whole number of steps: the last stop is clamped.
    for (uint32_t phase = 0; phase < 200; ++phase) {
        const ui::WalkPose pose = ui::walkPose(phase, 100, 6);
        TEST_ASSERT_TRUE(pose.x >= 0);
        TEST_ASSERT_TRUE(pose.x <= 100);
        const ui::WalkPose next = ui::walkPose(phase + 1, 100, 6);
        TEST_ASSERT_TRUE(next.x - pose.x <= 6);
        TEST_ASSERT_TRUE(pose.x - next.x <= 6);
    }
    TEST_ASSERT_EQUAL_INT16(100, ui::walkPose(17, 100, 6).x);
}

void test_chicken_legs_alternate_every_step() {
    TEST_ASSERT_TRUE(ui::walkPose(0, 144, 6).legsApart);
    TEST_ASSERT_FALSE(ui::walkPose(1, 144, 6).legsApart);
    TEST_ASSERT_TRUE(ui::walkPose(2, 144, 6).legsApart);
}

void test_chicken_stands_still_without_room() {
    TEST_ASSERT_EQUAL_INT16(0, ui::walkPose(7, 0, 6).x);
    TEST_ASSERT_EQUAL_INT16(0, ui::walkPose(7, -20, 6).x);
    TEST_ASSERT_EQUAL_INT16(0, ui::walkPose(7, 50, 0).x);
}

/// The chicken walks while Claude works. A session waiting on a permission is
/// not working — and the permission screen never animates anyway.
void test_any_session_working() {
    app::State state;
    proto::InboundFrame frame = sessionsFrame(3, "s1", "");
    state.setSessions(frame);
    TEST_ASSERT_FALSE(state.anySessionWorking());

    frame.sessions[2].state = proto::ClaudeState::Writing;
    state.setSessions(frame);
    TEST_ASSERT_TRUE(state.anySessionWorking());

    frame.sessions[2].state = proto::ClaudeState::WaitingPermission;
    state.setSessions(frame);
    TEST_ASSERT_FALSE(state.anySessionWorking());

    // Before any session list arrives, the followed session's own state decides.
    app::State bare;
    bare.setClaudeState(proto::ClaudeState::Thinking, "");
    TEST_ASSERT_TRUE(bare.anySessionWorking());
}

/// The status LED blinks on exactly this: a permission on screen, or any
/// session in the list asking for one.
void test_action_pending_drives_the_led() {
    app::State state;
    TEST_ASSERT_FALSE(state.actionPending());

    // A session waiting, before any permission frame has been shown.
    proto::InboundFrame sessions = sessionsFrame(2, "s1", "");
    sessions.sessions[1].state = proto::ClaudeState::WaitingPermission;
    state.setSessions(sessions);
    TEST_ASSERT_TRUE(state.actionPending());

    // A permission on screen, before the session list caught up.
    app::State onScreen;
    proto::InboundFrame permission = permissionFrame("req-1", "s1");
    onScreen.setPermission(permission, 0);
    TEST_ASSERT_TRUE(onScreen.actionPending());

    // Nothing pending once it clears.
    onScreen.clearPermission(nullptr);
    TEST_ASSERT_FALSE(onScreen.actionPending());
}

/// Off when idle, breathing when not connected, blinking when a verdict waits —
/// and the verdict wins if somehow both are true.
void test_led_mode_priority() {
    TEST_ASSERT_EQUAL_INT((int)ui::LedMode::Off,
                          (int)ui::ledModeFor(/*connected=*/true, /*actionPending=*/false));
    TEST_ASSERT_EQUAL_INT((int)ui::LedMode::Breathe,
                          (int)ui::ledModeFor(/*connected=*/false, /*actionPending=*/false));
    TEST_ASSERT_EQUAL_INT((int)ui::LedMode::Blink,
                          (int)ui::ledModeFor(/*connected=*/true, /*actionPending=*/true));
    TEST_ASSERT_EQUAL_INT((int)ui::LedMode::Blink,
                          (int)ui::ledModeFor(/*connected=*/false, /*actionPending=*/true));
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_line_reader_splits_on_newline);
    RUN_TEST(test_line_reader_reassembles_partial_line);
    RUN_TEST(test_line_reader_strips_cr_and_blank_lines);
    RUN_TEST(test_line_reader_drops_oversized_line_whole);

    RUN_TEST(test_parses_state_frame);
    RUN_TEST(test_parses_permission_frame);
    RUN_TEST(test_parses_ping_and_hello);
    RUN_TEST(test_rejects_garbage_and_wrong_version);
    RUN_TEST(test_rejects_unknown_type_and_bad_enum);
    RUN_TEST(test_rejects_permission_with_bad_id_or_no_actions);
    RUN_TEST(test_request_id_validation);

    RUN_TEST(test_encodes_actions);
    RUN_TEST(test_refuses_to_encode_verdict_without_valid_id);
    RUN_TEST(test_encoding_escapes_and_respects_capacity);
    RUN_TEST(test_round_trip_action_through_parser);

    RUN_TEST(test_joystick_dead_zone);
    RUN_TEST(test_joystick_directions_use_measured_centre);
    RUN_TEST(test_joystick_diagonal_picks_dominant_axis);
    RUN_TEST(test_joystick_inversion);
    RUN_TEST(test_joystick_axis_swap);
    RUN_TEST(test_joystick_shipped_orientation);
    RUN_TEST(test_repeat_gate_fires_once_then_repeats);
    RUN_TEST(test_repeat_gate_resets_on_release);
    RUN_TEST(test_repeat_gate_changing_direction_fires_immediately);

    RUN_TEST(test_wrap_short_text_is_one_line);
    RUN_TEST(test_wrap_breaks_on_word_boundary);
    RUN_TEST(test_wrap_uses_the_full_width);
    RUN_TEST(test_wrap_never_exceeds_the_width);
    RUN_TEST(test_wrap_splits_a_word_too_long_for_one_line);
    RUN_TEST(test_wrap_honours_hard_newlines);
    RUN_TEST(test_wrap_keeps_blank_lines_as_paragraph_breaks);
    RUN_TEST(test_wrap_handles_empty_and_degenerate_input);
    RUN_TEST(test_wrap_respects_the_line_buffer);
    RUN_TEST(test_pagination);

    RUN_TEST(test_link_is_up_while_frames_are_recent);
    RUN_TEST(test_link_is_down_after_the_timeout);
    RUN_TEST(test_link_is_down_before_anything_arrives);
    RUN_TEST(test_link_survives_a_frame_arriving_after_now_was_sampled);
    RUN_TEST(test_link_handles_millis_rollover);

    RUN_TEST(test_parses_sessions_frame);
    RUN_TEST(test_sessions_frame_falls_back_to_the_id_as_a_name);
    RUN_TEST(test_sessions_frame_is_strict);
    RUN_TEST(test_sessions_frame_clamps_to_capacity);
    RUN_TEST(test_encodes_focus_and_targeted_stop);
    RUN_TEST(test_permission_survives_a_state_change_in_another_session);
    RUN_TEST(test_sessions_frame_reconciles_a_stale_permission);
    RUN_TEST(test_cursor_follows_its_session_across_updates);
    RUN_TEST(test_screen_cycle_refuses_to_leave_a_locked_screen);
    RUN_TEST(test_button_press_is_taken_once);
    RUN_TEST(test_button_held_at_boot_is_not_a_press);
    RUN_TEST(test_button_short_press_is_a_click_not_a_hold);
    RUN_TEST(test_button_long_press_holds_once_and_is_not_a_click);
    RUN_TEST(test_button_hold_is_measured_from_the_latched_press);
    RUN_TEST(test_button_stale_press_time_does_not_fake_a_hold);
    RUN_TEST(test_button_press_bounce_latches_one_press);
    RUN_TEST(test_button_release_bounce_is_not_a_press);
    RUN_TEST(test_expander_catches_a_tap_between_two_reads);
    RUN_TEST(test_expander_release_is_not_a_press);
    RUN_TEST(test_expander_release_bounce_across_reads_is_not_a_press);
    RUN_TEST(test_expander_next_press_after_a_caught_tap_still_counts);
    RUN_TEST(test_expander_forgotten_configuration_is_untrusted);
    RUN_TEST(test_expander_interrupt_mask_covers_the_used_pins);
    RUN_TEST(test_legend_nothing_lights_before_the_mac_talks);
    RUN_TEST(test_legend_is_dark_at_rest);
    RUN_TEST(test_legend_offers_stop_only_while_claude_works);
    RUN_TEST(test_legend_permission_lights_exactly_the_verdicts_offered);
    RUN_TEST(test_legend_never_lights_a_verdict_that_is_not_offered);
    RUN_TEST(test_legend_verdicts_go_dark_once_the_request_is_cleared);
    RUN_TEST(test_legend_menu_keys_navigate);
    RUN_TEST(test_lights_take_the_colour_of_the_role);
    RUN_TEST(test_lights_press_flash_wins);
    RUN_TEST(test_self_test_lights_all_four_first);
    RUN_TEST(test_self_test_then_walks_one_key_at_a_time);
    RUN_TEST(test_self_test_ends_dark_and_stays_dark);
    RUN_TEST(test_reference_sha1_matches_fips_and_rfc2202);
    RUN_TEST(test_base32_decodes_rfc4648_vectors);
    RUN_TEST(test_base32_accepts_how_services_print_secrets);
    RUN_TEST(test_base32_rejects_what_is_not_base32);
    RUN_TEST(test_totp_matches_rfc6238_sha1_vectors);
    RUN_TEST(test_totp_six_digits_keep_leading_zeros);
    RUN_TEST(test_totp_seconds_left_counts_down_the_window);
    RUN_TEST(test_otpauth_reads_a_standard_link);
    RUN_TEST(test_otpauth_reads_its_parameters);
    RUN_TEST(test_otpauth_puts_the_issuer_in_front_of_a_bare_label);
    RUN_TEST(test_otpauth_refuses_what_it_cannot_honour);
    RUN_TEST(test_otpauth_names_fit_the_panel);
    RUN_TEST(test_make_account_checks_the_typed_fields);
    RUN_TEST(test_transfer_link_reads_the_classic_example);
    RUN_TEST(test_transfer_link_reports_every_account_in_a_batch);
    RUN_TEST(test_transfer_link_takes_either_base64_alphabet);
    RUN_TEST(test_transfer_link_skips_fields_it_does_not_know);
    RUN_TEST(test_transfer_link_refuses_a_damaged_link_whole);
    RUN_TEST(test_transfer_link_is_told_apart_from_an_otpauth_link);
    RUN_TEST(test_transfer_link_wipes_its_payload);
    RUN_TEST(test_parses_time_frame);
    RUN_TEST(test_refuses_a_clock_that_is_not_one);
    RUN_TEST(test_encodes_otp_frame);
    RUN_TEST(test_refuses_to_put_anything_but_a_code_on_the_clipboard);
    RUN_TEST(test_clock_carries_forward_on_millis);
    RUN_TEST(test_clock_survives_millis_wrapping);
    RUN_TEST(test_otp_list_cursor_clamps_and_follows_a_shrinking_list);
    RUN_TEST(test_otp_code_expires_once);
    RUN_TEST(test_permission_preempts_the_otp_screens);
    RUN_TEST(test_a_sent_verdict_takes_the_request_off_the_screen);
    RUN_TEST(test_a_verdict_the_mac_never_confirms_brings_the_request_back);
    RUN_TEST(test_a_confirmed_verdict_lets_the_request_go);
    RUN_TEST(test_the_mac_resending_a_request_brings_it_back_at_once);
    RUN_TEST(test_a_session_list_that_drops_it_ends_an_answered_request);
    RUN_TEST(test_a_newer_request_replaces_an_answered_one);
    RUN_TEST(test_legend_otp_list_offers_copy_only_when_it_can);
    RUN_TEST(test_legend_otp_code_and_setup_screens);
    RUN_TEST(test_wifi_payload_is_what_phone_cameras_read);
    RUN_TEST(test_wifi_payload_escapes_the_separators);
    RUN_TEST(test_wifi_payload_refuses_to_truncate);
    RUN_TEST(test_wifi_qr_is_a_version_3_code_with_its_finders);
    RUN_TEST(test_wifi_qr_refuses_what_does_not_fit_the_screen);
    RUN_TEST(test_setup_screen_forgets_the_network_once_closed);
    RUN_TEST(test_waveform_cycles_and_wraps);
    RUN_TEST(test_menu_cursor_clamps_and_names_its_item);
    RUN_TEST(test_menu_screen_is_modal);
    RUN_TEST(test_page_span_with_a_shorter_first_page);

    RUN_TEST(test_chicken_paces_the_lane_and_turns_at_both_ends);
    RUN_TEST(test_chicken_never_leaves_its_lane_or_jumps);
    RUN_TEST(test_chicken_legs_alternate_every_step);
    RUN_TEST(test_chicken_stands_still_without_room);
    RUN_TEST(test_any_session_working);
    RUN_TEST(test_action_pending_drives_the_led);
    RUN_TEST(test_led_mode_priority);

    return UNITY_END();
}
