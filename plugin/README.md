# ChickenMicro permissions plugin

A Claude Code plugin that lets the box answer permission requests.

Claude Code runs its `PermissionRequest` hook beside the permission dialog and
hands it the whole request. This one forwards it to the daemon over a local
socket; ✓ on the box prints "allow this once", ✕ prints "deny", and Claude Code
takes the dialog down by itself. Nothing is pressed, so it works for the requests
Claude Desktop's dialog gives no one-off button for — file edits above all — and
for a window nobody is looking at.

It is the better way to answer, not the only one. With no plugin, no daemon, or the
box's **Answer via** set to *window only*, the daemon presses the buttons in Claude's
window exactly as it always did.

Five more hooks — `SessionStart`, `UserPromptSubmit`, `Stop`, `StopFailure` (what runs
instead of `Stop` when an API error ends a turn) and `SessionEnd` — tell the daemon
which sessions are running and whether each is working, so that a terminal or VS Code
session is on the box as soon as it starts, not only once it asks for something. They
send the event, the session's id, the project folder, the front-end, why a session
started (`SessionStart` only) and, when you gave the session one with `--name` or
`/rename`, its name — nothing else, never the prompt — print nothing, and never block
a turn.

## Install

The daemon has to be running with hooks on, which is the default:

```bash
cd mac && node bin/claude-remote.js run
```

Then install the hook, either as a plugin or as plain settings.

**As a plugin**, from a local marketplace:

```bash
claude plugin marketplace add ./plugin
claude plugin install chickenmicro-permissions@chickenmicro
```

**As settings**, without the plugin machinery. The daemon prints the block, with
the right path in it:

```bash
cd mac && node bin/claude-remote.js hook-config
```

Merge its `hooks` block into `~/.claude/settings.json` (every project) or into a
project's `.claude/settings.local.json` (that one only). Claude Code's docs say
hooks are read when a session starts; in testing a new message was enough, and if
yours is not, start a new session.

Either way the hook is started by a small launcher, [`permission-hook.sh`](chickenmicro-permissions/bin/permission-hook.sh),
which runs it with the `node` the daemon is running under: the daemon leaves that
path next to its socket. So it works from VS Code opened from the Dock, whose `PATH`
often has no `node`, as well as from a terminal. The launcher runs what that file
names, so it takes it only if the file is the user's own and holds a full path; the
daemon, for its part, leaves it only in a folder no one else can write to. With no
usable file, it looks for `node` on the `PATH`, and with none there either it stands
down quietly.

**In a terminal or in VS Code**, Claude Code runs the same hooks, and the daemon needs
no Accessibility grant for them (`run --no-accessibility`). Install the plugin or the
settings at user level, so that every session has the hooks. The root
[README](../README.md#in-a-terminal-or-in-vs-code) says what the box shows for them.

## Updating

**The marketplace above is a folder of this repository, so Claude Code loads the
plugin from it in place.** The files you have checked out are the installed plugin:
a change is picked up by the next session, or by `/reload-plugins` in a running one,
with no version to raise. (`claude plugin list --json` shows the install record, which
can lag — an old `version` and an `installPath` under `~/.claude/plugins/cache/` —
next to `readFromFolder` and `folderVersion`, which are the truth; `claude plugin
details chickenmicro-permissions` lists the hooks it will run.) Two things follow:

- Switching this repository to another branch, stashing, or moving the folder changes
  or removes the hooks of every new session. On a branch from before the session
  events there is only the permission hook, and the sessions silently vanish from
  the box.
- For a folder, `claude plugin marketplace update` and `claude plugin update` are not
  needed: they refresh the install record and a cached copy that, by `readFromFolder`,
  Claude Code does not read. If the plugin was installed from a git URL instead, it
  *is* a copy, and it is these two commands, after raising `version` in
  [`plugin.json`](chickenmicro-permissions/.claude-plugin/plugin.json), that replace it:

```bash
claude plugin marketplace update chickenmicro
claude plugin update chickenmicro-permissions@chickenmicro
```

A session that was already running keeps the hooks it started with until you run
`/reload-plugins` in it, or restart it. And **restart the daemon** too: it refuses a
message it does not know, and a daemon from before the session events turns every
one of them away (`[HOOK] rejected a hook request (unknown hook frame "event")`).
To check, start a new session and run `/hooks` in it: the six events must show the
plugin as their source.

## What it does and does not do

- **It never answers by default.** No daemon, no socket, a request answered in the
  app, a timeout or any error: it prints nothing, exits 0, and Claude's dialog is in
  charge.
- **It only ever allows once.** It returns no permission update, so nothing is
  remembered, no mode changes and "always allow" is never chosen for you.
- **It waits at most 120 seconds** for a permission, the `timeout` in [`hooks/hooks.json`](chickenmicro-permissions/hooks/hooks.json).
  After that Claude Code stops it and the dialog stays for you to answer. The session
  events wait for nothing, and are cut at 5 seconds.
- **It forwards less than it is given.** The tool, the session, the project folder
  (where the session started, not where Claude has since gone), the front-end and a
  trimmed copy of the input for a permission; the name you gave a session, and why it
  started, for the events. Never the prompt, the transcript path or Claude's own
  suggestions.

The wire format and the safety rules are in
[`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md#10-the-hook-channel-mac-local) and
[`SECURITY.md`](../SECURITY.md).

## Check that it works

Make Claude ask for something it has to ask for, and watch the daemon:

```
[HOOK] request h1: Bash
[SESSION c1] permission h1 (shell) in chickenMicro
[SERIAL] Received approve h1
[HOOK] approve h1 — answered through the hook
```

That is a terminal or VS Code session, tagged with its own id (`c1`, `c2`…): it is
listed beside Claude Desktop's windows, or alone when none is read. A request from a
Claude Desktop conversation carries the tag of the window whose dialog it was matched
to (`s1`), or a bare `[SESSION]` when no dialog claimed it — and so does a request
from a session that did not say where it runs, which is what an older plugin does.
In Claude Desktop, a request that shows up as `s1.req-N` instead of `hN` came through
the dialog, which means no hook claimed it: the plugin is not installed, not loaded
yet, or the launcher found no `node` (the daemon's `node` file is missing and there
is none on the `PATH`).

## Remove it

```bash
claude plugin uninstall chickenmicro-permissions
```

or delete the `hooks` block you merged.
