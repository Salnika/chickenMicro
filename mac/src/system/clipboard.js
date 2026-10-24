import { execFileSync } from 'node:child_process';

/**
 * The macOS pasteboard, through `pbcopy` and `pbpaste`.
 *
 * Synchronous on purpose: a one-time code is a few bytes, both tools return in
 * milliseconds, and the log should only say "copied" once it is true.
 */
export const systemClipboard = {
  copy(text) {
    execFileSync('pbcopy', { input: text, timeout: 2000 });
  },
  read() {
    return execFileSync('pbpaste', { encoding: 'utf8', timeout: 2000 });
  },
};
