import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

// What Claude Code is told to run is configuration, not code, and nothing else
// exercises it: a misspelt event or a missing hook would leave the box without
// sessions and without a single error.

const PLUGIN = new URL('../../plugin/chickenmicro-permissions/', import.meta.url);
const read = (path) => JSON.parse(readFileSync(fileURLToPath(new URL(path, PLUGIN)), 'utf8'));

describe('the plugin Claude Code installs', () => {
  const { hooks } = read('hooks/hooks.json');
  const commands = (event) => hooks[event].flatMap((group) => group.hooks);

  it('listens for the permissions and for the life of a session, and nothing else', () => {
    assert.deepEqual(Object.keys(hooks).sort(),
                     ['PermissionRequest', 'SessionEnd', 'SessionStart', 'Stop', 'UserPromptSubmit']);
  });

  it('starts every hook through the launcher, which finds node', () => {
    for (const event of Object.keys(hooks)) {
      for (const hook of commands(event)) {
        assert.equal(hook.type, 'command');
        assert.equal(hook.command, '/bin/sh "${CLAUDE_PLUGIN_ROOT}/bin/permission-hook.sh"', event);
      }
    }
  });

  it('lets a permission wait for a key, and never holds a turn up for the rest', () => {
    for (const hook of commands('PermissionRequest')) assert.ok(hook.timeout >= 120);
    for (const event of ['SessionStart', 'UserPromptSubmit', 'Stop', 'SessionEnd']) {
      for (const hook of commands(event)) {
        assert.ok(hook.timeout > 0 && hook.timeout <= 10, `${event} may wait ${hook.timeout}s`);
      }
    }
  });

  it('asks about every tool, not only some', () => {
    assert.equal(hooks.PermissionRequest[0].matcher, '*');
  });

  it('carries a version that moves, so Claude Code replaces the copy it keeps', () => {
    const { version } = read('.claude-plugin/plugin.json');
    assert.match(version, /^\d+\.\d+\.\d+$/);
    const [major, minor] = version.split('.').map(Number);
    assert.ok(major > 0 || minor >= 2, `${version} predates the session events`);
  });
});
