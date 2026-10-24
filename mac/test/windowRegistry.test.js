import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { WindowRegistry } from '../src/accessibility/windowRegistry.js';

/**
 * A registry over toy "elements".
 *
 * An element is `{ key, alive }`; the registry only ever sees it as an opaque
 * reference. `alive` stands in for whether its window still exists — the thing
 * the real daemon reads back off the live element.
 */
function harness() {
  const released = [];
  const registry = new WindowRegistry({
    release: (element) => released.push(element),
    isAlive: (element) => element.alive,
  });
  return { registry, released };
}

const element = (key) => ({ key, alive: true });
const visible = (...elements) => elements.map((element) => ({ key: element.key, element }));
const keys = (windows) => windows.map((window) => window.key);

describe('window registry — surviving other Spaces', () => {
  it('keeps a window that left AXWindows but is still alive', () => {
    // The reported bug: a full-screen app takes the Space in front, AXWindows
    // comes back empty, and every session vanished.
    const { registry } = harness();
    const window = element('w7');
    registry.update(42, visible(window));
    assert.deepEqual(keys(registry.update(42, [])), ['w7']);
  });

  it('lets a window go once its element reports it gone, poll after poll', () => {
    const { registry, released } = harness();
    const window = element('w7');
    registry.update(42, visible(window));

    window.alive = false;   // the window was closed
    registry.update(42, []);
    registry.update(42, []);
    assert.deepEqual(registry.update(42, []), []);
    assert.ok(released.includes(window));
  });

  it('keeps a window whose element did not answer once', () => {
    // An app too busy to answer reads exactly like a closed window. Believing
    // it renumbered every session on the box and made the request id the
    // device was showing go stale.
    const { registry, released } = harness();
    const window = element('w7');
    registry.update(42, visible(window));

    window.alive = false;
    assert.deepEqual(keys(registry.update(42, [])), ['w7']);
    window.alive = true;                                   // it answers again
    assert.deepEqual(keys(registry.update(42, [])), ['w7']);
    assert.deepEqual(released, []);

    // And the count starts afresh, so one slow poll now and one later never add up.
    window.alive = false;
    registry.update(42, []);
    registry.update(42, []);
    assert.deepEqual(keys(registry.update(42, [])), []);
  });

  it('picks up a never-seen window the first time it is on the active Space', () => {
    const { registry } = harness();
    assert.deepEqual(registry.update(42, []), []);
    const late = element('w9');
    assert.deepEqual(keys(registry.update(42, visible(late))), ['w9']);
  });

  it('keeps an off-Space window across many polls that never list it', () => {
    const { registry } = harness();
    const window = element('w7');
    registry.update(42, visible(window));
    for (let poll = 0; poll < 10; poll += 1) {
      assert.deepEqual(keys(registry.update(42, [])), ['w7']);
    }
  });
});

describe('window registry — references', () => {
  it('holds one reference per window however many polls it survives', () => {
    // Every poll hands over a fresh reference, even when the app returns the
    // same cached element each time — so every older one has to be released.
    const { registry, released } = harness();
    const cached = element('w7');
    for (let poll = 0; poll < 5; poll += 1) {
      registry.update(42, [{ key: 'w7', element: cached }]);
    }
    // Same object each time: nothing to release.
    assert.equal(released.length, 0);

    const fresh = () => registry.update(42, visible(element('w7')));
    fresh();
    assert.equal(released.length, 1, 'a new reference for the same window frees the old one');
  });

  it('lets go of every window of a process that exited', () => {
    const { registry, released } = harness();
    registry.update(42, visible(element('w7'), element('w8')));
    registry.retainOnly(new Set([99]));
    assert.equal(released.length, 2);
  });

  it('keeps processes listed in retainOnly', () => {
    const { registry, released } = harness();
    registry.update(42, visible(element('w7')));
    registry.retainOnly(new Set([42]));
    assert.equal(released.length, 0);
  });
});

describe('window registry — ordering', () => {
  it('keeps first-seen order, and a refresh does not reshuffle it', () => {
    const { registry } = harness();
    const a = element('wa');
    const b = element('wb');
    registry.update(42, visible(a, b));
    // b comes back alone this poll, then both again: order must hold.
    registry.update(42, visible(b));
    assert.deepEqual(keys(registry.update(42, visible(b, a))), ['wa', 'wb']);
  });

  it('appends a newly opened window after the existing ones', () => {
    const { registry } = harness();
    const a = element('wa');
    registry.update(42, visible(a));
    assert.deepEqual(keys(registry.update(42, visible(a, element('wz')))), ['wa', 'wz']);
  });
});
