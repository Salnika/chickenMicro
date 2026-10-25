import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { AX_PRESS, AXNode, AXRef, DEFAULT_TREE_DEPTH } from '../src/accessibility/axNode.js';
import { ClaudeInterpreter } from '../src/accessibility/interpreter.js';
import { AXTreeFormatter } from '../src/accessibility/treeFormatter.js';
import { matches, normalise, selectors } from '../src/accessibility/selectors.js';
import { MESSAGE_MAX_CHARS } from '../src/protocol/constants.js';
import { rank } from '../src/transport/portDiscovery.js';
import { button, fixtures, transcript } from '../src/simulation/fixtures.js';

const interpret = (tree) => new ClaudeInterpreter().interpret(tree);

describe('interpreter — state', () => {
  it('reports disconnected with no tree', () => {
    const { snapshot, bindings } = interpret(null);
    assert.equal(snapshot.state, 'disconnected');
    assert.equal(snapshot.permission, null);
    assert.equal(bindings.size, 0);
  });

  it('reads an idle transcript', () => {
    const { snapshot } = interpret(fixtures.idle());
    assert.equal(snapshot.state, 'idle');
    assert.equal(snapshot.detail, 'What does this component do?');
    assert.equal(snapshot.lastMessage, 'It renders the table header and handles sorting.');
  });

  it('distinguishes thinking from writing', () => {
    // A stop control with no assistant text yet is thinking; once text is
    // visible it is writing.
    assert.equal(interpret(fixtures.thinking()).snapshot.state, 'thinking');
    assert.equal(interpret(fixtures.writing('Working on it')).snapshot.state, 'writing');
  });

  it('binds Claude\'s own stop control', () => {
    assert.equal(interpret(fixtures.thinking()).bindings.get('stop').marker, 'stop');
  });

  it('reports done once after a turn, then idle', () => {
    const interpreter = new ClaudeInterpreter();
    interpreter.interpret(fixtures.thinking());
    assert.equal(interpreter.interpret(fixtures.idle()).snapshot.state, 'done');
    assert.equal(interpreter.interpret(fixtures.idle()).snapshot.state, 'idle');
  });
});

describe('interpreter — permission', () => {
  it('detects a permission and binds both verdicts', () => {
    const { snapshot, bindings } = interpret(fixtures.permission());
    assert.equal(snapshot.state, 'waiting_permission');
    assert.equal(snapshot.permission.kind, 'shell');
    assert.equal(snapshot.permission.title, 'Run command');
    assert.equal(snapshot.permission.content, 'git push origin master');
    assert.deepEqual(snapshot.permission.actions, ['deny', 'approve']);
    assert.equal(bindings.get('approve').marker, 'allow');
    assert.equal(bindings.get('deny').marker, 'deny');
  });

  it('keeps transcript text and button labels out of the dialog content', () => {
    // Otherwise the operator would read the wrong command.
    const { permission } = interpret(fixtures.permission()).snapshot;
    assert.equal(permission.content, 'git push origin master');
    const shown = `${permission.title} ${permission.content}`.toLowerCase();
    assert.ok(!shown.includes('push my branch'));
    assert.ok(!shown.includes('allow'));
    assert.ok(!shown.includes('deny'));
  });

  it('picks the innermost dialog, not the window around it', () => {
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [new AXNode({
          role: 'AXGroup',
          children: [
            new AXNode({ role: 'AXStaticText', value: 'outer noise' }),
            new AXNode({
              role: 'AXGroup',
              subrole: 'AXDialog',
              children: [
                new AXNode({ role: 'AXStaticText', value: 'Write file' }),
                new AXNode({ role: 'AXStaticText', value: 'src/main.ts' }),
                button('Deny', 'd'),
                button('Allow', 'a'),
              ],
            }),
          ],
        })],
      })],
    });
    const { permission } = interpret(tree).snapshot;
    assert.equal(permission.title, 'Write file');
    assert.equal(permission.content, 'src/main.ts');
    assert.equal(permission.kind, 'write');
  });

  it('does not treat a lone Allow as a permission dialog', () => {
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({ role: 'AXWindow', children: [button('Allow', 'a')] })],
    });
    assert.equal(interpret(tree).snapshot.permission, null);
  });

  it('never binds a non-pressable element', () => {
    // Static text saying "Allow" is not a button.
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [
          new AXNode({ role: 'AXStaticText', value: 'Allow' }),
          new AXNode({ role: 'AXStaticText', value: 'Deny' }),
        ],
      })],
    });
    const { snapshot, bindings } = interpret(tree);
    assert.equal(snapshot.permission, null);
    assert.equal(bindings.get('approve'), undefined);
  });

  it('keeps the request id stable across re-reads and fresh when it changes', () => {
    const interpreter = new ClaudeInterpreter();
    const tree = fixtures.permission();
    const first = interpreter.interpret(tree).snapshot.permission.id;
    assert.equal(interpreter.interpret(tree).snapshot.permission.id, first);
    const next = interpreter.interpret(fixtures.permission('Run command', 'rm -rf /'));
    assert.notEqual(next.snapshot.permission.id, first);
  });

  it('infers the kind tag', () => {
    for (const [content, expected] of [
      ['git push origin master', 'shell'],
      ['src/index.ts', 'tool'],
      ['https://example.com/api', 'fetch'],
    ]) {
      const { snapshot } = new ClaudeInterpreter().interpret(fixtures.permission('Proceed?', content));
      assert.equal(snapshot.permission.kind, expected, `content "${content}"`);
    }
  });
});


describe('the real Claude Desktop permission dialog', () => {
  // Reproduced verbatim from tools/capture-permission.sh against Claude Desktop
  // in French. The container is itself an AXButton with the request as its
  // title, and every choice carries a trailing keyboard-shortcut digit.
  const realDialog = () => new AXNode({
    role: 'AXApplication',
    title: 'Claude',
    children: [new AXNode({
      role: 'AXWindow',
      children: [new AXNode({
        role: 'AXWebArea',
        children: [new AXNode({
          role: 'AXButton',
          title: 'Autoriser Claude à modifier selectors.js ?',
          actions: [AX_PRESS],
          ref: new AXRef('dialog'),
          children: [
            new AXNode({
              role: 'AXStaticText',
              value: 'Permission demandée : modifier. '
                + '/Users/you/dev/chickenMicro/mac/src/accessibility/selectors.js',
            }),
            new AXNode({
              role: 'AXButton', title: 'Refuser 1',
              actions: [AX_PRESS], ref: new AXRef('refuser'),
            }),
            new AXNode({
              role: 'AXButton', title: 'Toujours autoriser 2',
              actions: [AX_PRESS], ref: new AXRef('toujours'),
            }),
            new AXNode({
              role: 'AXButton', title: 'Autoriser une fois 3',
              actions: [AX_PRESS], ref: new AXRef('une-fois'),
            }),
          ],
        })],
      })],
    })],
  });

  it('detects it', () => {
    const { snapshot } = interpret(realDialog());
    assert.equal(snapshot.state, 'waiting_permission');
    assert.notEqual(snapshot.permission, null);
  });

  it('binds APPROVE to "allow once", never to "always allow"', () => {
    // The whole point: a desk button must not grant a standing permission.
    const { bindings } = interpret(realDialog());
    assert.equal(bindings.get('approve').marker, 'une-fois');
    assert.equal(bindings.get('deny').marker, 'refuser');
    const bound = [...bindings.values()].map((ref) => ref.marker);
    assert.ok(!bound.includes('toujours'), '"Toujours autoriser" must never be bound');
  });

  it('shows the request on the device', () => {
    const { permission } = interpret(realDialog()).snapshot;
    assert.match(permission.title, /Permission demandée : modifier/);
    assert.match(permission.title + (permission.content ?? ''), /selectors\.js/);
    assert.deepEqual(permission.actions, ['deny', 'approve']);
  });

  it('still offers DENY when only a standing grant is available', () => {
    // Detection must not collapse just because "allow once" is missing.
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [new AXNode({
          role: 'AXGroup',
          children: [
            new AXNode({ role: 'AXStaticText', value: 'Permission demandée : modifier.' }),
            new AXNode({
              role: 'AXButton', title: 'Refuser 1',
              actions: [AX_PRESS], ref: new AXRef('refuser'),
            }),
            new AXNode({
              role: 'AXButton', title: 'Toujours autoriser 2',
              actions: [AX_PRESS], ref: new AXRef('toujours'),
            }),
          ],
        })],
      })],
    });
    const { snapshot, bindings } = interpret(tree);
    assert.notEqual(snapshot.permission, null, 'the dialog must still be detected');
    assert.deepEqual(snapshot.permission.actions, ['deny']);
    assert.equal(bindings.get('deny').marker, 'refuser');
    assert.equal(bindings.get('approve'), undefined);
  });

  it('detects the dialog whose only allow button is the session-wide grant', () => {
    // Captured from a later Claude Desktop release: "Refuser" beside a single
    // "Toujours autoriser" whose title carries its own description, and an
    // options menu. No "Autoriser une fois" button exists in the tree.
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [new AXNode({
          role: 'AXGroup',
          children: [
            new AXNode({ role: 'AXStaticText', value: 'Modifier test-permission.txt' }),
            new AXNode({
              role: 'AXButton', title: 'Refuser',
              actions: [AX_PRESS], ref: new AXRef('refuser'),
            }),
            new AXNode({
              role: 'AXButton',
              title: 'Toujours autoriser , S’applique uniquement à cette session.',
              actions: [AX_PRESS], ref: new AXRef('toujours'),
              children: [
                new AXNode({ role: 'AXStaticText', value: 'Toujours autoriser' }),
                new AXNode({ role: 'AXStaticText', value: 'S’applique uniquement à cette session.' }),
              ],
            }),
            new AXNode({
              role: 'AXPopUpButton', description: 'Plus d’options d’autorisation',
              actions: [AX_PRESS], ref: new AXRef('options'),
            }),
          ],
        })],
      })],
    });
    const { snapshot, bindings } = interpret(tree);
    assert.equal(snapshot.state, 'waiting_permission');
    assert.deepEqual(snapshot.permission.actions, ['deny']);
    assert.equal(bindings.get('deny').marker, 'refuser');
    assert.equal(bindings.get('approve'), undefined, 'the session-wide grant must never be bound');
  });

  it('refuses a standing grant even if it is listed as an approve label', () => {
    // The guard is defence in depth: today "Toujours autoriser" is simply
    // absent from approveLabels, so nothing binds it. This proves the
    // exclusion also holds if someone later adds it there by mistake — which
    // is exactly how that would happen.
    const careless = {
      ...selectors,
      approveLabels: ['toujours autoriser', ...selectors.approveLabels],
    };
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXGroup',
        children: [
          new AXNode({ role: 'AXButton', title: 'Refuser 1', actions: [AX_PRESS], ref: new AXRef('d') }),
          new AXNode({
            role: 'AXButton', title: 'Toujours autoriser 2',
            actions: [AX_PRESS], ref: new AXRef('toujours'),
          }),
          new AXNode({
            role: 'AXButton', title: 'Autoriser une fois 3',
            actions: [AX_PRESS], ref: new AXRef('une-fois'),
          }),
        ],
      })],
    });
    const { bindings } = new ClaudeInterpreter({ selectors: careless }).interpret(tree);
    assert.equal(bindings.get('approve').marker, 'une-fois');
  });

  it('prefers the most restrictive approve regardless of tree order', () => {
    const ordered = (first, second) => new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXGroup',
        children: [
          new AXNode({ role: 'AXButton', title: 'Refuser 1', actions: [AX_PRESS], ref: new AXRef('d') }),
          new AXNode({ role: 'AXButton', title: first, actions: [AX_PRESS], ref: new AXRef(first) }),
          new AXNode({ role: 'AXButton', title: second, actions: [AX_PRESS], ref: new AXRef(second) }),
        ],
      })],
    });
    // "Autoriser" alone is broader than "Autoriser une fois"; the narrow one
    // must win either way round.
    assert.equal(
      interpret(ordered('Autoriser', 'Autoriser une fois 3')).bindings.get('approve').marker,
      'Autoriser une fois 3',
    );
    assert.equal(
      interpret(ordered('Autoriser une fois 3', 'Autoriser')).bindings.get('approve').marker,
      'Autoriser une fois 3',
    );
  });
});

describe('tree depth', () => {
  it('reaches deeper than Claude Desktop actually nests', () => {
    // Measured on a live permission dialog: Claude's stop control sits at
    // depth 32 and the verdict buttons at ~34, under roughly twenty nested
    // AXGroups below the AXWebArea. The daemon used to walk 24 and `dump` 40,
    // so the dump showed every control while the daemon saw none — it reported
    // "no stop control available" for a button that was plainly on screen.
    const OBSERVED_DEEPEST_CONTROL = 34;
    assert.ok(
      DEFAULT_TREE_DEPTH > OBSERVED_DEEPEST_CONTROL,
      `depth ${DEFAULT_TREE_DEPTH} must exceed the observed ${OBSERVED_DEEPEST_CONTROL}`,
    );
  });

  it('finds a dialog buried at Electron-like depth', () => {
    // Same dialog as the live capture, wrapped in 30 groups.
    let node = new AXNode({
      role: 'AXGroup',
      children: [
        new AXNode({ role: 'AXStaticText', value: 'Permission demandée : modifier.' }),
        new AXNode({ role: 'AXButton', title: 'Refuser 1', actions: [AX_PRESS], ref: new AXRef('deny') }),
        new AXNode({
          role: 'AXButton', title: 'Autoriser une fois 3',
          actions: [AX_PRESS], ref: new AXRef('approve'),
        }),
      ],
    });
    for (let level = 0; level < 30; level += 1) {
      node = new AXNode({ role: 'AXGroup', children: [node] });
    }
    const tree = new AXNode({ role: 'AXApplication', children: [node] });

    const { snapshot, bindings } = interpret(tree);
    assert.equal(snapshot.state, 'waiting_permission');
    assert.equal(bindings.get('approve').marker, 'approve');
    assert.equal(bindings.get('deny').marker, 'deny');
  });
});

describe('label matching', () => {
  it('matches exactly after normalisation, never as a substring', () => {
    // "Disallowed" must not read as "allow", or a stray control could end up
    // wired to APPROVE.
    assert.ok(matches('Allow', selectors.approveLabels));
    assert.ok(matches('  ALLOW  ', selectors.approveLabels));
    assert.ok(matches('Allow once', selectors.approveLabels));
    assert.ok(matches("Don't allow", selectors.denyLabels));
    assert.ok(matches('Don’t allow', selectors.denyLabels));
    assert.ok(!matches('Disallowed', selectors.approveLabels));
    assert.ok(!matches('Allow list settings', selectors.approveLabels));
    assert.ok(!matches(null, selectors.approveLabels));
    assert.ok(!matches('   ', selectors.approveLabels));
  });
});

describe('transcript extraction', () => {
  it('still yields a message when there are no speaker markers', () => {
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [new AXNode({
          role: 'AXScrollArea',
          children: [
            new AXNode({ role: 'AXStaticText', value: 'first line' }),
            new AXNode({ role: 'AXStaticText', value: 'second line' }),
          ],
        })],
      })],
    });
    assert.equal(interpret(tree).snapshot.lastMessage, 'first line\nsecond line');
  });

  it('clips the message to the protocol budget', () => {
    const long = 'word '.repeat(400);
    assert.ok(interpret(fixtures.writing(long)).snapshot.lastMessage.length <= MESSAGE_MAX_CHARS);
  });

  it('reports no message when there is no transcript container', () => {
    // Observed on the Code tab: no AXScrollArea anywhere, so the old fallback
    // scraped the whole app and presented tooltips and error toasts as Claude's
    // reply. Silence is the correct answer here.
    const chrome = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXWindow',
        children: [new AXNode({
          role: 'AXWebArea',
          children: [
            new AXNode({ role: 'AXStaticText', value: 'Tapez / pour les commandes.' }),
            new AXNode({ role: 'AXStaticText', value: 'Mode rapide desactive' }),
            new AXNode({ role: 'AXStaticText', value: "L'analyse a ete interrompue." }),
          ],
        })],
      })],
    });
    const { snapshot } = interpret(chrome);
    assert.equal(snapshot.lastMessage, null);
    assert.equal(snapshot.detail, null);
  });

  it('picks the densest scroll area as the transcript', () => {
    const sparse = transcript([['User', 'hi']]);
    const dense = transcript([['User', 'a'], ['Claude', 'b'], ['User', 'c'], ['Claude', 'the real reply']]);
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({ role: 'AXWindow', children: [sparse, dense] })],
    });
    assert.equal(interpret(tree).snapshot.lastMessage, 'the real reply');
  });
});


describe('menu bar is never actuated', () => {
  /** A menu item, which supports AXPress exactly like a button. */
  const menuItem = (label, marker) => new AXNode({
    role: 'AXMenuItem', title: label, actions: [AX_PRESS], ref: new AXRef(marker),
  });

  /** The real shape observed on a French Claude Desktop. */
  const withMenuBar = (windowChildren = []) => new AXNode({
    role: 'AXApplication',
    title: 'Claude',
    children: [
      new AXNode({
        role: 'AXMenuBar',
        children: [new AXNode({
          role: 'AXMenuBarItem',
          title: 'Édition',
          children: [new AXNode({
            role: 'AXMenu',
            children: [
              menuItem('Annuler', 'edit-undo'),
              menuItem('Rétablir', 'edit-redo'),
            ],
          })],
        })],
      }),
      new AXNode({ role: 'AXWindow', title: 'Claude', children: windowChildren }),
    ],
  });

  it('never binds STOP to a menu item', () => {
    // Two independent guards stop this, so test them separately.
    //
    // 1. The label: "Annuler" is Cancel in a dialog but Undo in the Edit menu.
    //    STOP bound to it and fired Edit > Undo on a live conversation.
    assert.equal(interpret(withMenuBar()).bindings.get('stop'), undefined);

    // 2. The role, isolated: even a menu item whose label *is* a stop verb must
    //    never be bound, because a menu is not where dialog controls live.
    const stopMenu = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXMenuBar',
        children: [new AXNode({
          role: 'AXMenu',
          children: [menuItem('Stop', 'menu-stop')],
        })],
      })],
    });
    assert.equal(interpret(stopMenu).bindings.get('stop'), undefined);
  });

  it('still binds STOP to a real stop button alongside a menu bar', () => {
    const { bindings } = interpret(withMenuBar([button('Stop', 'stop')]));
    assert.equal(bindings.get('stop').marker, 'stop');
  });

  it('does not mistake a menu for a permission dialog', () => {
    // A menu offering both verdicts must not become an approve screen.
    const tree = new AXNode({
      role: 'AXApplication',
      children: [new AXNode({
        role: 'AXMenuBar',
        children: [new AXNode({
          role: 'AXMenu',
          children: [menuItem('Autoriser', 'm-allow'), menuItem('Refuser', 'm-deny')],
        })],
      })],
    });
    const { snapshot, bindings } = interpret(tree);
    assert.equal(snapshot.permission, null);
    assert.equal(bindings.get('approve'), undefined);
    assert.equal(bindings.get('deny'), undefined);
  });

  it('keeps AXPress visible to dump even where it must not be pressed', () => {
    // `dump` has to show the truth; only binding is restricted.
    const item = menuItem('Annuler', 'edit-undo');
    assert.equal(item.canPress, true);
    assert.equal(item.isActuatable, false);
  });
});

describe('accented labels', () => {
  it('matches French wording with diacritics', () => {
    // Before folding, "Arrêter" normalised to "arr ter" and matched nothing.
    assert.equal(normalise('Arrêter'), 'arreter');
    assert.ok(matches('Arrêter', selectors.stopLabels));
    assert.ok(matches('Refuser', selectors.denyLabels));
  });

  it('no longer treats a generic Cancel as a stop control', () => {
    assert.ok(!matches('Cancel', selectors.stopLabels));
    assert.ok(!matches('Annuler', selectors.stopLabels));
  });
});

describe('AXTreeFormatter', () => {
  it('renders the tree with connectors', () => {
    const rendered = new AXTreeFormatter().render(fixtures.permission());
    assert.ok(rendered.startsWith('AXApplication title="Claude"'));
    assert.ok(rendered.includes('└── '));
    assert.ok(rendered.includes('AXButton title="Allow"'));
    assert.ok(rendered.includes('actions=[AXPress]'));
  });

  it('filters to matching elements only', () => {
    const rendered = new AXTreeFormatter().renderMatches(fixtures.permission(), 'button');
    assert.ok(rendered.includes('AXButton title="Allow"'));
    assert.ok(rendered.includes('AXButton title="Deny"'));
    assert.ok(!rendered.includes('git push origin master'));
    assert.ok(rendered.includes('2 matches'));
  });

  it('reports an empty filter result explicitly', () => {
    assert.ok(new AXTreeFormatter().renderMatches(fixtures.idle(), 'zzz').includes('no element matches'));
  });

  it('flattens and clips long values', () => {
    const node = new AXNode({ role: 'AXStaticText', value: `${'x'.repeat(400)}\nsecond` });
    const rendered = new AXTreeFormatter({ maxValueChars: 20 }).render(node);
    assert.ok(!rendered.slice(0, -1).includes('\n'), 'multi-line values must stay on one line');
    assert.ok(rendered.includes('…'));
  });
});

describe('port discovery', () => {
  it('ranks UART bridges above native USB and excludes tty/Bluetooth', () => {
    assert.deepEqual(
      rank(['cu.usbmodem1101', 'cu.usbserial-0001', 'tty.usbserial-0001', 'cu.Bluetooth-Incoming-Port'])
        .map((port) => port.path),
      ['/dev/cu.usbserial-0001', '/dev/cu.usbmodem1101'],
    );
  });

  it('returns nothing when no candidate is plausible', () => {
    assert.deepEqual(rank(['tty.usbserial-0001', 'cu.Bluetooth-Incoming-Port', 'cu.debug-console']), []);
  });

  it('is order-stable for equal scores', () => {
    const names = ['cu.usbserial-b', 'cu.usbserial-a'];
    assert.deepEqual(
      rank(names).map((p) => p.path),
      rank([...names].reverse()).map((p) => p.path),
    );
  });
});
