// Wire-protocol constants. Mirrored in esp32/include/Protocol.h and documented
// in protocol/PROTOCOL.md — change all three together.

/** Protocol version carried in every frame as `v`. */
export const VERSION = 1;

/**
 * Longest accepted NDJSON line, in bytes. Longer lines are dropped whole, so
 * the encoder must guarantee every frame it produces fits — see `clipForWire`.
 */
export const MAX_LINE_BYTES = 768;

/** Assistant message text is truncated to this many characters. */
export const MESSAGE_MAX_CHARS = 512;

/** Upper bound on a `state` frame's `detail` field. */
export const DETAIL_MAX_CHARS = 96;

/** Upper bound on a permission `id`. */
export const REQUEST_ID_MAX_CHARS = 64;

/**
 * Sessions listed in one `sessions` frame.
 *
 * Mirrors `proto::kMaxSessions`. Beyond this the frame carries `total` so the
 * device can say how many it is not showing.
 */
export const SESSIONS_MAX = 8;

/** Upper bound on a session `id`, mirroring `proto::kMaxSessionIdChars`. */
export const SESSION_ID_MAX_CHARS = 16;

/** Upper bound on a session `name`: what fits a list row on the panel. */
export const SESSION_NAME_MAX_CHARS = 30;

/** A permission is not actionable until this long after it was displayed. */
export const DEFAULT_APPROVE_DELAY_MS = 200;

/** Interval between liveness pings sent by the daemon. */
export const PING_INTERVAL_MS = 3000;

/** Unanswered pings tolerated before the daemon reopens the port. */
export const MAX_MISSED_PINGS = 3;

/** Delay between a transport failure and the next reopen attempt. */
export const RECONNECT_DELAY_MS = 1000;

/** How often the Mac re-sends its clock. The device carries it forward in between. */
export const TIME_SYNC_INTERVAL_MS = 60_000;

/** A one-time code: 6 to 8 digits, and nothing else may reach the clipboard. */
export const OTP_CODE_PATTERN = /^[0-9]{6,8}$/;

/** Account names on the device fit 24 characters. */
export const OTP_LABEL_MAX_CHARS = 24;

/** The device refuses code periods over 300 s, so no window runs longer. */
export const OTP_EXPIRES_MAX_MS = 300_000;

/** Slack after a code's window closes before its copy is wiped from the clipboard. */
export const OTP_CLEAR_GRACE_MS = 2_000;

const REQUEST_ID_PATTERN = /^[A-Za-z0-9._:-]+$/;

export function isValidRequestId(id) {
  return typeof id === 'string'
    && id.length > 0
    && id.length <= REQUEST_ID_MAX_CHARS
    && REQUEST_ID_PATTERN.test(id);
}

/** Same charset as a request id, bounded by the device's shorter buffer. */
export function isValidSessionId(id) {
  return typeof id === 'string'
    && id.length > 0
    && id.length <= SESSION_ID_MAX_CHARS
    && REQUEST_ID_PATTERN.test(id);
}
