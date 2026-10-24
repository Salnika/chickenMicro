import { AX_PRESS, AXNode, AXRef } from '../accessibility/axNode.js';

/**
 * An AccessibilitySource backed by a tree you supply.
 *
 * Used by `claude-remote simulate` and by the tests, so the full pipeline —
 * interpreter, gate, coordinator, transport, firmware — can be exercised with
 * Claude Desktop closed.
 */
export class FakeAccessibilitySource {
  /** Markers of every element pressRef was called on, in order. */
  pressed = [];
  /** When set, pressRef throws it instead of recording. */
  pressError = null;
  /** Window keys to report as off the active Space (behind a full-screen app). */
  offSpace = new Set();

  constructor(tree = null) {
    this.tree = tree;
  }

  get targetPid() { return this.tree === null ? null : 4242; }

  runningClaude() {
    return this.tree === null ? null : { pid: 4242, path: '/Applications/Claude.app/Contents/MacOS/Claude' };
  }

  readTree() { return this.tree; }

  /** Each AXWindow of the fake tree, shaped like `LiveAccessibilitySource`. */
  readSessions() {
    if (this.tree === null) return null;
    return this.tree.children
      .filter((node) => node.role === 'AXWindow')
      .map((window, index) => {
        const key = window.identifier ?? `4242.${index}`;
        return {
          key,
          pid: 4242,
          profile: null,
          title: window.title,
          subrole: window.subrole,
          // The first window stands in for the one in front on the Mac.
          isMain: index === 0,
          onActiveSpace: !this.offSpace.has(key),
          tree: window,
        };
      });
  }

  pressRef(ref) {
    if (this.pressError !== null) throw this.pressError;
    this.pressed.push(ref.marker);
  }

  dispose() {}
}

/** A pressable button whose ref carries a marker but no live element. */
export function button(label, marker) {
  return new AXNode({ role: 'AXButton', title: label, actions: [AX_PRESS], ref: new AXRef(marker) });
}

/** A scroll area holding alternating speaker / text static text nodes. */
export function transcript(turns) {
  const children = [];
  for (const [speaker, text] of turns) {
    children.push(new AXNode({ role: 'AXStaticText', value: speaker }));
    children.push(new AXNode({ role: 'AXStaticText', value: text }));
  }
  return new AXNode({ role: 'AXScrollArea', children });
}

/**
 * Synthetic Accessibility trees shaped like Claude Desktop's.
 *
 * These mirror the structure shown by `claude-remote dump`: a window, a
 * scroll-area transcript of AXStaticText runs, and — when a tool needs
 * authorising — a dialog group holding the two verdict buttons.
 */
export const fixtures = {
  idle() {
    return app([
      transcript([
        ['User', 'What does this component do?'],
        ['Claude', 'It renders the table header and handles sorting.'],
      ]),
    ]);
  },

  thinking(request = 'Refactor Table.tsx', marks = '') {
    return app([transcript([['User', request]]), button('Stop', `${marks}stop`)]);
  },

  writing(text) {
    return app([
      transcript([['User', 'Please update the tests'], ['Claude', text]]),
      button('Stop', 'stop'),
    ]);
  },

  /**
   * `marks` prefixes the buttons' markers, so a test with two windows asking at
   * once can tell which window's button was actually pressed.
   */
  permission(title = 'Run command', content = 'git push origin master', marks = '') {
    return app([
      transcript([['User', 'Push my branch']]),
      new AXNode({
        role: 'AXGroup',
        subrole: 'AXDialog',
        children: [
          new AXNode({ role: 'AXStaticText', value: title }),
          new AXNode({ role: 'AXStaticText', value: content }),
          button('Deny', `${marks}deny`),
          button('Allow', `${marks}allow`),
        ],
      }),
    ]);
  },

  /** A permission whose payload needs several pages on the device. */
  longPermission() {
    return fixtures.permission(
      'Run command',
      'git push --force-with-lease origin refactor/table-header '
      + '&& gh pr create --base main --title "Refactor table header" '
      + '--body "Splits the sorting logic out of Table.tsx and adds '
      + 'coverage for the empty and single-row cases."',
    );
  },

  /**
   * Several windows in one app, each built from a single-window fixture.
   *
   * `[{ name, tree }]` in, one multi-session tree out — which is what the
   * daemon sees when more than one Claude Desktop window is open.
   */
  workspace(entries) {
    return new AXNode({
      role: 'AXApplication',
      title: 'Claude',
      children: entries.map(({ name, tree }, index) => new AXNode({
        role: 'AXWindow',
        title: name ?? 'Claude',
        identifier: `w${index + 1}`,
        children: tree.children[0].children,
      })),
    });
  },

  longMessage() {
    return fixtures.writing(
      "I'll inspect the component first, then split the sorting logic out of "
      + "Table.tsx into a hook so it can be tested on its own. After that I'll "
      + 'update the snapshot tests, check the empty-state rendering, and run the '
      + 'suite to confirm nothing else depended on the old prop shape.',
    );
  },
};

function app(windowChildren) {
  return new AXNode({
    role: 'AXApplication',
    title: 'Claude',
    children: [new AXNode({ role: 'AXWindow', title: 'Claude', children: windowChildren })],
  });
}
