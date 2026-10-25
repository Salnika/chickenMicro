import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { Log } from '../src/log.js';
import { PermissionGate } from '../src/session/permissionGate.js';
import { SessionCoordinator } from '../src/session/sessionCoordinator.js';
import { FakeAccessibilitySource, fixtures } from '../src/simulation/fixtures.js';

/** Captures what would go down the wire. */
class RecordingTransport {
  onLine = null;
  onConnect = null;
  isConnected = true;
  sent = [];
  resets = 0;

  start() {}
  stop() {}
  reset() { this.resets += 1; }
  send(line) { this.sent.push(line.trimEnd()); }

  get types() { return this.sent.map((line) => JSON.parse(line).type); }
  framesOfType(type) { return this.sent.map((l) => JSON.parse(l)).filter((f) => f.type === type); }
  clear() { this.sent.length = 0; }
}

/** One assembled session with an injectable clock. */
function fixture({ approveDelayMs = 200 } = {}) {
  const transport = new RecordingTransport();
  const source = new FakeAccessibilitySource();
  const clock = { now: 1_000_000 };
  /** Every line the daemon logged, for the tests that check what it says. */
  const logged = [];
  const coordinator = new SessionCoordinator({
    transport,
    source,
    gate: new PermissionGate(approveDelayMs),
    log: new Log({ level: 'info', color: false, out: { write: (line) => logged.push(line) } }),
    now: () => clock.now,
  });
  return {
    transport,
    source,
    clock,
    logged,
    coordinator,
    /** Moves past the arming delay. */
    arm() { clock.now += 1000; },
    inbound(line) { coordinator.handleInbound(line); },
    /** Shows a permission and returns its allocated id. */
    present(content = 'git push origin master') {
      source.tree = fixtures.permission('Run command', content);
      coordinator.refresh();
      return transport.framesOfType('permission').at(-1)?.id;
    },
  };
}

describe('snapshot mirroring', () => {
  it('sends state and message on the first sync, nothing on an unchanged re-read', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    f.coordinator.refresh();
    assert.deepEqual(f.transport.types, ['sessions', 'state', 'message']);

    // Re-reading an unchanged tree must not cost an e-ink refresh.
    f.transport.clear();
    f.coordinator.refresh();
    assert.deepEqual(f.transport.types, []);
  });

  it('clears the permission before the new state', () => {
    // Clearing after would redraw the device twice, since the permission screen
    // has priority.
    const f = fixture();
    f.present();
    f.transport.clear();
    f.source.tree = fixtures.idle();
    f.coordinator.refresh();
    // The session list sits between them: it carries the active permission id,
    // so the device can reconcile even if the clear above is lost.
    assert.deepEqual(f.transport.types.slice(0, 3),
                     ['permission_cleared', 'sessions', 'state']);
  });

  it('resends everything when the device says hello', () => {
    const f = fixture();
    f.present();
    f.transport.clear();
    f.inbound('{"v":1,"type":"hello","role":"device","app":"chickenmicro-fw"}');
    assert.ok(f.transport.types.includes('state'));
    assert.ok(f.transport.types.includes('permission'));
  });
});

describe('action mapping', () => {
  it('approve presses Allow', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('deny presses Deny', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(`{"v":1,"type":"action","action":"deny","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['deny']);
  });

  it("stop presses Claude's stop control", () => {
    const f = fixture();
    f.source.tree = fixtures.thinking();
    f.coordinator.refresh();
    f.inbound('{"v":1,"type":"action","action":"stop"}');
    assert.deepEqual(f.source.pressed, ['stop']);
  });

  it('context presses nothing on the Mac', () => {
    const f = fixture();
    f.present();
    f.arm();
    f.inbound('{"v":1,"type":"action","action":"context"}');
    assert.deepEqual(f.source.pressed, []);
  });

  it('answers a ping with a matching pong', () => {
    const f = fixture();
    f.inbound('{"v":1,"type":"ping","seq":17}');
    assert.equal(f.transport.framesOfType('pong').at(-1).seq, 17);
  });
});

describe('approve safety, end to end', () => {
  it('refuses a stale requestId', () => {
    const f = fixture();
    f.present();
    f.arm();
    f.inbound('{"v":1,"type":"action","action":"approve","requestId":"req-999"}');
    assert.deepEqual(f.source.pressed, [], 'a stale id must never press Allow');
  });

  it('refuses before the arming delay', () => {
    const f = fixture();
    const id = f.present();
    f.clock.now += 50;
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, []);
  });

  it('refuses once the dialog has vanished', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.source.tree = fixtures.idle();   // the user acted in the app first
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, []);
  });

  it('refuses when Claude swapped the request between display and press', () => {
    // The fresh re-read inside the coordinator must catch this even though the
    // gate still holds the old id.
    const f = fixture();
    const id = f.present();
    f.arm();
    f.source.tree = fixtures.permission('Run command', 'rm -rf /');
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(
      f.source.pressed, [],
      'approving the old id must not authorise the command now on screen',
    );
  });

  it('refuses with nothing pending', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    f.coordinator.refresh();
    f.inbound('{"v":1,"type":"action","action":"approve","requestId":"req-1"}');
    assert.deepEqual(f.source.pressed, []);
  });

  it('does not press twice for a replayed approve', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['allow']);

    f.source.tree = fixtures.idle();   // Claude tore the dialog down
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.deepEqual(f.source.pressed, ['allow'], 'a replayed approve must not press twice');
  });

  it('lets no malformed frame reach AXPress', () => {
    const f = fixture();
    f.present();
    f.arm();
    const junk = [
      '',
      'garbage',
      '{"v":1,"type":"action"}',
      '{"v":1,"type":"action","action":"approve"}',
      '{"v":2,"type":"action","action":"approve","requestId":"req-1"}',
      '{"v":1,"type":"action","action":"Approve","requestId":"req-1"}',
      '{"v":1,"type":"action","action":"approve","requestId":""}',
      '{"type":"action","action":"approve","requestId":"req-1"}',
      '{"v":1,"type":"action","action":"allow","requestId":"req-1"}',
    ];
    for (const line of junk) f.inbound(line);
    assert.deepEqual(f.source.pressed, []);
  });

  it('never presses anything for navigation', () => {
    const f = fixture();
    f.present();
    f.arm();
    for (const direction of ['up', 'down', 'left', 'right', 'select']) {
      f.inbound(`{"v":1,"type":"navigation","action":"${direction}"}`);
    }
    assert.deepEqual(f.source.pressed, []);
  });

  it('reports a failed press back to the device', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.source.pressError = new Error('the element is no longer in the tree');
    f.inbound(`{"v":1,"type":"action","action":"approve","requestId":"${id}"}`);
    assert.equal(f.transport.framesOfType('error').length, 1);
  });
});

/**
 * Two windows: the one in front is quiet, the other is asking. Taken from a
 * real session log where the box flipped between them every few seconds.
 */
function frontIsQuiet() {
  return fixtures.workspace([
    { name: 'chickenMicro', tree: fixtures.idle() },
    { name: 'docs', tree: fixtures.permission('Write file', 'docs/wiring.md', 'b.') },
  ]);
}

const approveLine = (id) => `{"v":1,"type":"action","action":"approve","requestId":"${id}"}`;

describe('a request Claude keeps on screen', () => {
  it('can still be answered when the first press did not take', () => {
    // Seen in the wild: the press reports success, the dialog stays, and every
    // later press was refused with "no permission is pending" — the box dead
    // for that request until Claude moved on by itself.
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    assert.deepEqual(f.source.pressed, ['allow']);

    f.coordinator.refresh();   // the same dialog is still there
    f.arm();
    f.inbound(approveLine(id));
    assert.deepEqual(f.source.pressed, ['allow', 'allow']);
  });

  it('waits out the arming delay again before the next press counts', () => {
    // Re-arming must not turn a bouncing key into a second verdict.
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.coordinator.refresh();
    f.clock.now += 50;                    // inside the 200 ms delay
    f.inbound(approveLine(id));
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('does not redraw the device while it stays', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.transport.clear();
    f.coordinator.refresh();
    assert.deepEqual(f.transport.types, []);
  });
});

describe('which session the box describes', () => {
  it('follows the one that is asking, not the window in front', () => {
    // Otherwise the state and the message under the dialog belong to another
    // conversation, and they flip every time the front window changes.
    const f = fixture();
    f.source.tree = frontIsQuiet();
    f.coordinator.refresh();

    const list = f.transport.framesOfType('sessions').at(-1);
    const permission = f.transport.framesOfType('permission').at(-1);
    assert.equal(list.focus, permission.session);
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'waiting_permission');
  });

  it('keeps the session the operator chose on the box', () => {
    const f = fixture();
    f.source.tree = frontIsQuiet();
    f.coordinator.refresh();
    f.inbound('{"v":1,"type":"focus","sessionId":"s1"}');
    f.transport.clear();
    f.coordinator.refresh();

    const list = f.transport.framesOfType('sessions').at(-1);
    assert.equal(list?.focus ?? 's1', 's1');
  });

  it('goes back to the window in front once the request is gone', () => {
    const f = fixture();
    f.source.tree = frontIsQuiet();
    f.coordinator.refresh();
    f.source.tree = fixtures.workspace([
      { name: 'chickenMicro', tree: fixtures.idle() },
      { name: 'docs', tree: fixtures.idle() },
    ]);
    f.transport.clear();
    f.coordinator.refresh();

    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 's1');
  });
});

const denyLine = (id) => `{"v":1,"type":"action","action":"deny","requestId":"${id}"}`;
const stillShows = (f) => f.logged.filter((line) => /still shows/.test(line));

describe('a press that did nothing', () => {
  it('is pressed once more when the dialog is still there', () => {
    // Seen in the wild: the first press on a dialog reports success and does
    // nothing, and the next one lands. The operator pressed once; the daemon
    // repeats that press rather than leaving them to do it again.
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    assert.deepEqual(f.source.pressed, ['allow']);

    f.coordinator.refresh();          // still there, but only just pressed
    assert.deepEqual(f.source.pressed, ['allow']);

    f.clock.now += 2000;
    f.coordinator.refresh();
    assert.deepEqual(f.source.pressed, ['allow', 'allow']);
  });

  it('presses a refusal once more as well', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(denyLine(id));
    f.coordinator.refresh();          // the next poll, which re-arms the gate
    f.clock.now += 2000;
    f.coordinator.refresh();
    assert.deepEqual(f.source.pressed, ['deny', 'deny']);
  });

  it('presses once more, and no more, however long the dialog stays', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.coordinator.refresh();
    for (let poll = 0; poll < 5; poll += 1) {
      f.clock.now += 2000;
      f.coordinator.refresh();
    }
    assert.deepEqual(f.source.pressed, ['allow', 'allow']);
  });

  it('does not press again once the dialog has gone', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.source.tree = fixtures.idle();
    f.clock.now += 2000;
    f.coordinator.refresh();
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('never presses on behalf of a different request', () => {
    // The retry repeats one press on one request. A dialog that has been
    // replaced by another command is not that request.
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.source.tree = fixtures.permission('Run command', 'rm -rf /');
    f.clock.now += 2000;
    f.coordinator.refresh();
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('waits for the gate like any press', () => {
    // Re-arming after the first press restarts the arming delay; a retry that
    // came sooner than that would be a press the gate had not let through.
    const f = fixture({ approveDelayMs: 5000 });
    const id = f.present();
    f.clock.now += 6000;
    f.inbound(approveLine(id));
    f.coordinator.refresh();
    f.clock.now += 2000;
    f.coordinator.refresh();
    assert.deepEqual(f.source.pressed, ['allow']);
    assert.equal(f.logged.filter((line) => /not pressing approve/.test(line)).length, 1);
  });

  it('is reported once the second press did nothing either', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.coordinator.refresh();

    f.clock.now += 2000;
    f.coordinator.refresh();          // the retry
    assert.deepEqual(stillShows(f), [], 'the retry is not yet a failure');
    f.coordinator.refresh();          // the poll that re-arms the gate

    f.clock.now += 2000;
    f.coordinator.refresh();
    f.coordinator.refresh();          // said once, not on every poll
    assert.equal(stillShows(f).length, 1);
    assert.match(stillShows(f)[0], /twice/);
    assert.deepEqual(f.source.pressed, ['allow', 'allow']);
  });

  it('says nothing when the dialog goes as it should', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.inbound(approveLine(id));
    f.source.tree = fixtures.idle();
    f.clock.now += 2000;
    f.coordinator.refresh();

    assert.deepEqual(f.logged.filter((line) => /still shows/.test(line)), []);
  });
});

describe('a verdict that could not be applied', () => {
  it('puts the request back on the box', () => {
    // The box clears its screen as soon as it sends a verdict, so a refusal
    // has to send the request back or the decision is quietly lost.
    const f = fixture();
    const id = f.present();
    f.transport.clear();
    // Inside the arming delay: refused.
    f.inbound(approveLine(id));

    assert.deepEqual(f.source.pressed, []);
    assert.deepEqual(f.transport.framesOfType('permission').map((frame) => frame.id), [id]);
  });

  it('puts it back when the press itself fails', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.source.pressError = new Error('the element is no longer in the tree');
    f.transport.clear();
    f.inbound(approveLine(id));

    assert.equal(f.transport.framesOfType('error').length, 1);
    assert.deepEqual(f.transport.framesOfType('permission').map((frame) => frame.id), [id]);
  });

  it('does not send one back when the request has genuinely gone', () => {
    const f = fixture();
    const id = f.present();
    f.arm();
    f.source.tree = fixtures.idle();
    f.transport.clear();
    f.inbound(approveLine(id));

    assert.deepEqual(f.transport.framesOfType('permission'), []);
    assert.equal(f.transport.framesOfType('permission_cleared').length, 1);
  });
});
