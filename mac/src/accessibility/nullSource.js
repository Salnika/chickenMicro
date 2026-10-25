/**
 * A source with nothing to read, for a daemon that answers through hooks alone.
 *
 * Claude Code in a terminal or in VS Code has no window the daemon could read, and
 * the macOS Accessibility grant that reading Claude Desktop needs is no use to
 * them. Everything such a daemon knows comes from the hooks, so what it reads from
 * windows is nothing at all: `null` is "Claude Desktop is not running", which the
 * coordinator has always handled.
 *
 * It implements the whole surface the coordinator touches, so no call can fall
 * through to something that does not exist.
 */
export class NullAccessibilitySource {
  get targetPid() { return null; }

  runningClaude() { return null; }

  runningProcesses() { return []; }

  readTree() { return null; }

  readSessions() { return null; }

  refusedProcesses() { return []; }

  /** Nothing is ever bound, so nothing is ever pressed; this is the guard if one were. */
  pressRef() {
    throw new Error('this daemon reads no windows, so there is no button to press');
  }

  dispose() {}
}
