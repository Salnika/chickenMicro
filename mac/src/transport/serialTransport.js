import { redactForLog } from '../protocol/frames.js';
import { SerialPort } from 'serialport';

import { LogTag } from '../log.js';
import { MAX_LINE_BYTES, RECONNECT_DELAY_MS } from '../protocol/constants.js';
import { LineAssembler } from '../protocol/lineAssembler.js';
import { bestGuess } from './portDiscovery.js';

/**
 * Serial link with automatic port discovery and reconnection.
 *
 * Reconnection is the Mac's job (protocol/PROTOCOL.md §6): on any error or port
 * disappearance the handle is closed, and the loop rescans every
 * RECONNECT_DELAY_MS until the device reappears. On every successful open the
 * session re-sends `hello` and the full snapshot, so the device resynchronises
 * without needing its own request mechanism.
 */
export class SerialTransport {
  onLine = null;
  onConnect = null;

  #port = null;
  #log;
  #assembler = new LineAssembler();
  #reportedDrops = 0;
  #shouldRun = false;
  #retryTimer = null;
  #loggedWaiting = false;

  constructor({ path = null, baudRate = 115200, log }) {
    this.requestedPath = path;
    this.baudRate = baudRate;
    this.#log = log;
  }

  get isConnected() {
    return this.#port?.isOpen === true;
  }

  start() {
    this.#shouldRun = true;
    this.#open();
  }

  stop() {
    this.#shouldRun = false;
    this.#clearRetry();
    this.#closePort();
  }

  /** Drops the link so it is reopened from scratch. */
  reset() {
    this.#log.info(LogTag.serial, 'resetting link');
    this.#closePort();
    this.#scheduleRetry();
  }

  send(line) {
    if (!this.isConnected) {
      this.#log.debug(LogTag.serial, `dropped frame, link down: ${line.trimEnd()}`);
      return;
    }
    this.#port.write(line, (error) => {
      if (error) {
        this.#log.warn(LogTag.serial, `write failed: ${error.message}; reconnecting`);
        this.#closePort();
        this.#scheduleRetry();
        return;
      }
      this.#log.debug(LogTag.serial, `-> ${line.trimEnd()}`);
    });
  }

  // --- Connection lifecycle -------------------------------------------------

  #resolvePath() {
    if (this.requestedPath !== null) return this.requestedPath;
    return bestGuess()?.path ?? null;
  }

  #open() {
    if (!this.#shouldRun) return;
    // A retry can fire while a port is already live — an error handler and a
    // close handler both schedule one. Opening a second reader on the same
    // device interleaves two byte streams into one assembler, which splices
    // frames together.
    if (this.#port !== null) return;

    const path = this.#resolvePath();
    if (path === null) {
      if (!this.#loggedWaiting) {
        this.#log.info(LogTag.serial, `waiting for ${this.requestedPath ?? 'any USB-serial port'}`);
        this.#loggedWaiting = true;
      }
      this.#scheduleRetry();
      return;
    }

    const port = new SerialPort({ path, baudRate: this.baudRate, autoOpen: false });

    port.open((error) => {
      if (error) {
        // Absent or busy: normal while the board is unplugged or the monitor
        // holds the port, so this is debug rather than a warning.
        this.#log.debug(LogTag.serial, `cannot open ${path}: ${error.message}`);
        this.#scheduleRetry();
        return;
      }

      this.#port = port;
      this.#loggedWaiting = false;
      this.#assembler.reset();
      this.#log.info(LogTag.serial, `connected ${path} @ ${this.baudRate} 8N1`);
      this.onConnect?.();
    });

    // Bind the handler to *this* port object. A port that has been replaced
    // must never feed the shared assembler again: two byte streams interleaved
    // into one assembler splice frames together, which surfaces as oversized
    // lines and stray fragments like `"}`.
    port.on('data', (chunk) => {
      if (this.#port !== port) return;
      this.#consume(chunk);
    });

    port.on('error', (error) => {
      this.#log.warn(LogTag.serial, `link error on ${path}: ${error.message}`);
      this.#closePort();
      this.#scheduleRetry();
    });

    port.on('close', () => {
      if (this.#port === port) {
        this.#log.warn(LogTag.serial, `link lost on ${path}`);
        this.#port = null;
        this.#scheduleRetry();
      }
    });
  }

  #consume(chunk) {
    const lines = this.#assembler.feed(chunk);
    if (this.#assembler.droppedLines !== this.#reportedDrops) {
      this.#reportedDrops = this.#assembler.droppedLines;
      this.#log.warn(
        LogTag.serial,
        `dropped ${this.#reportedDrops} oversized line(s) (> ${MAX_LINE_BYTES} B)`,
      );
    }
    for (const line of lines) {
      this.#log.debug(LogTag.serial, `<- ${redactForLog(line)}`);
      this.onLine?.(line);
    }
  }

  #closePort() {
    const port = this.#port;
    this.#port = null;
    if (!port) return;

    // Detach before closing: `serialport` can still emit buffered `data` while
    // the close is in flight, and those bytes belong to a stream that is over.
    port.removeAllListeners('data');
    port.removeAllListeners('error');
    port.removeAllListeners('close');
    // Half a frame may be sitting in the assembler; carrying it into the next
    // connection would glue it to the head of a new frame.
    this.#assembler.reset();

    if (port.isOpen) {
      port.close(() => { /* already reported */ });
    }
  }

  #scheduleRetry() {
    if (!this.#shouldRun || this.#retryTimer !== null) return;
    this.#retryTimer = setTimeout(() => {
      this.#retryTimer = null;
      this.#open();
    }, RECONNECT_DELAY_MS);
    this.#retryTimer.unref?.();
  }

  #clearRetry() {
    if (this.#retryTimer !== null) {
      clearTimeout(this.#retryTimer);
      this.#retryTimer = null;
    }
  }
}
