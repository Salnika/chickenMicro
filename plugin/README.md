# ChickenMicro permissions plugin

A Claude Code plugin that lets the box answer permission requests.

Claude Code runs its `PermissionRequest` hook beside the permission dialog and
hands it the whole request. This one forwards it to the daemon over a local
socket; ✓ on the box prints "allow this once", ✕ prints "deny", and Claude Code
takes the dialog down by itself. Nothing is pressed, so it works for the requests
Claude Desktop's dialog gives no one-off button for — file edits above all — and
for a window nobody is looking at.

It is the better way to answer, not the only one. With no plugin, no daemon, or the
box set to **Interface**, the daemon presses the buttons in Claude's window exactly
as it always did.

Four more hooks — `SessionStart`, `UserPromptSubmit`, `Stop` and `SessionEnd` — tell
the daemon which sessions are running and whether each is working, so that a
terminal or VS Code session is on the box as soon as it starts, not only once it asks
for something. They send the session's id, folder and front-end and nothing else
(never the prompt), print nothing, and never block a turn.

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

Claude Code runs the copy of the plugin it keeps under `~/.claude/plugins/cache/`,
not this folder. Editing the files here changes nothing there until the plugin's
`version` in [`plugin.json`](chickenmicro-permissions/.claude-plugin/plugin.json) is
raised and Claude Code is told to fetch it:

```bash
claude plugin marketplace update chickenmicro
claude plugin update chickenmicro-permissions@chickenmicro
```

A session that was already running keeps the hooks it started with until you run
`/reload-plugins` in it, or restart it. The hooks of version 0.2.0 add the session
events; 0.1.0 only answers permissions.

A plugin whose marketplace was added from a local folder may be loaded from that
folder as it is, with no copy in the cache; this one was copied (look for it under
`~/.claude/plugins/cache/`), which is why the version has to move.

## What it does and does not do

- **It never answers by default.** No daemon, no socket, a request answered in the
  app, a timeout or any error: it prints nothing, exits 0, and Claude's dialog is in
  charge.
- **It only ever allows once.** It returns no permission update, so nothing is
  remembered, no mode changes and "always allow" is never chosen for you.
- **It waits at most 120 seconds** for a permission, the `timeout` in [`hooks/hooks.json`](chickenmicro-permissions/hooks/hooks.json).
  After that Claude Code stops it and the dialog stays for you to answer. The session
  events wait for nothing, and are cut at 5 seconds.
- **It forwards less than it is given.** The tool, the session, the directory, the
  front-end and a trimmed copy of the input; never the prompt, the transcript path or
  Claude's own suggestions.

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

That is a terminal or VS Code session with no Claude Desktop window read, tagged
`c1`. With windows read, a hook request carries the tag of the window whose dialog
it was matched to (`s1`), or a bare `[SESSION]` when no dialog claimed it — which
is what a terminal or VS Code request looks like then. In Claude Desktop, a request
that shows up as `s1.req-N`
instead of `hN` came through the dialog, which means no hook claimed it: the plugin
is not installed, not loaded yet, or the launcher found no `node` (the daemon's
`node` file is missing and there is none on the `PATH`).

## Remove it

```bash
claude plugin uninstall chickenmicro-permissions
```

or delete the `hooks` block you merged.
