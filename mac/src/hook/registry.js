import { normalise } from '../accessibility/selectors.js';
import { PermissionMode } from '../protocol/enums.js';
import { summarise } from './summary.js';
import { Decision, encodeHook } from './wire.js';

/**
 * How long an AX-only request waits for a hook to claim it, once hooks have been
 * seen recently. The hook usually arrives first, so this is a safety margin, not
 * a delay anyone should feel.
 */
export const HOOK_GRACE_MS = 600;

/** How long after the last hook request hooks still count as "installed". */
export const HOOK_ACTIVE_FOR_MS = 600_000;

/**
 * After the box answers through a hook, its dialog is still on screen for a
 * moment. The request that dialog was matched to stays hidden for this long, so
 * the box does not flicker the same question back as a fresh one.
 */
export const HOOK_SETTLE_MS = 2_500;

/** Requests waiting at once. Beyond this a hook is told to stand down. */
export const HOOK_MAX_PENDING = 8;

/** What Claude is told when the box refuses. */
export const DENY_MESSAGE = 'Refused from the ChickenMicro box';

/**
 * The requests Claude Code hooks have handed the daemon, and who is waiting for
 * an answer.
 *
 * No socket in here: a hook is a `channel` with `send(line)` and `end()`, so the
 * whole thing is testable with a clock and two functions. `HookServer` is the
 * adapter that gives it real sockets.
 *
 * It decides nothing. A verdict only ever goes out through `answer`, which the
 * coordinator calls after the gate has accepted a press on the box.
 */
export class HookRegistry {
  #entries = new Map();
  #nextId = 1;
  #mode = PermissionMode.auto;
  #lastSeenAt = null;
  #now;
  #max;

  /** Called whenever the set of requests changes, so the coordinator can re-read. */
  onChange = null;

  constructor({ now = () => Date.now(), maxPending = HOOK_MAX_PENDING } = {}) {
    this.#now = now;
    this.#max = maxPending;
  }

  get mode() { return this.#mode; }

  /** Set from the box's `mode` frame. Going to `ui` stands every waiting hook down. */
  setMode(value) {
    if (value === this.#mode) return;
    this.#mode = value;
    if (value === PermissionMode.ui) this.#cancelAll('the box answers through the interface');
    this.#changed();
  }

  /**
   * A hook asking to be answered.
   *
   * @param {{tool: string, session: string, cwd: string, input: object}} request
   * @param {{send: (line: string) => void, end: () => void}} channel
   * @returns {{accepted: true, id: string} | {accepted: false, reason: string}}
   */
  submit(request, channel) {
    if (this.#mode === PermissionMode.ui) {
      return this.#refuse(channel, 'the box answers through the interface');
    }
    if (this.live().length >= this.#max) {
      return this.#refuse(channel, 'too many requests are waiting');
    }

    const now = this.#now();
    const id = `h${this.#nextId}`;
    this.#nextId += 1;
    this.#entries.set(id, {
      id,
      ...summarise(request),
      tool: request.tool,
      session: request.session,
      cwd: request.cwd,
      channel,
      at: now,
      claimedBy: null,
      answered: false,
      answeredAt: null,
    });
    this.#lastSeenAt = now;
    this.#changed();
    return { accepted: true, id };
  }

  /** Waiting for a verdict, oldest first. */
  live() {
    return [...this.#entries.values()].filter((entry) => !entry.answered);
  }

  get(id) { return this.#entries.get(id); }

  isLive(id) {
    const entry = this.#entries.get(id);
    return entry !== undefined && !entry.answered;
  }

  /**
   * Whether hooks are in play: the box is set to use them, and one asked
   * recently. An AX request is given a moment to be claimed only then, so a
   * machine with no plugin installed never waits.
   */
  isActive(now = this.#now()) {
    return this.#mode === PermissionMode.auto
      && this.#lastSeenAt !== null
      && now - this.#lastSeenAt < HOOK_ACTIVE_FOR_MS;
  }

  holdsOff(firstSeenAt, now = this.#now()) {
    return this.isActive(now) && now - firstSeenAt < HOOK_GRACE_MS;
  }

  /**
   * The hook request standing for a dialog the interpreter read, if any.
   *
   * A request already claimed for this dialog keeps its claim — including while
   * it settles after a verdict. Otherwise the oldest unclaimed request whose
   * salient text appears in the dialog takes it.
   *
   * @param {{id: string, title: ?string, content: ?string}} ax
   */
  claimFor(ax, now = this.#now()) {
    for (const entry of this.#entries.values()) {
      if (entry.claimedBy === ax.id && (!entry.answered || this.#settling(entry, now))) return entry;
    }
    for (const entry of this.#entries.values()) {
      if (entry.answered || entry.claimedBy !== null) continue;
      if (appearsIn(entry, ax)) {
        entry.claimedBy = ax.id;
        return entry;
      }
    }
    return null;
  }

  /**
   * Stands down every waiting request whose dialog has left the screen: the
   * operator answered in the app, and a verdict from the box would land on
   * nothing — or worse, on the next dialog.
   */
  cancelGone(presentAxIds) {
    let changed = false;
    for (const entry of this.live()) {
      if (entry.claimedBy === null || presentAxIds.has(entry.claimedBy)) continue;
      this.#stand(entry, 'answered in the app');
      changed = true;
    }
    if (changed) this.#changed();
  }

  /**
   * The box's verdict. Writes it to the hook that is waiting and keeps the entry
   * long enough for its dialog to go.
   *
   * @returns {boolean} false when the request is no longer waiting
   */
  answer(id, decision, now = this.#now()) {
    const entry = this.#entries.get(id);
    if (entry === undefined || entry.answered) return false;
    entry.channel.send(encodeHook.verdict(
      decision,
      decision === Decision.deny ? DENY_MESSAGE : undefined,
    ));
    entry.channel.end();
    entry.answered = true;
    entry.answeredAt = now;
    return true;
  }

  /** The hook went away without an answer: its timeout, or it was killed. */
  channelClosed(id) {
    const entry = this.#entries.get(id);
    if (entry === undefined || entry.answered) return;
    this.#entries.delete(id);
    this.#changed();
  }

  /** Forgets answered requests once their dialogs have had time to go. */
  prune(now = this.#now()) {
    for (const [id, entry] of this.#entries) {
      if (entry.answered && !this.#settling(entry, now)) this.#entries.delete(id);
    }
  }

  /** Stands every waiting hook down. */
  close() {
    this.#cancelAll('the daemon is stopping');
  }

  // --- Internals -------------------------------------------------------------

  #settling(entry, now) {
    return entry.answered && now - entry.answeredAt < HOOK_SETTLE_MS;
  }

  #refuse(channel, reason) {
    channel.send(encodeHook.cancel(reason));
    channel.end();
    return { accepted: false, reason };
  }

  #stand(entry, reason) {
    entry.channel.send(encodeHook.cancel(reason));
    entry.channel.end();
    this.#entries.delete(entry.id);
  }

  #cancelAll(reason) {
    for (const entry of this.live()) this.#stand(entry, reason);
  }

  #changed() {
    this.onChange?.();
  }
}

/**
 * Whether a dialog shows what this request is about.
 *
 * Compared after the same folding the labels get, on the first stretch of each
 * salient string: a long command is cut short by the dialog, and a path or a
 * command is the one thing both sides are sure to carry.
 */
function appearsIn(entry, ax) {
  const haystack = normalise(`${ax.title ?? ''} ${ax.content ?? ''}`);
  if (haystack === null) return false;
  return entry.salient.some((needle) => {
    const folded = normalise(needle);
    return folded !== null && folded.length >= 3 && haystack.includes(folded.slice(0, 60));
  });
}
