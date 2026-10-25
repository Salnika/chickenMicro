import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { chooseAccessibility } from '../src/accessibility/choose.js';
import { NullAccessibilitySource } from '../src/accessibility/nullSource.js';

describe('choosing whether to read Claude Desktop', () => {
  it('never loads the native module when it is switched off', async () => {
    let loaded = false;
    const choice = await chooseAccessibility({
      off: true,
      load: async () => { loaded = true; return {}; },
    });
    assert.equal(loaded, false, 'a terminal or VS Code user must not need it to load');
    assert.equal(choice.trusted, false);
    assert.equal(choice.module, null);
    assert.match(choice.why, /--no-accessibility/);
  });

  it('carries on without it when the native module will not load', async () => {
    const choice = await chooseAccessibility({
      load: async () => { throw new Error('dlopen failed'); },
    });
    assert.equal(choice.trusted, false);
    assert.equal(choice.module, null);
    assert.match(choice.why, /did not load.*dlopen failed/);
  });

  it('carries on without it when the grant is missing, and keeps the module to explain it', async () => {
    const module = { isTrusted: () => false };
    const choice = await chooseAccessibility({ load: async () => module });
    assert.equal(choice.trusted, false);
    assert.equal(choice.module, module, 'a caller that insists on the grant needs it');
    assert.match(choice.why, /granted/);
  });

  it('reads Claude Desktop when the grant is there', async () => {
    const module = { isTrusted: () => true };
    const choice = await chooseAccessibility({ load: async () => module });
    assert.equal(choice.trusted, true);
    assert.equal(choice.module, module);
    assert.equal(choice.why, '');
  });
});

describe('a source with nothing to read', () => {
  it('finds no Claude, no window and no tree', () => {
    const source = new NullAccessibilitySource();
    assert.equal(source.targetPid, null);
    assert.equal(source.runningClaude(), null);
    assert.deepEqual(source.runningProcesses(), []);
    assert.equal(source.readTree(), null);
    assert.equal(source.readSessions(), null);
    assert.deepEqual(source.refusedProcesses(), []);
  });

  it('presses nothing, whatever it is asked', () => {
    assert.throws(() => new NullAccessibilitySource().pressRef({ marker: 'x' }), /no button to press/);
  });
});
