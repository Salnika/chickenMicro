import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

/**
 * The daemon's log on disk.
 *
 * The terminal's scrollback is not a record: the warnings that matter — a copy
 * of Claude Desktop that stopped answering, a verdict the daemon refused, a
 * link that dropped — are logged once, hours before anyone looks. This keeps
 * them, in ~/Library/Logs, where macOS keeps a user's logs and Console shows
 * them.
 *
 * Writes are synchronous. The volume is a few lines a second at most, and a log
 * that survives a crash is the whole point of having one.
 *
 * Nothing here may take the daemon down: a full disk, a read-only home, a path
 * that is not writable — each is reported once and then the file is given up,
 * while the terminal keeps its stream.
 */

/** Where the log goes when no path is given. */
export function defaultLogPath() {
  return path.join(os.homedir(), 'Library', 'Logs', 'claude-remote', 'claude-remote.log');
}

/** Rotate at 2 MB, which is several days of an ordinary desk session. */
export const DEFAULT_MAX_BYTES = 2 * 1024 * 1024;
/** Rotated files kept beside the live one: claude-remote.log.1 … .3 */
export const DEFAULT_KEEP = 3;

export class LogFile {
  #path;
  #maxBytes;
  #keep;
  #banner;
  #onError;
  #fd = null;
  #size = 0;
  #broken = false;

  /**
   * @param {object} [options]
   * @param {string} [options.path]     file to append to
   * @param {number} [options.maxBytes] rotate once the file passes this size
   * @param {number} [options.keep]     how many rotated files to keep
   * @param {string} [options.banner]   one line written when the file opens,
   *   so a reader can tell one run of the daemon from the next
   * @param {(error: Error) => void} [options.onError]  called once, the first
   *   time writing fails
   */
  constructor({
    path: filePath = defaultLogPath(),
    maxBytes = DEFAULT_MAX_BYTES,
    keep = DEFAULT_KEEP,
    banner = null,
    onError = () => {},
  } = {}) {
    this.#path = filePath;
    this.#maxBytes = maxBytes;
    this.#keep = keep;
    this.#banner = banner;
    this.#onError = onError;
    // Opened now rather than on the first line, so the banner carries the time
    // the run started and the file is there even when nothing is logged.
    try {
      this.#open();
    } catch (error) {
      this.#giveUp(error);
    }
  }

  get path() {
    return this.#path;
  }

  /** Appends one line, reopening the file if it was closed. */
  write(line) {
    if (this.#broken) return;
    try {
      if (this.#fd === null) this.#open();
      const bytes = Buffer.byteLength(line);
      // Rotate before the line rather than after, so the limit is never passed.
      if (this.#size > 0 && this.#size + bytes > this.#maxBytes) this.#rotate();
      fs.writeSync(this.#fd, line);
      this.#size += bytes;
    } catch (error) {
      this.#giveUp(error);
    }
  }

  close() {
    if (this.#fd === null) return;
    try {
      fs.closeSync(this.#fd);
    } catch {
      // Closing a file that is already gone is not worth reporting.
    }
    this.#fd = null;
  }

  #open() {
    fs.mkdirSync(path.dirname(this.#path), { recursive: true });
    this.#fd = fs.openSync(this.#path, 'a');
    this.#size = fs.fstatSync(this.#fd).size;
    if (this.#banner !== null) {
      const line = `\n=== ${new Date().toISOString()} ${this.#banner} ===\n`;
      fs.writeSync(this.#fd, line);
      this.#size += Buffer.byteLength(line);
    }
  }

  /** claude-remote.log -> .1, .1 -> .2, …; the oldest falls off the end. */
  #rotate() {
    this.close();
    const oldest = `${this.#path}.${this.#keep}`;
    fs.rmSync(oldest, { force: true });
    for (let index = this.#keep - 1; index >= 1; index -= 1) {
      const from = `${this.#path}.${index}`;
      if (fs.existsSync(from)) fs.renameSync(from, `${this.#path}.${index + 1}`);
    }
    if (this.#keep >= 1) fs.renameSync(this.#path, `${this.#path}.1`);
    else fs.rmSync(this.#path, { force: true });
    this.#open();
  }

  #giveUp(error) {
    this.#broken = true;
    this.close();
    this.#onError(error);
  }
}
