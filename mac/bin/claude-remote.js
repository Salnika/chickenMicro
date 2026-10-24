#!/usr/bin/env node
// ---------------------------------------------------------------------------
// claude-remote — drive Claude Desktop from an ESP32 box.
//
// Wiring ............ docs/wiring.md
// Wire protocol ..... protocol/PROTOCOL.md
// ---------------------------------------------------------------------------

import process from 'node:process';
import { statSync } from 'node:fs';

import { Log, LogTag } from '../src/log.js';
import { parseArguments } from '../src/cli/args.js';
import { DEFAULT_TREE_DEPTH } from '../src/accessibility/axNode.js';
import { UNEXPOSED_TREE_NODES, refusalMessage } from '../src/accessibility/readiness.js';
import { LogFile, defaultLogPath } from '../src/system/logFile.js';
import { DEFAULT_APPROVE_DELAY_MS } from '../src/protocol/constants.js';
import { ClaudeState } from '../src/protocol/enums.js';
import { encode } from '../src/protocol/frames.js';
import { PermissionGate } from '../src/session/permissionGate.js';
import { SessionCoordinator, VERSION } from '../src/session/sessionCoordinator.js';
import { SessionTracker } from '../src/session/sessionTracker.js';
import { describedPorts } from '../src/transport/portDiscovery.js';
import { SerialTransport } from '../src/transport/serialTransport.js';
import { StdoutTransport } from '../src/transport/stdoutTransport.js';
import { FakeAccessibilitySource } from '../src/simulation/fixtures.js';
import { SCENES, Simulator } from '../src/simulation/simulator.js';

const USAGE = `claude-remote ${VERSION} — drive Claude Desktop from an ESP32 box

USAGE
  claude-remote <command> [options]

COMMANDS
  ports                       list candidate USB-serial ports
  sessions                    list the Claude Desktop windows the daemon sees
  dump                        print Claude Desktop's Accessibility tree
  run                         run the daemon (Accessibility <-> serial)
  simulate [scene]            feed fake events to the device, no Claude needed
                              scenes: ${SCENES.join(', ')}
  doctor                      check permissions, port and Claude Desktop
  help                        this text

OPTIONS
  --serial <path>             serial port (default: auto-detect)
  --baud <rate>               baud rate (default: 115200)
  --filter <text>             dump: only elements matching text
  --depth <n>                 dump/run: max tree depth (default: 60)
  --approve-delay <ms>        arming delay before APPROVE counts (default: 200)
  --poll <ms>                 run: base polling interval (default: 250)
  --log <level>               debug | info | warn | error (default: info)
  --log-file <path>           run: where to keep the log
                              (default: ~/Library/Logs/claude-remote/claude-remote.log)
  --log-file-level <level>    run: level for the file (default: same as --log)
  --no-log-file               run: keep no log file
  --stdout                    simulate: print frames instead of using serial
  --no-prompt                 do not open the macOS Accessibility settings pane

EXAMPLES
  claude-remote ports
  claude-remote sessions
  claude-remote dump --filter button
  claude-remote run --serial /dev/cu.usbserial-0001
  claude-remote simulate permission --stdout`;

const args = parseArguments(process.argv.slice(2));

// Only `simulate` takes a positional. Silently ignoring the rest turned a
// mistyped `run dump --filter button` into "start the daemon", which looks like
// the dump command hanging.
const COMMANDS_WITH_POSITIONALS = new Set(['simulate']);
if (args.command !== undefined
    && !COMMANDS_WITH_POSITIONALS.has(args.command)
    && args.positionals.length > 0) {
  process.stderr.write(
    `\`${args.command}\` takes no arguments, but got: ${args.positionals.join(' ')}\n`
    + `Did you mean \`claude-remote ${args.positionals[0]}\`?\n\n`,
  );
  process.exit(1);
}
const logLevel = args.value('--log');
const logFileLevel = args.value('--log-file-level');
// Only the daemon keeps a log: the one-shot commands print their result, and a
// file of them would be noise between two real runs.
const logFile = args.command === 'run' && !args.has('--no-log-file')
  ? new LogFile({
    path: args.value('--log-file') ?? defaultLogPath(),
    banner: `claude-remote ${VERSION} run — pid ${process.pid}`,
    // Losing the file must not cost the run: say so once, keep going.
    onError: (error) => process.stderr.write(`!! [LOG] no log file: ${error.message}\n`),
  })
  : null;
const log = new Log({
  level: Log.isLevel(logLevel) ? logLevel : 'info',
  file: logFile,
  fileLevel: Log.isLevel(logFileLevel) ? logFileLevel : undefined,
});

// The Accessibility modules pull in koffi, which loads native frameworks. They
// are imported lazily so `ports`, `simulate` and `help` work even where that
// cannot load.
async function accessibility() {
  return {
    ...await import('../src/accessibility/source.js'),
    ...await import('../src/accessibility/permissions.js'),
    ...await import('../src/accessibility/interpreter.js'),
    ...await import('../src/accessibility/treeFormatter.js'),
    ...await import('../src/accessibility/watcher.js'),
  };
}

function makeTransport() {
  if (args.has('--stdout')) return new StdoutTransport();
  return new SerialTransport({
    path: args.value('--serial') ?? null,
    baudRate: args.int('--baud') ?? 115200,
    log,
  });
}

function makeCoordinator(transport, source) {
  const approveDelayMs = args.int('--approve-delay') ?? DEFAULT_APPROVE_DELAY_MS;
  return new SessionCoordinator({
    transport,
    source,
    gate: new PermissionGate(approveDelayMs),
    log,
    treeDepth: args.int('--depth') ?? DEFAULT_TREE_DEPTH,
  });
}

/** Runs until SIGINT/SIGTERM. */
function runUntilInterrupted(onShutdown) {
  let shuttingDown = false;
  const stop = () => {
    if (shuttingDown) return;
    shuttingDown = true;
    onShutdown();
    process.exit(0);
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  // Nothing else keeps the loop alive: every timer is unref'd.
  setInterval(() => {}, 1 << 30);
}

// --- Commands ---------------------------------------------------------------

async function commandPorts() {
  const ports = await describedPorts();
  if (ports.length === 0) {
    console.log('No USB-serial port found.');
    console.log(`
Check that:
  - the ESP32 is plugged in over USB (a data cable, not charge-only)
  - the USB-UART driver is installed if the board uses a CH340/CH9102
  - \`ls /dev/cu.*\` shows the device`);
    return 1;
  }
  console.log('Candidate serial ports (best first):\n');
  ports.forEach((port, index) => {
    console.log(`${index === 0 ? '->' : '  '} ${port.path}`);
    console.log(`     ${port.hint}`);
  });
  console.log('\nUse the top one automatically with `claude-remote run`,');
  console.log(`or pass it explicitly: claude-remote run --serial ${ports[0].path}`);
  return 0;
}

async function commandDump() {
  const ax = await accessibility();
  if (!ax.ensureTrusted(log, { prompt: !args.has('--no-prompt') })) return 2;

  const source = new ax.LiveAccessibilitySource({ log });
  const target = source.runningClaude();
  if (target === null) {
    process.stderr.write(`Claude Desktop is not running (no process matched).

Open Claude Desktop and run this again. If it *is* open, its executable name may
have changed — add it to selectors.processNames in
mac/src/accessibility/selectors.js
`);
    return 1;
  }

  const tree = source.readTree(args.int('--depth') ?? ax.DEFAULT_TREE_DEPTH);
  if (tree === null) {
    process.stderr.write('Could not read the Accessibility tree.\n');
    return 1;
  }

  log.info(LogTag.ax, `Claude Desktop detected (pid ${target.pid})`);
  const formatter = new ax.AXTreeFormatter();
  const filter = args.value('--filter');
  process.stdout.write(filter ? formatter.renderMatches(tree, filter) : formatter.render(tree));

  // Show what the daemon would make of it — the fastest way to tell whether the
  // selectors still match after a Claude Desktop update.
  console.log('\n--- interpreted ---');
  printSessions(source, args.int('--depth') ?? ax.DEFAULT_TREE_DEPTH);
  source.dispose();
  return 0;
}

/**
 * What the daemon sees, per window.
 *
 * The same view `run` mirrors to the device, printed once: when the box shows
 * nothing, this says whether the daemon found the window, the state, and the
 * buttons — before touching any hardware.
 */
function printSessions(source, depth) {
  const raw = source.readSessions(depth);
  if (raw === null) {
    console.log('No Claude Desktop process, or Accessibility is not granted.');
    return 1;
  }
  const view = new SessionTracker().update(raw);
  // A copy refusing Accessibility has no window to list: say so, or it simply
  // looks absent.
  const refusals = source.refusedProcesses?.() ?? [];
  const printRefusals = () => {
    for (const refusal of refusals) console.log(`!! ${refusalMessage(refusal)}`);
  };
  if (view.sessions.length === 0) {
    if (refusals.length > 0) {
      printRefusals();
    } else {
      console.log('Claude Desktop is running with no window open.');
    }
    return 1;
  }

  const rawByKey = new Map(raw.map((entry) => [entry.key, entry]));
  for (const session of view.sessions) {
    const marker = session.id === view.focusId ? '->' : '  ';
    console.log(`${marker} ${session.id}  ${session.name}  [pid ${session.pid}]`);
    console.log(`     state:       ${session.snapshot.state}`);

    // What the window actually exposed. A session stuck on "idle" is either a
    // window with nothing going on, or one whose web content never reached the
    // Accessibility tree — and these counts tell the two apart at a glance.
    const tree = rawByKey.get(session.key)?.tree;
    if (tree) {
      const nodes = tree.findAll(() => true).length;
      const pressable = tree.findAll((node) => node.isActuatable);
      const webArea = tree.findAll((node) => node.role === 'AXWebArea').length > 0;
      console.log(`     tree:        ${nodes} nodes, ${pressable.length} pressable, `
                  + `web area ${webArea ? 'yes' : 'NO'}`);
      // A real Claude window runs to hundreds of nodes; one whose web content
      // was never exposed stops at the window chrome and an empty web area.
      if (webArea && nodes < UNEXPOSED_TREE_NODES) {
        console.log('     note:        web content not exposed yet — the daemon has just');
        console.log('                  asked Electron for it; run this again in a second');
      }
      if (session.bindings.size === 0 && pressable.length > 0) {
        const labels = [...new Set(pressable.map((node) => node.label).filter(Boolean))];
        console.log(`     buttons:     ${labels.slice(0, 8).join(' | ')}`
                    + `${labels.length > 8 ? ` (+${labels.length - 8})` : ''}`);
      }
    }
    console.log(`     detail:      ${session.snapshot.detail ?? '-'}`);
    console.log(`     lastMessage: ${session.snapshot.lastMessage ?? '-'}`);
    if (session.snapshot.permission) {
      const p = session.snapshot.permission;
      console.log(`     permission:  ${p.id} kind=${p.kind}`);
      console.log(`       title:     ${p.title ?? '-'}`);
      console.log(`       content:   ${p.content ?? '-'}`);
      console.log(`       actions:   ${p.actions.join(', ')}`);
    } else {
      console.log('     permission:  none');
    }
    const bound = [...session.bindings].map(([action, ref]) => `${action}=${ref.marker}`).sort();
    console.log(`     bindings:    ${bound.length === 0 ? 'none' : bound.join(' ')}`);
  }
  console.log(`\n-> is the session the device follows; ${view.sessions.length} in total.`);
  if (refusals.length > 0) {
    console.log('');
    printRefusals();
  }
  return 0;
}

async function commandSessions() {
  const ax = await accessibility();
  if (!ax.ensureTrusted(log, { prompt: !args.has('--no-prompt') })) return 2;

  const source = new ax.LiveAccessibilitySource({ log });
  const status = printSessions(source, args.int('--depth') ?? ax.DEFAULT_TREE_DEPTH);
  source.dispose();
  return status;
}

async function commandRun() {
  const ax = await accessibility();
  if (!ax.ensureTrusted(log, { prompt: !args.has('--no-prompt') })) return 2;

  const source = new ax.LiveAccessibilitySource({ log });
  const transport = makeTransport();
  const coordinator = makeCoordinator(transport, source);
  const watcher = new ax.ClaudeWatcher({
    source,
    log,
    onChange: () => coordinator.refresh(),
    pollIntervalMs: args.int('--poll') ?? 250,
  });

  coordinator.start();
  watcher.start();
  log.info(LogTag.session, `claude-remote ${VERSION} running; Ctrl-C to stop`);
  if (log.filePath !== null) log.info(LogTag.session, `keeping a log in ${log.filePath}`);

  runUntilInterrupted(() => {
    log.info(LogTag.session, 'shutting down');
    watcher.stop();
    coordinator.stop();
    source.dispose();
    log.close();
  });
  return 0;
}

function commandSimulate() {
  const scene = args.positionals[0] ?? 'demo';
  if (!SCENES.includes(scene)) {
    process.stderr.write(`Unknown scene "${scene}". Available: ${SCENES.join(', ')}\n`);
    return 1;
  }

  const source = new FakeAccessibilitySource();
  const transport = makeTransport();
  const coordinator = makeCoordinator(transport, source);
  const simulator = new Simulator({ coordinator, source, log });

  coordinator.start();
  log.info(LogTag.sim, 'simulating without Claude Desktop; Ctrl-C to stop');

  if (args.has('--stdout')) {
    // No device to wait for: emit the scene and exit so the output is pipeable.
    if (scene === 'demo') simulator.playAll();
    else simulator.play(scene);
    coordinator.stop();
    return 0;
  }

  // The device may not be connected yet; onConnect replays the snapshot.
  simulator.play(scene);
  runUntilInterrupted(() => {
    log.info(LogTag.sim, 'shutting down');
    simulator.stop();
    coordinator.stop();
  });
  return 0;
}

async function commandDoctor() {
  let problems = 0;
  console.log(`claude-remote ${VERSION} — environment check\n`);

  let ax = null;
  try {
    ax = await accessibility();
    console.log('[ok] native Accessibility bindings loaded (koffi)');
  } catch (error) {
    problems += 1;
    console.log('[!!] could not load the Accessibility bindings');
    console.log(`     ${error.message}`);
  }

  if (ax) {
    const trusted = ax.isTrusted();
    const responsible = ax.responsibleProcess();
    console.log(`[${trusted ? 'ok' : '!!'}] Accessibility permission`);
    console.log(`     this process: ${ax.executablePath()}`);
    if (responsible) {
      // macOS attributes the request to the launching app, not to node.
      console.log(`     launched by:  ${responsible.path}`);
      console.log('     macOS grants Accessibility to the launching app, so authorise that one');
    } else {
      console.log('     launched by:  (no parent app — the grant lands on the binary itself)');
    }
    if (!trusted) {
      problems += 1;
      console.log('     grant it in System Settings > Privacy & Security > Accessibility');
    }

    const running = ax.findClaudeProcesses();
    const target = running[0] ?? null;
    if (target) {
      console.log(`[ok] Claude Desktop running (${running.length} instance(s))`);
      for (const instance of running) {
        console.log(`     pid ${instance.pid}${instance.profile ? ` — profile ${instance.profile}` : ''}`);
      }
      console.log(`     ${target.path}`);
    } else {
      problems += 1;
      console.log('[!!] Claude Desktop not running');
      console.log('     open it before `claude-remote run`');
    }

    if (trusted && target) {
      const source = new ax.LiveAccessibilitySource({ log: new Log({ level: 'error' }) });
      const started = Date.now();
      const sessions = new SessionTracker().update(source.readSessions()).sessions;
      const elapsed = Date.now() - started;
      const bound = sessions.reduce((total, session) => total + session.bindings.size, 0);
      console.log('[ok] Accessibility read');
      console.log(`     ${sessions.length} session(s), ${bound} button(s) bound, ${elapsed} ms`);
      for (const session of sessions) {
        console.log(`     ${session.id} ${session.name}: ${session.snapshot.state}`);
      }
      // A copy that refuses Accessibility is running but invisible: the most
      // confusing way for a session to be missing, so it is named here.
      for (const refusal of source.refusedProcesses()) {
        problems += 1;
        console.log(`[!!] ${refusalMessage(refusal)}`);
      }
      if (elapsed > 500) {
        console.log('     slow tree: the watcher will back off automatically');
      }
      if (sessions.every((s) => s.snapshot.state === ClaudeState.idle
                             && s.snapshot.lastMessage === null)) {
        console.log('     no transcript text found — if Claude has messages on screen,');
        console.log('     run `claude-remote sessions` and check selectors.js');
      }
      source.dispose();
    }
  }

  // Where to look after the fact: the warning that explains a bad evening is
  // logged once, long before anyone reads it.
  const logPath = args.value('--log-file') ?? defaultLogPath();
  console.log('[ok] log file');
  try {
    const { size, mtime } = statSync(logPath);
    console.log(`     ${logPath}`);
    console.log(`     ${Math.round(size / 1024)} KB, last written ${mtime.toLocaleString()}`);
  } catch {
    console.log(`     ${logPath}`);
    console.log('     not there yet — `claude-remote run` starts it');
  }

  const ports = await describedPorts();
  if (ports.length > 0) {
    console.log('[ok] serial port');
    console.log(`     ${ports[0].path} — ${ports[0].hint}`);
    if (ports.length > 1) {
      console.log(`     ${ports.length - 1} other candidate(s); pass --serial to choose`);
    }
  } else {
    problems += 1;
    console.log('[!!] no USB-serial port found');
    console.log('     plug in the ESP32, then run `claude-remote ports`');
  }

  console.log(`\n${problems === 0 ? 'All checks passed.' : `${problems} problem(s) found.`}`);
  return problems === 0 ? 0 : 1;
}

// --- Dispatch ---------------------------------------------------------------

let status;
switch (args.command) {
  case 'ports': status = await commandPorts(); break;
  case 'dump': status = await commandDump(); break;
  case 'sessions': status = await commandSessions(); break;
  case 'run': status = await commandRun(); break;
  case 'simulate': status = commandSimulate(); break;
  case 'doctor': status = await commandDoctor(); break;
  case 'version':
  case '--version': console.log(VERSION); status = 0; break;
  case 'help':
  case '--help':
  case '-h': console.log(USAGE); status = 0; break;
  case undefined: console.log(USAGE); status = 1; break;
  default:
    process.stderr.write(`Unknown command "${args.command}"\n\n`);
    console.log(USAGE);
    status = 1;
}

// `run` and `simulate` keep the loop alive themselves; everything else is done.
if (status !== 0 || !['run', 'simulate'].includes(args.command)) {
  process.exitCode = status;
  if (args.command !== 'run' && args.command !== 'simulate') process.exit(status);
}

export { encode };
