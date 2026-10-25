import {
  DETAIL_MAX_CHARS,
  MAX_LINE_BYTES,
  MESSAGE_MAX_CHARS,
  OTP_CODE_PATTERN,
  OTP_EXPIRES_MAX_MS,
  OTP_LABEL_MAX_CHARS,
  REQUEST_ID_MAX_CHARS,
  SESSIONS_MAX,
  SESSION_NAME_MAX_CHARS,
  VERSION,
  isValidRequestId,
  isValidSessionId,
} from './constants.js';
import { toDisplayText } from './displayText.js';
import {
  DEVICE_ACTIONS,
  LOG_LEVELS,
  NAVIGATION_ACTIONS,
  PERMISSION_MODES,
  isClaudeState,
  requiresRequestId,
} from './enums.js';
import { clipForWire, fit, line } from './jsonLine.js';

// ---------------------------------------------------------------------------
// Mac -> ESP32. See protocol/PROTOCOL.md §1.
// ---------------------------------------------------------------------------

export const encode = {
  hello(app, version, approveDelayMs) {
    return line({ v: VERSION, type: 'hello', role: 'mac', app, version, approveDelayMs });
  },

  state(state, detail) {
    const frame = { v: VERSION, type: 'state', state };
    if (detail != null && detail !== '') {
      frame.detail = fit(frame, 'detail', toDisplayText(detail), DETAIL_MAX_CHARS);
    }
    return line(frame);
  },

  message(text) {
    const frame = { v: VERSION, type: 'message' };
    frame.text = fit(frame, 'text', toDisplayText(text), MESSAGE_MAX_CHARS);
    return line(frame);
  },

  /**
   * Every session the Mac can see, and which one the `state` and `message`
   * frames describe.
   *
   * `permission` carries the id of the request the Mac considers active, so the
   * device can reconcile against it: a `permission_cleared` lost to line noise
   * cannot leave a dead request on screen waiting for a verdict.
   */
  sessions(sessions, { focus = null, permission = null } = {}) {
    const frame = { v: VERSION, type: 'sessions' };
    if (focus != null) frame.focus = focus;
    if (permission != null) frame.permission = permission;
    frame.total = sessions.length;
    frame.sessions = [];

    for (const session of sessions.slice(0, SESSIONS_MAX)) {
      const entry = {
        id: session.id,
        name: clipForWire(toDisplayText(session.name ?? session.id), SESSION_NAME_MAX_CHARS,
                          SESSION_NAME_MAX_CHARS * 2),
        state: session.state,
      };
      // Sessions are appended one at a time and measured: a burst of long names
      // must cost the tail of the list, never a frame the device drops whole.
      const probe = line({ ...frame, sessions: [...frame.sessions, entry] });
      if (Buffer.byteLength(probe, 'utf8') > MAX_LINE_BYTES) break;
      frame.sessions.push(entry);
    }
    return line(frame);
  },

  permission(request) {
    const frame = { v: VERSION, type: 'permission', id: request.id, kind: request.kind };
    if (request.session != null) frame.session = request.session;
    // Reserve room for the actions array, written last, so a long command
    // cannot push it past the line budget.
    const actionsCost = request.actions.reduce((total, a) => total + a.length + 3, 12);
    if (request.title != null && request.title !== '') {
      frame.title = fit(frame, 'title', toDisplayText(request.title), DETAIL_MAX_CHARS, actionsCost);
    }
    if (request.content != null && request.content !== '') {
      frame.content =
        fit(frame, 'content', toDisplayText(request.content), MESSAGE_MAX_CHARS, actionsCost);
    }
    frame.actions = request.actions;
    return line(frame);
  },

  permissionCleared(id) {
    const frame = { v: VERSION, type: 'permission_cleared' };
    if (id != null) frame.id = id;
    return line(frame);
  },

  /** The Mac's clock, which the device's one-time codes are made from. */
  time(unixMs) { return line({ v: VERSION, type: 'time', unixMs: Math.trunc(unixMs) }); },
  ping(seq) { return line({ v: VERSION, type: 'ping', seq }); },
  pong(seq) { return line({ v: VERSION, type: 'pong', seq }); },
  error(message) { return line({ v: VERSION, type: 'error', message: toDisplayText(message) }); },
};

// ---------------------------------------------------------------------------
// ESP32 -> Mac. See protocol/PROTOCOL.md §2.
// ---------------------------------------------------------------------------

/** Why a line coming off the wire was refused. Every rejection is logged. */
export const Rejection = Object.freeze({
  notJson: 'not valid JSON',
  missingType: 'missing "type"',
  unknownType: (t) => `unknown type "${t}"`,
  versionMismatch: (v) => `protocol version ${v ?? 'absent'} != ${VERSION}`,
  missingField: (f) => `missing field "${f}"`,
  invalidEnum: (field, value) => `invalid ${field} "${value}"`,
  invalidRequestId: (id) => `malformed requestId "${id}"`,
  invalidSessionId: (id) => `malformed sessionId "${id}"`,
  // Never echoes the value: it may be a live one-time code.
  invalidOtpCode: 'malformed one-time code',
  outOfRange: (field) => `${field} out of range`,
});

/**
 * A line as it may be written to a log. A one-time code stays valid for the
 * rest of its window, so its digits are masked; nothing else changes. Only
 * `otp` frames carry a "code" field.
 */
export function redactForLog(text) {
  return text.replace(/("code"\s*:\s*")[^"]*(")/g, '$1******$2');
}

/**
 * Strictly an integer: JSON `true`/`false` and fractional numbers are rejected.
 *
 * JS has one number type, so this is where the firmware's CFNumber-vs-CFBoolean
 * distinction lives: a boolean must never pass as the version or a sequence
 * number.
 */
function readInt(value) {
  return typeof value === 'number' && Number.isInteger(value) ? value : undefined;
}

function readString(value) {
  return typeof value === 'string' ? value : undefined;
}

/**
 * Decodes one NDJSON line. Anything not matching the protocol exactly is
 * refused with a reason, never coerced.
 *
 * @returns {{frame: object} | {rejected: string}}
 */
export function decodeInbound(text) {
  let object;
  try {
    object = JSON.parse(text);
  } catch {
    return { rejected: Rejection.notJson };
  }
  if (object === null || typeof object !== 'object' || Array.isArray(object)) {
    return { rejected: Rejection.notJson };
  }

  const version = readInt(object.v);
  if (version !== VERSION) return { rejected: Rejection.versionMismatch(version) };

  const type = readString(object.type);
  if (type === undefined) return { rejected: Rejection.missingType };

  switch (type) {
    case 'hello':
      return { frame: { type, app: readString(object.app), version: readString(object.version) } };

    case 'action': {
      const action = readString(object.action);
      if (action === undefined) return { rejected: Rejection.missingField('action') };
      if (!DEVICE_ACTIONS.includes(action)) {
        return { rejected: Rejection.invalidEnum('action', action) };
      }
      const requestId = readString(object.requestId);
      if (requiresRequestId(action)) {
        if (requestId === undefined) return { rejected: Rejection.missingField('requestId') };
        if (!isValidRequestId(requestId)) {
          return { rejected: Rejection.invalidRequestId(requestId) };
        }
      }
      // Only `stop` and `context` are addressed by session; a verdict is bound
      // by its requestId, which already names exactly one session.
      const sessionId = readString(object.sessionId);
      if (sessionId !== undefined && !isValidSessionId(sessionId)) {
        return { rejected: Rejection.invalidSessionId(sessionId) };
      }
      const frame = { type, action, requestId };
      if (sessionId !== undefined) frame.sessionId = sessionId;
      return { frame };
    }

    case 'focus': {
      const sessionId = readString(object.sessionId);
      if (sessionId === undefined) return { rejected: Rejection.missingField('sessionId') };
      if (!isValidSessionId(sessionId)) return { rejected: Rejection.invalidSessionId(sessionId) };
      return { frame: { type, sessionId } };
    }

    case 'mode': {
      const value = readString(object.value);
      if (value === undefined) return { rejected: Rejection.missingField('value') };
      if (!PERMISSION_MODES.includes(value)) {
        return { rejected: Rejection.invalidEnum('value', value) };
      }
      return { frame: { type, value } };
    }

    case 'navigation': {
      const action = readString(object.action);
      if (action === undefined) return { rejected: Rejection.missingField('action') };
      if (!NAVIGATION_ACTIONS.includes(action)) {
        return { rejected: Rejection.invalidEnum('action', action) };
      }
      return { frame: { type, action } };
    }

    case 'log': {
      const message = readString(object.message);
      if (message === undefined) return { rejected: Rejection.missingField('message') };
      const level = readString(object.level) ?? 'info';
      if (!LOG_LEVELS.includes(level)) {
        return { rejected: Rejection.invalidEnum('level', level) };
      }
      return { frame: { type, level, message } };
    }

    case 'otp': {
      const code = readString(object.code);
      if (code === undefined) return { rejected: Rejection.missingField('code') };
      if (!OTP_CODE_PATTERN.test(code)) return { rejected: Rejection.invalidOtpCode };
      const expiresInMs = readInt(object.expiresInMs);
      if (expiresInMs === undefined) return { rejected: Rejection.missingField('expiresInMs') };
      if (expiresInMs < 0 || expiresInMs > OTP_EXPIRES_MAX_MS) {
        return { rejected: Rejection.outOfRange('expiresInMs') };
      }
      const label = (readString(object.label) ?? '').slice(0, OTP_LABEL_MAX_CHARS);
      return { frame: { type, code, label, expiresInMs } };
    }

    case 'ping':
    case 'pong': {
      const seq = readInt(object.seq);
      if (seq === undefined) return { rejected: Rejection.missingField('seq') };
      return { frame: { type, seq } };
    }

    default:
      return { rejected: Rejection.unknownType(type) };
  }
}

export { isClaudeState, REQUEST_ID_MAX_CHARS };
