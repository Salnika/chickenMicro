import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { AXNode } from '../src/accessibility/axNode.js';
import {
  ExposurePolicy,
  UNEXPOSED_TREE_NODES,
  looksUnexposed,
  refusalMessage,
} from '../src/accessibility/readiness.js';

/** A window whose tree holds `count` nodes, itself included. */
function windowOf(count) {
  const children = Array.from({ length: count - 1 }, () => new AXNode({ role: 'AXGroup' }));
  return new AXNode({ role: 'AXWindow', children });
}

describe('readiness — a window showing none of its web content', () => {
  it('reads the chrome-only window seen on a real Mac as unexposed', () => {
    // 14 nodes: title-bar buttons and an empty web area, observed for minutes
    // on a copy asked only once.
    assert.equal(looksUnexposed(windowOf(14)), true);
  });

  it('reads a window with its conversation as exposed', () => {
    assert.equal(looksUnexposed(windowOf(462)), false);
    assert.equal(looksUnexposed(windowOf(UNEXPOSED_TREE_NODES)), false);
    assert.equal(looksUnexposed(windowOf(UNEXPOSED_TREE_NODES - 1)), true);
  });
});

describe('readiness — when to ask a copy for its web content', () => {
  it('asks every copy the first time it is seen', () => {
    const policy = new ExposurePolicy({ intervalMs: 5000 });
    assert.equal(policy.shouldAsk(101, 0, false), 'first');
    assert.equal(policy.shouldAsk(202, 0, false), 'first');
  });

  it('never asks again while its windows show their content', () => {
    const policy = new ExposurePolicy({ intervalMs: 5000 });
    policy.shouldAsk(101, 0, false);
    assert.equal(policy.shouldAsk(101, 60_000, false), null);
  });

  it('asks again while a window stays empty, once per interval', () => {
    const policy = new ExposurePolicy({ intervalMs: 5000 });
    policy.shouldAsk(101, 0, false);
    assert.equal(policy.shouldAsk(101, 250, true), null);     // too soon
    assert.equal(policy.shouldAsk(101, 5000, true), 'again');
    assert.equal(policy.shouldAsk(101, 5250, true), null);    // spaced from that one
    assert.equal(policy.shouldAsk(101, 10_000, true), 'again');
  });

  it('treats a reused pid as a new copy once the old one has quit', () => {
    const policy = new ExposurePolicy({ intervalMs: 5000 });
    policy.shouldAsk(101, 0, false);
    policy.retainOnly(new Set([202]));
    assert.equal(policy.shouldAsk(101, 100, false), 'first');
  });
});

describe('readiness — a copy refusing Accessibility', () => {
  it('names the update as the cause when the executable is gone', () => {
    const message = refusalMessage({ pid: 36838, profile: 'Claude-Work', executableGone: true });
    assert.match(message, /pid 36838, profile Claude-Work/);
    assert.match(message, /updated while running/);
    assert.match(message, /Quit that copy and open it again/);
  });

  it('still says what to do when the cause is unknown', () => {
    const message = refusalMessage({ pid: 44477, profile: null, executableGone: false });
    assert.match(message, /Claude Desktop \(pid 44477\)/);
    assert.doesNotMatch(message, /profile/);
    assert.match(message, /kAXErrorAPIDisabled/);
    assert.match(message, /Quit that copy and open it again/);
  });
});
