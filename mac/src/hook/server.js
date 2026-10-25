import {
  chmodSync, existsSync, mkdirSync, renameSync, rmSync, statSync, unlinkSync, writeFileSync,
} from 'node:fs';
import net from 'node:net';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import process from 'node:process';

import { LogTag } from '../log.js';
import { HOOK_MAX_LINE_BYTES, decodeHookLine } from './wire.js';

/** Overrides where the socket lives, for the daemon and for the hook alike. */
export const SOCKET_ENV = 'CLAUDE_REMOTE_SOCKET';

/** How long a connection may sit without sending its request line. */
const REQUEST_WAIT_MS = 5_000;

export function defaultSocketPath(env = process.env) {
  return env[SOCKET_ENV] ?? join(homedir(), '.claude-remote', 'hook.sock');
}

/**
 * The Unix socket Claude Code hooks connect to.
 *
 * Reachable by this user only — the directory is 0700 and the socket 0600 — which
 * is the whole of its authentication. A process that can connect can put a
 * request on the box, which is harmless: nothing is answered until a key on the
 * box is pressed, and an answer goes only to the connection that asked.
 */
export class HookServer {
  #registry;
  #log;
  #path;
  #server = null;

  constructor({ registry, log, path = defaultSocketPath() }) {
    this.#registry = registry;
    this.#log = log;
    this.#path = path;
  }

  get path() { return this.#path; }

  async start() {
    const directory = dirname(this.#path);
    const created = !existsSync(directory);
    mkdirSync(directory, { recursive: true, mode: 0o700 });
    // Only a directory made here is tightened: a path given by the caller may
    // be somewhere shared, and is not ours to change.
    if (created) chmodSync(directory, 0o700);

    await this.#clearStale();
    await new Promise((resolve, reject) => {
      const server = net.createServer((socket) => this.#serve(socket));
      server.once('error', reject);
      server.listen(this.#path, () => {
        server.off('error', reject);
        this.#server = server;
        resolve();
      });
    });
    try {
      chmodSync(this.#path, 0o600);
    } catch (error) {
      // Listening but not locked down, and nobody holds the handle: take it back.
      this.stop();
      throw error;
    }
    this.#leaveNode(directory);
    this.#log.info(LogTag.hook, `listening on ${this.#path}`);
  }

  /**
   * A hook starts with whatever PATH its host gave it, and VS Code often gives one
   * without node. This daemon is running under a node that works, so it leaves
   * the path where the hook's launcher looks.
   *
   * The launcher runs what it finds there, so the file is only ever left in a
   * directory this user alone can write to, and put in place with a rename: a
   * file or a symlink already standing there is replaced, never written through.
   * It is a convenience, so failing to leave it never takes the hook channel down.
   */
  #leaveNode(directory) {
    const staged = join(directory, `.node.${process.pid}`);
    try {
      const { uid, mode } = statSync(directory);
      const mine = typeof process.getuid !== 'function' || uid === process.getuid();
      if (!mine || (mode & 0o022) !== 0) {
        this.#log.warn(
          LogTag.hook,
          `not leaving the path of node in ${directory}: it is not private to this user, `
          + 'so a hook will look for node on its own PATH',
        );
        return;
      }
      rmSync(staged, { force: true });
      writeFileSync(staged, `${process.execPath}\n`, { mode: 0o600, flag: 'wx' });
      renameSync(staged, join(directory, 'node'));
    } catch (error) {
      rmSync(staged, { force: true });
      this.#log.warn(LogTag.hook, `could not leave the path of node for the hook launcher (${error.message})`);
    }
  }

  stop() {
    this.#registry.close();
    if (this.#server === null) return;
    this.#server.close();
    this.#server = null;
    try { unlinkSync(this.#path); } catch { /* already gone */ }
  }

  /** A socket file left by a daemon that died is removed; a live one is respected. */
  #clearStale() {
    if (!existsSync(this.#path)) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const probe = net.connect(this.#path);
      probe.once('connect', () => {
        probe.destroy();
        reject(new Error(`another daemon is already listening on ${this.#path}`));
      });
      probe.once('error', (error) => {
        if (error.code === 'ECONNREFUSED' || error.code === 'ENOENT') {
          try { unlinkSync(this.#path); } catch { /* raced with another cleanup */ }
          resolve();
        } else {
          reject(error);
        }
      });
    });
  }

  #serve(socket) {
    socket.setEncoding('utf8');
    let buffer = '';
    let handled = false;
    let id = null;
    const idle = setTimeout(() => socket.destroy(), REQUEST_WAIT_MS);
    idle.unref?.();

    socket.on('data', (chunk) => {
      if (handled) return;
      buffer += chunk;
      if (Buffer.byteLength(buffer, 'utf8') > HOOK_MAX_LINE_BYTES) {
        handled = true;
        this.#log.warn(LogTag.hook, 'dropped an oversized hook request');
        socket.destroy();
        return;
      }
      const newline = buffer.indexOf('\n');
      if (newline === -1) return;

      handled = true;
      clearTimeout(idle);
      const decoded = decodeHookLine(buffer.slice(0, newline));
      if (decoded.rejected !== undefined) {
        this.#log.warn(LogTag.hook, `rejected a hook request (${decoded.rejected})`);
        socket.destroy();
        return;
      }
      if (decoded.event !== undefined) {
        // Nothing waits on an event: take it and let the hook go.
        const taken = this.#registry.event(decoded.event);
        this.#log.debug(
          LogTag.hook,
          `${decoded.event.name} in ${decoded.event.session || decoded.event.cwd} `
          + `(front-end ${decoded.event.entrypoint || 'not said'})${taken ? '' : ', ignored'}`,
        );
        socket.end();
        return;
      }

      const result = this.#registry.submit(decoded.request, {
        send: (text) => { if (!socket.destroyed) socket.write(text); },
        end: () => socket.end(),
      });
      if (result.accepted) {
        id = result.id;
        this.#log.info(LogTag.hook, `request ${id}: ${decoded.request.tool}`);
      } else {
        this.#log.info(LogTag.hook, `request stood down: ${result.reason}`);
      }
    });

    socket.on('close', () => {
      clearTimeout(idle);
      if (id !== null) this.#registry.channelClosed(id);
    });
    socket.on('error', () => socket.destroy());
  }
}
