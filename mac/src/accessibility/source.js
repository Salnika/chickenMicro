import { execFileSync } from 'node:child_process';

import { LogTag } from '../log.js';
import {
  AXError,
  actionNames,
  applicationElement,
  attributeError,
  axErrorName,
  boolAttribute,
  childElements,
  elementHash,
  enableWebAccessibility,
  isElementValid,
  isTrusted,
  press,
  release,
  stringAttribute,
  valueAttribute,
  windowElements,
  withReleasePool,
} from './ax.js';
import { executableOf } from '../system/processInfo.js';
import { AXNode, AXRef, DEFAULT_TREE_DEPTH, MENU_ROLES } from './axNode.js';
import { ExposurePolicy, looksUnexposed, refusalMessage } from './readiness.js';
import { selectors as defaultSelectors } from './selectors.js';
import { WindowRegistry } from './windowRegistry.js';

/** Guards against Electron trees that are deep or self-referential. */
const MAX_CHILDREN_PER_NODE = 256;

/** Windows read per process. More than this on one Claude is not a session list. */
const MAX_WINDOWS_PER_PROCESS = 16;

export class AccessibilityError extends Error {}

/**
 * Finds Claude Desktop without needing any permission of its own.
 *
 * `ps` is used rather than NSWorkspace because there is no AppKit here, and
 * because it costs nothing: Electron names its helper processes
 * "Claude Helper (Renderer)" and friends, so an exact match on the executable
 * name picks the main process.
 */
export function findClaudeProcess(selectors = defaultSelectors) {
  return findClaudeProcesses(selectors)[0] ?? null;
}

/**
 * Every running copy, not just the first.
 *
 * Two Claude Desktop builds side by side (a release and a beta, say) are two
 * separate sets of sessions, and a box on the desk should see both.
 */
export function findClaudeProcesses(selectors = defaultSelectors) {
  let output;
  try {
    output = execFileSync('ps', ['-axo', 'pid=,ppid=,comm='], {
      encoding: 'utf8',
      maxBuffer: 8 << 20,
    });
  } catch {
    return [];
  }

  const candidates = [];
  for (const rawLine of output.split('\n')) {
    const match = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(rawLine);
    if (match === null) continue;
    const [, rawPid, rawParent, command] = match;

    const executable = command.trim().split('/').pop();
    if (!selectors.processNames.includes(executable)) continue;
    // Only the main app bundle executable, never a helper.
    if (!command.includes('.app/Contents/MacOS/')) continue;
    candidates.push({
      pid: Number.parseInt(rawPid, 10),
      parent: Number.parseInt(rawParent, 10),
      path: command.trim(),
    });
  }

  // Electron re-executes its own binary for some child processes, so the same
  // path can show up several times for one running app. The main process is the
  // one whose parent is not itself a Claude: keeping only those leaves one entry
  // per running copy, and still finds two copies running side by side.
  const pids = new Set(candidates.map((candidate) => candidate.pid));
  const main = candidates.filter((candidate) => !pids.has(candidate.parent));
  const profiles = profileLabels(main.map((candidate) => candidate.pid));
  return main.map(({ pid, path }) => ({ pid, path, profile: profiles.get(pid) ?? null }));
}

/** Profile label per pid, cached: it is fixed for the life of the process. */
const profileCache = new Map();

/**
 * The name of each copy's `--user-data-dir`, when it has one.
 *
 * Running several Claude Desktops means running several profiles, and the
 * profile is usually the only thing that tells them apart — every window is
 * titled "Claude". Read with a second `ps` limited to the pids already matched,
 * rather than parsing the full command line of every process on the machine:
 * the executable name is what identifies Claude, and "Claude Helper" would
 * match a looser pattern.
 */
function profileLabels(pids) {
  for (const pid of [...profileCache.keys()]) {
    if (!pids.includes(pid)) profileCache.delete(pid);
  }
  const missing = pids.filter((pid) => !profileCache.has(pid));

  if (missing.length > 0) {
    for (const pid of missing) profileCache.set(pid, null);
    try {
      const output = execFileSync('ps', ['-o', 'pid=,args=', '-p', missing.join(',')], {
        encoding: 'utf8',
        maxBuffer: 1 << 20,
      });
      for (const rawLine of output.split('\n')) {
        const match = /^\s*(\d+)\s+(.*)$/.exec(rawLine);
        if (match === null) continue;
        // The path holds spaces ("Application Support"), so the value runs to
        // the next option or the end of the line.
        const directory = /--user-data-dir=(.*?)(?:\s+--|$)/.exec(match[2]);
        if (directory === null) continue;
        const label = directory[1].replace(/\/+$/, '').split('/').pop();
        if (label) profileCache.set(Number.parseInt(match[1], 10), label);
      }
    } catch {
      // No labels; sessions fall back to being numbered.
    }
  }
  return new Map(pids.map((pid) => [pid, profileCache.get(pid) ?? null]));
}

/** Reads the real Claude Desktop process through AXUIElement. */
export class LiveAccessibilitySource {
  #log;
  #selectors;
  /** Elements retained for later pressing, released on the next read. */
  #retained = [];
  /** When to ask each process to expose its web content. */
  #exposure = new ExposurePolicy();
  /** Processes one of whose windows showed none of its content on the last read. */
  #unexposed = new Set();
  /** Processes refusing every Accessibility call, by pid, as of the last read. */
  #refusals = new Map();
  /**
   * Window elements retained *across* reads, so a window hidden behind a
   * full-screen app on another Space is still reachable. Longer-lived than
   * `#retained`, which is only the pressables of the current read.
   */
  #registry = new WindowRegistry({ release, isAlive: isElementValid });

  constructor({ log, selectors = defaultSelectors } = {}) {
    this.#log = log;
    this.#selectors = selectors;
  }

  get targetPid() {
    return findClaudeProcess(this.#selectors)?.pid ?? null;
  }

  runningClaude() {
    return findClaudeProcess(this.#selectors);
  }

  /** Every running copy. Several profiles side by side is a supported setup. */
  runningProcesses() {
    return findClaudeProcesses(this.#selectors);
  }

  /** Nodes reached at exactly the depth limit on the last walk. */
  #truncatedNodes = 0;

  /** Returns null when Claude Desktop is not running. */
  readTree(maxDepth = DEFAULT_TREE_DEPTH) {
    const target = findClaudeProcess(this.#selectors);
    if (target === null) return null;
    if (!isTrusted()) return null;

    // The previous walk's retained elements are stale now.
    this.#releaseRetained();

    this.#truncatedNodes = 0;
    const root = applicationElement(target.pid);
    try {
      this.#exposeWebContent(root, target.pid);
      return this.#snapshot(root, 'app', 0, maxDepth);
    } finally {
      release(root);
      this.#warnIfTruncated(maxDepth);
    }
  }

  /**
   * One entry per Claude Desktop window, across every running copy.
   *
   * This is what `readTree` cannot give: the app's whole tree merged the
   * windows together, so a stop control in one window and a permission dialog
   * in another looked like one session — and pressing "approve" could have
   * bound to the wrong window's button. Each entry carries its own subtree, and
   * the interpreter runs on it alone.
   *
   * Returns null when no Claude Desktop is running, and an empty array when one
   * is running with no window open.
   */
  readSessions(maxDepth = DEFAULT_TREE_DEPTH) {
    const targets = findClaudeProcesses(this.#selectors);
    if (targets.length === 0) return null;
    if (!isTrusted()) return null;

    // The previous walk's retained elements are stale now.
    this.#releaseRetained();
    this.#truncatedNodes = 0;

    const live = new Set(targets.map((target) => target.pid));
    this.#exposure.retainOnly(live);
    for (const pid of [...this.#unexposed]) {
      if (!live.has(pid)) this.#unexposed.delete(pid);
    }
    for (const pid of [...this.#refusals.keys()]) {
      if (!live.has(pid)) this.#refusals.delete(pid);
    }
    // A copy that quit must not leave its windows retained forever.
    this.#registry.retainOnly(live);

    const sessions = [];
    const seenKeys = new Set();
    for (const target of targets) {
      const application = applicationElement(target.pid);
      let frontmost = false;
      try {
        this.#exposeWebContent(application, target.pid);
        frontmost =
          withReleasePool((pool) => boolAttribute(pool, application, 'AXFrontmost')) === true;

        // `AXWindows` lists only the windows on the Space in front. Handing them
        // to the registry — which keeps the ones it saw on earlier polls — is
        // what makes a window survive a full-screen app on another Space. Each
        // ref handed over now belongs to the registry; it is not released here.
        const visible = withReleasePool(
          (pool) => windowElements(pool, application, MAX_WINDOWS_PER_PROCESS))
          .map((window) => ({ key: `${target.pid}.${elementHash(window).toString(36)}`, element: window }));

        this.#checkRefusal(application, target, visible.length);
        this.#registry.update(target.pid, visible);
      } finally {
        release(application);
      }

      // Snapshot every window the registry now holds for this pid: the ones
      // visible this poll and the ones remembered from a Space not in front.
      let unexposed = false;
      this.#registry.windowsFor(target.pid).forEach(({ key, element, onActiveSpace }, index) => {
        // A hash collision between two live windows would otherwise merge them.
        const uniqueKey = seenKeys.has(key) ? `${key}.${index}` : key;
        seenKeys.add(uniqueKey);

        const meta = withReleasePool((pool) => ({
          title: stringAttribute(pool, element, 'AXTitle'),
          subrole: stringAttribute(pool, element, 'AXSubrole'),
          main: boolAttribute(pool, element, 'AXMain') === true,
        }));

        const tree = this.#snapshot(element, `pid${target.pid}#${index}`, 1, maxDepth);
        if (looksUnexposed(tree)) unexposed = true;
        sessions.push({
          key: uniqueKey,
          pid: target.pid,
          profile: target.profile ?? null,
          title: meta.title,
          subrole: meta.subrole,
          // Only the frontmost app has a main window worth following.
          isMain: frontmost && meta.main,
          // False when the window is only remembered from an earlier poll —
          // behind a full-screen app on another Space, say.
          onActiveSpace,
          tree,
        });
      });
      // Read on the next pass: that one asks this copy again if need be.
      if (unexposed) this.#unexposed.add(target.pid);
      else this.#unexposed.delete(target.pid);
    }

    this.#warnIfTruncated(maxDepth);
    return this.#withoutAuxiliaryWindows(sessions);
  }

  /**
   * Drops windows that are not a conversation — the quick-entry overlay and
   * system panels.
   *
   * If the filter would empty the list it is abandoned: a Claude Desktop
   * release that renames its subroles should cost the session list its tidiness,
   * never its contents.
   */
  /**
   * Asks a Claude process to expose its web content: when first seen, and
   * again while one of its windows still shows none of it.
   *
   * Electron leaves the web accessibility tree off until an assistive client
   * asks for it, per process. VoiceOver flips that switch on its own; this
   * daemon has to flip it itself. Without it a window's tree stops at an empty
   * web area — no stop control, no permission dialog — so the session reads
   * "idle" whatever it is doing. With several copies of Claude running, one
   * window reading correctly while the others stay idle is what that looks like.
   *
   * Once per process is not enough: a copy asked a few seconds after it
   * launched kept an empty web area for minutes, and filled it in as soon as it
   * was asked again. ExposurePolicy spaces the repeats; a window showing its
   * content stops them.
   *
   * `AXEnhancedUserInterface`, Chromium's older switch, is deliberately not
   * used: it is known to disturb window managers and animations in the target
   * app.
   *
   * Chromium builds the tree asynchronously, so the read straight after this
   * can still come back empty; the next poll has it.
   */
  #exposeWebContent(application, pid) {
    const reason = this.#exposure.shouldAsk(pid, Date.now(), this.#unexposed.has(pid));
    if (reason === null) return;
    const code = enableWebAccessibility(application);
    if (code !== AXError.success) {
      this.#log?.debug(LogTag.ax, `AXManualAccessibility on pid ${pid}: ${axErrorName(code)}`);
    } else if (reason === 'first') {
      this.#log?.info(LogTag.ax, `asked Claude Desktop (pid ${pid}) to expose its web content`);
    } else {
      this.#log?.debug(LogTag.ax,
        `asked Claude Desktop (pid ${pid}) again: a window still shows none of its content`);
    }
  }

  /**
   * Notices a copy that refuses every Accessibility call, and says why once.
   *
   * An empty window list is ordinary — a copy with its windows closed — so it
   * only counts as a refusal when the application element itself answers
   * kAXErrorAPIDisabled. This process is trusted by then (readSessions checks),
   * so the refusal comes from the target: see readiness.js for how that
   * happens, and why the fix is to relaunch it.
   */
  #checkRefusal(application, target, windowCount) {
    const refused = windowCount === 0
      && attributeError(application, 'AXRole') === AXError.apiDisabled;
    if (!refused) {
      if (this.#refusals.delete(target.pid)) {
        this.#log?.info(LogTag.ax, `Claude Desktop (pid ${target.pid}) answers Accessibility again`);
      }
      return;
    }
    if (this.#refusals.has(target.pid)) return;
    const refusal = {
      pid: target.pid,
      profile: target.profile ?? null,
      executableGone: executableOf(target.pid) === null,
    };
    this.#refusals.set(target.pid, refusal);
    this.#log?.warn(LogTag.ax, refusalMessage(refusal));
  }

  /** Copies of Claude Desktop refusing every Accessibility call, as of the last read. */
  refusedProcesses() {
    return [...this.#refusals.values()];
  }

  #withoutAuxiliaryWindows(sessions) {
    const ignored = this.#selectors.ignoredWindowSubroles ?? [];
    const kept = sessions.filter((session) => !ignored.includes(session.subrole));
    return kept.length > 0 ? kept : sessions;
  }

  #warnIfTruncated(maxDepth) {
    if (this.#truncatedNodes === 0) return;
    // Anything below the cut is invisible to the interpreter, which is how a
    // dialog can be on screen while the daemon reports nothing.
    this.#log?.warn(
      LogTag.ax,
      `tree hit the depth limit (${this.#truncatedNodes} node(s) at depth `
      + `${maxDepth}); controls below that are invisible — raise --depth`,
    );
  }

  #snapshot(element, path, depth, maxDepth) {
    return withReleasePool((pool) => {
      const role = stringAttribute(pool, element, 'AXRole') ?? 'AXUnknown';
      const title = stringAttribute(pool, element, 'AXTitle');
      const marker = `${path}/${role}${title ? `[${title}]` : ''}`;

      const actions = actionNames(pool, element);

      // Only elements that can actually be actuated need to outlive the walk.
      let ref = new AXRef(marker);
      if (actions.includes('AXPress') && !MENU_ROLES.has(role)) {
        const kept = pool.keep(element);
        if (kept) {
          this.#retained.push(kept);
          ref = new AXRef(marker, kept);
        }
      }

      // The menu bar holds no control this daemon may press, and walking it
      // costs hundreds of cross-process attribute reads on every poll. Keep the
      // node so `dump` still shows it exists, but stop descending here.
      const children = [];
      if (depth >= maxDepth) this.#truncatedNodes += 1;
      if (depth < maxDepth && role !== 'AXMenuBar') {
        const childRefs = childElements(pool, element, MAX_CHILDREN_PER_NODE);
        childRefs.forEach((child, index) => {
          try {
            children.push(this.#snapshot(child, `${marker}#${index}`, depth + 1, maxDepth));
          } finally {
            release(child);
          }
        });
      }

      return new AXNode({
        role,
        subrole: stringAttribute(pool, element, 'AXSubrole'),
        title,
        roleDescription: stringAttribute(pool, element, 'AXDescription'),
        help: stringAttribute(pool, element, 'AXHelp'),
        value: valueAttribute(pool, element),
        identifier: stringAttribute(pool, element, 'AXIdentifier'),
        enabled: boolAttribute(pool, element, 'AXEnabled'),
        actions,
        children,
        ref,
      });
    });
  }

  #releaseRetained() {
    for (const element of this.#retained) release(element);
    this.#retained.length = 0;
  }


  /** Presses an element found in a previous readTree. */
  pressRef(ref) {
    if (!ref?.element) {
      throw new AccessibilityError('the element is no longer in the tree');
    }
    const code = press(ref.element);
    if (code !== AXError.success) {
      throw new AccessibilityError(`AXPress on ${ref.marker} failed (${axErrorName(code)})`);
    }
    this.#log?.info(LogTag.ax, `pressed "${ref.marker}"`);
  }

  dispose() {
    this.#releaseRetained();
    this.#registry.dispose();
  }
}
