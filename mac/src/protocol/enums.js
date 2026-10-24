// Normalised enums shared by both sides. See protocol/PROTOCOL.md §3 and §4.

/** Claude Desktop state. */
export const ClaudeState = Object.freeze({
  disconnected: 'disconnected',
  idle: 'idle',
  thinking: 'thinking',
  writing: 'writing',
  waitingPermission: 'waiting_permission',
  done: 'done',
  error: 'error',
});

export const CLAUDE_STATES = Object.freeze(Object.values(ClaudeState));

export function isClaudeState(value) {
  return CLAUDE_STATES.includes(value);
}

/** Actions the device can originate. Strict enum: nothing else is accepted. */
export const DeviceAction = Object.freeze({
  approve: 'approve',
  deny: 'deny',
  approveAll: 'approve_all',
  stop: 'stop',
  context: 'context',
});

export const DEVICE_ACTIONS = Object.freeze(Object.values(DeviceAction));

/**
 * Actions that authorise or reject a specific request, and therefore must quote
 * the id of the permission on screen.
 *
 * `approve_all` is included: it approves every queued request, but it still
 * quotes the id of the one *on the device's screen* so the arming and staleness
 * guards apply to it exactly as they do to a single approve — the bulk effect
 * is never armed a moment sooner than a single press would be.
 */
export function requiresRequestId(action) {
  return action === DeviceAction.approve
    || action === DeviceAction.deny
    || action === DeviceAction.approveAll;
}

/** Joystick movement. Never destructive. */
export const NAVIGATION_ACTIONS = Object.freeze(['up', 'down', 'left', 'right', 'select']);

export const LOG_LEVELS = Object.freeze(['debug', 'info', 'warn', 'error']);

/** Short uppercase label for the e-ink header. */
export function stateLabel(state) {
  return state === ClaudeState.waitingPermission ? 'PERMISSION' : state.toUpperCase();
}
