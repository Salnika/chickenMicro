/**
 * Keeps every Claude window reachable, on every Space.
 *
 * `AXWindows` only lists the windows on the Space in front. The moment a
 * full-screen app takes over, every Claude window drops out of that list and the
 * box reported "no session" while Claude was busy a swipe away.
 *
 * The window is still there, though: an Accessibility element for a window stays
 * valid — readable and pressable — wherever the window goes, it just stops being
 * *listed*. So this remembers the elements. Each poll:
 *
 *  - every window `AXWindows` shows is recorded under its identity (CFHash),
 *    refreshing the reference held for it;
 *  - a window the poll did not show is kept as long as its element still
 *    answers, and dropped once it has reported itself gone `tolerance` polls
 *    running — which is how a closed window is told apart from one that merely
 *    moved to another Space.
 *
 * The tolerance is what keeps a busy app from renumbering every session: an app
 * that does not answer in time reads exactly like one whose window closed, and
 * dropping a window mints a new id for it, so the list on the box shuffled and
 * the request ids in flight went stale.
 *
 * That covers the case that matters: you work in Claude, the daemon records the
 * windows, then you switch to a full-screen app — the windows are remembered and
 * still read correctly behind it. A window the daemon has *never* seen (it was
 * already on another Space when the daemon started) is picked up the first time
 * it touches the active Space.
 *
 * Pure bookkeeping: liveness and reference-release are handed in, so all of this
 * is tested without any UI.
 */
export class WindowRegistry {
  /** pid -> Map(key -> element), Map preserving first-seen order. */
  #windows = new Map();
  #release;
  #isAlive;

  /**
   * @param {object} options
   * @param {(element) => void} options.release  drops the registry's reference.
   * @param {(element) => boolean} options.isAlive  whether an element whose
   *   window was not listed this poll still exists.
   */
  #tolerance;

  constructor({ release, isAlive, tolerance = 3 }) {
    this.#release = release;
    this.#isAlive = isAlive;
    this.#tolerance = tolerance;
  }

  /**
   * Folds one poll of a process's visible windows in, and returns every window
   * known for it — visible or merely remembered — in first-seen order, so a
   * session's place never depends on which window was clicked last.
   *
   * @param {number} pid
   * @param {Array<{key: string, element: unknown}>} visible  what `AXWindows`
   *   showed. Each element's reference now belongs to the registry.
   * @returns {Array<{key: string, element: unknown}>}
   */
  update(pid, visible) {
    let windows = this.#windows.get(pid);
    if (windows === undefined) {
      windows = new Map();
      this.#windows.set(pid, windows);
    }

    const seen = new Set();
    for (const { key, element } of visible) {
      seen.add(key);
      this.#store(windows, key, element);
    }

    // A remembered window the poll did not show is either off-Space (keep it,
    // marked as not on the active Space) or closed (let it go). Only its element
    // can say which — and an app too busy to answer says "closed", so it takes
    // several polls running to be believed.
    for (const [key, entry] of [...windows]) {
      if (seen.has(key)) continue;
      entry.onActiveSpace = false;
      if (this.#isAlive(entry.element)) {
        entry.missed = 0;
      } else if ((entry.missed = (entry.missed ?? 0) + 1) >= this.#tolerance) {
        this.#drop(windows, key);
      }
    }

    return this.windowsFor(pid);
  }

  /**
   * Every window currently held for `pid`, in first-seen order. `onActiveSpace`
   * is true for a window the last poll actually listed (on the Space in front),
   * false for one only remembered from before — which is how the caller tells a
   * dialog that was dismissed (window present, no dialog) from one merely hidden
   * behind a full-screen app (window off the active Space).
   */
  windowsFor(pid) {
    const windows = this.#windows.get(pid);
    if (windows === undefined) return [];
    return [...windows.entries()].map(([key, entry]) => ({
      key,
      element: entry.element,
      onActiveSpace: entry.onActiveSpace,
    }));
  }

  /** Lets go of everything belonging to processes no longer in `pids`. */
  retainOnly(pids) {
    for (const [pid, windows] of [...this.#windows]) {
      if (pids.has(pid)) continue;
      for (const entry of windows.values()) this.#release(entry.element);
      this.#windows.delete(pid);
    }
  }

  dispose() {
    this.retainOnly(new Set());
  }

  // --- Internals -------------------------------------------------------------

  /**
   * Replaces the reference held for a window.
   *
   * The previous one is released first, even when it is the very same object:
   * each poll hands over a reference of its own, so skipping the release
   * whenever the app returned a cached element would leak one per poll. A Map
   * re-`set` keeps the key's original position, so refreshing a window does not
   * reorder the list.
   */
  #store(windows, key, element) {
    const previous = windows.get(key);
    if (previous !== undefined) {
      if (previous.element !== element) this.#release(previous.element);
      previous.element = element;
      previous.onActiveSpace = true;
      previous.missed = 0;
      return;
    }
    windows.set(key, { element, onActiveSpace: true, missed: 0 });
  }

  #drop(windows, key) {
    this.#release(windows.get(key).element);
    windows.delete(key);
  }
}
