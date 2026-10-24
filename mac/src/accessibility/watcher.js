import { LogTag } from '../log.js';

/**
 * Notifies when Claude Desktop's UI may have changed.
 *
 * The Swift version used AXObserver, which pushes notifications instead of
 * polling. That is not available here: AXObserver delivers its callbacks on a
 * CFRunLoop, and Node's loop is libuv. Pumping a CFRunLoop from a timer to get
 * callbacks back into JS is possible but puts an FFI callback on the path that
 * presses "Allow", which is not a good trade.
 *
 * So this polls — but adaptively, which keeps it from being aggressive:
 *
 *   - reads are timed, and the next delay is at least `dutyCycle` times the
 *     last read duration, so a slow tree throttles itself instead of pinning a
 *     core (a 400 ms read polls every ~1.6 s, not every 500 ms);
 *   - the cheap `ps` check for the process appearing or disappearing runs on
 *     every tick regardless, because it costs nothing;
 *   - the interval is clamped so a pathological tree cannot stall the UI
 *     entirely.
 */
export class ClaudeWatcher {
  #source;
  #log;
  #onChange;
  #timer = null;
  #stopped = false;
  #lastPids = null;
  #lastReadMs = 0;

  constructor({
    source,
    log,
    onChange,
    // How quickly a change in Claude reaches the box is bounded by this poll:
    // fast enough to feel responsive, and the duty-cycle backoff below still
    // keeps a slow tree from pinning a core.
    pollIntervalMs = 250,
    maxIntervalMs = 1500,
    /** Multiple of the last read duration to idle for before reading again. */
    dutyCycle = 2,
  }) {
    this.#source = source;
    this.#log = log;
    this.#onChange = onChange;
    this.pollIntervalMs = pollIntervalMs;
    this.maxIntervalMs = maxIntervalMs;
    this.dutyCycle = dutyCycle;
  }

  start() {
    this.#stopped = false;
    this.#schedule(0);
  }

  stop() {
    this.#stopped = true;
    if (this.#timer !== null) {
      clearTimeout(this.#timer);
      this.#timer = null;
    }
  }

  /** Duration of the most recent tree read, for `doctor` and the logs. */
  get lastReadMs() {
    return this.#lastReadMs;
  }

  #schedule(delay) {
    if (this.#stopped) return;
    this.#timer = setTimeout(() => this.#tick(), delay);
    // A pending poll must not hold the process open on its own.
    this.#timer.unref?.();
  }

  #tick() {
    if (this.#stopped) return;

    // Several copies of Claude Desktop can run at once — one per profile — so
    // this tracks the whole set. Reporting only the first made a second
    // instance's windows look like they came from nowhere.
    const running = this.#source.runningProcesses?.()
      ?? [this.#source.runningClaude?.()].filter(Boolean);
    const pids = running.map((process) => process.pid).sort((a, b) => a - b).join(',');
    if (pids !== this.#lastPids) {
      if (pids === '') {
        this.#log.info(LogTag.ax, 'Claude Desktop is not running');
      } else {
        this.#log.info(
          LogTag.ax,
          `Claude Desktop: ${running.length} instance(s) (pid ${pids.split(',').join(', ')})`,
        );
      }
      this.#lastPids = pids;
    }

    const started = Date.now();
    try {
      this.#onChange();
    } catch (error) {
      this.#log.error(LogTag.ax, `poll failed: ${error.message}`);
    }
    this.#lastReadMs = Date.now() - started;

    const next = Math.min(
      Math.max(this.pollIntervalMs, this.#lastReadMs * this.dutyCycle),
      this.maxIntervalMs,
    );
    if (this.#lastReadMs > this.pollIntervalMs) {
      this.#log.debug(
        LogTag.ax,
        `tree read took ${this.#lastReadMs} ms; backing off to ${next} ms`,
      );
    }
    this.#schedule(next);
  }
}
