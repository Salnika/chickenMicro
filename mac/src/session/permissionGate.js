import { DEFAULT_APPROVE_DELAY_MS } from '../protocol/constants.js';
import { requiresRequestId } from '../protocol/enums.js';

/** Reasons a verdict can be refused, phrased for the log. */
export const Refusal = Object.freeze({
  noPendingRequest: 'no permission is pending',
  missingRequestId: 'missing requestId',
  stale: (received, expected) => `stale requestId ${received}, current is ${expected}`,
  notArmed: (remainingMs) => `arming delay not elapsed (${Math.round(remainingMs)} ms left)`,
});

/**
 * Guards the only destructive action in the system.
 *
 * The rules are listed in protocol/PROTOCOL.md §5. They reduce to: an `approve`
 * is only honoured when it quotes the id of the permission *currently* on the
 * device's screen, and only once that screen has been up long enough that the
 * press cannot have been meant for whatever was there before.
 *
 * `now` is passed in on every call, so the whole gate is testable without
 * sleeping.
 */
export class PermissionGate {
  #pending = null;
  #presentedAt = null;

  constructor(approveDelayMs = DEFAULT_APPROVE_DELAY_MS) {
    this.approveDelayMs = approveDelayMs;
  }

  get pending() {
    return this.#pending;
  }

  /**
   * Records that `request` is now the permission being displayed.
   *
   * Re-presenting the same id does not restart the arming delay, so a snapshot
   * refresh cannot keep a request permanently unarmed.
   */
  present(request, now) {
    if (this.#pending?.id === request.id) {
      this.#pending = request;
      return;
    }
    this.#pending = request;
    this.#presentedAt = now;
  }

  /**
   * Drops the pending request. Called when Claude's dialog disappears, when the
   * state leaves `waiting_permission`, and after a successful action.
   */
  clear() {
    this.#pending = null;
    this.#presentedAt = null;
  }

  /**
   * Validates an inbound action. Non-critical actions (`stop`, `context`) need
   * no pending request and are always accepted.
   *
   * @returns {{accepted: true} | {accepted: false, reason: string}}
   */
  validate(action, requestId, now) {
    if (!requiresRequestId(action)) return { accepted: true };

    if (this.#pending === null) {
      return { accepted: false, reason: Refusal.noPendingRequest };
    }
    if (requestId == null) {
      return { accepted: false, reason: Refusal.missingRequestId };
    }
    if (requestId !== this.#pending.id) {
      return { accepted: false, reason: Refusal.stale(requestId, this.#pending.id) };
    }

    const elapsed = now - this.#presentedAt;
    if (elapsed < this.approveDelayMs) {
      return { accepted: false, reason: Refusal.notArmed(this.approveDelayMs - elapsed) };
    }
    return { accepted: true };
  }
}
