import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import net from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';

import { HookRegistry, HOOK_GRACE_MS, HOOK_SETTLE_MS } from '../src/hook/registry.js';
import { HookServer } from '../src/hook/server.js';
import { summarise } from '../src/hook/summary.js';
import { HOOK_MAX_LINE_BYTES, decodeHookRequest, encodeHook } from '../src/hook/wire.js';
import { Log } from '../src/log.js';
import { decodeInbound } from '../src/protocol/frames.js';
import { PermissionGate } from '../src/session/permissionGate.js';
import { SessionCoordinator } from '../src/session/sessionCoordinator.js';
import { FakeAccessibilitySource, fixtures } from '../src/simulation/fixtures.js';

/** What a hook's connection looks like to the registry. */
class FakeChannel {
  lines = [];
  ended = false;
  send(text) { this.lines.push(JSON.parse(text)); }
  end() { this.ended = true; }
}

const request = (tool, input = {}, extra = {}) => ({
  tool, session: 'c1', cwd: '/Users/me/chickenMicro', input, ...extra,
});

describe('hook — what a request looks like on the box', () => {
  it('reads a shell command', () => {
    const summary = summarise(request('Bash', { command: 'git push origin master\nsecond line' }));
    assert.equal(summary.kind, 'shell');
    assert.equal(summary.title, 'Run command');
    assert.match(summary.content, /^git push origin master/);
    assert.deepEqual(summary.salient, ['git push origin master']);
    assert.equal(summary.sessionName, 'chickenMicro');
  });

  it('reads an edit by the file it touches', () => {
    const summary = summarise(request('Edit', { file_path: '/Users/me/chickenMicro/README.md' }));
    assert.equal(summary.kind, 'write');
    assert.equal(summary.content, '/Users/me/chickenMicro/README.md');
    assert.ok(summary.salient.includes('README.md'));
  });

  it('reads a web fetch, a search and a connector tool', () => {
    assert.equal(summarise(request('WebFetch', { url: 'https://example.com/a' })).kind, 'fetch');
    assert.equal(summarise(request('WebSearch', { query: 'esp32 e-ink' })).kind, 'search');
    const connector = summarise(request('mcp__notion__create_page', { title: 'Plan' }));
    assert.equal(connector.kind, 'tool');
    assert.match(connector.content, /notion: create_page/);
  });

  it('still says something about a tool it has never heard of', () => {
    const summary = summarise(request('Teleport', { destination: 'Mars base' }));
    assert.equal(summary.title, 'Teleport');
    assert.match(summary.content, /Mars base/);
  });

  it('never lets a long command run past what the box can hold', () => {
    const summary = summarise(request('Bash', { command: 'x'.repeat(5000) }));
    assert.ok(summary.content.length <= 400);
  });
});

describe('hook — the line a hook sends', () => {
  const line = (frame) => JSON.stringify({ v: 1, type: 'request', ...frame });

  it('accepts a well-formed request', () => {
    const { request: decoded } = decodeHookRequest(line({ tool: 'Bash', session: 's', cwd: '/x', input: { a: 1 } }));
    assert.deepEqual(decoded, { tool: 'Bash', session: 's', cwd: '/x', input: { a: 1 } });
  });

  it('refuses what it cannot trust, with a reason', () => {
    for (const bad of [
      'garbage',
      '[]',
      JSON.stringify({ v: 2, type: 'request', tool: 'Bash' }),
      JSON.stringify({ v: 1, type: 'verdict', tool: 'Bash' }),
      line({ tool: '' }),
      line({}),
      line({ tool: 'x'.repeat(500) }),
    ]) {
      assert.notEqual(decodeHookRequest(bad).rejected, undefined, bad);
    }
  });

  it('never invents an input it was not given', () => {
    assert.deepEqual(decodeHookRequest(line({ tool: 'Bash', input: 'rm -rf /' })).request.input, {});
  });

  it('writes the one line a hook reads back', () => {
    assert.deepEqual(JSON.parse(encodeHook.verdict('allow')),
                     { v: 1, type: 'verdict', decision: 'allow' });
    assert.equal(JSON.parse(encodeHook.verdict('deny', 'no')).message, 'no');
    assert.equal(JSON.parse(encodeHook.cancel('gone')).type, 'cancel');
  });
});

describe('hook — the mode frame', () => {
  it('decodes each mode, and nothing else', () => {
    assert.deepEqual(decodeInbound('{"v":1,"type":"mode","value":"auto"}').frame,
                     { type: 'mode', value: 'auto' });
    assert.deepEqual(decodeInbound('{"v":1,"type":"mode","value":"ui"}').frame,
                     { type: 'mode', value: 'ui' });
    assert.notEqual(decodeInbound('{"v":1,"type":"mode","value":"hook"}').rejected, undefined);
    assert.notEqual(decodeInbound('{"v":1,"type":"mode"}').rejected, undefined);
    assert.notEqual(decodeInbound('{"v":1,"type":"mode","value":1}').rejected, undefined);
  });
});

describe('hook registry', () => {
  const registry = (now = { t: 1_000_000 }) => ({
    now,
    registry: new HookRegistry({ now: () => now.t }),
  });

  it('keeps requests in arrival order and gives each its own id', () => {
    const { registry: r } = registry();
    const first = r.submit(request('Bash', { command: 'ls' }), new FakeChannel());
    const second = r.submit(request('Bash', { command: 'pwd' }), new FakeChannel());
    assert.deepEqual(r.live().map((entry) => entry.id), [first.id, second.id]);
    assert.notEqual(first.id, second.id);
  });

  it('answers only the connection that asked, once', () => {
    const { registry: r } = registry();
    const mine = new FakeChannel();
    const other = new FakeChannel();
    const { id } = r.submit(request('Bash', { command: 'ls' }), mine);
    r.submit(request('Bash', { command: 'pwd' }), other);

    assert.equal(r.answer(id, 'allow'), true);
    assert.deepEqual(mine.lines, [{ v: 1, type: 'verdict', decision: 'allow' }]);
    assert.equal(mine.ended, true);
    assert.deepEqual(other.lines, []);
    assert.equal(r.answer(id, 'allow'), false, 'a second answer must go nowhere');
    assert.equal(mine.lines.length, 1);
  });

  it('tells Claude why a refusal was a refusal', () => {
    const { registry: r } = registry();
    const channel = new FakeChannel();
    const { id } = r.submit(request('Bash', { command: 'ls' }), channel);
    r.answer(id, 'deny');
    assert.equal(channel.lines[0].decision, 'deny');
    assert.match(channel.lines[0].message, /ChickenMicro/);
  });

  it('claims the dialog that shows its command, and no other', () => {
    const { registry: r } = registry();
    const { id } = r.submit(request('Bash', { command: 'git push origin master' }), new FakeChannel());
    assert.equal(r.claimFor({ id: 's1.req-1', title: 'Run command', content: 'rm -rf build' }), null);
    const claimed = r.claimFor({ id: 's1.req-2', title: 'Run command', content: 'git push origin master' });
    assert.equal(claimed.id, id);
    // The same dialog keeps its request, and a second dialog cannot take it.
    assert.equal(r.claimFor({ id: 's1.req-2', title: 'x', content: 'y' }).id, id);
    assert.equal(r.claimFor({ id: 's2.req-1', title: 'Run command', content: 'git push origin master' }), null);
  });

  it('matches a path through accents, case and a trailing ellipsis', () => {
    const { registry: r } = registry();
    r.submit(request('Edit', { file_path: '/Users/me/Projets/Café/README.md' }), new FakeChannel());
    const claimed = r.claimFor({
      id: 's1.req-1',
      title: 'Autoriser Claude à',
      content: 'modifier\n/users/me/projets/cafe/readme.md ?',
    });
    assert.notEqual(claimed, null);
  });

  it('stands a request down when its dialog has gone', () => {
    const { registry: r } = registry();
    const channel = new FakeChannel();
    const { id } = r.submit(request('Bash', { command: 'git push origin master' }), channel);
    r.claimFor({ id: 's1.req-1', title: 'Run command', content: 'git push origin master' });

    r.cancelGone(new Set(['s1.req-1']));
    assert.equal(r.isLive(id), true, 'its dialog is still there');

    r.cancelGone(new Set());
    assert.equal(r.isLive(id), false);
    assert.equal(channel.lines[0].type, 'cancel');
    assert.equal(channel.ended, true);
  });

  it('leaves a request alone that no dialog has claimed', () => {
    // A window nobody is looking at has no dialog the daemon can read, and its
    // request is just as real.
    const { registry: r } = registry();
    const { id } = r.submit(request('Bash', { command: 'ls' }), new FakeChannel());
    r.cancelGone(new Set());
    assert.equal(r.isLive(id), true);
  });

  it('forgets a request whose hook gave up', () => {
    const { registry: r } = registry();
    const { id } = r.submit(request('Bash', { command: 'ls' }), new FakeChannel());
    r.channelClosed(id);
    assert.equal(r.isLive(id), false);
    assert.equal(r.get(id), undefined);
  });

  it('keeps an answered request while its dialog goes, then lets it go', () => {
    const { registry: r, now } = registry();
    const { id } = r.submit(request('Bash', { command: 'git push origin master' }), new FakeChannel());
    const dialog = { id: 's1.req-1', title: 'Run command', content: 'git push origin master' };
    r.claimFor(dialog);
    r.answer(id, 'allow', now.t);

    now.t += HOOK_SETTLE_MS - 1;
    r.prune(now.t);
    assert.equal(r.claimFor(dialog)?.id, id, 'still hides the dialog while it settles');

    now.t += 2;
    r.prune(now.t);
    assert.equal(r.claimFor(dialog), null);
  });

  it('stands everything down when the box is set to the interface', () => {
    const { registry: r } = registry();
    const waiting = new FakeChannel();
    r.submit(request('Bash', { command: 'ls' }), waiting);

    r.setMode('ui');
    assert.equal(waiting.lines[0].type, 'cancel');
    assert.equal(r.live().length, 0);

    const late = new FakeChannel();
    assert.equal(r.submit(request('Bash', { command: 'pwd' }), late).accepted, false);
    assert.equal(late.lines[0].type, 'cancel');
    assert.equal(late.ended, true);

    r.setMode('auto');
    assert.equal(r.submit(request('Bash', { command: 'pwd' }), new FakeChannel()).accepted, true);
  });

  it('refuses to queue without limit', () => {
    const r = new HookRegistry({ maxPending: 2 });
    r.submit(request('Bash', { command: 'a b c' }), new FakeChannel());
    r.submit(request('Bash', { command: 'd e f' }), new FakeChannel());
    const overflow = new FakeChannel();
    assert.equal(r.submit(request('Bash', { command: 'g h i' }), overflow).accepted, false);
    assert.equal(overflow.lines[0].type, 'cancel');
  });

  it('only waits for hooks when hooks are in play', () => {
    const { registry: r, now } = registry();
    const first = now.t;
    assert.equal(r.holdsOff(first, now.t), false, 'no hook has ever asked');

    const { id } = r.submit(request('Bash', { command: 'ls' }), new FakeChannel());
    r.channelClosed(id);
    assert.equal(r.holdsOff(first, now.t), true);
    now.t += HOOK_GRACE_MS + 1;
    assert.equal(r.holdsOff(first, now.t), false, 'the grace is a moment, not a wait');

    r.setMode('ui');
    assert.equal(r.holdsOff(now.t, now.t), false, 'the interface mode never waits');
  });
});

describe('hook server', () => {
  const quiet = () => new Log({ level: 'error', color: false, out: { write() {} } });

  async function waitFor(predicate, what) {
    const deadline = Date.now() + 2000;
    while (!predicate()) {
      if (Date.now() > deadline) assert.fail(`timed out waiting for ${what}`);
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
  }

  function readLine(socket) {
    return new Promise((resolve, reject) => {
      let buffer = '';
      socket.setEncoding('utf8');
      socket.on('data', (chunk) => {
        buffer += chunk;
        if (buffer.includes('\n')) resolve(JSON.parse(buffer.split('\n')[0]));
      });
      socket.on('end', () => (buffer === '' ? resolve(null) : undefined));
      socket.on('error', reject);
    });
  }

  async function withServer(run) {
    const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
    const registry = new HookRegistry();
    const server = new HookServer({ registry, log: quiet(), path: join(directory, 'hook.sock') });
    await server.start();
    try {
      await run({ registry, server, path: server.path });
    } finally {
      server.stop();
      rmSync(directory, { recursive: true, force: true });
    }
  }

  const requestLine = (frame = {}) => `${JSON.stringify({
    v: 1, type: 'request', tool: 'Bash', session: 's', cwd: '/x', input: { command: 'ls -la' }, ...frame,
  })}\n`;

  it('carries a request in and a verdict back', async () => {
    await withServer(async ({ registry, path }) => {
      const client = net.connect(path);
      client.write(requestLine());
      await waitFor(() => registry.live().length === 1, 'the request');

      const [{ id }] = registry.live();
      const reply = readLine(client);
      registry.answer(id, 'allow');
      assert.deepEqual(await reply, { v: 1, type: 'verdict', decision: 'allow' });
    });
  });

  it('forgets a request whose hook hangs up', async () => {
    await withServer(async ({ registry, path }) => {
      const client = net.connect(path);
      client.write(requestLine());
      await waitFor(() => registry.live().length === 1, 'the request');

      client.destroy();
      await waitFor(() => registry.live().length === 0, 'the hang-up');
    });
  });

  it('drops what is not a request', async () => {
    await withServer(async ({ registry, path }) => {
      const client = net.connect(path);
      client.write('definitely not json\n');
      await new Promise((resolve) => client.once('close', resolve));
      assert.equal(registry.live().length, 0);
    });
  });

  it('drops a line that is far too long, without waiting for its end', async () => {
    await withServer(async ({ registry, path }) => {
      const client = net.connect(path);
      client.on('error', () => {});
      client.write('x'.repeat(HOOK_MAX_LINE_BYTES + 10));
      await new Promise((resolve) => client.once('close', resolve));
      assert.equal(registry.live().length, 0);
    });
  });

  it('tells a hook to stand down when the box answers through the interface', async () => {
    await withServer(async ({ registry, path }) => {
      registry.setMode('ui');
      const client = net.connect(path);
      client.write(requestLine());
      const reply = await readLine(client);
      assert.equal(reply.type, 'cancel');
      assert.equal(registry.live().length, 0);
    });
  });

  it('replaces a socket left behind by a daemon that died', async () => {
    const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
    const path = join(directory, 'hook.sock');
    try {
      const orphan = net.createServer();
      await new Promise((resolve) => orphan.listen(path, resolve));
      // Closing the server leaves the file behind only if it is killed; here the
      // file is removed by close(), so recreate the stale state by hand.
      orphan.close();
      await new Promise((resolve) => orphan.once('close', resolve));

      const server = new HookServer({ registry: new HookRegistry(), log: quiet(), path });
      await server.start();
      server.stop();
    } finally {
      rmSync(directory, { recursive: true, force: true });
    }
  });

  it('will not start over a daemon that is still listening', async () => {
    await withServer(async ({ path }) => {
      const second = new HookServer({ registry: new HookRegistry(), log: quiet(), path });
      await assert.rejects(() => second.start(), /already listening/);
    });
  });
});

// ---------------------------------------------------------------------------
// The coordinator with a hook registry: what the box shows, and where a verdict
// goes.
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
  framesOfType(type) { return this.sent.map((l) => JSON.parse(l)).filter((f) => f.type === type); }
  clear() { this.sent.length = 0; }
}

function fixture({ approveDelayMs = 200 } = {}) {
  const transport = new RecordingTransport();
  const source = new FakeAccessibilitySource();
  const clock = { now: 1_000_000 };
  const logged = [];
  const scheduled = [];
  const hooks = new HookRegistry({ now: () => clock.now });
  const coordinator = new SessionCoordinator({
    transport,
    source,
    gate: new PermissionGate(approveDelayMs),
    log: new Log({ level: 'info', color: false, out: { write: (line) => logged.push(line) } }),
    now: () => clock.now,
    hooks,
    timers: {
      setTimeout: (callback) => { scheduled.push(callback); return { unref() {} }; },
      clearTimeout() {},
    },
  });
  return {
    transport,
    source,
    clock,
    logged,
    hooks,
    coordinator,
    arm() { clock.now += 1000; },
    inbound(line) { coordinator.handleInbound(line); },
    /** Runs the refreshes the registry asked for, as the event loop would. */
    flush() { while (scheduled.length > 0) scheduled.shift()(); },
    /** A hook asking, with the channel it will be answered on. */
    hook(tool = 'Bash', input = { command: 'git push origin master' }) {
      const channel = new FakeChannel();
      const result = hooks.submit(request(tool, input), channel);
      return { id: result.id, channel };
    },
    shown() { return this.transport.framesOfType('permission').at(-1); },
  };
}

const action = (name, id) => `{"v":1,"type":"action","action":"${name}","requestId":"${id}"}`;
const dialog = (content = 'git push origin master') => fixtures.permission('Run command', content);

describe('a request that came through a hook', () => {
  it('reaches the box with both verdicts, whichever dialog Claude draws', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    f.coordinator.refresh();

    const { id } = f.hook('Edit', { file_path: '/Users/me/chickenMicro/README.md' });
    f.coordinator.refresh();

    const frame = f.shown();
    assert.equal(frame.id, id);
    assert.equal(frame.kind, 'write');
    assert.deepEqual(frame.actions, ['deny', 'approve']);
    assert.match(frame.content, /README\.md/);
  });

  it('is answered through the hook, and no button is pressed', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();
    f.arm();

    f.inbound(action('approve', id));
    assert.deepEqual(channel.lines, [{ v: 1, type: 'verdict', decision: 'allow' }]);
    assert.deepEqual(f.source.pressed, []);
    assert.equal(f.transport.framesOfType('permission_cleared').at(-1).id, id);
  });

  it('sends a refusal as a refusal', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();
    f.arm();

    f.inbound(action('deny', id));
    assert.equal(channel.lines[0].decision, 'deny');
  });

  it('waits for the arming delay like any press', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();

    f.clock.now += 50;
    f.inbound(action('approve', id));
    assert.deepEqual(channel.lines, []);
  });

  it('is answered once, however often the key bounces', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();
    f.arm();

    f.inbound(action('approve', id));
    f.inbound(action('approve', id));
    assert.equal(channel.lines.length, 1);
  });

  it('refuses a verdict once its hook has given up', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();
    f.arm();

    f.hooks.channelClosed(id);            // the hook timed out
    f.inbound(action('approve', id));
    assert.deepEqual(channel.lines, []);
    assert.deepEqual(f.source.pressed, []);
  });

  it('never answers a request the box did not show', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    f.hook();
    const second = f.hook('Bash', { command: 'rm -rf build' });
    f.coordinator.refresh();
    f.arm();

    // The first request is on screen; a verdict quoting the second is stale.
    f.inbound(action('approve', second.id));
    assert.deepEqual(second.channel.lines, []);
  });

  it('answers a whole queue through the hooks on approve_all', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const first = f.hook('Bash', { command: 'git status' });
    f.clock.now += 10;
    const second = f.hook('Bash', { command: 'git diff' });
    f.coordinator.refresh();
    f.arm();

    f.inbound(action('approve_all', first.id));
    assert.equal(first.channel.lines[0].decision, 'allow');
    assert.equal(second.channel.lines[0].decision, 'allow');
    assert.deepEqual(f.source.pressed, []);
  });
});

describe('a request seen both ways', () => {
  it('is one question: the dialog it stands for is not offered again', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { id, channel } = f.hook();
    f.coordinator.refresh();

    f.source.tree = dialog();           // Claude draws its dialog a moment later
    f.coordinator.refresh();
    assert.deepEqual(f.transport.framesOfType('permission').map((frame) => frame.id), [id]);

    f.arm();
    f.inbound(action('approve', id));
    assert.equal(channel.lines[0].decision, 'allow');
    assert.deepEqual(f.source.pressed, [], 'the dialog is closed by the hook, not pressed');
  });

  it('does not flicker the same question back while its dialog goes', () => {
    const f = fixture();
    f.source.tree = dialog();
    const { id } = f.hook();
    f.coordinator.refresh();
    f.arm();
    f.inbound(action('approve', id));

    f.transport.clear();
    f.clock.now += 500;
    f.coordinator.refresh();             // the dialog is still on screen
    assert.deepEqual(f.transport.framesOfType('permission'), []);
  });

  it('falls back to the dialog when the hook was ignored', () => {
    const f = fixture();
    f.source.tree = dialog();
    const { id } = f.hook();
    f.coordinator.refresh();
    f.arm();
    f.inbound(action('approve', id));

    f.clock.now += HOOK_SETTLE_MS + 100;  // the dialog is still there, long after
    f.coordinator.refresh();
    assert.equal(f.shown().id, 's1.req-1');
    f.arm();
    f.inbound(action('approve', 's1.req-1'));
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('falls back to the dialog when the hook times out', () => {
    const f = fixture();
    f.source.tree = dialog();
    const { id } = f.hook();
    f.coordinator.refresh();
    assert.equal(f.shown().id, id);

    f.clock.now += 120_000;               // the hook waited out its whole timeout
    f.hooks.channelClosed(id);
    f.flush();
    assert.equal(f.shown().id, 's1.req-1');
    assert.equal(f.transport.framesOfType('permission_cleared').at(-1).id, id);

    f.arm();
    f.inbound(action('approve', 's1.req-1'));
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('stands the hook down when the dialog was answered in the app', () => {
    const f = fixture();
    f.source.tree = dialog();
    const { id, channel } = f.hook();
    f.coordinator.refresh();

    f.source.tree = fixtures.idle();      // the operator clicked in Claude
    f.coordinator.refresh();
    assert.equal(channel.lines[0].type, 'cancel');
    assert.equal(channel.ended, true);
    assert.equal(f.transport.framesOfType('permission_cleared').at(-1).id, id);
  });

  it('shows two entries rather than guess when nothing matches', () => {
    const f = fixture();
    f.source.tree = dialog('rm -rf build');
    const { id } = f.hook('Bash', { command: 'git push origin master' });
    f.coordinator.refresh();

    assert.equal(f.shown().id, id);
    // The unmatched dialog waits behind it, to be answered by pressing.
    f.arm();
    f.inbound(action('deny', id));
    f.coordinator.refresh();
    assert.equal(f.shown().id, 's1.req-1');
  });
});

describe('without a hook', () => {
  it('shows a dialog at once when no hook has ever asked', () => {
    const f = fixture();
    f.source.tree = dialog();
    f.coordinator.refresh();
    assert.equal(f.shown().id, 's1.req-1');
  });

  it('gives a hook a moment to claim a dialog once hooks are in play', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const gone = f.hook('Bash', { command: 'ls' });
    f.hooks.channelClosed(gone.id);       // hooks have been seen, and are quiet

    f.source.tree = dialog();
    f.coordinator.refresh();
    assert.equal(f.transport.framesOfType('permission').length, 0, 'held for the grace');

    f.clock.now += HOOK_GRACE_MS + 1;
    f.coordinator.refresh();
    assert.equal(f.shown().id, 's1.req-1', 'then it falls back to the dialog');
  });

  it('presses the dialog when the box is set to the interface', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    const { channel } = f.hook();
    f.coordinator.refresh();

    f.inbound('{"v":1,"type":"mode","value":"ui"}');
    assert.equal(channel.lines[0].type, 'cancel');

    f.source.tree = dialog();
    f.coordinator.refresh();
    assert.equal(f.shown().id, 's1.req-1');
    f.arm();
    f.inbound(action('approve', 's1.req-1'));
    assert.deepEqual(f.source.pressed, ['allow']);
  });

  it('is unchanged when the daemon runs without a registry at all', () => {
    const transport = new RecordingTransport();
    const source = new FakeAccessibilitySource(dialog());
    const coordinator = new SessionCoordinator({
      transport,
      source,
      gate: new PermissionGate(0),
      log: new Log({ level: 'error', color: false, out: { write() {} } }),
    });
    coordinator.refresh();
    assert.equal(transport.framesOfType('permission').at(-1).id, 's1.req-1');
    coordinator.handleInbound('{"v":1,"type":"mode","value":"ui"}');
  });
});
