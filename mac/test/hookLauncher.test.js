import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import { HookRegistry } from '../src/hook/registry.js';
import { HookServer } from '../src/hook/server.js';
import { Log } from '../src/log.js';

// The launcher is what makes the hook work from VS Code, which is often opened
// with a PATH that has no node on it. These run it the way Claude Code does, with
// a PATH as bare as that and a HOME of its own.

const LAUNCHER = fileURLToPath(
  new URL('../../plugin/chickenmicro-permissions/bin/permission-hook.sh', import.meta.url),
);

/** A throwaway executable shell script, standing in for node. */
function fakeNode(directory, name, output) {
  const path = join(directory, name);
  writeFileSync(path, `#!/bin/sh\necho "${output} $1"\n`);
  chmodSync(path, 0o755);
  return path;
}

function launch({ home, path = '', socket, stdin = '{}', cwd }) {
  const env = { HOME: home, PATH: path };
  if (socket !== undefined) env.CLAUDE_REMOTE_SOCKET = socket;
  const child = spawn('/bin/sh', [LAUNCHER], { env, cwd, stdio: ['pipe', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  child.stdin.end(stdin);
  return new Promise((resolve) => {
    child.on('close', (code) => resolve({ code, stdout, stderr }));
  });
}

function sandbox(run) {
  const home = mkdtempSync(join(tmpdir(), 'claude-remote-home-'));
  mkdirSync(join(home, '.claude-remote'), { recursive: true });
  const bin = mkdtempSync(join(tmpdir(), 'claude-remote-bin-'));
  return run({ home, bin, dir: join(home, '.claude-remote') })
    .finally(() => {
      rmSync(home, { recursive: true, force: true });
      rmSync(bin, { recursive: true, force: true });
    });
}

describe('the hook launcher', () => {
  it('runs the hook with the node the daemon left behind, whatever the PATH', () => sandbox(async ({ home, bin, dir }) => {
    const recorded = fakeNode(bin, 'recorded-node', 'recorded');
    writeFileSync(join(dir, 'node'), `${recorded}\n`);

    const { code, stdout } = await launch({ home });
    assert.equal(code, 0);
    assert.match(stdout, /^recorded .*permission-hook\.mjs/);
  }));

  it('falls back to the node on the PATH when the daemon left none', () => sandbox(async ({ home, bin }) => {
    fakeNode(bin, 'node', 'from-path');
    const { stdout } = await launch({ home, path: bin });
    assert.match(stdout, /^from-path .*permission-hook\.mjs/);
  }));

  it('does not trust a recorded node that is gone', () => sandbox(async ({ home, bin, dir }) => {
    writeFileSync(join(dir, 'node'), '/nowhere/at/all/node\n');
    fakeNode(bin, 'node', 'from-path');
    const { stdout } = await launch({ home, path: bin });
    assert.match(stdout, /^from-path /, 'a stale record must not make the hook silent');
  }));

  it('does not trust a recorded node that is not a full path', () => sandbox(async ({ home, bin, dir }) => {
    // A hook runs in the project's folder: a relative path would name whatever
    // happens to be there.
    fakeNode(bin, 'recorded-node', 'planted');
    writeFileSync(join(dir, 'node'), 'recorded-node\n');
    const elsewhere = mkdtempSync(join(tmpdir(), 'claude-remote-path-'));
    try {
      fakeNode(elsewhere, 'node', 'from-path');
      const { stdout } = await launch({ home, path: elsewhere, cwd: bin });
      assert.match(stdout, /^from-path /);
    } finally {
      rmSync(elsewhere, { recursive: true, force: true });
    }
  }));

  it('stands down quietly when there is no node anywhere', () => sandbox(async ({ home }) => {
    const { code, stdout, stderr } = await launch({ home });
    assert.equal(code, 0);
    assert.equal(stdout, '');
    assert.equal(stderr, '');
  }));

  it('looks beside the socket it was told about', () => sandbox(async ({ home, bin }) => {
    const elsewhere = mkdtempSync(join(tmpdir(), 'claude-remote-sock-'));
    try {
      writeFileSync(join(elsewhere, 'node'), `${fakeNode(bin, 'elsewhere-node', 'elsewhere')}\n`);
      const { stdout } = await launch({ home, socket: join(elsewhere, 'hook.sock') });
      assert.match(stdout, /^elsewhere /);
    } finally {
      rmSync(elsewhere, { recursive: true, force: true });
    }
  }));

  it('answers a request end to end, with no node on the PATH at all', async () => {
    // The real thing: the daemon writes its own node path, and the launcher uses
    // it to run the real hook against the real server.
    const directory = mkdtempSync(join(tmpdir(), 'claude-remote-'));
    const registry = new HookRegistry();
    const server = new HookServer({
      registry,
      log: new Log({ level: 'error', color: false, out: { write() {} } }),
      path: join(directory, 'hook.sock'),
    });
    await server.start();
    try {
      const pending = launch({
        home: directory,
        socket: server.path,
        stdin: JSON.stringify({
          hook_event_name: 'PermissionRequest',
          session_id: 'c1',
          cwd: '/Users/me/chickenMicro',
          tool_name: 'Bash',
          tool_input: { command: 'git status' },
        }),
      });
      const deadline = Date.now() + 5000;
      while (registry.live().length === 0) {
        if (Date.now() > deadline) assert.fail('the request never reached the daemon');
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
      registry.answer(registry.live()[0].id, 'allow');

      const { code, stdout } = await pending;
      assert.equal(code, 0);
      assert.equal(JSON.parse(stdout).hookSpecificOutput.decision.behavior, 'allow');
    } finally {
      server.stop();
      rmSync(directory, { recursive: true, force: true });
    }
  });
});
