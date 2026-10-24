import { DETAIL_MAX_CHARS, MESSAGE_MAX_CHARS } from '../protocol/constants.js';
import { ClaudeState, DeviceAction } from '../protocol/enums.js';
import { RequestIdAllocator } from '../session/requestIdAllocator.js';
import { matchIndex, matches, normalise, selectors as defaultSelectors } from './selectors.js';

/**
 * Turns an Accessibility snapshot into a normalised view of Claude.
 *
 * Deliberately free of any live Accessibility call: it consumes AXNode values,
 * so the whole recognition path is exercised by unit tests that build synthetic
 * trees. `LiveAccessibilitySource` is what produces those values from the real
 * app.
 *
 * Stateful on purpose: it owns request-id stability across re-reads of the same
 * dialog, and needs the previous state to tell `done` from `idle`.
 */
export class ClaudeInterpreter {
  #previousState = ClaudeState.disconnected;
  #allocator;

  constructor({ selectors = defaultSelectors, allocator = new RequestIdAllocator() } = {}) {
    this.selectors = selectors;
    this.#allocator = allocator;
  }

  /**
   * @param {import('./axNode.js').AXNode | null} tree  null when Claude is not running.
   * @returns {{snapshot: object, bindings: Map<string, import('./axNode.js').AXRef>}}
   */
  interpret(tree) {
    if (tree === null || tree === undefined) {
      this.#allocator.clear();
      this.#previousState = ClaudeState.disconnected;
      return {
        snapshot: { state: ClaudeState.disconnected, detail: null, lastMessage: null, permission: null },
        bindings: new Map(),
      };
    }

    const bindings = new Map();
    const pressables = tree.findAll((node) => node.isActuatable);

    // Claude's own "stop generating" control, so the device's STOP button can
    // press the real thing.
    const stop = pressables.find((node) => matches(node.label, this.selectors.stopLabels));
    if (stop?.ref) bindings.set(DeviceAction.stop, stop.ref);

    const permission = this.#findPermission(tree);
    if (permission) {
      if (permission.approveRef) bindings.set(DeviceAction.approve, permission.approveRef);
      if (permission.denyRef) bindings.set(DeviceAction.deny, permission.denyRef);
    }

    const conversation = this.#extractConversation(tree);
    const state = this.#inferState({
      hasPermission: permission !== null,
      isGenerating: bindings.has(DeviceAction.stop),
      hasMessage: conversation.assistant !== null,
    });
    this.#previousState = state;

    return {
      snapshot: {
        state,
        detail: conversation.user,
        lastMessage: conversation.assistant,
        permission: permission?.request ?? null,
      },
      bindings,
    };
  }

  // --- State ---------------------------------------------------------------

  #inferState({ hasPermission, isGenerating, hasMessage }) {
    if (hasPermission) return ClaudeState.waitingPermission;
    if (isGenerating) {
      // Claude shows the stop control for the whole turn. Treat the part that
      // has already produced visible text as `writing`.
      return hasMessage ? ClaudeState.writing : ClaudeState.thinking;
    }
    // A turn that has just ended reads as `done` once, then settles to `idle` —
    // the only distinction the tree itself cannot make.
    switch (this.#previousState) {
      case ClaudeState.thinking:
      case ClaudeState.writing:
      case ClaudeState.waitingPermission:
        return ClaudeState.done;
      default:
        return ClaudeState.idle;
    }
  }

  // --- Permission detection -------------------------------------------------

  /**
   * Finds the deepest container whose subtree holds *both* an approve-ish and a
   * deny-ish pressable. Using the deepest such node means we land on the dialog
   * itself rather than on the window, so the text scraped belongs to the
   * request and not to the whole conversation.
   */
  #findPermission(tree) {
    const container = this.#deepestPermissionContainer(tree);
    if (!container) return null;

    const pressables = container.findAll((node) => node.isActuatable);
    const approve = this.#bestApprove(pressables);
    const deny = pressables.find((node) => matches(node.label, this.selectors.denyLabels));

    const actions = [];
    if (deny) actions.push(DeviceAction.deny);
    if (approve) actions.push(DeviceAction.approve);
    if (actions.length === 0) return null;

    // Dialog text, minus the button labels themselves.
    const buttonLabels = new Set(pressables.map((node) => normalise(node.label)).filter(Boolean));
    const texts = dedupe(
      container
        .findAll((node) => node.role === 'AXStaticText' || node.role === 'AXHeading')
        .map((node) => node.label)
        .filter((label) => {
          const key = normalise(label);
          return key !== null && !buttonLabels.has(key);
        }),
    );

    const title = texts[0] ?? container.label ?? null;
    const content = texts.length > 1 ? texts.slice(1).join('\n') : null;
    const kind = this.#inferKind([title, ...texts].filter(Boolean));

    const id = this.#allocator.idFor(RequestIdAllocator.fingerprint(kind, title, content));

    return {
      request: { id, kind, title, content, actions },
      approveRef: approve?.ref ?? null,
      denyRef: deny?.ref ?? null,
    };
  }

  #deepestPermissionContainer(node) {
    if (!this.#holdsBothVerdicts(node)) return null;
    for (const child of node.children) {
      const deeper = this.#deepestPermissionContainer(child);
      if (deeper) return deeper;
    }
    return node;
  }

  /**
   * The approve button to bind, or undefined.
   *
   * Two rules, both safety rather than cosmetics:
   *
   *  - anything granting a *standing* permission is refused outright, so the
   *    box can never hand over a category of actions permanently;
   *  - among the rest, the label earliest in `approveLabels` wins, so
   *    "Autoriser une fois" is preferred over a broader "Autoriser" regardless
   *    of which one the tree yields first.
   */
  #bestApprove(pressables) {
    let best;
    let bestRank = Number.POSITIVE_INFINITY;
    for (const node of pressables) {
      if (matches(node.label, this.selectors.standingPermissionLabels)) continue;
      const rank = matchIndex(node.label, this.selectors.approveLabels);
      if (rank >= 0 && rank < bestRank) {
        best = node;
        bestRank = rank;
      }
    }
    return best;
  }

  /**
   * Whether this container looks like a verdict dialog.
   *
   * A standing-permission button counts here even though it is never bound: a
   * dialog offering only "Toujours autoriser" and "Refuser" should still reach
   * the device, showing DENY with no APPROVE, rather than going undetected.
   */
  #holdsBothVerdicts(node) {
    let allow = false;
    let deny = false;
    node.walk((child) => {
      if (!child.isActuatable) return;
      if (matches(child.label, this.selectors.approveLabels)
          || matches(child.label, this.selectors.standingPermissionLabels)) {
        allow = true;
      }
      if (matches(child.label, this.selectors.denyLabels)) deny = true;
    });
    return allow && deny;
  }

  #inferKind(texts) {
    // Folded the same way button labels are, so each keyword is written once
    // and without accents — "modifier" catches "Permission demandée : modifier".
    const haystack = normalise(texts.join(' ')) ?? '';
    for (const [keyword, kind] of this.selectors.kindKeywords) {
      if (haystack.includes(keyword)) return kind;
    }
    return this.selectors.defaultKind;
  }

  // --- Conversation extraction ----------------------------------------------

  /**
   * Picks the densest scroll area — the transcript — and splits its static text
   * by speaker markers. When the tree carries no markers, the trailing text is
   * taken as the assistant's latest message, which is the common case for
   * Electron transcripts.
   */
  #extractConversation(tree) {
    const scrollAreas = tree.findAll((node) => node.role === 'AXScrollArea');
    if (scrollAreas.length === 0) {
      // No transcript container, so there is no way to tell conversation from
      // chrome. Falling back to the whole tree used to scrape tooltips, toasts
      // and sidebar entries and present the tail of them as Claude's reply —
      // worse than silence on a device whose job is to tell you what Claude
      // said. Claude Desktop's chat view has a scroll area; the Code tab does
      // not, and is not supported for message extraction.
      return { user: null, assistant: null };
    }
    const transcript = scrollAreas.reduce(
      (best, area) => (staticTextCount(area) > staticTextCount(best) ? area : best),
      scrollAreas[0],
    );

    // Adjacent-only deduplication here, not global: Electron repeats a label
    // across a parent and its child, which lands consecutively in pre-order,
    // but the speaker markers ("User", "Claude") legitimately repeat once per
    // turn. Global dedupe would delete every marker after the first and merge
    // the whole conversation into one segment.
    const texts = dedupeAdjacent(
      transcript.findAll((node) => node.role === 'AXStaticText').map((node) => node.label).filter(Boolean),
    );
    if (texts.length === 0) return { user: null, assistant: null };

    const segments = [];
    for (const text of texts) {
      if (matches(text, this.selectors.assistantMarkers)) {
        segments.push({ speaker: 'assistant', lines: [] });
        continue;
      }
      if (matches(text, this.selectors.userMarkers)) {
        segments.push({ speaker: 'user', lines: [] });
        continue;
      }
      if (normalise(text) === null) continue;
      if (segments.length === 0) segments.push({ speaker: null, lines: [] });
      segments.at(-1).lines.push(text);
    }

    const hasMarkers = segments.some((segment) => segment.speaker !== null);
    if (hasMarkers) {
      const last = (speaker) =>
        segments.filter((s) => s.speaker === speaker && s.lines.length > 0).at(-1);
      const assistant = last('assistant');
      const user = last('user');
      return {
        user: user ? clip(user.lines.join('\n'), DETAIL_MAX_CHARS) : null,
        assistant: assistant ? clip(assistant.lines.join('\n'), MESSAGE_MAX_CHARS) : null,
      };
    }

    // No speaker markers: assume the tail of the transcript is the reply.
    const tail = segments.flatMap((segment) => segment.lines).slice(-6).join('\n');
    return { user: null, assistant: clip(tail, MESSAGE_MAX_CHARS) };
  }
}

function staticTextCount(node) {
  return node ? node.findAll((child) => child.role === 'AXStaticText').length : -1;
}

/** Drops only immediately repeated values, preserving legitimate repeats. */
function dedupeAdjacent(items) {
  const out = [];
  for (const item of items) {
    const trimmed = item.trim();
    if (trimmed === '' || trimmed === out.at(-1)) continue;
    out.push(trimmed);
  }
  return out;
}

/** Drops every repeat, used where a value appearing twice is always noise. */
function dedupe(items) {
  const seen = new Set();
  const out = [];
  for (const item of items) {
    const trimmed = item.trim();
    if (trimmed === '' || seen.has(trimmed)) continue;
    seen.add(trimmed);
    out.push(trimmed);
  }
  return out;
}

function clip(text, limit) {
  return text.length <= limit ? text : `${text.slice(0, limit - 1)}…`;
}
