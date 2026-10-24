import { LogTag } from '../log.js';
import { fixtures } from './fixtures.js';

export const SCENES = [
  'idle', 'thinking', 'writing', 'message',
  'permission', 'long-permission', 'sessions', 'queue', 'error', 'demo',
];

/** The scenes `demo` walks through, in order. */
const DEMO_ORDER = [
  'idle', 'thinking', 'writing', 'permission', 'long-permission', 'sessions', 'queue', 'error',
];

/**
 * Drives the device from synthetic trees, with Claude Desktop closed.
 *
 * Scenes are pushed through the *real* interpreter, gate and coordinator, so
 * what the firmware receives is byte-identical to a live session — request ids
 * and approve gating included. That is what makes the box developable on its
 * own.
 */
export class Simulator {
  #coordinator;
  #source;
  #log;
  #timer = null;

  constructor({ coordinator, source, log }) {
    this.#coordinator = coordinator;
    this.#source = source;
    this.#log = log;
  }

  play(scene) {
    switch (scene) {
      case 'idle':
        this.#log.info(LogTag.sim, 'scene: idle');
        this.#source.tree = fixtures.idle();
        this.#coordinator.refresh();
        break;

      case 'thinking':
        this.#log.info(LogTag.sim, 'scene: thinking');
        this.#source.tree = fixtures.thinking();
        this.#coordinator.refresh();
        break;

      case 'writing':
      case 'message':
        this.#log.info(LogTag.sim, `scene: ${scene}`);
        this.#source.tree = fixtures.longMessage();
        this.#coordinator.refresh();
        break;

      case 'permission':
        this.#log.info(LogTag.sim, 'scene: permission');
        this.#source.tree = fixtures.permission();
        this.#coordinator.refresh();
        break;

      case 'long-permission':
        this.#log.info(LogTag.sim, 'scene: long permission');
        this.#source.tree = fixtures.longPermission();
        this.#coordinator.refresh();
        break;

      case 'sessions':
        this.#log.info(LogTag.sim, 'scene: three sessions');
        this.#source.tree = fixtures.workspace([
          { name: 'chickenMicro', tree: fixtures.thinking('Improve the device interface') },
          { name: 'table-header refactor', tree: fixtures.longMessage() },
          { name: 'docs', tree: fixtures.idle() },
        ]);
        this.#coordinator.refresh();
        break;

      case 'queue':
        // Two windows asking at once: the device answers them one at a time,
        // oldest first, and says how many are still waiting.
        this.#log.info(LogTag.sim, 'scene: two permissions at once');
        this.#source.tree = fixtures.workspace([
          { name: 'chickenMicro', tree: fixtures.permission('Run command', 'rm -rf build') },
          { name: 'docs', tree: fixtures.permission('Write file', 'docs/wiring.md') },
        ]);
        this.#coordinator.refresh();
        break;

      case 'error':
        this.#log.info(LogTag.sim, 'scene: error');
        this.#source.tree = null;
        this.#coordinator.refresh();
        this.#coordinator.showError('simulated failure');
        break;

      case 'demo':
        this.runDemo();
        break;

      default:
        throw new Error(`unknown scene "${scene}"`);
    }
  }

  /** Every scene once, without waiting — used by `--stdout`. */
  playAll() {
    for (const scene of DEMO_ORDER) this.play(scene);
  }

  /**
   * Cycles through every scene so the e-ink layouts can be reviewed in one
   * pass. Pressing APPROVE or DENY during a permission scene goes through the
   * normal coordinator path: validated, logged, and recorded by the fake tree.
   */
  runDemo(sceneDurationMs = 6000) {
    let index = 0;
    this.play(DEMO_ORDER[0]);
    this.#timer = setInterval(() => {
      index = (index + 1) % DEMO_ORDER.length;
      this.play(DEMO_ORDER[index]);
    }, sceneDurationMs);
    this.#timer.unref?.();
  }

  stop() {
    if (this.#timer !== null) {
      clearInterval(this.#timer);
      this.#timer = null;
    }
  }
}
