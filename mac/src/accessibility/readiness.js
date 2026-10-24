/**
 * Whether a running copy of Claude Desktop can be read, and what to say when it
 * cannot.
 *
 * Two ways a copy goes missing from the box, both seen on a real Mac with two
 * profiles open:
 *
 *  - Its windows answer, but show none of their web content: a dozen nodes and
 *    an empty web area, so no stop control, no permission dialog, and a state
 *    that reads idle whatever Claude is doing. Electron has to be asked for that
 *    content (AXManualAccessibility), and asking once per process was not
 *    enough — a window stayed empty for minutes until it was asked again.
 *
 *  - It answers nothing at all: every Accessibility call returns
 *    kAXErrorAPIDisabled, although this process is trusted. macOS checks each
 *    request with TCC from inside the target app, and that check fails when the
 *    app's own executable no longer exists — which is what an update installed
 *    while the copy was running leaves behind (Jamf moved the old bundle to a
 *    temporary folder and deleted it). No Accessibility client can read such a
 *    copy; quitting it and opening it again can.
 */

/** A real Claude window runs to hundreds of nodes; below this it shows none of its content. */
export const UNEXPOSED_TREE_NODES = 40;

/** Time between two requests to a copy while one of its windows still reads empty. */
export const REEXPOSE_INTERVAL_MS = 5000;

/** Whether a window's tree shows none of its web content. */
export function looksUnexposed(tree) {
  return tree.findAll(() => true).length < UNEXPOSED_TREE_NODES;
}

/**
 * When to ask each Claude process to expose its web content: the first time it
 * is seen, then again only while one of its windows still reads empty, at most
 * once per interval. A window that shows its content costs nothing further.
 */
export class ExposurePolicy {
  #lastAskedMs = new Map();
  #intervalMs;

  constructor({ intervalMs = REEXPOSE_INTERVAL_MS } = {}) {
    this.#intervalMs = intervalMs;
  }

  /**
   * @param {number} pid
   * @param {number} nowMs
   * @param {boolean} unexposed  whether one of its windows read empty last time
   * @returns {'first' | 'again' | null}  why to ask now, or null not to
   */
  shouldAsk(pid, nowMs, unexposed) {
    const last = this.#lastAskedMs.get(pid);
    if (last === undefined) {
      this.#lastAskedMs.set(pid, nowMs);
      return 'first';
    }
    if (!unexposed || nowMs - last < this.#intervalMs) return null;
    this.#lastAskedMs.set(pid, nowMs);
    return 'again';
  }

  /** Forgets processes that have quit, so a reused pid starts afresh. */
  retainOnly(livePids) {
    for (const pid of [...this.#lastAskedMs.keys()]) {
      if (!livePids.has(pid)) this.#lastAskedMs.delete(pid);
    }
  }
}

/**
 * What to tell the operator about a copy that refuses every Accessibility call.
 *
 * @param {{pid: number, profile: string | null, executableGone: boolean}} refusal
 *   `executableGone` when the kernel can no longer resolve the process's
 *   executable — the mark of an update installed while it was running.
 */
export function refusalMessage({ pid, profile, executableGone }) {
  const who = `Claude Desktop (pid ${pid}${profile ? `, profile ${profile}` : ''})`;
  if (executableGone) {
    return `${who} refuses Accessibility: it was updated while running, and macOS no longer `
      + 'finds the executable it needs to check who is asking. Quit that copy and open it again.';
  }
  return `${who} refuses Accessibility (kAXErrorAPIDisabled) although this process is `
    + 'trusted. Quit that copy and open it again.';
}
