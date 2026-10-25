import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { describe, it } from 'node:test';

import { shellQuote } from '../src/cli/shellQuote.js';

// `claude-remote hook-config` prints a command that Claude Code hands to a shell,
// with the path of a checkout in it. Whatever that path holds must come out of the
// shell as that path, so these ask a real shell.

const through = (word) => execFileSync('/bin/sh', ['-c', `printf %s ${shellQuote(word)}`], {
  encoding: 'utf8',
  env: { PATH: '/usr/bin:/bin', work: 'EXPANDED' },
});

describe('quoting a word for the shell', () => {
  for (const word of [
    '/Users/me/chickenMicro/permission-hook.sh',
    '/Users/me/with space/permission-hook.sh',
    '/Users/me/$work/permission-hook.sh',
    '/Users/me/`id`/permission-hook.sh',
    '/Users/me/$(id)/permission-hook.sh',
    '/Users/me/say "hi"/permission-hook.sh',
    '/Users/me/back\\slash/permission-hook.sh',
    '/Users/me/back\\$work/permission-hook.sh',
    '/Users/me/ends-with-a-backslash\\',
    "/Users/me/it's/permission-hook.sh",
    "/Users/me/''/permission-hook.sh",
    '',
  ]) {
    it(`hands back ${JSON.stringify(word)} as it is`, () => {
      assert.equal(through(word), word);
    });
  }
});
