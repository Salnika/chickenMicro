import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { Log, LogTag } from '../src/log.js';

/** A Log writing into arrays instead of a terminal and a file. */
function harness(options = {}) {
  const terminal = [];
  const file = [];
  const sink = { write: (line) => file.push(line), close: () => file.push('<closed>') };
  const log = new Log({
    out: { write: (line) => terminal.push(line) },
    file: sink,
    color: false,
    ...options,
  });
  return { log, terminal, file };
}

describe('log — the terminal and the file', () => {
  it('writes a line to both', () => {
    const { log, terminal, file } = harness();
    log.info(LogTag.ax, 'asked Claude Desktop (pid 1) to expose its web content');

    assert.equal(terminal.length, 1);
    assert.equal(file.length, 1);
    assert.match(terminal[0], /\[AX\] asked Claude Desktop \(pid 1\)/);
    assert.match(file[0], /\[AX\] asked Claude Desktop \(pid 1\)/);
  });

  it('gives the file a full date, where the terminal shows the time', () => {
    const { log, terminal, file } = harness();
    log.warn(LogTag.session, 'refused approve: stale request');

    assert.match(file[0], /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z ! \[SESSION\] /);
    assert.match(terminal[0], /^\d{2}:\d{2}:\d{2}\.\d{3} ! \[SESSION\] /);
  });

  it('never colours the file, whatever the terminal does', () => {
    const { log, terminal, file } = harness({ color: true });
    log.info(log.sessionTag('s2'), 'state idle -> thinking', 's2');

    assert.match(terminal[0], /\u001b\[38;5;/);          // coloured for the eye
    assert.doesNotMatch(file[0], /\u001b\[/);            // plain, for grep
  });

  it('records at its own level, so a quiet terminal still keeps everything', () => {
    const { log, terminal, file } = harness({ level: 'info', fileLevel: 'debug' });
    log.debug(LogTag.serial, '<- {"type":"hello"}');

    assert.equal(terminal.length, 0);
    assert.equal(file.length, 1);
  });

  it('builds a message once, and not at all when nothing wants it', () => {
    const { log, terminal, file } = harness({ level: 'warn', fileLevel: 'warn' });
    let built = 0;
    log.debug(LogTag.serial, () => { built += 1; return 'expensive'; });
    assert.equal(built, 0);
    assert.equal(terminal.length + file.length, 0);

    log.error(LogTag.serial, () => { built += 1; return 'expensive'; });
    assert.equal(built, 1);
    assert.equal(terminal.length, 1);
    assert.equal(file.length, 1);
  });

  it('stops writing to the file once closed, and keeps the terminal', () => {
    const { log, terminal, file } = harness();
    log.close();
    log.info(LogTag.session, 'shutting down');

    assert.deepEqual(file, ['<closed>']);
    assert.equal(terminal.length, 1);
    assert.equal(log.filePath, null);
  });
});
