import { MAX_LINE_BYTES } from './constants.js';

/**
 * Turns an arbitrary byte stream into complete NDJSON lines.
 *
 * Serial delivers partial reads, so this buffers until it sees `\n`. A line
 * that grows past MAX_LINE_BYTES is abandoned whole — the tail is *not* parsed
 * as if it were a fresh frame, which is what stops line noise from ever being
 * interpreted as an action.
 */
export class LineAssembler {
  #buffer = [];
  #overflowed = false;

  droppedLines = 0;

  /** Feeds bytes and returns whatever complete lines they completed. */
  feed(bytes) {
    const lines = [];
    for (const byte of bytes) {
      if (byte === 0x0a) {
        if (this.#overflowed) {
          this.droppedLines += 1;
          this.#overflowed = false;
        } else if (this.#buffer.length > 0) {
          lines.push(Buffer.from(this.#buffer).toString('utf8'));
        }
        this.#buffer.length = 0;
        continue;
      }
      if (byte === 0x0d) continue;
      if (this.#overflowed) continue;
      if (this.#buffer.length >= MAX_LINE_BYTES) {
        this.#overflowed = true;
        this.#buffer.length = 0;
        continue;
      }
      this.#buffer.push(byte);
    }
    return lines;
  }

  reset() {
    this.#buffer.length = 0;
    this.#overflowed = false;
  }
}
