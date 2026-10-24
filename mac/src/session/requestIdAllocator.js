/**
 * Separator between fingerprint fields, so "ab" + "c" cannot collide with
 * "a" + "bc". A control character is used because it cannot appear in the
 * dialog text being fingerprinted.
 */
const FIELD_SEPARATOR = '\u0001';

/**
 * Allocates permission ids.
 *
 * The Accessibility tree carries no request identifier, so the daemon
 * synthesises one. The id must be *stable* while the same dialog stays on
 * screen (the tree is re-read on every tick) and *fresh* as soon as the
 * dialog's content differs — otherwise a press meant for one command could be
 * matched against the next one.
 */
export class RequestIdAllocator {
  #counter = 0;
  #fingerprint;
  #id;
  #prefix;

  /**
   * @param {string} prefix  namespace for the ids, so allocators belonging to
   *   different sessions cannot both mint "req-1" for two dialogs that happen
   *   to be showing at once. Must keep the id inside the protocol's charset.
   */
  constructor(prefix = '') {
    this.#prefix = prefix;
  }

  /** Returns the id for a request whose visible content hashes to `fingerprint`. */
  idFor(fingerprint) {
    if (fingerprint === this.#fingerprint && this.#id !== undefined) return this.#id;
    this.#counter += 1;
    this.#fingerprint = fingerprint;
    this.#id = `${this.#prefix}req-${this.#counter}`;
    return this.#id;
  }

  /**
   * Forgets the current request, so the next one gets a fresh id even if its
   * content happens to be identical.
   */
  clear() {
    this.#fingerprint = undefined;
    this.#id = undefined;
  }

  /** Fingerprint built from the fields a user would read on screen. */
  static fingerprint(kind, title, content) {
    return [kind, title ?? '', content ?? ''].join(FIELD_SEPARATOR);
  }
}
