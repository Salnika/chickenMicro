#!/usr/bin/env node
// A Claude Code PermissionRequest hook that lets the ChickenMicro box answer.
//
// Claude Code runs this beside the permission dialog and hands it the request on
// stdin. It forwards the request to the daemon over a local socket and waits for
// a key on the box. ✓ prints "allow this once", ✕ prints "deny"; Claude Code
// takes the dialog down by itself. Wire format: protocol/PROTOCOL.md §10.
//
// The one rule: it never decides anything the box did not. With no daemon, no
// socket, a daemon told to answer through the interface, a request answered in
// the app, a timeout or any error at all, it prints nothing and exits 0 — and
// Claude Code's own dialog stays in charge. It never prints "allow" for lack of
// an answer, never returns a permission update (so nothing is remembered and no
// mode changes), and never touches the request's input.
//
// No dependencies: it must start fast and cannot be allowed to break on a
// missing module.
import net from 'node:net';
import { homedir } from 'node:os';
import { join } from 'node:path';
import process from 'node:process';

const SOCKET = process.env.CLAUDE_REMOTE_SOCKET ?? join(homedir(), '.claude-remote', 'hook.sock');

/** No daemon should take longer than this to accept a local connection. */
const CONNECT_MS = 500;

/** Past these, a value is cut: the box shows a few hundred characters at most. */
const MAX_STRING_CHARS = 2000;
const MAX_LINE_BYTES = 60_000;

const HOOK_VERSION = 1;

/** Cuts long strings and deep structures, so a Write of a big file still fits a line. */
function trimmed(value, depth = 0) {
  if (typeof value === 'string') {
    return value.length > MAX_STRING_CHARS ? `${value.slice(0, MAX_STRING_CHARS)}...` : value;
  }
  if (value === null || typeof value !== 'object') return value;
  if (depth >= 2) return undefined;
  if (Array.isArray(value)) return value.slice(0, 20).map((item) => trimmed(item, depth + 1));
  return Object.fromEntries(
    Object.entries(value).slice(0, 30).map(([key, item]) => [key, trimmed(item, depth + 1)]),
  );
}

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  return Buffer.concat(chunks).toString('utf8');
}

/** The one line to send, or null when this is not a request worth sending. */
function requestLine(text) {
  let payload;
  try {
    payload = JSON.parse(text);
  } catch {
    return null;
  }
  if (payload?.hook_event_name !== 'PermissionRequest') return null;
  if (typeof payload.tool_name !== 'string' || payload.tool_name === '') return null;

  const frame = {
    v: HOOK_VERSION,
    type: 'request',
    tool: payload.tool_name,
    session: typeof payload.session_id === 'string' ? payload.session_id : '',
    cwd: typeof payload.cwd === 'string' ? payload.cwd : '',
    input: trimmed(payload.tool_input ?? {}) ?? {},
  };
  let line = `${JSON.stringify(frame)}\n`;
  // Too big even trimmed: the box still learns which tool is asking.
  if (Buffer.byteLength(line, 'utf8') > MAX_LINE_BYTES) {
    line = `${JSON.stringify({ ...frame, input: {} })}\n`;
  }
  return line;
}

/** `{decision, message?}` for a verdict line, and null for everything else. */
function readVerdict(text) {
  let frame;
  try {
    frame = JSON.parse(text);
  } catch {
    return null;
  }
  if (frame?.v !== HOOK_VERSION || frame.type !== 'verdict') return null;
  if (frame.decision !== 'allow' && frame.decision !== 'deny') return null;
  return {
    decision: frame.decision,
    message: typeof frame.message === 'string' ? frame.message : undefined,
  };
}

/** Resolves with the box's verdict, or null whenever it did not give one. */
function ask(line) {
  return new Promise((resolve) => {
    const socket = net.connect(SOCKET);
    let settled = false;
    let buffer = '';
    const finish = (value) => {
      if (settled) return;
      settled = true;
      clearTimeout(connectTimer);
      socket.destroy();
      resolve(value);
    };
    const connectTimer = setTimeout(() => finish(null), CONNECT_MS);

    socket.setEncoding('utf8');
    socket.once('connect', () => {
      clearTimeout(connectTimer);
      socket.write(line);
    });
    socket.on('data', (chunk) => {
      buffer += chunk;
      const newline = buffer.indexOf('\n');
      if (newline !== -1) finish(readVerdict(buffer.slice(0, newline)));
    });
    socket.on('error', () => finish(null));
    socket.on('close', () => finish(null));
    // Claude Code stops a hook that outlives its timeout with SIGTERM. Closing
    // the socket tells the daemon the request is no longer being waited on.
    process.once('SIGTERM', () => finish(null));
  });
}

function print(verdict) {
  const decision = verdict.decision === 'allow'
    ? { behavior: 'allow' }
    : { behavior: 'deny', message: verdict.message ?? 'Refused from the ChickenMicro box' };
  process.stdout.write(
    `${JSON.stringify({ hookSpecificOutput: { hookEventName: 'PermissionRequest', decision } })}\n`,
  );
}

async function main() {
  const line = requestLine(await readStdin());
  if (line === null) return;
  const verdict = await ask(line);
  if (verdict !== null) print(verdict);
}

try {
  await main();
} catch {
  // Whatever went wrong, the dialog is still there and still works.
}
process.exit(0);
