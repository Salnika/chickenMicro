import { LogTag } from '../log.js';
import {
  DEFAULT_APPROVE_DELAY_MS,
  MAX_MISSED_PINGS,
  OTP_CLEAR_GRACE_MS,
  PING_INTERVAL_MS,
  SESSIONS_MAX,
  TIME_SYNC_INTERVAL_MS,
} from '../protocol/constants.js';
import { ClaudeState, DeviceAction } from '../protocol/enums.js';
import { decodeInbound, encode, redactForLog } from '../protocol/frames.js';
import { systemClipboard } from '../system/clipboard.js';
import { DEFAULT_TREE_DEPTH } from '../accessibility/axNode.js';
import { Decision } from '../hook/wire.js';
import { PermissionGate } from './permissionGate.js';
import { EMPTY_SNAPSHOT, SessionTracker } from './sessionTracker.js';

export const VERSION = '1.1.0';

/**
 * How long Claude is given to take down a dialog after its button was pressed.
 *
 * Past this the press is pressed once more, and reported if that does nothing
 * either: the button answered AXPress with success, so the only way to tell it
 * did nothing is that the request is still there. Long enough that a busy app is
 * not accused of it.
 */
const PRESS_SETTLE_MS = 1500;

/**
 * Wires Accessibility to the device and back.
 *
 * Single-threaded by contract, which Node gives for free: every entry point
 * runs on the event loop, so there is no window in which a snapshot refresh and
 * an inbound approve interleave. That is what makes the permission gate's
 * reasoning sound.
 */
export class SessionCoordinator {
  #transport;
  #source;
  #tracker;
  #gate;
  #log;
  #now;
  #treeDepth;

  /**
   * Last frames actually mirrored to the device, used to send diffs only —
   * e-ink must not be redrawn for unchanged content.
   */
  #mirrored = null;
  /** Last interpreted view, so a focus change can be answered without a re-read. */
  #view = null;

  #pingSeq = 0;
  #missedPings = 0;
  #pingTimer = null;
  #timeTimer = null;

  /** A verdict pressed and not yet seen to clear, so a press that did nothing is said aloud. */
  #awaitingClear = null;
  /** The press above, once it has been waited on in vain and is due its one retry. */
  #retryDue = null;

  /**
   * Requests handed over by Claude Code hooks, or null when the daemon runs
   * without them. With none, every path below is the one that ran before hooks
   * existed.
   */
  #hooks;
  /** The hook session the operator picked on the box, when no window is read. */
  #hookChoice = null;
  /** When each dialog the interpreter reads was first seen, by request id. */
  #axSeen = new Map();
  #refreshQueued = false;

  #clipboard;
  #timers;
  /** The one-time code last put on the clipboard, until it is wiped. */
  #clipboardCode = null;
  #clipboardTimer = null;

  constructor({
    transport,
    source,
    tracker = new SessionTracker(),
    gate = new PermissionGate(),
    log,
    appName = 'claude-remote',
    appVersion = VERSION,
    treeDepth = DEFAULT_TREE_DEPTH,
    now = () => Date.now(),
    clipboard = systemClipboard,
    timers = { setTimeout, clearTimeout },
    hooks = null,
  }) {
    this.#hooks = hooks;
    if (hooks !== null) hooks.onChange = () => this.#scheduleRefresh();
    this.#clipboard = clipboard;
    this.#timers = timers;
    this.#transport = transport;
    this.#source = source;
    this.#tracker = tracker;
    this.#gate = gate;
    this.#log = log;
    this.appName = appName;
    this.appVersion = appVersion;
    this.#treeDepth = treeDepth;
    this.#now = now;
  }

  // --- Lifecycle -------------------------------------------------------------

  start() {
    this.#transport.onLine = (line) => this.handleInbound(line);
    this.#transport.onConnect = () => this.#handleConnect();
    this.#transport.start();

    this.#pingTimer = setInterval(() => this.#sendPing(), PING_INTERVAL_MS);
    this.#pingTimer.unref?.();
    // The device carries the clock forward on its own crystal; a minute's drift
    // is milliseconds, far inside a 30 s code window.
    this.#timeTimer = setInterval(() => this.#sendTime(), TIME_SYNC_INTERVAL_MS);
    this.#timeTimer.unref?.();
  }

  stop() {
    if (this.#pingTimer !== null) {
      clearInterval(this.#pingTimer);
      this.#pingTimer = null;
    }
    if (this.#timeTimer !== null) {
      clearInterval(this.#timeTimer);
      this.#timeTimer = null;
    }
    // Leaving: take a still-live code off the clipboard now rather than never.
    if (this.#clipboardCode !== null) this.#wipeClipboardCode();
    this.#transport.stop();
  }

  #handleConnect() {
    this.#log.info(LogTag.session, 'device connected, resynchronising');
    this.#missedPings = 0;
    this.#awaitingClear = null;
    this.#retryDue = null;
    this.send(encode.hello(this.appName, this.appVersion, this.#gate.approveDelayMs));
    this.#sendTime();
    // Force a full resend: the device has just booted or reconnected and knows
    // nothing.
    this.#mirrored = null;
    this.refresh();
  }

  // --- Accessibility -> device ----------------------------------------------

  /**
   * Re-reads every Claude window, diffs against what the device already shows,
   * and sends only what changed. Safe to call as often as the watcher fires.
   */
  refresh() {
    this.#mirror(this.#tracker.update(this.#source.readSessions(this.#treeDepth)));
    // After the mirror, never inside it: a retry reads the tree again and can
    // mirror what it finds.
    this.#retryPress();
  }

  /**
   * Surfaces a daemon-side failure on the device. The state is the daemon's
   * own, so it is not tied to any session.
   */
  showError(message) {
    this.send(encode.state(ClaudeState.error, message));
    this.send(encode.error(message));
    this.#mirrored = { ...(this.#mirrored ?? {}), state: ClaudeState.error, detail: message };
  }

  /** Mirrors an interpreted view. */
  #mirror(view) {
    this.#view = view;
    // A window brought to the front takes the box back from a session picked on it.
    if (view.frontChanged) this.#hookChoice = null;
    const previous = this.#mirrored;
    const permission = this.#choosePermission(view);
    // The box asks one question at a time, and everything it shows around that
    // question must describe the session that asked it. Following the window in
    // front instead had the state flip between two conversations whenever the
    // operator changed window — and answering a request left the box on a
    // session they never chose. A session picked on the box still wins.
    const windowFollowed = permission && !view.selected
      ? view.sessions.find((session) => session.id === permission.sessionId) ?? view.focused
      : view.focused;
    // A terminal or VS Code session has no window, so the box learns of it from its
    // hooks. They are listed beside the windows, and described instead of "no
    // session" when there is no window at all.
    const hookSessions = this.#hookSessions(view);
    const hookCurrent = this.#hookCurrent(hookSessions, permission, view, windowFollowed);
    const followed = hookCurrent ? null : windowFollowed;
    const snapshot = followed?.snapshot ?? hookCurrent?.snapshot ?? EMPTY_SNAPSHOT;
    this.#notePressOutcome(permission);

    // Frame order matters on e-ink. The permission screen has priority on the
    // device, so clearing it *before* the new state — and setting it *after* —
    // means each transition costs one redraw instead of two.
    if (previous?.permission
        && previous.permission.request.id !== permission?.request.id) {
      const gone = previous.permission;
      this.#log.info(
        this.#tagFor(gone.sessionId),
        `permission ${gone.request.id} cleared`,
        gone.sessionId ?? undefined,
      );
      this.#gate.clear();
      this.send(encode.permissionCleared(gone.request.id));
    }

    // The session list is level-triggered: it names the permission the daemon
    // considers active, so the device can reconcile even if a clear was lost.
    const listed = [
      ...view.sessions.map((session) => ({
        id: session.id,
        name: session.name,
        state: session.snapshot.state,
      })),
      ...hookSessions.map(({ id, name, state }) => ({ id, name, state })),
    ];
    const sessionsLine = encode.sessions(
      listed,
      { focus: followed?.id ?? hookCurrent?.id ?? null, permission: permission?.request.id ?? null },
    );
    if (sessionsLine !== previous?.sessionsLine) {
      this.#logSessions(view, hookSessions);
      this.send(sessionsLine);
    }

    if (previous?.state !== snapshot.state || previous?.detail !== snapshot.detail) {
      // The state describes the followed session, so the line wears its colour.
      const tag = followed ? this.#log.sessionTag(followed.id) : LogTag.session;
      this.#log.info(tag, `state ${previous?.state ?? '-'} -> ${snapshot.state}`,
                     followed?.id ?? undefined);
      this.send(encode.state(snapshot.state, snapshot.detail));
    }

    // Switching session must not leave the previous one's reply on screen, so
    // an empty message is sent rather than skipped.
    const message = snapshot.lastMessage ?? '';
    if (message !== (previous?.lastMessage ?? '')) {
      this.send(encode.message(message));
    }

    if (permission) {
      const arrived = previous?.permission?.request.id !== permission.request.id;
      // A press clears the gate. When the same request is still on screen
      // afterwards — a press Claude did not act on, or the very same question
      // asked again — nothing used to re-arm it, and every later press was
      // refused as "no permission is pending": the box went dead for that
      // request. Re-arming restarts the delay, so a bouncing key still cannot
      // answer twice, and the re-read before pressing still guards the press.
      if (arrived || this.#gate.pending === null) {
        this.#gate.present(permission.request, this.#now());
      }
      if (arrived) {
        this.#log.info(
          this.#tagFor(permission.sessionId),
          `permission ${permission.request.id} (${permission.request.kind}) `
          + `in ${permission.sessionName}`,
          permission.sessionId ?? undefined,
        );
        // Only a new request is drawn: re-sending it would refresh the e-ink
        // under the operator's finger.
        this.send(encode.permission({ ...permission.request, session: permission.sessionId }));
      }
    }

    this.#mirrored = {
      state: snapshot.state,
      detail: snapshot.detail,
      lastMessage: snapshot.lastMessage,
      permission,
      sessionsLine,
      // What the device is actually following, which a stop with no session
      // named falls back to.
      focusId: followed?.id ?? null,
    };
  }

  /**
   * Puts the request back on the device.
   *
   * The box leaves its permission screen the moment it sends a verdict, so the
   * press feels immediate rather than waiting on a read, a press and a poll.
   * When that verdict could not be applied the request is still the one to
   * answer, and re-sending it brings it back at once instead of after the
   * device's own timeout. A device still showing it is not redrawn: the same id
   * changes nothing there.
   */
  #resendPermission() {
    const permission = this.#mirrored?.permission;
    if (!permission) return;
    this.#log.debug(LogTag.session, `sending ${permission.request.id} again`);
    this.send(encode.permission({ ...permission.request, session: permission.sessionId }));
  }

  /**
   * Notes what became of a pressed verdict, and asks for it to be pressed once
   * more when its dialog is still there.
   *
   * `AXPress` answers success for an element that is no longer wired to
   * anything, and in the wild the first press on a dialog is often dropped while
   * the next one lands. So a verdict whose request is still on screen
   * `PRESS_SETTLE_MS` later is due one retry, see `#retryPress`. If that does
   * nothing either — or Claude asked the very same question again, which the
   * fingerprinted id cannot tell from the first — the operator sees a request
   * they thought they had answered, so the log says so rather than leaving them
   * to guess.
   */
  #notePressOutcome(permission) {
    const awaiting = this.#awaitingClear;
    if (awaiting === null) return;
    if (permission?.request.id !== awaiting.id) {
      this.#awaitingClear = null;
      return;
    }
    if (awaiting.warned || this.#now() - awaiting.at < PRESS_SETTLE_MS) return;
    awaiting.warned = true;
    if (!awaiting.retried) {
      this.#retryDue = awaiting;
      return;
    }
    this.#warnStillShown(awaiting);
  }

  #warnStillShown(awaiting) {
    this.#log.warn(
      LogTag.session,
      `Claude still shows ${awaiting.id} after its ${awaiting.action} was pressed`
      + `${awaiting.retried ? ' twice' : ''}: the press did nothing, or the same question `
      + 'came back. The box can answer it again.',
    );
  }

  /**
   * Presses a verdict a second time, once, when Claude left its dialog standing.
   *
   * The operator pressed once, deliberately; this repeats *that* press and
   * nothing else. It goes through everything a press does — the gate, so the
   * request must still be the one pending and armed, and the fresh read that
   * finds the dialog still showing this exact request id — and it happens at
   * most once per press: a second failure is reported, never retried.
   */
  #retryPress() {
    const due = this.#retryDue;
    if (due === null) return;
    this.#retryDue = null;
    if (this.#awaitingClear !== due) return;

    const verdict = this.#gate.validate(due.action, due.id, this.#now());
    if (!verdict.accepted) {
      this.#log.warn(LogTag.session,
                     `not pressing ${due.action} ${due.id} again: ${verdict.reason}`);
      this.#warnStillShown(due);
      return;
    }
    this.#log.info(
      LogTag.session,
      `${due.id} is still on screen after its ${due.action} was pressed; pressing it once more`,
    );
    this.#performVerdict(due.action, due.id, { retry: true });
  }

  /**
   * The one permission the device shows.
   *
   * The screen holds a single decision at a time, so with several windows
   * asking at once they are answered oldest first — and the one already on
   * screen keeps its place until it is gone. A newer request must never take
   * over the screen under the operator's finger.
   */
  #choosePermission(view) {
    const candidates = this.#candidates(view);
    // "No dialog" is real news, wherever the window sits. The registry keeps a
    // window's element and reads its subtree on any Space, so an empty list is
    // not a blind spot — it means the request is gone and the device must be
    // told at once. Holding it back while the window was off the active Space
    // left the box asking for a decision the user had already made in the app.
    if (candidates.length === 0) return null;

    const current = this.#mirrored?.permission?.request.id;
    const held = candidates.find((candidate) => candidate.request.id === current);
    return held ?? candidates[0];
  }

  /**
   * Every request the box could be asked about, oldest first.
   *
   * Two sources feed it. Dialogs read from Claude's windows are what the box has
   * always shown. Requests handed over by a Claude Code hook are the better
   * source when there is one: the verdict goes straight to the hook, with no
   * button to find and press, and the hook sees requests from a window nobody is
   * looking at.
   *
   * A hook request and the dialog Claude draws for it are one question, so a
   * dialog whose text matches a request is *claimed* by it and not offered twice.
   * A dialog no hook claims is offered as before — at once when hooks are not in
   * play, after a short grace when they are, so a hook that is merely a moment
   * behind is not shown up. That is the fallback: no plugin, a stopped hook or a
   * box set to `ui` all end here.
   */
  #candidates(view) {
    const now = this.#now();
    const pending = SessionTracker.pending(view.sessions);
    const fromDialog = (session) => ({
      request: session.snapshot.permission,
      sessionId: session.id,
      sessionName: session.name,
      bindings: session.bindings,
    });

    const hooks = this.#hooks;
    if (hooks === null) return pending.map(fromDialog);
    // The box lists some hook sessions itself (see #hookSessions), so a request
    // from one of them can name the session it came from.
    const listedHookSessions = new Set(this.#hookSessions(view).map((session) => session.id));

    const seen = new Map();
    for (const session of pending) {
      const id = session.snapshot.permission.id;
      seen.set(id, this.#axSeen.get(id) ?? now);
    }
    this.#axSeen = seen;

    hooks.prune(now);
    const claimedBy = new Map();
    for (const session of pending) {
      const entry = hooks.claimFor(session.snapshot.permission, now);
      if (entry !== null) claimedBy.set(session.snapshot.permission.id, entry);
    }
    // A request whose dialog has gone was answered in the app.
    hooks.cancelGone(new Set(seen.keys()));

    const fromHooks = hooks.live().map((entry) => {
      const owner = pending.find(
        (session) => claimedBy.get(session.snapshot.permission.id) === entry,
      );
      return {
        at: entry.at,
        candidate: {
          request: {
            id: entry.id,
            kind: entry.kind,
            title: entry.title,
            content: entry.content,
            actions: [DeviceAction.deny, DeviceAction.approve],
          },
          sessionId: owner?.id ?? (listedHookSessions.has(entry.sessionId) ? entry.sessionId : null),
          sessionName: owner?.name ?? hooks.nameFor(entry, now),
          bindings: new Map(),
        },
      };
    });
    const fromDialogs = pending
      .filter((session) => {
        const id = session.snapshot.permission.id;
        return !claimedBy.has(id) && !hooks.holdsOff(seen.get(id), now);
      })
      .map((session) => ({
        at: seen.get(session.snapshot.permission.id),
        candidate: fromDialog(session),
      }));

    if (fromHooks.length === 0) return fromDialogs.map(({ candidate }) => candidate);
    return [...fromHooks, ...fromDialogs]
      .sort((left, right) => left.at - right.at)
      .map(({ candidate }) => candidate);
  }

  /** A log tag for a request that may belong to no window the daemon can see. */
  #tagFor(sessionId) {
    return sessionId ? this.#log.sessionTag(sessionId) : LogTag.session;
  }

  /**
   * Re-reads once the current call has finished. A hook arriving or leaving
   * changes what the box should show, but it can do so from inside a mirror.
   */
  #scheduleRefresh() {
    if (this.#refreshQueued) return;
    this.#refreshQueued = true;
    const timer = this.#timers.setTimeout(() => {
      this.#refreshQueued = false;
      this.refresh();
    }, 0);
    timer?.unref?.();
  }

  /**
   * The Claude Code sessions the box lists that are not Claude Desktop windows.
   *
   * A terminal or VS Code session has no window, so its hooks are the only way the
   * box learns it exists. With no window read at all, every session a hook has
   * spoken for is listed. With windows read, only those that said they run in a
   * terminal or an editor: a Code-tab conversation in Claude Desktop is already one
   * of the windows, and nothing connects the two, so it would be listed twice.
   */
  #hookSessions(view) {
    if (this.#hooks === null) return [];
    const windowsRead = view.sessions.length > 0;
    const sessions = this.#hooks.sessions(this.#now(), { windowsRead }).map((session) => ({
      ...session,
      // Waiting on the box beats whatever else it is doing.
      state: session.pending ? ClaudeState.waitingPermission : session.state,
    }));

    // The box holds SESSIONS_MAX rows, windows included, and cuts the list at the
    // end. What it is told about — the session that is asking, the one it describes
    // — has to be in the rows it gets, so the list is cut here, once, and every
    // other use of it sees the same sessions: first the ones waiting on the box,
    // then the one the operator picked, then the most recently heard from.
    const room = Math.max(0, SESSIONS_MAX - view.sessions.length);
    if (sessions.length <= room) return sessions;
    const weight = (session) => (session.pending ? 2 : session.id === this.#hookChoice ? 1 : 0);
    const kept = new Set(
      [...sessions]
        .sort((left, right) => weight(right) - weight(left) || right.lastAt - left.lastAt)
        .slice(0, room)
        .map((session) => session.id),
    );
    return sessions.filter((session) => kept.has(session.id));
  }

  /**
   * The hook session the box describes, or null when it describes a window.
   *
   * Whichever is asking: everything around a question describes the session that
   * asked it. Else the one the operator picked on the box. Else, with no window to
   * describe, the first that is waiting, or the first. A window the operator picked
   * keeps the box, as it always did.
   */
  #hookCurrent(hookSessions, permission, view, windowFollowed) {
    if (hookSessions.length === 0 || view.selected) return null;
    // A window that asks takes the box, whichever session was picked: that is how
    // it has always been, and a request must not be shown over another's state.
    if (permission && view.sessions.some((session) => session.id === permission.sessionId)) {
      return null;
    }
    const current = hookSessions.find((session) => session.id === permission?.sessionId)
      ?? hookSessions.find((session) => session.id === this.#hookChoice)
      ?? (windowFollowed ? null : hookSessions.find((session) => session.pending) ?? hookSessions[0]);
    if (!current) return null;
    return {
      id: current.id,
      snapshot: { state: current.state, detail: null, lastMessage: null, permission: null },
    };
  }

  #logSessions(view, hookSessions = []) {
    if (view.sessions.length === 0 && hookSessions.length === 0) {
      this.#log.info(LogTag.session, 'no session yet: no Claude Desktop window, and no Claude Code hook has spoken');
      return;
    }
    // Each id is painted in its own colour, so the list is a legend for every
    // `[SESSION s2]` line that follows.
    const windows = view.sessions
      .map((session) => `${this.#log.paint(session.id)}:${session.name} ${session.snapshot.state}`);
    const hooks = hookSessions
      .map((session) => `${this.#log.paint(session.id)}:${session.name} ${session.state}`);
    const following = view.focusId ? this.#log.paint(view.focusId) : '-';
    const from = view.sessions.length === 0 ? ' (from Claude Code hooks)' : ` (following ${following})`;
    this.#log.info(LogTag.session, `sessions ${[...windows, ...hooks].join(', ')}${from}`);
  }

  // --- Device -> Accessibility ----------------------------------------------

  handleInbound(line) {
    const result = decodeInbound(line);
    if (result.rejected !== undefined) {
      this.#log.warn(LogTag.serial,
                     `rejected frame (${result.rejected}): ${clip(redactForLog(line))}`);
      return;
    }
    this.#handleFrame(result.frame);
  }

  #handleFrame(frame) {
    switch (frame.type) {
      case 'hello':
        this.#log.info(LogTag.device, `hello from ${frame.app ?? 'device'} ${frame.version ?? ''}`);
        // A device that says hello has just booted: no clock yet.
        this.#sendTime();
        this.#mirrored = null;
        this.refresh();
        break;

      case 'otp':
        this.#copyOtp(frame);
        break;

      case 'action':
        this.#perform(frame.action, frame.requestId, frame.sessionId);
        break;

      case 'focus':
        this.#follow(frame.sessionId);
        break;

      case 'mode':
        // The box's choice of how permissions are answered. Without hooks there
        // is only one way, so the announcement is noted and nothing changes.
        this.#log.info(LogTag.device, `mode ${frame.value}`);
        this.#hooks?.setMode(frame.value);
        break;

      case 'navigation':
        this.#log.debug(LogTag.device, `navigation ${frame.action}`);
        break;

      case 'log':
        this.#log[frame.level](LogTag.device, frame.message);
        break;

      case 'ping':
        this.send(encode.pong(frame.seq));
        break;

      case 'pong':
        this.#missedPings = 0;
        break;

      default:
        break;
    }
  }

  #perform(action, requestId, sessionId) {
    this.#log.info(LogTag.serial, `Received ${action}${requestId ? ` ${requestId}` : ''}`);

    const verdict = this.#gate.validate(action, requestId, this.#now());
    if (!verdict.accepted) {
      this.#log.warn(LogTag.session, `refused ${action}: ${verdict.reason}`);
      this.#resendPermission();
      return;
    }

    switch (action) {
      case DeviceAction.approve:
      case DeviceAction.deny:
        this.#performVerdict(action, requestId);
        break;
      case DeviceAction.approveAll:
        this.#performApproveAll(requestId);
        break;
      case DeviceAction.stop:
        this.#pressStop(sessionId);
        break;
      case DeviceAction.context:
        // No default mapping: Claude exposes no single "contextual" control.
        // The device uses that key locally, for paging and navigation.
        this.#log.debug(LogTag.session, 'context action is device-local; nothing to press');
        break;
      default:
        break;
    }
  }

  /**
   * Re-reads the tree immediately before pressing.
   *
   * The bindings captured at display time could be seconds old, and the only
   * thing that makes an approve safe is that the dialog on screen *right now*
   * is still the one the user saw. So the whole tree is read again, the session
   * still showing this exact request id is located, and its own button is
   * pressed — never another window's.
   */
  #performVerdict(action, requestId, { retry = false } = {}) {
    // A request that came from a hook is answered through the hook. There is no
    // button to find and press, so what stands in for the re-read is that the
    // hook is still waiting: one that gave up, or whose dialog was answered in
    // the app, is refused like any other stale verdict.
    if (this.#hooks?.get(requestId) !== undefined) {
      this.#performHookVerdict(action, requestId);
      return;
    }

    const view = this.#tracker.update(this.#source.readSessions(this.#treeDepth));
    const owner = view.sessions.find(
      (session) => session.snapshot.permission?.id === requestId,
    );

    // The window is readable and still shows this request: press its live
    // button. This is the normal path and the safest — the dialog is confirmed.
    if (owner) {
      const ref = owner.bindings.get(action);
      if (!ref) {
        this.#log.error(this.#log.sessionTag(owner.id),
                        `no ${action} button bound for ${requestId}`, owner.id);
        this.#resendPermission();
        return;
      }
      if (this.#pressFor(action, ref, owner.name, owner.id)) {
        this.#awaitingClear = {
          id: requestId, action, at: this.#now(), warned: false, retried: retry,
        };
      }
      return;
    }

    // The request is not visible. If some *other* request is on screen, the
    // dialog changed under the user — refuse, exactly as the re-read is meant to.
    if (view.sessions.some((session) => session.snapshot.permission)) {
      this.#log.warn(
        LogTag.session,
        `refused ${action} ${requestId}: Claude now shows a different request`,
      );
      this.#gate.clear();
      this.#mirror(view);
      return;
    }

    // Nothing is visible anywhere. The re-read is authoritative — a window is
    // read wherever it sits, full-screen app in front or not — so the dialog has
    // genuinely gone, and this verdict could only land on whatever replaced it.
    this.#log.warn(LogTag.session, `refused ${action} ${requestId}: the request is gone from Claude`);
    this.#gate.clear();
    this.#mirror(view);
  }

  /**
   * Answers a request a hook is waiting on.
   *
   * The same single press and the same gate as a button press: the gate has
   * already matched the id and the arming delay, and the hook must still be
   * waiting. The answer goes only to the connection that asked.
   */
  #performHookVerdict(action, requestId) {
    const decision = action === DeviceAction.approve ? Decision.allow : Decision.deny;
    if (!this.#hooks.answer(requestId, decision, this.#now())) {
      this.#log.warn(LogTag.hook, `refused ${action} ${requestId}: the hook is no longer waiting`);
      this.#gate.clear();
      this.refresh();
      return;
    }
    this.#log.info(LogTag.hook, `${action} ${requestId} — answered through the hook`);
    this.#gate.clear();
    // The request left the waiting list with its answer: the box is told now,
    // not at the next poll.
    this.refresh();
  }

  /**
   * Approves every queued request at once.
   *
   * Anchored on the request shown on the device (`requestId`), which the gate
   * has already armed and matched. Each pending window's own approve button is
   * pressed — the queued ones are authorised sight-unseen, which is the whole
   * point of the gesture, so it is deliberately a separate control on the box.
   * Requests a hook is waiting on are answered through it instead, and the
   * dialog each stands for is not pressed a second time.
   */
  #performApproveAll(requestId) {
    const view = this.#tracker.update(this.#source.readSessions(this.#treeDepth));
    const pending = SessionTracker.pending(view.sessions);
    const waiting = this.#hooks?.live() ?? [];
    const claimed = new Set(waiting.map((entry) => entry.claimedBy).filter((id) => id !== null));

    // If the anchor is gone but a different request is up, the screen changed
    // under the user — refuse the bulk action rather than approve the wrong set.
    const anchorVisible = waiting.some((entry) => entry.id === requestId)
      || pending.some((session) => session.snapshot.permission.id === requestId);
    if (!anchorVisible && (pending.length > 0 || waiting.length > 0)) {
      this.#log.warn(LogTag.session, 'refused approve_all: Claude now shows a different request');
      this.#gate.clear();
      this.#mirror(view);
      return;
    }

    let answered = 0;
    for (const entry of waiting) {
      if (this.#hooks.answer(entry.id, Decision.allow, this.#now())) answered += 1;
    }
    if (answered > 0) {
      this.#log.info(LogTag.hook, `approve_all answered ${answered} hook request(s)`);
    }

    let approved = 0;
    for (const owner of pending) {
      if (claimed.has(owner.snapshot.permission.id)) continue;
      const ref = owner.bindings.get(DeviceAction.approve);
      if (!ref) {
        this.#log.info(this.#log.sessionTag(owner.id),
                       `approve_all: ${owner.name} offers no one-off approve; left for you`, owner.id);
        continue;
      }
      if (this.#pressFor(DeviceAction.approve, ref, owner.name, owner.id, /*clear=*/false)) {
        approved += 1;
      }
    }

    this.#log.info(LogTag.session, `approve_all pressed ${approved} approve button(s)`);
    this.#gate.clear();
    if (answered > 0) this.refresh();
  }

  /**
   * Presses one verdict button and reports it. Clears the gate on success
   * unless `clear` is false (approve_all clears once at the end instead).
   */
  #pressFor(action, ref, name, sessionId, clear = true) {
    try {
      this.#log.info(this.#log.sessionTag(sessionId),
                     `${action} — pressing "${ref.marker}" in ${name}`, sessionId);
      this.#source.pressRef(ref);
      if (clear) this.#gate.clear();
      return true;
    } catch (error) {
      this.#log.error(LogTag.ax, `press failed: ${error.message}`);
      this.send(encode.error('press failed'));
      this.#resendPermission();
      return false;
    }
  }

  /**
   * Stops the turn in the session the device named, falling back to the one it
   * is following.
   */
  #pressStop(sessionId) {
    const sessions = this.#view?.sessions ?? [];
    // A terminal or editor session has no button to press. Falling back to the
    // window being followed would stop an unrelated conversation instead.
    if (sessionId != null && this.#hookSessions(this.#view ?? { sessions }).some((s) => s.id === sessionId)) {
      this.#log.info(this.#log.sessionTag(sessionId),
                     `no stop control for a session that has no window`, sessionId);
      return;
    }
    // Naming no session means the one being followed. Naming one that is gone — a
    // window that closed, a terminal that ended while the key was pressed — means
    // nothing: the followed window is some other conversation.
    const following = this.#mirrored?.focusId ?? this.#view?.focusId;
    const target = sessionId != null
      ? sessions.find((session) => session.id === sessionId)
      : sessions.find((session) => session.id === following);

    if (!target) {
      this.#log.info(LogTag.session, `no session to stop (${sessionId ?? 'none named'})`);
      return;
    }
    const ref = target.bindings.get(DeviceAction.stop);
    if (!ref) {
      this.#log.info(this.#log.sessionTag(target.id),
                     `no stop control in ${target.name} right now`, target.id);
      return;
    }
    try {
      this.#log.info(this.#log.sessionTag(target.id),
                     `stop — pressing "${ref.marker}" in ${target.name}`, target.id);
      this.#source.pressRef(ref);
    } catch (error) {
      this.#log.error(LogTag.ax, `press failed: ${error.message}`);
    }
  }

  /** Follows another session, and reports on it immediately. */
  #follow(sessionId) {
    if (!this.#tracker.focus(sessionId)) {
      if (!this.#followHookSession(sessionId)) {
        this.#log.warn(LogTag.session, `cannot follow ${sessionId}: no such session`);
      }
      return;
    }
    this.#hookChoice = null;
    this.#log.info(this.#log.sessionTag(sessionId), 'now following this session', sessionId);
    if (this.#view === null) {
      this.refresh();
      return;
    }
    // Re-mirroring the view already in hand answers the key press without
    // waiting on another Accessibility read, which can take a few hundred ms.
    this.#mirror({
      ...this.#view,
      focusId: sessionId,
      focused: this.#view.sessions.find((session) => session.id === sessionId) ?? null,
      selected: true,
    });
  }

  /**
   * Follows a session only a hook has spoken for. The box lists these, so a key
   * press on one has to do something.
   *
   * @returns {boolean} whether the id named a session the box is listing
   */
  #followHookSession(sessionId) {
    const view = this.#view ?? { sessions: [] };
    if (!this.#hookSessions(view).some((session) => session.id === sessionId)) return false;
    this.#hookChoice = sessionId;
    // A window picked earlier would otherwise keep the box for itself.
    this.#tracker.release();
    this.#log.info(this.#log.sessionTag(sessionId), 'now following this session', sessionId);
    if (this.#view === null) this.refresh();
    else this.#mirror({ ...this.#view, selected: false, frontChanged: false });
    return true;
  }

  // --- Liveness --------------------------------------------------------------

  /** The Mac's clock, for the device's one-time codes. */
  #sendTime() {
    this.send(encode.time(this.#now()));
  }

  /**
   * Puts a one-time code the device made — on a physical press, the only way
   * it makes one — on the clipboard, and wipes it once its window has closed.
   *
   * There is no way to ask the device for a code from here, by design: the
   * secrets stay on the box, and a code only exists because someone pressed a
   * key. The code itself is never logged.
   */
  #copyOtp({ code, label, expiresInMs }) {
    const who = label ? `"${label}"` : 'an account';
    try {
      this.#clipboard.copy(code);
    } catch (error) {
      this.#log.error(LogTag.device, `could not copy the code for ${who}: ${error.message}`);
      return;
    }
    this.#log.info(
      LogTag.device,
      `one-time code for ${who} copied to the clipboard, ${Math.round(expiresInMs / 1000)} s left`,
    );

    if (this.#clipboardTimer !== null) this.#timers.clearTimeout(this.#clipboardTimer);
    this.#clipboardCode = code;
    this.#clipboardTimer = this.#timers.setTimeout(() => {
      this.#clipboardTimer = null;
      this.#wipeClipboardCode();
    }, expiresInMs + OTP_CLEAR_GRACE_MS);
    this.#clipboardTimer?.unref?.();
  }

  /** Empties the clipboard if it still holds our code; what the user copied since is theirs. */
  #wipeClipboardCode() {
    const code = this.#clipboardCode;
    this.#clipboardCode = null;
    if (this.#clipboardTimer !== null) {
      this.#timers.clearTimeout(this.#clipboardTimer);
      this.#clipboardTimer = null;
    }
    try {
      if (this.#clipboard.read() === code) {
        this.#clipboard.copy('');
        this.#log.info(LogTag.device, 'expired one-time code cleared from the clipboard');
      }
    } catch (error) {
      this.#log.warn(LogTag.device, `could not check the clipboard: ${error.message}`);
    }
  }

  #sendPing() {
    if (!this.#transport.isConnected) return;
    if (this.#missedPings >= MAX_MISSED_PINGS) {
      this.#log.warn(LogTag.serial, `${this.#missedPings} pings unanswered; reopening the port`);
      this.#missedPings = 0;
      this.#transport.reset();
      return;
    }
    this.#pingSeq += 1;
    this.#missedPings += 1;
    this.send(encode.ping(this.#pingSeq));
  }

  // --- Helpers ---------------------------------------------------------------

  send(line) {
    this.#transport.send(line);
  }
}

function clip(line) {
  return line.length > 120 ? `${line.slice(0, 120)}…` : line;
}

export { ClaudeState, DEFAULT_APPROVE_DELAY_MS };
