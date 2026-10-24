import { execFile, execFileSync } from 'node:child_process';
import process from 'node:process';

import { LogTag } from '../log.js';
import { isTrusted } from './ax.js';

/** The Node binary running this code. */
export function executablePath() {
  return process.execPath;
}

/**
 * Best guess at the process macOS will attribute the Accessibility request to.
 *
 * TCC does not key the grant to the `node` binary. It attributes a request to
 * the *responsible process*, which for anything started from a shell is
 * normally the terminal application that owns it — which is why granting
 * Accessibility to Terminal or iTerm makes command-line tools work, and why the
 * same `node` binary is trusted from one terminal and not from another.
 *
 * There is no public API for this, so walk the parent chain to the nearest
 * .app bundle. That is the thing to authorise.
 */
export function responsibleProcess() {
  let pid = process.ppid;
  for (let hop = 0; hop < 12 && pid > 1; hop += 1) {
    let line;
    try {
      line = execFileSync('ps', ['-o', 'ppid=,comm=', '-p', String(pid)], {
        encoding: 'utf8',
      }).trim();
    } catch {
      return null;
    }
    if (line === '') return null;

    const separator = line.indexOf(' ');
    if (separator === -1) return null;
    const parent = Number.parseInt(line.slice(0, separator), 10);
    const command = line.slice(separator + 1).trim();

    if (command.includes('.app/Contents/MacOS/')) return { pid, path: command };
    if (!Number.isInteger(parent)) return null;
    pid = parent;
  }
  return null;
}

export function explanation() {
  const responsible = responsibleProcess();
  const target = responsible
    ? `    ${responsible.path}

That is the application that launched this process. macOS attributes the
request to it, not to the node binary (${executablePath()}), which is why the
same node can be trusted from one terminal and refused from another.`
    : `    ${executablePath()}

No parent application was found, so the grant should land on the node binary
itself. This is the case for a LaunchAgent.`;

  return `Accessibility permission is missing.

claude-remote reads Claude Desktop's UI through the macOS Accessibility API, so
macOS must allow the responsible process to control your computer:

${target}

Grant it in:
    System Settings > Privacy & Security > Accessibility
    (Réglages système > Confidentialité et sécurité > Accessibilité)

Add it with "+", enable its switch, then run the command again.

Note the scope: everything that application runs inherits the grant. Running the
daemon from a LaunchAgent instead attaches it to the binary alone — see
docs/setup-macos.md.
`;
}

/** Opens the Accessibility pane, which is friendlier than a modal prompt. */
export function openSettings() {
  execFile('open', ['x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility'],
    () => { /* best effort */ });
}

export { isTrusted };

/**
 * Checks the grant and, when missing, prints the explanation. Returns whether
 * the caller may proceed.
 */
export function ensureTrusted(log, { prompt = true } = {}) {
  if (isTrusted()) return true;
  log.error(LogTag.ax, 'Accessibility permission missing');
  process.stderr.write(`${explanation()}\n`);
  if (prompt) openSettings();
  return false;
}
