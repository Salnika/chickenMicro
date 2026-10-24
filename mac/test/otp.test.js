import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { decodeInbound, encode, redactForLog } from '../src/protocol/frames.js';
import { OTP_CLEAR_GRACE_MS } from '../src/protocol/constants.js';
import { PermissionGate } from '../src/session/permissionGate.js';
import { SessionCoordinator } from '../src/session/sessionCoordinator.js';
import { FakeAccessibilitySource } from '../src/simulation/fixtures.js';

const otpLine = (fields) => JSON.stringify({ v: 1, type: 'otp', ...fields });

describe('one-time codes: the wire', () => {
  it('decodes an otp frame', () => {
    assert.deepEqual(decodeInbound(otpLine({ label: 'GitHub:alice', code: '012345', expiresInMs: 18000 })),
                     { frame: { type: 'otp', label: 'GitHub:alice', code: '012345', expiresInMs: 18000 } });
  });

  it('refuses anything but 6 to 8 digits — and never echoes what it refused', () => {
    // Whatever passes lands on the clipboard, one paste away from a terminal.
    for (const code of ['12345', '123456789', '12a456', '123 456', 'rm -rf /', '']) {
      const result = decodeInbound(otpLine({ label: 'x', code, expiresInMs: 1000 }));
      assert.ok(result.rejected, `accepted ${JSON.stringify(code)}`);
      if (code) assert.ok(!result.rejected.includes(code), 'the rejection must not repeat the value');
    }
    assert.match(decodeInbound(otpLine({ label: 'x', expiresInMs: 1000 })).rejected, /code/);
  });

  it('refuses a window no code has', () => {
    for (const expiresInMs of [-1, 300_001, 1.5, true, '1000']) {
      assert.ok(decodeInbound(otpLine({ label: 'x', code: '123456', expiresInMs })).rejected,
                `accepted expiresInMs ${JSON.stringify(expiresInMs)}`);
    }
  });

  it('clips a label to what the device can hold', () => {
    const { frame } = decodeInbound(otpLine({ label: 'x'.repeat(80), code: '123456', expiresInMs: 1 }));
    assert.equal(frame.label.length, 24);
  });

  it('sends the clock as whole milliseconds', () => {
    assert.equal(encode.time(1726000000123.7), '{"v":1,"type":"time","unixMs":1726000000123}\n');
  });

  it('masks a code in anything written to a log', () => {
    assert.equal(redactForLog(otpLine({ label: 'x', code: '987654', expiresInMs: 1 })),
                 '{"v":1,"type":"otp","label":"x","code":"******","expiresInMs":1}');
    assert.equal(redactForLog('{"v":1,"type":"ping","seq":3}'), '{"v":1,"type":"ping","seq":3}');
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
  framesOfType(type) { return this.sent.map((l) => JSON.parse(l)).filter((f) => f.type === type); }
}

/** Every line the coordinator logs, at every level. */
class RecordingLog {
  lines = [];
  debug(tag, message) { this.lines.push(`${tag} ${message}`); }
  info(tag, message) { this.lines.push(`${tag} ${message}`); }
  warn(tag, message) { this.lines.push(`${tag} ${message}`); }
  error(tag, message) { this.lines.push(`${tag} ${message}`); }
  sessionTag(id) { return `SESSION ${id}`; }
  paint(id) { return id; }
  mentions(text) { return this.lines.some((line) => line.includes(text)); }
}

function fakeClipboard() {
  return {
    text: 'something the user copied earlier',
    copies: [],
    copy(text) { this.text = text; this.copies.push(text); },
    read() { return this.text; },
  };
}

function fakeTimers() {
  const pending = [];
  return {
    pending,
    setTimeout(callback, ms) {
      const timer = { callback, ms, cleared: false, unref() {} };
      pending.push(timer);
      return timer;
    },
    clearTimeout(timer) { if (timer) timer.cleared = true; },
    runAll() { for (const timer of pending.splice(0)) if (!timer.cleared) timer.callback(); },
  };
}

function fixture() {
  const transport = new RecordingTransport();
  const log = new RecordingLog();
  const clipboard = fakeClipboard();
  const timers = fakeTimers();
  const clock = { now: 1_726_000_000_000 };
  const coordinator = new SessionCoordinator({
    transport,
    source: new FakeAccessibilitySource(),
    gate: new PermissionGate(200),
    log,
    now: () => clock.now,
    clipboard,
    timers,
  });
  return {
    transport, log, clipboard, timers, clock, coordinator,
    inbound(line) { coordinator.handleInbound(line); },
  };
}

describe('one-time codes: the clipboard', () => {
  it('gives the device the clock as soon as it says hello', () => {
    // A device that says hello has just booted, and has no clock of its own.
    const f = fixture();
    f.inbound('{"v":1,"type":"hello","app":"chickenmicro-fw"}');
    assert.deepEqual(f.transport.framesOfType('time'), [{ v: 1, type: 'time', unixMs: f.clock.now }]);
  });

  it('copies the code, and never writes it to the log', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub:alice', code: '482913', expiresInMs: 18000 }));
    assert.deepEqual(f.clipboard.copies, ['482913']);
    assert.ok(f.log.mentions('GitHub:alice'), 'the account is logged');
    assert.ok(!f.log.mentions('482913'), 'the code is not');
  });

  it('wipes the code once its window has closed', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub', code: '482913', expiresInMs: 18000 }));
    assert.equal(f.timers.pending[0].ms, 18000 + OTP_CLEAR_GRACE_MS);
    f.timers.runAll();
    assert.equal(f.clipboard.text, '', 'an expired code does not linger to be pasted by mistake');
  });

  it('leaves the clipboard alone when the user copied something since', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub', code: '482913', expiresInMs: 18000 }));
    f.clipboard.text = 'the user\'s own text';
    f.timers.runAll();
    assert.equal(f.clipboard.text, 'the user\'s own text');
  });

  it('follows only the latest code', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub', code: '482913', expiresInMs: 18000 }));
    f.inbound(otpLine({ label: 'GitLab', code: '111222', expiresInMs: 25000 }));
    assert.equal(f.timers.pending.filter((timer) => !timer.cleared).length, 1);
    f.timers.runAll();
    assert.equal(f.clipboard.text, '');
    assert.deepEqual(f.clipboard.copies, ['482913', '111222', '']);
  });

  it('never logs a code, not even from a frame it refuses', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub', code: '482913', expiresInMs: 999_999 }));
    assert.deepEqual(f.clipboard.copies, [], 'a refused frame copies nothing');
    assert.ok(f.log.mentions('rejected'), 'the refusal is logged');
    assert.ok(!f.log.mentions('482913'), 'without the code');
  });

  it('takes a live code off the clipboard when it stops', () => {
    const f = fixture();
    f.inbound(otpLine({ label: 'GitHub', code: '482913', expiresInMs: 18000 }));
    f.coordinator.stop();
    assert.equal(f.clipboard.text, '');
  });
});
