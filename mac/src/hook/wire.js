// The Mac-local channel between a Claude Code permission hook and the daemon.
// Documented in protocol/PROTOCOL.md §10. It is not the serial protocol: it never
// leaves the machine, and the box never sees it.
//
// One connection carries one line from the hook. A `request` line asks for a
// verdict: the hook waits, and the daemon answers with a single `verdict` or
// `cancel` line and closes. A hook that gets anything else, or nothing, or a
// closed socket, exits with no decision and Claude's own dialog stays in charge.
// An `event` line only tells the daemon that something happened in a session —
// it started, was given a prompt, finished, ended — and expects no answer.

/** Version carried in every line as `v`, separate from the serial protocol's. */
export const HOOK_VERSION = 1;

/** A request line is dropped whole beyond this; the hook trims what it sends. */
export const HOOK_MAX_LINE_BYTES = 65_536;

export const HOOK_TOOL_MAX_CHARS = 128;
export const HOOK_SESSION_MAX_CHARS = 128;
export const HOOK_CWD_MAX_CHARS = 1024;
export const HOOK_ENTRYPOINT_MAX_CHARS = 64;

/** What happens in a session that a hook reports. */
export const HookEvent = Object.freeze({
  sessionStart: 'SessionStart',
  prompt: 'UserPromptSubmit',
  stop: 'Stop',
  sessionEnd: 'SessionEnd',
});
const HOOK_EVENTS = new Set(Object.values(HookEvent));

export const Decision = Object.freeze({ allow: 'allow', deny: 'deny' });

/** What a refused hook line is reported as. */
export const HookRejection = Object.freeze({
  notJson: 'not valid JSON',
  versionMismatch: (v) => `hook protocol version ${v ?? 'absent'} != ${HOOK_VERSION}`,
  unknownType: (t) => `unknown hook frame "${t}"`,
  unknownEvent: (e) => `unknown hook event "${e}"`,
  missingField: (f) => `missing field "${f}"`,
  tooLong: (f) => `${f} too long`,
});

const isString = (value) => typeof value === 'string';

/** The object a line holds, or the reason it is not one a hook may send. */
function parseLine(text) {
  let object;
  try {
    object = JSON.parse(text);
  } catch {
    return { rejected: HookRejection.notJson };
  }
  if (object === null || typeof object !== 'object' || Array.isArray(object)) {
    return { rejected: HookRejection.notJson };
  }
  if (object.v !== HOOK_VERSION) return { rejected: HookRejection.versionMismatch(object.v) };
  return { object };
}

/** The fields every line may carry about the session it comes from. */
function readOrigin(object) {
  const session = isString(object.session) ? object.session : '';
  if (session.length > HOOK_SESSION_MAX_CHARS) return { rejected: HookRejection.tooLong('session') };

  const cwd = isString(object.cwd) ? object.cwd : '';
  if (cwd.length > HOOK_CWD_MAX_CHARS) return { rejected: HookRejection.tooLong('cwd') };

  const entrypoint = isString(object.entrypoint) ? object.entrypoint : '';
  if (entrypoint.length > HOOK_ENTRYPOINT_MAX_CHARS) {
    return { rejected: HookRejection.tooLong('entrypoint') };
  }
  return { origin: { session, cwd, entrypoint } };
}

function readRequest(object) {
  if (!isString(object.tool) || object.tool === '') {
    return { rejected: HookRejection.missingField('tool') };
  }
  if (object.tool.length > HOOK_TOOL_MAX_CHARS) return { rejected: HookRejection.tooLong('tool') };

  const { origin, rejected } = readOrigin(object);
  if (rejected !== undefined) return { rejected };

  const input = object.input !== null && typeof object.input === 'object' && !Array.isArray(object.input)
    ? object.input
    : {};
  return { request: { tool: object.tool, ...origin, input } };
}

function readEvent(object) {
  if (!HOOK_EVENTS.has(object.event)) return { rejected: HookRejection.unknownEvent(object.event) };

  const { origin, rejected } = readOrigin(object);
  if (rejected !== undefined) return { rejected };
  // With neither, there is no telling which session it is about.
  if (origin.session === '' && origin.cwd === '') {
    return { rejected: HookRejection.missingField('session') };
  }
  return { event: { name: object.event, ...origin } };
}

/**
 * Decodes a `request` line. Strict like the serial decoder: anything that does
 * not match is refused with a reason, never coerced.
 *
 * @returns {{request: object} | {rejected: string}}
 */
export function decodeHookRequest(text) {
  const { object, rejected } = parseLine(text);
  if (rejected !== undefined) return { rejected };
  if (object.type !== 'request') return { rejected: HookRejection.unknownType(object.type) };
  return readRequest(object);
}

/**
 * Decodes whichever line a hook sent: a `request` that wants a verdict, or an
 * `event` that wants nothing.
 *
 * @returns {{request: object} | {event: object} | {rejected: string}}
 */
export function decodeHookLine(text) {
  const { object, rejected } = parseLine(text);
  if (rejected !== undefined) return { rejected };
  if (object.type === 'request') return readRequest(object);
  if (object.type === 'event') return readEvent(object);
  return { rejected: HookRejection.unknownType(object.type) };
}

const line = (frame) => `${JSON.stringify(frame)}\n`;

/** What the daemon writes back. */
export const encodeHook = {
  /** The box's answer. `message` reaches Claude as the reason for a refusal. */
  verdict(decision, message) {
    const frame = { v: HOOK_VERSION, type: 'verdict', decision };
    if (message != null && message !== '') frame.message = message;
    return line(frame);
  },

  /**
   * "Do not wait for me": the request was answered somewhere else, or the box is
   * set to answer through the interface, or there is no room. The hook exits
   * with no decision.
   */
  cancel(reason) {
    return line({ v: HOOK_VERSION, type: 'cancel', reason });
  },
};
