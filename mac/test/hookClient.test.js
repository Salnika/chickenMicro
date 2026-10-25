import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import { HookRegistry } from '../src/hook/registry.js';
import { HookServer } from '../src/hook/server.js';
import { Log } from '../src/log.js';

// The hook a plugin installs, run for real against the real server: the two
// ends of the channel are tested together here, and each alone in hook.test.js.

const SCRIPT = fileURLToPath(
  new URL('../../plugin/chickenmicro-permissions/bin/permission-hook.mjs', import.meta.url),
);

const permissionRequest = (extra = {}) => ({
  session_id: 'c1',
  cwd: '/Users/me/chickenMicro',
  permission_mode: 'default',
  hook_event_name: 'PermissionRequest',
  tool_name: 'Bash',
  tool_input: { command: 'git push origin master', description: 'Push the branch' },
  permission_suggestions: [{ type: 'setMode', mode: 'acceptEdits', destination: 'session' }],
  ...extra,
});

/** Starts the hook as Claude Code would: the request on stdin, the socket in the environment. */
function runHook(stdin, socket, { entrypoint, projectDir } = {}) {
  const env = { ...process.env, CLAUDE_REMOTE_SOCKET: socket };
  // The tests may themselves run inside Claude Code, which sets these.
  delete env.CLAUDE_CODE_ENTRYPOINT;
  delete env.CLAUDE_PROJECT_DIR;
  if (entrypoint !== undefined) env.CLAUDE_CODE_ENTRYPOINT = entrypoint;
  if (projectDir !== undefined) env.CLAUDE_PROJECT_DIR = projectDir;
  const child = spawn(process.execPath, [SCRIPT], { env, stdio: ['pipe', 'pipe', 'pipe'] });
  let stdout = '';
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stdin.end(typeof stdin === 'string' ? stdin : JSON.stringify(stdin));
  const done = new Promise((resolve) => {
    child.on('close', (code, signal) => resolve({ code, signal, stdout }));
  });
  return { child, done };
}

async function waitFor(predicate, what) {
  const deadline = Date.now() + 3000;
  while (!predicate()) {
    if (Date.now() > deadline) assert.fail(`timed out waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

async function withDaemon(run) {
  const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
  const registry = new HookRegistry();
  const server = new HookServer({
    registry,
    log: new Log({ level: 'error', color: false, out: { write() {} } }),
    path: join(directory, 'hook.sock'),
  });
  await server.start();
  try {
    await run({ registry, socket: server.path, directory });
  } finally {
    server.stop();
    rmSync(directory, { recursive: true, force: true });
  }
}

describe('the hook a plugin installs', () => {
  it('prints "allow once", and nothing else, when the box allows', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the request');

      const [entry] = registry.live();
      assert.equal(entry.tool, 'Bash');
      assert.equal(entry.session, 'c1');
      assert.equal(entry.cwd, '/Users/me/chickenMicro');
      assert.equal(entry.content, 'git push origin master');

      registry.answer(entry.id, 'allow');
      const { code, stdout } = await hook.done;
      assert.equal(code, 0);
      assert.deepEqual(JSON.parse(stdout), {
        hookSpecificOutput: { hookEventName: 'PermissionRequest', decision: { behavior: 'allow' } },
      });
    });
  });

  it('never hands back a permission update, so nothing is remembered', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the request');
      registry.answer(registry.live()[0].id, 'allow');
      const { stdout } = await hook.done;
      assert.doesNotMatch(stdout, /updatedPermissions|setMode|acceptEdits|updatedInput/);
    });
  });

  it('prints a refusal with the reason Claude is given', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the request');
      registry.answer(registry.live()[0].id, 'deny');

      const { stdout } = await hook.done;
      const { decision } = JSON.parse(stdout).hookSpecificOutput;
      assert.equal(decision.behavior, 'deny');
      assert.match(decision.message, /ChickenMicro/);
    });
  });

  it('decides nothing when there is no daemon', async () => {
    await withDaemon(async ({ directory }) => {
      const started = Date.now();
      const { code, stdout } = await runHook(permissionRequest(), join(directory, 'absent.sock')).done;
      assert.equal(code, 0);
      assert.equal(stdout, '');
      assert.ok(Date.now() - started < 2500, 'it must not make Claude wait for a daemon that is not there');
    });
  });

  it('decides nothing when the box answers through the interface', async () => {
    await withDaemon(async ({ registry, socket }) => {
      registry.setMode('ui');
      const { code, stdout } = await runHook(permissionRequest(), socket).done;
      assert.equal(code, 0);
      assert.equal(stdout, '');
    });
  });

  it('decides nothing when the request was answered in the app', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the request');
      registry.setMode('ui');                 // stands every waiting hook down
      const { code, stdout } = await hook.done;
      assert.equal(code, 0);
      assert.equal(stdout, '');
    });
  });

  it('decides nothing when the daemon goes away', async () => {
    const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
    const registry = new HookRegistry();
    const server = new HookServer({
      registry,
      log: new Log({ level: 'error', color: false, out: { write() {} } }),
      path: join(directory, 'hook.sock'),
    });
    await server.start();
    try {
      const hook = runHook(permissionRequest(), server.path);
      await waitFor(() => registry.live().length === 1, 'the request');
      server.stop();
      const { code, stdout } = await hook.done;
      assert.equal(code, 0);
      assert.equal(stdout, '');
    } finally {
      rmSync(directory, { recursive: true, force: true });
    }
  });

  it('lets go cleanly when Claude Code stops it at its timeout', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the request');

      hook.child.kill('SIGTERM');
      const { code, stdout } = await hook.done;
      assert.equal(code, 0);
      assert.equal(stdout, '');
      await waitFor(() => registry.live().length === 0, 'the daemon to see it gone');
    });
  });

  it('ignores anything that is not a permission request', async () => {
    await withDaemon(async ({ registry, socket }) => {
      for (const stdin of [
        permissionRequest({ hook_event_name: 'PreToolUse' }),
        permissionRequest({ tool_name: '' }),
        'not json at all',
        '',
      ]) {
        const { code, stdout } = await runHook(stdin, socket).done;
        assert.equal(code, 0);
        assert.equal(stdout, '');
      }
      assert.equal(registry.live().length, 0, 'none of them may reach the box');
    });
  });

  describe('told that something happened in a session', () => {
    const lifecycle = (event, extra = {}) => ({
      session_id: 'sess-1',
      cwd: '/Users/me/docs',
      transcript_path: '/Users/me/.claude/projects/x/sess-1.jsonl',
      hook_event_name: event,
      ...extra,
    });

    it('tells the daemon, and prints nothing for Claude to read', async () => {
      await withDaemon(async ({ registry, socket }) => {
        for (const event of ['SessionStart', 'UserPromptSubmit', 'Stop', 'StopFailure', 'SessionEnd']) {
          const extra = event === 'UserPromptSubmit' ? { prompt: 'a prompt the box must never see' } : {};
          const { code, stdout } = await runHook(lifecycle(event, extra), socket, { entrypoint: 'cli' }).done;
          // What SessionStart and UserPromptSubmit print is added to Claude's context.
          assert.equal(stdout, '', event);
          assert.equal(code, 0, event);
          if (event === 'UserPromptSubmit') {
            assert.deepEqual(registry.sessions().map((s) => [s.name, s.state, s.frontEnd]),
                             [['docs', 'thinking', 'own']]);
          }
        }
        assert.deepEqual(registry.sessions(), [], 'SessionEnd was the last word');
      });
    });

    it('tells the daemon a turn ended on an API error, which Stop does not', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('UserPromptSubmit'), socket, { entrypoint: 'cli' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.state), ['thinking']);
        const { stdout } = await runHook(lifecycle('StopFailure', { error: 'rate_limit' }), socket,
                                         { entrypoint: 'cli' }).done;
        assert.equal(stdout, '');
        assert.deepEqual(registry.sessions().map((s) => s.state), ['idle']);
      });
    });

    it('says which front-end started the session', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('SessionStart'), socket, { entrypoint: 'claude-desktop' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.frontEnd), ['desktop']);
      });
    });

    it('says nothing about the front-end when Claude Code did not', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('SessionStart'), socket).done;
        assert.deepEqual(registry.sessions().map((s) => s.frontEnd), ['unknown']);
      });
    });

    it('does not wait on the daemon, nor on one that is not there', async () => {
      await withDaemon(async ({ directory }) => {
        const started = Date.now();
        const { code, stdout } = await runHook(lifecycle('Stop'), join(directory, 'nobody.sock')).done;
        assert.equal(code, 0);
        assert.equal(stdout, '');
        assert.ok(Date.now() - started < 2000, 'a prompt must not be held up');
      });
    });

    it('names the session after the title its user gave it, when the payload has one', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('SessionStart', { session_title: 'firmware', source: 'startup' }), socket,
                      { entrypoint: 'cli' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.name), ['firmware']);
      });
    });

    it('names the session after the project it started in, not after where Claude has since gone', async () => {
      await withDaemon(async ({ registry, socket }) => {
        // `cwd` follows every `cd` Claude runs; CLAUDE_PROJECT_DIR stays put.
        await runHook(lifecycle('Stop', { cwd: '/Users/me/docs/sub/folder' }), socket,
                      { entrypoint: 'cli', projectDir: '/Users/me/docs' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.name), ['docs']);
      });
    });

    it('falls back to the cwd of the payload when it is not told the project', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('Stop', { cwd: '/Users/me/docs/sub' }), socket, { entrypoint: 'cli' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.name), ['sub']);
      });
    });

    it('does not send a title with a Stop, nor anything of the prompt with a start', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('SessionStart', { session_title: 'kept' }), socket, { entrypoint: 'cli' }).done;
        await runHook(lifecycle('Stop', { session_title: 'ignored here', last_assistant_message: 'secret' }),
                      socket, { entrypoint: 'cli' }).done;
        assert.deepEqual(registry.sessions().map((s) => s.name), ['kept']);
        assert.ok(!JSON.stringify(registry.sessions()).includes('secret'));
      });
    });

    it('never carries the prompt, the transcript or anything else it was handed', async () => {
      await withDaemon(async ({ registry, socket }) => {
        await runHook(lifecycle('UserPromptSubmit', { prompt: 'secret' }), socket, { entrypoint: 'cli' }).done;
        assert.ok(!JSON.stringify([registry.sessions(), registry.live()]).includes('secret'));
      });
    });
  });

  it('still reaches the box when a write carries a huge file', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const hook = runHook(permissionRequest({
        tool_name: 'Write',
        tool_input: { file_path: '/Users/me/chickenMicro/big.txt', content: 'x'.repeat(500_000) },
      }), socket);
      await waitFor(() => registry.live().length === 1, 'the request');

      const [entry] = registry.live();
      assert.equal(entry.kind, 'write');
      assert.equal(entry.content, '/Users/me/chickenMicro/big.txt');
      registry.answer(entry.id, 'deny');
      await hook.done;
    });
  });

  it('answers each of several waiting hooks, and only that one', async () => {
    await withDaemon(async ({ registry, socket }) => {
      const first = runHook(permissionRequest(), socket);
      await waitFor(() => registry.live().length === 1, 'the first request');
      const second = runHook(permissionRequest({
        tool_input: { command: 'rm -rf build' },
      }), socket);
      await waitFor(() => registry.live().length === 2, 'the second request');

      const [a, b] = registry.live();
      registry.answer(b.id, 'deny');
      const secondResult = await second.done;
      assert.equal(JSON.parse(secondResult.stdout).hookSpecificOutput.decision.behavior, 'deny');

      registry.answer(a.id, 'allow');
      const firstResult = await first.done;
      assert.equal(JSON.parse(firstResult.stdout).hookSpecificOutput.decision.behavior, 'allow');
    });
  });
});
