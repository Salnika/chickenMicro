import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { PermissionGate } from '../src/session/permissionGate.js';
import { RequestIdAllocator } from '../src/session/requestIdAllocator.js';

describe('RequestIdAllocator', () => {
  it('keeps the same id while the dialog content is unchanged', () => {
    const allocator = new RequestIdAllocator();
    const first = allocator.idFor('shell|git push');
    assert.equal(allocator.idFor('shell|git push'), first);
  });

  it('allocates a fresh id when the command changes', () => {
    const allocator = new RequestIdAllocator();
    const first = allocator.idFor('shell|git push');
    assert.notEqual(allocator.idFor('shell|rm -rf /'), first);
  });

  it('allocates a fresh id for identical content after a clear', () => {
    // Otherwise a press left over from the previous dialog would match the new
    // one.
    const allocator = new RequestIdAllocator();
    const before = allocator.idFor('shell|git push');
    allocator.clear();
    assert.notEqual(allocator.idFor('shell|git push'), before);
  });

  it('keeps fingerprint field boundaries distinct', () => {
    assert.notEqual(
      RequestIdAllocator.fingerprint('shell', 'ab', 'c'),
      RequestIdAllocator.fingerprint('shell', 'a', 'bc'),
    );
  });
});

describe('PermissionGate', () => {
  const t0 = 1_000_000;
  const request = { id: 'req-1', kind: 'shell', content: 'git push', actions: ['deny', 'approve'] };

  const armed = () => {
    const gate = new PermissionGate(200);
    gate.present(request, t0);
    return gate;
  };

  it('accepts a matching id once the arming delay has elapsed', () => {
    const gate = armed();
    assert.equal(gate.validate('approve', 'req-1', t0 + 500).accepted, true);
    assert.equal(gate.validate('deny', 'req-1', t0 + 500).accepted, true);
  });

  it('refuses a stale id', () => {
    const verdict = armed().validate('approve', 'req-0', t0 + 500);
    assert.equal(verdict.accepted, false);
    assert.match(verdict.reason, /stale requestId req-0, current is req-1/);
  });

  it('refuses a missing id', () => {
    assert.equal(armed().validate('approve', null, t0 + 500).accepted, false);
  });

  it('refuses when nothing is pending', () => {
    const verdict = new PermissionGate(200).validate('approve', 'req-1', t0);
    assert.equal(verdict.accepted, false);
    assert.match(verdict.reason, /no permission is pending/);
  });

  it('refuses after a clear', () => {
    const gate = armed();
    gate.clear();
    assert.equal(gate.validate('approve', 'req-1', t0 + 500).accepted, false);
    assert.equal(gate.pending, null);
  });

  it('refuses before the arming delay elapses', () => {
    // A press already travelling down the wire when the screen changed must not
    // land on the new request.
    const verdict = armed().validate('approve', 'req-1', t0 + 50);
    assert.equal(verdict.accepted, false);
    assert.match(verdict.reason, /arming delay not elapsed \(150 ms left\)/);
  });

  it('does not restart the arming delay when the same request is re-presented', () => {
    // The watcher re-reads the tree constantly while a dialog is up; that must
    // not keep the request permanently unarmed.
    const gate = new PermissionGate(200);
    gate.present(request, t0);
    for (let tick = 50; tick <= 500; tick += 50) gate.present(request, t0 + tick);
    assert.equal(gate.validate('approve', 'req-1', t0 + 550).accepted, true);
  });

  it('restarts the arming delay for a genuinely new request', () => {
    const gate = new PermissionGate(200);
    gate.present(request, t0);
    gate.present({ ...request, id: 'req-2', content: 'rm -rf /' }, t0 + 1000);
    assert.equal(gate.validate('approve', 'req-2', t0 + 1050).accepted, false);
  });

  it('lets non-critical actions through with no pending request', () => {
    const gate = new PermissionGate(200);
    assert.equal(gate.validate('stop', null, t0).accepted, true);
    assert.equal(gate.validate('context', null, t0).accepted, true);
  });
});
