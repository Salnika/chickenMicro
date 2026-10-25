import assert from 'node:assert/strict';
import {
  chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync,
} from 'node:fs';
import net from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { describe, it } from 'node:test';

import {
  HookRegistry,
  HOOK_DONE_MS,
  HOOK_GRACE_MS,
  HOOK_SESSION_TTL_MS,
  HOOK_SETTLE_MS,
  HOOK_THINKING_STALE_MS,
  frontEndOf,
} from '../src/hook/registry.js';
import { HookServer } from '../src/hook/server.js';
import { sessionNameOf, summarise } from '../src/hook/summary.js';
import {
  HOOK_MAX_LINE_BYTES, HookEvent, decodeHookLine, decodeHookRequest, encodeHook,
} from '../src/hook/wire.js';
import { Log } from '../src/log.js';
import { SESSIONS_MAX, SESSION_NAME_MAX_CHARS } from '../src/protocol/constants.js';
import { decodeInbound, encode } from '../src/protocol/frames.js';
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
    assert.deepEqual(decoded, { tool: 'Bash', session: 's', cwd: '/x', entrypoint: '', input: { a: 1 } });
  });

  it('carries the front-end a session runs in, when the hook says', () => {
    const { request } = decodeHookRequest(line({ tool: 'Bash', session: 's', entrypoint: 'cli' }));
    assert.equal(request.entrypoint, 'cli');
    assert.notEqual(decodeHookRequest(line({ tool: 'Bash', entrypoint: 'x'.repeat(100) })).rejected,
                    undefined);
  });

  describe('an event', () => {
    const event = (frame) => JSON.stringify({ v: 1, type: 'event', ...frame });

    it('is accepted for each thing that can happen in a session', () => {
      for (const name of ['SessionStart', 'UserPromptSubmit', 'Stop', 'StopFailure', 'SessionEnd']) {
        const decoded = decodeHookLine(event({ event: name, session: 's', cwd: '/x', entrypoint: 'cli' }));
        assert.deepEqual(decoded.event, {
          name, session: 's', cwd: '/x', entrypoint: 'cli', title: '', source: '',
        });
      }
    });

    it('carries the name a user gave the session, cut rather than refused when long', () => {
      const decoded = decodeHookLine(event({
        event: 'SessionStart', session: 's', title: 'firmware', source: 'resume',
      }));
      assert.equal(decoded.event.title, 'firmware');
      assert.equal(decoded.event.source, 'resume');

      const long = decodeHookLine(event({ event: 'UserPromptSubmit', session: 's', title: 'x'.repeat(500) }));
      assert.equal(long.event.title.length, 128);
      const odd = decodeHookLine(event({ event: 'SessionStart', session: 's', source: 'x'.repeat(40), title: 7 }));
      assert.equal(odd.event.source, '', 'a start source is a short word, or nothing');
      assert.equal(odd.event.title, '', 'a title is a string, or nothing');
    });

    it('is refused when it names nothing it knows, or no session', () => {
      for (const bad of [
        event({ event: 'PreToolUse', session: 's' }),
        event({ session: 's' }),
        event({ event: 'Stop' }),
        event({ event: 'Stop', session: '', cwd: '' }),
        event({ event: 'Stop', session: 'x'.repeat(500) }),
        event({ event: 'Stop', session: 's', entrypoint: 'x'.repeat(100) }),
      ]) {
        assert.notEqual(decodeHookLine(bad).rejected, undefined, bad);
      }
    });

    it('is not mistaken for a request, nor a request for an event', () => {
      assert.notEqual(decodeHookRequest(event({ event: 'Stop', session: 's' })).rejected, undefined);
      assert.notEqual(decodeHookLine(line({ tool: 'Bash' })).request, undefined);
      assert.notEqual(decodeHookLine(JSON.stringify({ v: 1, type: 'verdict' })).rejected, undefined);
    });
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

  it('leaves the path of its own node where a hook looks for one', async () => {
    // A hook may start with a PATH that has no node on it; the daemon, which is
    // running under one that works, says which.
    await withServer(async ({ path }) => {
      assert.equal(readFileSync(join(dirname(path), 'node'), 'utf8').trim(), process.execPath);
    });
  });

  /** Starts a server in a directory set up by `prepare`, and hands both to `run`. */
  async function inDirectory(prepare, run) {
    const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
    prepare(directory);
    const server = new HookServer({
      registry: new HookRegistry(),
      log: quiet(),
      path: join(directory, 'hook.sock'),
    });
    try {
      await server.start();
      await run({ directory, server });
    } finally {
      server.stop();
      rmSync(directory, { recursive: true, force: true });
    }
  }

  it('keeps the hook channel up when it cannot leave the path of node', async () => {
    // The path is a convenience for the launcher, which falls back to the PATH. A
    // failure to leave it must not leave a socket listening that nothing serves.
    await inDirectory(
      (directory) => mkdirSync(join(directory, 'node')),
      async ({ directory, server }) => {
        const client = net.connect(server.path);
        await new Promise((resolve, reject) => {
          client.once('connect', resolve);
          client.once('error', reject);
        });
        client.destroy();
        assert.equal(existsSync(join(directory, '.node.' + process.pid)), false,
                     'no half-written file is left behind');
      },
    );
  });

  it('does not write through a link standing where the path of node goes', async () => {
    let victim;
    await inDirectory(
      (directory) => {
        victim = join(directory, 'victim');
        writeFileSync(victim, 'keep');
        symlinkSync(victim, join(directory, 'node'));
      },
      async ({ directory }) => {
        assert.equal(readFileSync(victim, 'utf8'), 'keep');
        assert.equal(readFileSync(join(directory, 'node'), 'utf8').trim(), process.execPath);
      },
    );
  });

  it('leaves no path of node in a directory other users can write to', async () => {
    // The launcher runs what it finds there, so a shared directory is no place for it.
    await inDirectory(
      (directory) => chmodSync(directory, 0o777),
      async ({ directory }) => {
        assert.equal(existsSync(join(directory, 'node')), false);
      },
    );
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

describe('the sessions hooks have spoken for', () => {
  const UUID_A = '3f2a9c1e-7b44-4d2a-9d11-aaaaaaaaaaaa';
  const UUID_B = '9b1d0e55-0c2f-41c8-8a37-bbbbbbbbbbbb';

  it('gives each one a short id of its own, however long its own is', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    r.submit(request('Bash', { command: 'pwd' }, { session: UUID_B, cwd: '/Users/me/docs' }),
             new FakeChannel());

    const sessions = r.sessions();
    assert.deepEqual(sessions.map((s) => s.id), ['c1', 'c2']);
    assert.deepEqual(sessions.map((s) => s.name), ['chickenMicro', 'docs']);
    for (const session of sessions) {
      assert.ok(session.id.length <= 16, 'the wire allows 16 characters, a UUID has 36');
    }
  });

  it('keeps one id for one session across all its requests', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    r.submit(request('Bash', { command: 'pwd' }, { session: UUID_A }), new FakeChannel());
    assert.deepEqual(r.sessions().map((s) => s.id), ['c1']);
    const [first, second] = r.live();
    assert.equal(first.sessionId, second.sessionId);
  });

  it('tells two sessions in one folder apart, and only those', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    assert.equal(r.sessions()[0].name, 'chickenMicro', 'alone, it keeps the bare name');

    r.submit(request('Bash', { command: 'pwd' }, { session: UUID_B }), new FakeChannel());
    r.submit(request('Bash', { command: 'id' }, { session: 'other', cwd: '/Users/me/docs' }),
             new FakeChannel());
    assert.deepEqual(r.sessions().map((s) => s.name),
                     ['chickenMicro (3f2a)', 'chickenMicro (9b1d)', 'docs']);
  });

  it('says which of them is waiting', () => {
    const r = new HookRegistry();
    const { id } = r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    r.submit(request('Bash', { command: 'pwd' }, { session: UUID_B, cwd: '/Users/me/docs' }),
             new FakeChannel());
    assert.deepEqual(r.sessions().map((s) => s.pending), [true, true]);

    r.answer(id, 'allow');
    assert.deepEqual(r.sessions().map((s) => s.pending), [false, true]);
  });

  it('lets a quiet session age out, but never one that is waiting', () => {
    const now = { t: 1_000_000 };
    const r = new HookRegistry({ now: () => now.t });
    const waiting = r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    const done = r.submit(request('Bash', { command: 'pwd' }, { session: UUID_B, cwd: '/Users/me/docs' }),
                          new FakeChannel());
    r.answer(done.id, 'allow');

    now.t += HOOK_SESSION_TTL_MS + 1;
    assert.deepEqual(r.sessions().map((s) => s.id), ['c1'], 'only the one still asking stays');

    r.answer(waiting.id, 'allow');
    now.t += HOOK_SESSION_TTL_MS + 1;
    assert.deepEqual(r.sessions(), []);
  });

  it('does not list a session whose request was turned away', () => {
    const r = new HookRegistry();
    r.setMode('ui');
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    assert.deepEqual(r.sessions(), []);
  });

  it('keeps the tag where the box cuts a long name', () => {
    // The box cuts a name at its end, and the tag is at its end: it is the folder
    // that has to give way, or two sessions read alike.
    const r = new HookRegistry();
    const cwd = '/Users/me/claude-remote-feature-oauth-rewrite';
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A, cwd }), new FakeChannel());
    r.submit(request('Bash', { command: 'pwd' }, { session: UUID_B, cwd }), new FakeChannel());

    const names = r.sessions().map((s) => s.name);
    assert.equal(new Set(names).size, 2);
    for (const name of names) assert.ok(name.length <= SESSION_NAME_MAX_CHARS, name);
    assert.match(names[0], /\(3f2a\)$/);
    assert.match(names[1], /\(9b1d\)$/);

    const sent = JSON.parse(encode.sessions(r.sessions().map((s) => ({ ...s, state: 'idle' }))));
    assert.deepEqual(sent.sessions.map((s) => s.name), names, 'what is sent is what was named');
  });

  it('tells two folders of one name apart when their sessions have no id', () => {
    const r = new HookRegistry();
    // The paths differ in their first characters on purpose: a tag taken from the
    // path would then differ too, and the names would not be the short ids.
    r.submit(request('Bash', { command: 'ls' }, { session: '', cwd: '/a/app' }),
             new FakeChannel());
    r.submit(request('Bash', { command: 'ls' }, { session: '', cwd: '/b/app' }),
             new FakeChannel());
    assert.deepEqual(r.sessions().map((s) => s.name), ['app (c1)', 'app (c2)']);
  });

  /** What the box would draw for each session, as the sessions frame encodes it. */
  const drawn = (r) => JSON.parse(
    encode.sessions(r.sessions().map((s) => ({ ...s, state: 'idle' }))),
  ).sessions.map((s) => s.name);

  it('tags folders whose names the box cuts to the same text', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A, cwd: '/w/chickenMicro-feature-oauth-rewrite-v1' }),
             new FakeChannel());
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_B, cwd: '/w/chickenMicro-feature-oauth-rewrite-v2' }),
             new FakeChannel());

    const names = drawn(r);
    assert.equal(new Set(names).size, 2, `both read ${names[0]}`);
    assert.match(names[0], /\(3f2a\)$/);
    assert.match(names[1], /\(9b1d\)$/);
  });

  it('tags folders whose names the box folds to the same text', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A, cwd: '/w/cafe' }), new FakeChannel());
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_B, cwd: '/w/café' }), new FakeChannel());

    const names = drawn(r);
    assert.equal(new Set(names).size, 2, `both read ${names[0]}`);
  });

  it('keeps the tag when folding makes a long name longer', () => {
    // ß is drawn as ss: measured before folding, the name would fit and the box
    // would then cut the tag off the end.
    const r = new HookRegistry();
    const cwd = '/w/Straßenbau-Maßnahmenkatalog-Übersicht';
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_A, cwd }), new FakeChannel());
    r.submit(request('Bash', { command: 'ls' }, { session: UUID_B, cwd }), new FakeChannel());

    const names = drawn(r);
    assert.match(names[0], /\(3f2a\)$/);
    assert.match(names[1], /\(9b1d\)$/);
  });

  it('does not tell sessions apart by a tag they share', () => {
    const r = new HookRegistry();
    r.submit(request('Bash', { command: 'ls' }, { session: 'abcd-1111' }), new FakeChannel());
    r.submit(request('Bash', { command: 'ls' }, { session: 'abcd-2222' }), new FakeChannel());
    assert.deepEqual(r.sessions().map((s) => s.name),
                     ['chickenMicro (c1)', 'chickenMicro (c2)']);
  });

  it('drops the quietest session, not the newest, when more are heard than the box lists', () => {
    const now = { t: 1_000_000 };
    const r = new HookRegistry({ now: () => now.t });
    const first = r.submit(request('Bash', { command: 'ls' }, { session: 'first', cwd: '/x/first' }),
                           new FakeChannel());
    for (let i = 2; i <= SESSIONS_MAX; i += 1) {
      now.t += 1000;
      const { id } = r.submit(request('Bash', { command: 'ls' }, { session: `s${i}`, cwd: `/x/p${i}` }),
                              new FakeChannel());
      r.answer(id, 'allow');
    }
    // The first session is still waiting; the next one to speak must push out a
    // quiet one, not it and not itself.
    now.t += 1000;
    r.submit(request('Bash', { command: 'ls' }, { session: 'ninth', cwd: '/x/ninth' }),
             new FakeChannel());

    const sessions = r.sessions();
    assert.equal(sessions.length, SESSIONS_MAX);
    assert.ok(sessions.some((s) => s.name === 'ninth'), 'the one that is asking is listed');
    assert.ok(sessions.some((s) => s.name === 'first' && s.pending), 'so is the one still waiting');
    assert.ok(!sessions.some((s) => s.name === 'p2'), 'the quietest one went');
    assert.ok(first.accepted);
  });
});

describe('with no window to read', () => {
  const UUID_A = '3f2a9c1e-7b44-4d2a-9d11-aaaaaaaaaaaa';
  const UUID_B = '9b1d0e55-0c2f-41c8-8a37-bbbbbbbbbbbb';

  it('lists the session a hook spoke for, where the box used to say there is none', () => {
    const f = fixture();
    f.coordinator.refresh();
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'disconnected');

    const { id } = f.hook('Bash', { command: 'ls' });
    f.coordinator.refresh();

    const list = f.transport.framesOfType('sessions').at(-1);
    assert.deepEqual(list.sessions, [{ id: 'c1', name: 'chickenMicro', state: 'waiting_permission' }]);
    assert.equal(list.focus, 'c1');
    assert.equal(list.permission, id);
    assert.equal(f.shown().session, 'c1', 'the request names the session it came from');
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'waiting_permission');
  });

  it('is answered through the hook, and goes back to idle rather than to nothing', () => {
    const f = fixture();
    const { id, channel } = f.hook('Bash', { command: 'ls' });
    f.coordinator.refresh();
    f.arm();

    f.inbound(action('approve', id));
    assert.equal(channel.lines[0].decision, 'allow');
    assert.deepEqual(f.source.pressed, []);
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'idle');
    assert.deepEqual(f.transport.framesOfType('sessions').at(-1).sessions,
                     [{ id: 'c1', name: 'chickenMicro', state: 'idle' }]);
  });

  it('shows two sessions of one folder under names that differ', () => {
    const f = fixture();
    f.hooks.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    f.hooks.submit(request('Bash', { command: 'pwd' }, { session: UUID_B }), new FakeChannel());
    f.coordinator.refresh();

    const names = f.transport.framesOfType('sessions').at(-1).sessions.map((s) => s.name);
    assert.deepEqual(names, ['chickenMicro (3f2a)', 'chickenMicro (9b1d)']);
  });

  it('names a session by its short id on the wire, never by the id Claude gave it', () => {
    // Claude's own id is a 36-character UUID; the wire takes 16, and an id it
    // refuses is an id the box cannot send an action back for.
    const f = fixture();
    f.hooks.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    f.coordinator.refresh();

    const permission = f.shown();
    const list = f.transport.framesOfType('sessions').at(-1);
    assert.equal(permission.session, 'c1');
    assert.equal(list.focus, permission.session);
    assert.deepEqual(list.sessions.map((s) => s.id), ['c1']);
    assert.ok(permission.session.length <= 16);
  });

  /** Two sessions, both answered, so that both are idle. */
  function twoIdleSessions() {
    const f = fixture();
    const a = f.hooks.submit(request('Bash', { command: 'ls' }, { session: UUID_A }), new FakeChannel());
    const b = f.hooks.submit(
      request('Bash', { command: 'pwd' }, { session: UUID_B, cwd: '/Users/me/docs' }),
      new FakeChannel(),
    );
    f.coordinator.refresh();
    f.arm();
    f.inbound(action('approve', a.id));
    f.coordinator.refresh();
    f.arm();
    f.inbound(action('approve', b.id));
    f.coordinator.refresh();
    return f;
  }

  it('describes the session that is asking, not the first one listed', () => {
    const f = twoIdleSessions();
    assert.deepEqual(f.transport.framesOfType('sessions').at(-1).sessions.map((s) => s.state),
                     ['idle', 'idle']);

    const { id } = f.hooks.submit(
      request('Bash', { command: 'id' }, { session: UUID_B, cwd: '/Users/me/docs' }),
      new FakeChannel(),
    );
    f.coordinator.refresh();

    const list = f.transport.framesOfType('sessions').at(-1);
    assert.equal(list.focus, 'c2');
    assert.equal(list.permission, id);
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'waiting_permission');
  });

  it('follows the session the box picks, when no window is read', () => {
    const f = twoIdleSessions();
    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 'c1');

    f.inbound('{"v":1,"type":"focus","sessionId":"c2"}');
    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 'c2');
    assert.ok(!f.logged.some((line) => /cannot follow/.test(line)));

    // One that is not listed changes nothing, and says so.
    f.inbound('{"v":1,"type":"focus","sessionId":"c9"}');
    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 'c2');
    assert.ok(f.logged.some((line) => /cannot follow c9/.test(line)));
  });

  it('goes to the session that asks, even after another was picked', () => {
    const f = twoIdleSessions();
    f.inbound('{"v":1,"type":"focus","sessionId":"c2"}');

    f.hooks.submit(request('Bash', { command: 'id' }, { session: UUID_A }), new FakeChannel());
    f.coordinator.refresh();
    assert.equal(f.transport.framesOfType('sessions').at(-1).focus, 'c1');
  });

  it('forgets a session once it has been quiet long enough', () => {
    const f = fixture();
    const { id } = f.hook('Bash', { command: 'ls' });
    f.coordinator.refresh();
    f.arm();
    f.inbound(action('approve', id));

    f.clock.now += HOOK_SESSION_TTL_MS + 1000;
    f.coordinator.refresh();
    assert.deepEqual(f.transport.framesOfType('sessions').at(-1).sessions, []);
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'disconnected');
  });

  it('leaves the list to the windows when there are any', () => {
    // Nothing connects a hook's session to a window, so listing both would show
    // one conversation twice.
    const f = fixture();
    f.source.tree = fixtures.idle();
    f.hook('Bash', { command: 'ls' });
    f.coordinator.refresh();

    const list = f.transport.framesOfType('sessions').at(-1);
    assert.deepEqual(list.sessions.map((s) => s.id), ['s1']);
    assert.equal(f.shown().session, undefined, 'the request names no window of its own');
  });
});

// ---------------------------------------------------------------------------
// What hooks say about a session that has asked for nothing: it started, was
// given a prompt, finished, ended.
// ---------------------------------------------------------------------------

describe('what hooks say about a session', () => {
  const SESSION = '3f2a9c1e-7b44-4d2a-9d11-aaaaaaaaaaaa';
  const OTHER = '9b1d0e55-0c2f-41c8-8a37-bbbbbbbbbbbb';
  const tell = (r, name, extra = {}) => r.event({
    name, session: SESSION, cwd: '/Users/me/chickenMicro', entrypoint: 'cli', ...extra,
  });
  const clock = () => {
    const time = { t: 1_000_000 };
    return { time, registry: new HookRegistry({ now: () => time.t }) };
  };

  it('lists a session that has asked for nothing', () => {
    const { registry } = clock();
    assert.equal(tell(registry, HookEvent.sessionStart), true);
    assert.deepEqual(registry.sessions().map(({ id, name, state, pending }) => ({ id, name, state, pending })),
                     [{ id: 'c1', name: 'chickenMicro', state: 'idle', pending: false }]);
  });

  it('reads thinking from a prompt to the end of the turn', () => {
    const { time, registry } = clock();
    tell(registry, HookEvent.sessionStart);
    tell(registry, HookEvent.prompt);
    assert.equal(registry.sessions()[0].state, 'thinking');

    tell(registry, HookEvent.stop);
    assert.equal(registry.sessions()[0].state, 'done');
    time.t += HOOK_DONE_MS + 1;
    assert.equal(registry.sessions()[0].state, 'idle', 'done is only shown for a moment');
  });

  it('does not stay thinking about a turn that ended without a Stop', () => {
    const { time, registry } = clock();
    tell(registry, HookEvent.prompt);
    time.t += HOOK_THINKING_STALE_MS + 1;
    assert.equal(registry.sessions()[0].state, 'idle');
  });

  it('keeps thinking through a start that is only a resume or a compaction', () => {
    const { registry } = clock();
    tell(registry, HookEvent.prompt);
    tell(registry, HookEvent.sessionStart);
    assert.equal(registry.sessions()[0].state, 'thinking');
  });

  it('forgets a session the moment it ends, however recently it spoke', () => {
    const { registry } = clock();
    tell(registry, HookEvent.prompt);
    tell(registry, HookEvent.sessionEnd);
    assert.deepEqual(registry.sessions(), []);
    assert.equal(tell(registry, HookEvent.sessionEnd), false, 'ending twice is not an event');
  });

  it('comes back, as a new session, with the next prompt', () => {
    const { registry } = clock();
    tell(registry, HookEvent.prompt);
    tell(registry, HookEvent.sessionEnd);
    tell(registry, HookEvent.prompt);
    assert.deepEqual(registry.sessions().map((s) => s.id), ['c2']);
  });

  it('keeps a session through a long quiet, and not for ever', () => {
    const { time, registry } = clock();
    tell(registry, HookEvent.sessionStart);
    time.t += HOOK_SESSION_TTL_MS - 1000;
    assert.equal(registry.sessions().length, 1);
    time.t += 2000;
    assert.deepEqual(registry.sessions(), []);
  });

  it('is still thinking once a request is answered, when it was thinking before', () => {
    const { registry } = clock();
    tell(registry, HookEvent.prompt);
    const { id } = registry.submit(request('Bash', { command: 'ls' }, { session: SESSION }), new FakeChannel());
    registry.answer(id, 'allow');
    assert.equal(registry.sessions()[0].state, 'thinking');
  });

  it('knows nothing between two requests of a session that sent no events', () => {
    const { registry } = clock();
    const { id } = registry.submit(request('Bash', { command: 'ls' }, { session: SESSION }), new FakeChannel());
    registry.answer(id, 'allow');
    assert.equal(registry.sessions()[0].state, 'idle');
  });

  it('is deaf to events when the box answers through the interface', () => {
    const { registry } = clock();
    registry.setMode('ui');
    assert.equal(tell(registry, HookEvent.prompt), false);
    assert.deepEqual(registry.sessions(), []);
  });

  it('never lists a headless run, whatever it says', () => {
    const { registry } = clock();
    assert.equal(tell(registry, HookEvent.prompt, { entrypoint: 'sdk-cli' }), false);
    registry.submit(request('Bash', { command: 'ls' }, { session: OTHER, entrypoint: 'sdk-ts' }),
                    new FakeChannel());
    assert.deepEqual(registry.sessions(), []);
    assert.deepEqual(registry.sessions(undefined, { windowsRead: true }), []);
  });

  describe('beside the windows of Claude Desktop', () => {
    it('lists a terminal or an editor, and not what Desktop already shows', () => {
      const { registry } = clock();
      tell(registry, HookEvent.prompt, { session: 'term', cwd: '/w/a', entrypoint: 'cli' });
      tell(registry, HookEvent.prompt, { session: 'edit', cwd: '/w/b', entrypoint: 'claude-vscode' });
      tell(registry, HookEvent.prompt, { session: 'desk', cwd: '/w/c', entrypoint: 'claude-desktop' });
      tell(registry, HookEvent.prompt, { session: 'old', cwd: '/w/d', entrypoint: '' });

      assert.deepEqual(registry.sessions(undefined, { windowsRead: true }).map((s) => s.name), ['a', 'b']);
      assert.deepEqual(registry.sessions().map((s) => s.name), ['a', 'b', 'c', 'd'],
                       'with no window read, nothing is shown twice, so all are listed');
    });

    it('does not tag a name because of a session that is left out', () => {
      const { registry } = clock();
      tell(registry, HookEvent.prompt, { session: 'term', cwd: '/w/app', entrypoint: 'cli' });
      tell(registry, HookEvent.prompt, { session: 'desk', cwd: '/w/app', entrypoint: 'claude-desktop' });
      assert.deepEqual(registry.sessions(undefined, { windowsRead: true }).map((s) => s.name), ['app']);
    });
  });

  it('sorts where a session runs', () => {
    assert.equal(frontEndOf('cli'), 'own');
    assert.equal(frontEndOf('claude-vscode'), 'own');
    assert.equal(frontEndOf('claude-desktop'), 'desktop');
    assert.equal(frontEndOf('claude-desktop-3p'), 'desktop');
    assert.equal(frontEndOf('sdk-cli'), 'headless');
    assert.equal(frontEndOf('sdk-ts'), 'headless');
    assert.equal(frontEndOf('local-agent'), 'headless');
    assert.equal(frontEndOf('claude-in-slack'), 'headless');
    assert.equal(frontEndOf(''), 'unknown');
  });
});

describe('sessions beside the windows', () => {
  const SESSION = '3f2a9c1e-7b44-4d2a-9d11-aaaaaaaaaaaa';
  const tell = (f, name, extra = {}) => f.hooks.event({
    name, session: SESSION, cwd: '/Users/me/docs', entrypoint: 'cli', ...extra,
  });
  const states = (f) => f.transport.framesOfType('sessions').at(-1);

  it('lists a terminal session after the windows, though it never asked for anything', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();

    assert.deepEqual(states(f).sessions.map((s) => [s.id, s.state]), [['s1', 'idle'], ['c1', 'thinking']]);
    assert.equal(states(f).sessions[1].name, 'docs');
  });

  it('shows a session that Desktop shows once', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt, { entrypoint: 'claude-desktop' });
    f.coordinator.refresh();
    assert.deepEqual(states(f).sessions.map((s) => s.id), ['s1']);
  });

  it('keeps describing the window in front until a terminal session asks', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's1');

    const { id } = f.hooks.submit(
      request('Bash', { command: 'ls' }, { session: SESSION, cwd: '/Users/me/docs', entrypoint: 'cli' }),
      new FakeChannel(),
    );
    f.coordinator.refresh();
    assert.equal(states(f).focus, 'c1');
    assert.equal(states(f).permission, id);
    assert.equal(f.shown().session, 'c1', 'the request names the session it came from');
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'waiting_permission');

    f.arm();
    f.inbound(action('approve', id));
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's1', 'answered, the box goes back to the window in front');
  });

  it('follows the terminal session the box picks, and a window when it picks one back', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();

    f.inbound('{"v":1,"type":"focus","sessionId":"c1"}');
    assert.equal(states(f).focus, 'c1');
    assert.equal(f.transport.framesOfType('state').at(-1).state, 'thinking');

    f.coordinator.refresh();
    assert.equal(states(f).focus, 'c1', 'it stays picked across a re-read');

    f.inbound('{"v":1,"type":"focus","sessionId":"s1"}');
    assert.equal(states(f).focus, 's1');
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's1');
  });

  it('presses nothing in a window when STOP is pressed on a terminal session', () => {
    const f = fixture();
    f.source.tree = fixtures.workspace([
      { name: 'one', tree: fixtures.thinking('first', 'a.') },
    ]);
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();

    f.inbound('{"v":1,"type":"action","action":"stop","sessionId":"c1"}');
    assert.deepEqual(f.source.pressed, [], 'the window being followed is a different conversation');

    f.inbound('{"v":1,"type":"action","action":"stop","sessionId":"s1"}');
    assert.deepEqual(f.source.pressed, ['a.stop']);
  });

  it('drops a session from the list when it ends', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    tell(f, HookEvent.sessionEnd);
    f.coordinator.refresh();
    assert.deepEqual(states(f).sessions.map((s) => s.id), ['s1']);
  });

  it('shows a turn ending as done, then idle', () => {
    const f = fixture();
    f.source.tree = fixtures.idle();
    tell(f, HookEvent.prompt);
    tell(f, HookEvent.stop);
    f.coordinator.refresh();
    assert.equal(states(f).sessions[1].state, 'done');

    f.clock.now += HOOK_DONE_MS + 1;
    f.coordinator.refresh();
    assert.equal(states(f).sessions[1].state, 'idle');
  });
});

// ---------------------------------------------------------------------------
// What a review of the session lifecycle found, one test for each thing.
// ---------------------------------------------------------------------------

describe('what hooks say about a session, after review', () => {
  const SESSION = '3f2a9c1e-7b44-4d2a-9d11-aaaaaaaaaaaa';
  const tell = (r, name, extra = {}) => r.event({
    name, session: SESSION, cwd: '/Users/me/chickenMicro', entrypoint: 'cli', ...extra,
  });
  const clock = () => {
    const time = { t: 1_000_000 };
    return { time, registry: new HookRegistry({ now: () => time.t }) };
  };
  const real = (r, n) => r.event({
    name: HookEvent.prompt, session: `real-${n}`, cwd: `/w/real${n}`, entrypoint: 'cli',
  });

  it('reads a turn that ended on an API error as over, since Stop does not run then', () => {
    const { registry } = clock();
    tell(registry, HookEvent.prompt);
    assert.equal(registry.sessions()[0].state, 'thinking');
    assert.equal(tell(registry, HookEvent.stopFailure), true);
    assert.equal(registry.sessions()[0].state, 'idle');
  });

  it('starts afresh on a resume, a clear, a fork or a start, and not on a compaction', () => {
    for (const source of ['resume', 'clear', 'fork', 'startup']) {
      const { registry } = clock();
      tell(registry, HookEvent.prompt);
      tell(registry, HookEvent.sessionStart, { source });
      assert.equal(registry.sessions()[0].state, 'idle', source);
    }
    for (const source of ['compact', '']) {
      const { registry } = clock();
      tell(registry, HookEvent.prompt);
      tell(registry, HookEvent.sessionStart, { source });
      assert.equal(registry.sessions()[0].state, 'thinking', `"${source}"`);
    }
  });

  it('names a session by the title its user gave it, until a prompt comes without one', () => {
    const { registry } = clock();
    tell(registry, HookEvent.sessionStart, { title: 'firmware' });
    assert.equal(registry.sessions()[0].name, 'firmware');

    registry.submit(request('Bash', { command: 'ls' }, { session: SESSION }), new FakeChannel());
    tell(registry, HookEvent.stop);
    assert.equal(registry.sessions()[0].name, 'firmware', 'a request and a Stop carry no title, and keep it');

    tell(registry, HookEvent.prompt);
    assert.equal(registry.sessions()[0].name, 'chickenMicro', 'renamed to nothing, it is the folder again');
  });

  it('tells two sessions of one title apart', () => {
    const { registry } = clock();
    tell(registry, HookEvent.sessionStart, { title: 'docs' });
    tell(registry, HookEvent.sessionStart, { session: 'other-1', title: 'docs' });
    assert.deepEqual(registry.sessions().map((s) => s.name), ['docs (3f2a)', 'docs (othe)']);
  });

  it('takes a permission request for a sign that a long turn is still going', () => {
    const { time, registry } = clock();
    tell(registry, HookEvent.prompt);
    time.t += HOOK_THINKING_STALE_MS - 60_000;
    const { id } = registry.submit(request('Bash', { command: 'ls' }, { session: SESSION }), new FakeChannel());
    registry.answer(id, 'allow');

    time.t += HOOK_THINKING_STALE_MS - 60_000;
    assert.equal(registry.sessions()[0].state, 'thinking', 'nearly 30 minutes after the prompt');
    time.t += 120_000;
    assert.equal(registry.sessions()[0].state, 'idle', 'and a quiet stretch ends it as before');
  });

  it('does not remember a headless request as a session', () => {
    const { registry } = clock();
    registry.submit(request('Bash', { command: 'ls' }, { session: 'job', entrypoint: 'sdk-cli' }),
                    new FakeChannel());
    assert.equal(registry.live()[0].sessionId, null);
    for (let n = 1; n <= SESSIONS_MAX; n += 1) real(registry, n);
    assert.equal(registry.sessions().length, SESSIONS_MAX, 'a headless run took no place from a real session');
  });

  it('lets go of a session Desktop shows before a terminal, when it has to let go', () => {
    const { time, registry } = clock();
    real(registry, 1);
    time.t += 1000;
    real(registry, 2);
    for (let n = 1; n <= SESSIONS_MAX - 1; n += 1) {
      time.t += 1000;
      registry.event({ name: HookEvent.prompt, session: `desk-${n}`, cwd: `/w/desk${n}`, entrypoint: 'claude-desktop' });
    }
    // Nine are known: the quietest overall is a terminal, but a desktop one goes.
    const names = registry.sessions().map((s) => s.name);
    assert.equal(names.length, SESSIONS_MAX);
    assert.ok(names.includes('real1') && names.includes('real2'), names.join());
    assert.ok(!names.includes('desk1'), 'the quietest of the others went');
  });

  it('lists nothing from a hook in ui mode, and starts again from nothing after', () => {
    const { registry } = clock();
    real(registry, 1);
    registry.setMode('ui');
    assert.deepEqual(registry.sessions(), []);
    registry.setMode('auto');
    assert.deepEqual(registry.sessions(), [], 'what it knew before is gone, not frozen');
    real(registry, 2);
    assert.deepEqual(registry.sessions().map((s) => s.name), ['real2']);
  });
});

describe('sessions beside the windows, after review', () => {
  const tell = (f, name, extra = {}) => f.hooks.event({
    name, session: 'sess-1', cwd: '/Users/me/docs', entrypoint: 'cli', ...extra,
  });
  const states = (f) => f.transport.framesOfType('sessions').at(-1);
  const windows = (count, tree = (n) => fixtures.thinking(`win ${n}`, `${n}.`)) => fixtures.workspace(
    Array.from({ length: count }, (_, i) => ({ name: `window ${i + 1}`, tree: tree(i + 1) })),
  );

  it('keeps the session that asks in the rows the box is given', () => {
    const f = fixture();
    f.source.tree = windows(3);
    for (let n = 1; n <= 7; n += 1) {
      f.hooks.event({ name: HookEvent.prompt, session: `sess-${n}`, cwd: `/w/p${n}`, entrypoint: 'cli' });
    }
    f.hooks.submit(request('Bash', { command: 'ls' }, { session: 'sess-7', cwd: '/w/p7', entrypoint: 'cli' }),
                   new FakeChannel());
    f.coordinator.refresh();

    const list = states(f);
    const ids = list.sessions.map((s) => s.id);
    assert.ok(ids.length <= 8, `${ids.length} rows`);
    assert.equal(f.shown().session, 'c7');
    assert.ok(ids.includes('c7'), 'the asking session is one of the rows');
    assert.ok(ids.includes(list.focus), 'so is the one described');
    assert.equal(list.sessions.find((s) => s.id === 'c7').state, 'waiting_permission');
  });

  it('names no hook session when the windows leave no row for it', () => {
    const f = fixture();
    f.source.tree = windows(8);
    f.hooks.submit(request('Bash', { command: 'ls' }, { session: 'sess-1', cwd: '/w/p1', entrypoint: 'cli' }),
                   new FakeChannel());
    f.coordinator.refresh();

    const list = states(f);
    assert.ok(list.sessions.every((s) => s.id.startsWith('s')), 'eight windows fill the box');
    assert.equal(f.shown().session, undefined, 'the request names a session the box was never given');
    assert.ok(list.sessions.some((s) => s.id === list.focus));
  });

  it('presses nothing for a STOP that names a terminal session which has just ended', () => {
    const f = fixture();
    f.source.tree = windows(1);
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    tell(f, HookEvent.sessionEnd);

    // The key was pressed while the session was still on the box.
    f.inbound('{"v":1,"type":"action","action":"stop","sessionId":"c1"}');
    assert.deepEqual(f.source.pressed, [], 'the window being followed is another conversation');
  });

  it('gives the box back to the window in front once another one is brought to the front', () => {
    const f = fixture();
    f.source.tree = windows(2, () => fixtures.idle());
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's1');

    f.inbound('{"v":1,"type":"focus","sessionId":"c1"}');
    assert.equal(states(f).focus, 'c1');

    const reordered = windows(2, () => fixtures.idle());
    reordered.children.reverse();
    f.source.tree = reordered;
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's2', 'the window now in front, not the session picked before');
  });

  it('shows the window that asks, though a terminal session was picked', () => {
    const f = fixture();
    f.source.tree = windows(1, () => fixtures.idle());
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    f.inbound('{"v":1,"type":"focus","sessionId":"c1"}');
    assert.equal(states(f).focus, 'c1');

    f.source.tree = windows(1, () => fixtures.permission('Run command', 'rm -rf build', 'a.'));
    f.coordinator.refresh();
    assert.equal(f.shown().session, 's1');
    assert.equal(states(f).focus, 's1', 'the state around a question is that of the session that asked it');
  });

  it('lists a session under the title its user gave it', () => {
    const f = fixture();
    f.source.tree = windows(1, () => fixtures.idle());
    tell(f, HookEvent.sessionStart, { title: 'firmware' });
    f.coordinator.refresh();
    assert.deepEqual(states(f).sessions.map((s) => s.name).slice(1), ['firmware']);
  });

  it('drops the terminal sessions when the box is set to the interface, and takes them again after', () => {
    const f = fixture();
    f.source.tree = windows(1, () => fixtures.idle());
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    assert.deepEqual(states(f).sessions.map((s) => s.id), ['s1', 'c1']);

    f.inbound('{"v":1,"type":"mode","value":"ui"}');
    f.flush();
    assert.deepEqual(states(f).sessions.map((s) => s.id), ['s1']);

    f.inbound('{"v":1,"type":"mode","value":"auto"}');
    tell(f, HookEvent.prompt);
    f.flush();
    f.coordinator.refresh();
    assert.deepEqual(states(f).sessions.map((s) => s.id), ['s1', 'c2']);
  });

  it('still says how many sessions there are when the box has no row for some', () => {
    const f = fixture();
    f.source.tree = windows(2, () => fixtures.idle());
    for (let n = 1; n <= 8; n += 1) {
      f.hooks.event({ name: HookEvent.prompt, session: `sess-${n}`, cwd: `/w/p${n}`, entrypoint: 'cli' });
    }
    f.coordinator.refresh();

    const list = states(f);
    assert.equal(list.sessions.length, 8, 'the box holds eight rows');
    assert.equal(list.total, 10, 'and is told there are ten sessions');
  });

  it('releases a picked terminal session even when the window came to the front during a press', () => {
    // The tracker reports a window being brought to the front once, on the read
    // that sees it. If that read is the one before a press, it must not be lost.
    const window = (n) => (n === 1 ? fixtures.permission('Run command', 'rm -rf build', 'a.') : fixtures.idle());
    const f = fixture();
    f.source.tree = windows(2, window);
    tell(f, HookEvent.prompt);
    f.coordinator.refresh();
    f.inbound('{"v":1,"type":"focus","sessionId":"c1"}');
    const { id } = f.shown();

    const reordered = windows(2, window);
    reordered.children.reverse();
    f.source.tree = reordered;
    f.arm();
    f.inbound(action('approve', id));
    assert.deepEqual(f.source.pressed, ['a.allow'], 'the press itself went through');

    f.source.tree = (() => { const t = windows(2, () => fixtures.idle()); t.children.reverse(); return t; })();
    f.coordinator.refresh();
    assert.equal(states(f).focus, 's2', 'the window now in front, not the session picked before');
  });
});

describe('what a session is called, after review', () => {
  const tell = (r, name, extra = {}) => r.event({
    name, session: 'sess-1', cwd: '/Users/me/chickenMicro', entrypoint: 'cli', ...extra,
  });

  it('never lets a title break a line of the panel or of the log', () => {
    const r = new HookRegistry();
    tell(r, HookEvent.prompt, { title: 'firmware\n2026-10-25 [HOOK] approve h9\u001b[31m' });
    const [session] = r.sessions();
    assert.equal(session.name, 'firmware 2026-10-25 [HOOK] approve h9?[31m');
    assert.ok(!/[\n\u001b]/.test(session.name));
  });

  it('keeps the folder when the title is nothing the panel can draw', () => {
    const r = new HookRegistry();
    tell(r, HookEvent.prompt, { title: '🚀' });
    assert.equal(r.sessions()[0].name, 'chickenMicro');
    tell(r, HookEvent.prompt, { title: '​​' });
    assert.equal(r.sessions()[0].name, 'chickenMicro');
  });

  it('never names a session by nothing', () => {
    const r = new HookRegistry();
    tell(r, HookEvent.prompt, { cwd: '/' });
    assert.equal(r.sessions()[0].name, 'Claude');
    assert.equal(sessionNameOf('/w/line\nbreak'), 'line break');
    assert.equal(sessionNameOf('/w/🚀'), 'Claude');
  });
});
