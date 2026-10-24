/**
 * Writes frames to stdout instead of a serial port.
 *
 * Used by `claude-remote simulate --stdout` to inspect the wire format, and by
 * the tests.
 */
export class StdoutTransport {
  onLine = null;
  onConnect = null;

  get isConnected() { return true; }

  start() { this.onConnect?.(); }
  stop() {}
  reset() {}

  send(line) {
    process.stdout.write(line);
  }
}
