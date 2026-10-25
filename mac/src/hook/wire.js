// The Mac-local channel between a Claude Code permission hook and the daemon.
// Documented in protocol/PROTOCOL.md §10. It is not the serial protocol: it never
// leaves the machine, and the box never sees it.
//
// One connection carries one request. The hook opens the socket, writes a single
// `request` line, and waits. The daemon answers with a single `verdict` or
// `cancel` line and closes. A hook that gets anything else, or nothing, or a
// closed socket, exits with no decision and Claude's own dialog stays in charge.

/** Version carried in every line as `v`, separate from the serial protocol's. */
export const HOOK_VERSION = 1;

/** A request line is dropped whole beyond this; the hook trims what it sends. */
export const HOOK_MAX_LINE_BYTES = 65_536;

export const HOOK_TOOL_MAX_CHARS = 128;
export const HOOK_SESSION_MAX_CHARS = 128;
export const HOOK_CWD_MAX_CHARS = 1024;

export const Decision = Object.freeze({ allow: 'allow', deny: 'deny' });

/** What a refused hook line is reported as. */
export const HookRejection = Object.freeze({
  notJson: 'not valid JSON',
  versionMismatch: (v) => `hook protocol version ${v ?? 'absent'} != ${HOOK_VERSION}`,
  unknownType: (t) => `unknown hook frame "${t}"`,
  missingField: (f) => `missing field "${f}"`,
  tooLong: (f) => `${f} too long`,
});

const isString = (value) => typeof value === 'string';

/**
 * Decodes the one line a hook sends. Strict like the serial decoder: anything
 * that does not match is refused with a reason, never coerced.
 *
 * @returns {{request: object} | {rejected: string}}
 */
export function decodeHookRequest(text) {
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
  if (object.type !== 'request') return { rejected: HookRejection.unknownType(object.type) };

  if (!isString(object.tool) || object.tool === '') {
    return { rejected: HookRejection.missingField('tool') };
  }
  if (object.tool.length > HOOK_TOOL_MAX_CHARS) return { rejected: HookRejection.tooLong('tool') };

  const session = isString(object.session) ? object.session : '';
  if (session.length > HOOK_SESSION_MAX_CHARS) return { rejected: HookRejection.tooLong('session') };

  const cwd = isString(object.cwd) ? object.cwd : '';
  if (cwd.length > HOOK_CWD_MAX_CHARS) return { rejected: HookRejection.tooLong('cwd') };

  const input = object.input !== null && typeof object.input === 'object' && !Array.isArray(object.input)
    ? object.input
    : {};
  return { request: { tool: object.tool, session, cwd, input } };
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
