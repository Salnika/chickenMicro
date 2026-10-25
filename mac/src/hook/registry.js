import { normalise } from '../accessibility/selectors.js';
import { SESSIONS_MAX, SESSION_NAME_MAX_CHARS } from '../protocol/constants.js';
import { toDisplayText } from '../protocol/displayText.js';
import { ClaudeState, PermissionMode } from '../protocol/enums.js';
import { clipForWire } from '../protocol/jsonLine.js';
import { sessionNameOf, summarise } from './summary.js';
import { Decision, HookEvent, encodeHook } from './wire.js';

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

/**
 * How long a Claude Code session stays on the box after the last word from it.
 *
 * A session that ends says so (SessionEnd) and goes at once. This is for the one
 * that cannot: a terminal that was killed, a laptop that went to sleep. It is
 * long, because a session left open and idle is still a session, and it comes back
 * with its next prompt anyway.
 */
export const HOOK_SESSION_TTL_MS = 2 * 60 * 60_000;

/** How long a session that has just finished a turn reads *done* before *idle*. */
export const HOOK_DONE_MS = 4_000;

/**
 * A session reads *thinking* from its prompt until its Stop. Interrupting a turn
 * may end it without one, and a box that says *thinking* about a session that
 * stopped long ago is worse than one that says *idle* about a long build — so
 * past this, with nothing heard, it reads idle.
 */
export const HOOK_THINKING_STALE_MS = 15 * 60_000;

/** How many characters of its own id a session wears after a name it shares. */
const TAG_CHARS = 4;

/** What Claude is told when the box refuses. */
export const DENY_MESSAGE = 'Refused from the ChickenMicro box';

/**
 * Where a session runs, from the `CLAUDE_CODE_ENTRYPOINT` its hook reported.
 *
 * - `desktop`: the Code tab of Claude Desktop. Its window is read through
 *   Accessibility, so listing it here too would show one conversation twice.
 * - `headless`: the Agent SDK, `claude -p`, Slack, Desktop's local agents. They come
 *   and go in a moment, and nobody is at a keyboard to answer: not worth a row.
 * - `own`: a terminal or an editor (`cli`, `claude-vscode`), which has no window
 *   anywhere else.
 * - `unknown`: a hook that did not say, such as an older plugin.
 *
 * Claude Code does not document this variable; the values are the ones its own
 * telemetry documents (`cli`, `claude-vscode`, `sdk-*`, `claude-desktop`…), and
 * `claude-desktop` was seen in a Code-tab session. A value nobody expected lands in
 * `own` and is listed.
 */
export function frontEndOf(entrypoint) {
  if (entrypoint === '') return 'unknown';
  if (/desktop/i.test(entrypoint)) return 'desktop';
  if (/^(sdk|local-agent|remote|claude-in-)/i.test(entrypoint)) return 'headless';
  return 'own';
}

/**
 * A name as the box will draw it: folded to what the panel can draw, then cut to
 * what it can hold. Two names are the same to the operator when this is the same,
 * whatever they were as folder names.
 */
function onTheBox(name) {
  return clipForWire(toDisplayText(name), SESSION_NAME_MAX_CHARS, SESSION_NAME_MAX_CHARS * 2);
}

/**
 * A session's name with what tells it from the others that read the same.
 *
 * The box cuts a name at the end to make it fit, and the tag is at the end — so it
 * is the folder that is shortened, never the tag, and by the length it will have
 * once folded. A tag two sessions share (or none, for a session that came without
 * an id) tells nothing: the short id does.
 */
function tagged(record, records) {
  const seen = onTheBox(record.name);
  const clash = records.some(
    (other) => other !== record && onTheBox(other.name) === seen && other.tag === record.tag,
  );
  const suffix = ` (${record.tag !== '' && !clash ? record.tag : record.id})`;
  const name = toDisplayText(record.name);
  const room = SESSION_NAME_MAX_CHARS - suffix.length;
  const base = name.length > room ? `${name.slice(0, room - 3)}...` : name;
  return `${base}${suffix}`;
}

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
  /** Claude Code sessions seen, by their own id (else their folder), in first-seen order. */
  #sessions = new Map();
  #nextSession = 1;
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
    const summary = summarise(request);
    const session = this.#touchSession(request, summary.sessionName, now);
    this.#entries.set(id, {
      id,
      ...summary,
      sessionId: session.id,
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

  /**
   * A hook reporting what happened in a session: it started, was given a prompt,
   * finished a turn, or ended. Nothing is answered, and nothing waits.
   *
   * This is how the box learns of a session that has asked for nothing. A turn
   * reads *thinking* from its prompt to its Stop, and a session that ends is
   * forgotten at once.
   *
   * @param {{name: string, session: string, cwd: string, entrypoint: string}} event
   * @returns {boolean} whether the event was taken
   */
  event({ name, session, cwd, entrypoint }) {
    // Set to the interface, the box does not use hooks at all.
    if (this.#mode === PermissionMode.ui) return false;
    if (frontEndOf(entrypoint) === 'headless') return false;

    const now = this.#now();
    if (name === HookEvent.sessionEnd) {
      const key = session !== '' ? session : cwd;
      const ended = this.#sessions.delete(key);
      if (ended) this.#changed();
      return ended;
    }

    const known = this.#sessions.has(session !== '' ? session : cwd);
    const record = this.#touchSession({ session, cwd, entrypoint }, sessionNameOf(cwd), now);
    record.lifecycle = true;
    if (name === HookEvent.prompt) this.#setState(record, ClaudeState.thinking, now);
    else if (name === HookEvent.stop) this.#setState(record, ClaudeState.done, now);
    // A start in a session already known is a resume or a compaction, in the
    // middle of whatever it was doing.
    else if (!known) this.#setState(record, ClaudeState.idle, now);
    this.#changed();
    return true;
  }

  /**
   * The Claude Code sessions hooks have spoken for lately, as the box lists them.
   *
   * Each has a short id of its own — a Claude session id is a 36-character UUID and
   * the wire allows 16 — a name taken from its folder, and what it is doing as far
   * as hooks can tell. Two sessions whose names would read alike on a 250-pixel
   * panel — the same folder name, or names the panel cuts or folds to the same text
   * — get the first characters of their id after the name, and only those do.
   *
   * @param {number} now
   * @param {{windowsRead?: boolean}} [options] with Claude Desktop's windows read,
   *   a session that Desktop shows itself, or that did not say where it runs, is
   *   left out: it would be listed twice. Headless runs are never listed.
   * @returns {{id: string, name: string, pending: boolean, state: string, frontEnd: string}[]}
   */
  sessions(now = this.#now(), { windowsRead = false } = {}) {
    this.#expireSessions(now);
    const records = [...this.#sessions.values()].filter((record) => {
      const frontEnd = frontEndOf(record.entrypoint);
      if (frontEnd === 'headless') return false;
      return !(windowsRead && (frontEnd === 'desktop' || frontEnd === 'unknown'));
    });
    const names = this.#namesOf(records);
    const asking = new Set(this.live().map((entry) => entry.sessionId));
    return records.map((record) => ({
      id: record.id,
      name: names.get(record.id),
      pending: asking.has(record.id),
      state: this.#stateOf(record, now),
      frontEnd: frontEndOf(record.entrypoint),
    }));
  }

  /** The name the box shows for the session a request came from. */
  nameFor(entry, now = this.#now()) {
    this.#expireSessions(now);
    return this.#namesOf([...this.#sessions.values()]).get(entry.sessionId) ?? entry.sessionName;
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

  /**
   * A session is known by its own id; one that came without is told by its folder.
   * Only a real id gives a tag worth showing: the first characters of a path read
   * the same for every folder.
   */
  #touchSession({ session: sessionId, cwd, entrypoint = '' }, name, now) {
    const key = sessionId !== '' ? sessionId : cwd;
    let record = this.#sessions.get(key);
    if (record === undefined) {
      const id = `c${this.#nextSession}`;
      this.#nextSession += 1;
      record = {
        id,
        name,
        tag: sessionId.replace(/[^A-Za-z0-9]/g, '').slice(0, TAG_CHARS),
        entrypoint: '',
        // Whether lifecycle events have ever been heard from it. Without them the
        // registry knows only that it asks for things, so it cannot say what it is
        // doing between two requests.
        lifecycle: false,
        state: ClaudeState.idle,
        stateAt: now,
        lastAt: now,
      };
      this.#sessions.set(key, record);
      this.#forgetBeyondTheBox(key);
    }
    // The folder is what names it, and a session can move between them.
    record.name = name;
    record.lastAt = now;
    if (entrypoint !== '') record.entrypoint = entrypoint;
    return record;
  }

  #setState(record, state, now) {
    record.state = state;
    record.stateAt = now;
  }

  /** What a session is doing now, as far as the events heard from it say. */
  #stateOf(record, now) {
    if (!record.lifecycle) return ClaudeState.idle;
    if (record.state === ClaudeState.done && now - record.stateAt > HOOK_DONE_MS) {
      return ClaudeState.idle;
    }
    if (record.state === ClaudeState.thinking && now - record.stateAt > HOOK_THINKING_STALE_MS) {
      return ClaudeState.idle;
    }
    return record.state;
  }

  /** The name each of these sessions is shown under, by session id. */
  #namesOf(records) {
    const shared = new Map();
    for (const record of records) {
      const seen = onTheBox(record.name);
      shared.set(seen, (shared.get(seen) ?? 0) + 1);
    }
    return new Map(records.map((record) => [
      record.id,
      shared.get(onTheBox(record.name)) > 1 ? tagged(record, records) : record.name,
    ]));
  }

  /**
   * The box lists a handful of sessions, in the order they were first heard, and
   * cuts the list there. Left alone, sessions that went quiet fill the front of it
   * and the one that is asking now is the one cut. So the quietest session goes
   * first, never one that is asking and never the one just heard.
   */
  #forgetBeyondTheBox(newest) {
    while (this.#sessions.size > SESSIONS_MAX) {
      const asking = new Set(this.live().map((entry) => entry.sessionId));
      let quietest = null;
      for (const [key, record] of this.#sessions) {
        if (key === newest || asking.has(record.id)) continue;
        if (quietest === null || record.lastAt < quietest.record.lastAt) quietest = { key, record };
      }
      if (quietest === null) return;
      this.#sessions.delete(quietest.key);
    }
  }

  #expireSessions(now) {
    const asking = new Set(this.live().map((entry) => entry.sessionId));
    for (const [key, record] of this.#sessions) {
      if (!asking.has(record.id) && now - record.lastAt > HOOK_SESSION_TTL_MS) {
        this.#sessions.delete(key);
      }
    }
  }

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
