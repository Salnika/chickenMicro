import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { after, describe, it } from 'node:test';

import { LogFile } from '../src/system/logFile.js';

const roots = [];

/** A directory of its own per test, removed when the file is done. */
function scratch() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'claude-remote-log-'));
  roots.push(root);
  return root;
}

after(() => {
  for (const root of roots) fs.rmSync(root, { recursive: true, force: true });
});

const read = (file) => (fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null);

describe('log file — keeping the lines', () => {
  it('creates the directory it was pointed at and appends', () => {
    const file = path.join(scratch(), 'nested', 'deeper', 'claude-remote.log');
    const log = new LogFile({ path: file });
    log.write('first\n');
    log.write('second\n');
    log.close();

    assert.equal(read(file), 'first\nsecond\n');
  });

  it('marks where a run starts, so two runs can be told apart', () => {
    const file = path.join(scratch(), 'claude-remote.log');
    const first = new LogFile({ path: file, banner: 'claude-remote 1.1.0 run — pid 1' });
    first.write('from the first run\n');
    first.close();
    const second = new LogFile({ path: file, banner: 'claude-remote 1.1.0 run — pid 2' });
    second.write('from the second run\n');
    second.close();

    const lines = read(file).split('\n').filter((line) => line.startsWith('==='));
    assert.equal(lines.length, 2);
    assert.match(lines[0], /pid 1 ===$/);
    assert.match(lines[1], /pid 2 ===$/);
    assert.match(lines[0], /^=== \d{4}-\d{2}-\d{2}T/);   // dated, for a file read later
  });

  it('keeps the live file under the limit by rotating first', () => {
    const file = path.join(scratch(), 'claude-remote.log');
    const log = new LogFile({ path: file, maxBytes: 40 });
    log.write('a'.repeat(29) + '\n');   // 30 bytes
    log.write('b'.repeat(29) + '\n');   // would pass 40: rotates
    log.close();

    assert.equal(read(`${file}.1`), `${'a'.repeat(29)}\n`);
    assert.equal(read(file), `${'b'.repeat(29)}\n`);
    assert.ok(fs.statSync(file).size <= 40);
  });

  it('keeps only the rotated files it was told to, the oldest dropped', () => {
    const file = path.join(scratch(), 'claude-remote.log');
    const log = new LogFile({ path: file, maxBytes: 12, keep: 2 });
    for (const line of ['one\n', 'two\n', 'three\n', 'four\n', 'five\n', 'six\n', 'seven\n']) {
      log.write(line);
    }
    log.close();

    // A file fills to the limit before rotating, so each holds several lines.
    assert.equal(read(file), 'seven\n');
    assert.equal(read(`${file}.1`), 'five\nsix\n');
    assert.equal(read(`${file}.2`), 'three\nfour\n');
    assert.equal(read(`${file}.3`), null);
    // Two rotated files kept means the first lines are gone for good.
    const kept = [file, `${file}.1`, `${file}.2`].map(read).join('');
    assert.doesNotMatch(kept, /one|two/);
  });

  it('gives up once and keeps quiet when it cannot write', () => {
    const root = scratch();
    // A file where a directory would have to be: mkdir fails with ENOTDIR.
    fs.writeFileSync(path.join(root, 'blocked'), '');
    const failures = [];
    const log = new LogFile({
      path: path.join(root, 'blocked', 'claude-remote.log'),
      onError: (error) => failures.push(error),
    });

    assert.doesNotThrow(() => log.write('one\n'));
    assert.doesNotThrow(() => log.write('two\n'));
    assert.equal(failures.length, 1);
    assert.doesNotThrow(() => log.close());
  });
});
