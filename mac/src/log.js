// Tiny levelled logger writing `[TAG] message` to stderr, so stdout stays clean
// for command output such as `ports` and `dump`.
//
// The same lines can go to a file as well (see system/logFile.js), with a level
// of their own: a terminal one reads live wants less than a record one reads
// after the fact. The file never gets the colour codes, and always gets a full
// date — a log read a week later needs to say which day.

import process from 'node:process';

export const LogTag = {
  ax: 'AX',
  serial: 'SERIAL',
  session: 'SESSION',
  sim: 'SIM',
  device: 'DEVICE',
  hook: 'HOOK',
};

const LEVELS = { debug: 0, info: 1, warn: 2, error: 3 };

/**
 * ANSI 256-colour codes for tagging sessions, chosen to stay legible on both
 * light and dark terminals and to be easy to tell apart from each other. A
 * session keeps its colour for the life of the daemon, so `[SESSION s2]` is the
 * same hue every time it appears and the eye can follow one conversation down
 * the log.
 */
const SESSION_COLORS = [39, 208, 41, 170, 214, 45, 205, 154];
const RESET = '[0m';

/**
 * Picks a session's colour from its id.
 *
 * `s1`, `s2`, … map to consecutive entries so adjacent sessions never collide;
 * anything else falls back to a hash, so a non-numbered id still gets a stable
 * colour rather than throwing the scheme off.
 */
function colorIndex(key) {
  const numbered = /^s(\d+)$/.exec(key);
  if (numbered !== null) return (Number.parseInt(numbered[1], 10) - 1) % SESSION_COLORS.length;
  let hash = 0;
  for (let index = 0; index < key.length; index += 1) hash = (hash * 31 + key.charCodeAt(index)) | 0;
  return Math.abs(hash) % SESSION_COLORS.length;
}

export class Log {
  #level;
  #timestamps;
  #color;
  #out;
  #file;
  #fileLevel;

  /**
   * @param {object} [options]
   * @param {string} [options.level]
   * @param {boolean} [options.timestamps]
   * @param {boolean} [options.color]  colour session tags; defaults to on only
   *   when stderr is a terminal and NO_COLOR is unset.
   * @param {{write: (line: string) => void, close: () => void}} [options.file]
   *   where to keep the same lines, if anywhere.
   * @param {string} [options.fileLevel]  level for the file; defaults to the
   *   terminal's, so `--log debug` is recorded as well as shown.
   * @param {{write: (line: string) => void}} [options.out]  the stream to
   *   write to, for tests.
   */
  constructor({ level = 'info', timestamps = true, color, file = null, fileLevel, out } = {}) {
    this.#level = LEVELS[level] ?? LEVELS.info;
    this.#timestamps = timestamps;
    this.#color = color ?? (process.stderr.isTTY === true && !process.env.NO_COLOR);
    this.#out = out ?? process.stderr;
    this.#file = file;
    this.#fileLevel = LEVELS[fileLevel] ?? this.#level;
  }

  /** Where the lines are being kept, if they are. */
  get filePath() {
    return this.#file?.path ?? null;
  }

  /** Closes the log file, if any. Safe to call twice. */
  close() {
    this.#file?.close();
    this.#file = null;
  }

  static isLevel(name) {
    return Object.hasOwn(LEVELS, name);
  }

  /** Whether colour is on, so callers can skip building coloured strings. */
  get color() {
    return this.#color;
  }

  /**
   * Wraps `text` (default: the id itself) in a session's colour. Returns it
   * unchanged when colour is off, so it is always safe to call.
   */
  paint(sessionId, text = sessionId) {
    if (!this.#color) return text;
    return `[38;5;${SESSION_COLORS[colorIndex(sessionId)]}m${text}${RESET}`;
  }

  /**
   * A `[SESSION s2]` tag in that session's colour, for a log line about one
   * session. Falls back to the plain tag when colour is off.
   */
  sessionTag(sessionId) {
    return `${LogTag.session} ${sessionId}`;
  }

  debug(tag, message, sessionId) { this.#write('debug', tag, message, sessionId); }
  info(tag, message, sessionId) { this.#write('info', tag, message, sessionId); }
  warn(tag, message, sessionId) { this.#write('warn', tag, message, sessionId); }
  error(tag, message, sessionId) { this.#write('error', tag, message, sessionId); }

  #write(level, tag, message, sessionId) {
    const toTerminal = LEVELS[level] >= this.#level;
    const toFile = this.#file !== null && LEVELS[level] >= this.#fileLevel;
    if (!toTerminal && !toFile) return;

    const now = new Date();
    const mark = level === 'warn' ? '! ' : level === 'error' ? '!! ' : '';
    // `message` may be a function, so building it can be skipped when the level
    // is filtered out — which is why it is built here and not above.
    const text = typeof message === 'function' ? message() : message;

    if (toTerminal) {
      const stamp = this.#timestamps ? `${now.toISOString().slice(11, 23)} ` : '';
      // The whole `[TAG]` is coloured when the line is about a particular
      // session, so the bracketed tag itself carries the session's hue.
      const label = sessionId !== undefined && this.#color
        ? this.paint(sessionId, `[${tag}]`)
        : `[${tag}]`;
      this.#out.write(`${stamp}${mark}${label} ${text}\n`);
    }
    if (toFile) this.#file.write(`${now.toISOString()} ${mark}[${tag}] ${text}\n`);
  }
}
