import { ClaudeInterpreter } from '../accessibility/interpreter.js';
import { normalise, selectors as defaultSelectors } from '../accessibility/selectors.js';
import { ClaudeState } from '../protocol/enums.js';
import { RequestIdAllocator } from './requestIdAllocator.js';

/** What Electron puts between a document title and the app's name. */
const TITLE_SEPARATORS = [' - ', ' — ', ' – ', ' | '];

/**
 * Keeps track of every Claude Desktop window as a session.
 *
 * One window is one session: its own interpreter (so `done` vs `idle` is per
 * conversation), its own request ids, and its own bindings — pressing
 * "Autoriser" must press the button in the window that asked, not the one in
 * whichever window the tree yielded first.
 *
 * Identity comes from the window's Accessibility element, which is stable while
 * the window lives. The short `s1`, `s2` ids handed to the device are allocated
 * from that, so a session keeps its id across polls and across reordering, and
 * a closed window's id is never reused.
 */
export class SessionTracker {
  #entries = new Map();
  #selectors;
  #nextId = 1;
  /** Orders permissions by arrival, so the oldest is answered first. */
  #sequence = 0;
  /** Session the device asked to follow, if it is still open. */
  #selectedId = null;
  /** Last window seen as frontmost-and-main on the Mac. */
  #lastMainKey = null;

  constructor({ selectors = defaultSelectors } = {}) {
    this.#selectors = selectors;
  }

  /**
   * @param {Array | null} rawSessions  from `source.readSessions()`; null when
   *   no Claude Desktop is running.
   * @returns {{sessions: Array, focusId: string | null, focused: object | null}}
   */
  update(rawSessions) {
    if (rawSessions === null || rawSessions === undefined) {
      this.#entries.clear();
      this.#selectedId = null;
      this.#lastMainKey = null;
      return { sessions: [], focusId: null, focused: null };
    }

    const seen = new Set();
    const taken = new Set();
    const sessions = rawSessions.map((raw, index) => {
      seen.add(raw.key);
      const entry = this.#entryFor(raw.key);
      const { snapshot, bindings } = entry.interpreter.interpret(raw.tree);

      // A new request in this session takes the next sequence number; a request
      // still on screen keeps the one it had, so re-reads do not reorder the
      // queue.
      const permissionId = snapshot.permission?.id ?? null;
      if (permissionId !== entry.permissionId) {
        entry.permissionId = permissionId;
        entry.permissionSeq = permissionId === null ? 0 : ++this.#sequence;
      }

      const name = this.#nameFor(raw, index, taken, entry);
      taken.add(name);
      return {
        id: entry.id,
        key: raw.key,
        pid: raw.pid,
        name,
        snapshot,
        bindings,
        permissionSeq: entry.permissionSeq,
        // Carried through from the source: false when the window is only
        // remembered (off the active Space), which the coordinator uses to tell
        // an occluded dialog from a dismissed one.
        onActiveSpace: raw.onActiveSpace !== false,
      };
    });

    for (const key of [...this.#entries.keys()]) {
      if (!seen.has(key)) this.#entries.delete(key);
    }

    const focusId = this.#resolveFocus(rawSessions, sessions);
    return {
      sessions,
      focusId,
      focused: sessions.find((session) => session.id === focusId) ?? null,
      // Whether the focus is the operator's own choice, made on the box, or
      // just the window that happens to be in front on the Mac. A request
      // arriving elsewhere may move the second; it must not move the first.
      selected: this.#selectedId !== null,
    };
  }

  /**
   * Follows `id` from now on. Returns false for a session that is not open, so
   * the caller can log a stale request from the device rather than silently
   * pointing at nothing.
   */
  focus(id) {
    const known = [...this.#entries.values()].some((entry) => entry.id === id);
    if (!known) return false;
    this.#selectedId = id;
    return true;
  }

  /**
   * Forgets the window the device picked, so the box follows the one in front again.
   * For when the device picks something that is not a window.
   */
  release() {
    this.#selectedId = null;
  }

  get focusId() {
    return this.#selectedId;
  }

  /** Sessions holding a permission, oldest first. */
  static pending(sessions) {
    return sessions
      .filter((session) => session.snapshot.permission !== null)
      .sort((left, right) => left.permissionSeq - right.permissionSeq);
  }

  // --- Internals -------------------------------------------------------------

  #entryFor(key) {
    let entry = this.#entries.get(key);
    if (entry !== undefined) return entry;

    const id = `s${this.#nextId}`;
    this.#nextId += 1;
    entry = {
      id,
      /// The last name this window gave of itself, kept so a poll that loses
      /// its web content does not rename the session.
      name: null,
      // Request ids are namespaced by session: two windows showing the same
      // dialog would otherwise both allocate "req-1", and a verdict quoting it
      // could be matched against the wrong one.
      interpreter: new ClaudeInterpreter({
        selectors: this.#selectors,
        allocator: new RequestIdAllocator(`${id}.`),
      }),
      permissionId: null,
      permissionSeq: 0,
    };
    this.#entries.set(key, entry);
    return entry;
  }

  /**
   * What to call a window on a 250x122 panel.
   *
   * Claude Desktop titles every window "Claude", so the title alone is useless;
   * the web area carries the document title, which is the session's own name
   * with the app's appended — "Migration React 19 - Claude Code".
   *
   * The name a window once gave is remembered, because Electron drops the web
   * accessibility tree from time to time: without that, a session renamed
   * itself to its profile mid-conversation and the list on the box shuffled
   * under the operator. Failing everything, the profile tells two running
   * copies apart — which is what `--user-data-dir` is for — then a number.
   */
  #nameFor(raw, index, taken, entry) {
    for (const candidate of [raw.title, ...webAreaTitles(raw.tree)]) {
      const name = this.#distinctive(this.#withoutAppName(candidate));
      if (name === null) continue;
      entry.name = name;
      return unique(name, taken, index);
    }
    if (entry.name !== null) return unique(entry.name, taken, index);
    if (raw.profile) return unique(raw.profile, taken, index);
    return `Session ${index + 1}`;
  }

  /**
   * Drops the app's own name off the end of a document title, so the box shows
   * the session rather than what it is running in.
   */
  #withoutAppName(label) {
    if (typeof label !== 'string') return label;
    for (const separator of TITLE_SEPARATORS) {
      const at = label.lastIndexOf(separator);
      if (at <= 0) continue;
      const tail = normalise(label.slice(at + separator.length));
      if (tail !== null && this.#selectors.genericWindowTitles.includes(tail)) {
        return label.slice(0, at).trim();
      }
    }
    return label;
  }

  /** A label that names the session rather than the application. */
  #distinctive(label) {
    if (typeof label !== 'string') return null;
    const trimmed = label.trim();
    if (trimmed === '') return null;
    const key = normalise(trimmed);
    if (key === null) return null;
    return this.#selectors.genericWindowTitles.includes(key) ? null : trimmed;
  }

  /**
   * Which session the `state` and `message` frames describe.
   *
   * The device's choice wins while that session is open. Otherwise it follows
   * the window in front on the Mac — and a *change* of that window clears the
   * device's choice, so whichever of the two was touched last is the one being
   * reported.
   */
  #resolveFocus(rawSessions, sessions) {
    const mainKey = rawSessions.find((raw) => raw.isMain)?.key ?? null;
    if (mainKey !== this.#lastMainKey) {
      this.#lastMainKey = mainKey;
      if (mainKey !== null) this.#selectedId = null;
    }

    if (this.#selectedId !== null
        && sessions.some((session) => session.id === this.#selectedId)) {
      return this.#selectedId;
    }
    this.#selectedId = null;

    const main = sessions.find((session) => session.key === mainKey);
    return main?.id ?? sessions[0]?.id ?? null;
  }
}

/** The snapshot a session shows when its window is gone. */
export const EMPTY_SNAPSHOT = Object.freeze({
  state: ClaudeState.disconnected,
  detail: null,
  lastMessage: null,
  permission: null,
});

/**
 * Document titles of the window's web areas, in tree order.
 *
 * Electron sets each from `document.title`, and the conversation's follows the
 * session's name — the closest thing to one the tree carries. A window holds
 * several web areas now (the app's own chrome, a side panel, the conversation),
 * and the first is usually the untitled one, so they are all offered and the
 * caller takes the first that says something.
 */
function webAreaTitles(tree) {
  if (!tree) return [];
  return tree.findAll((node) => node.role === 'AXWebArea')
    .map((area) => area.label)
    .filter((label) => typeof label === 'string' && label.trim() !== '');
}

function unique(name, taken, index) {
  return taken.has(name) ? `${name} ${index + 1}` : name;
}
