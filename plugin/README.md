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

Either way it needs `node` on the `PATH` Claude Code runs hooks with.

## What it does and does not do

- **It never answers by default.** No daemon, no socket, a request answered in the
  app, a timeout or any error: it prints nothing, exits 0, and Claude's dialog is in
  charge.
- **It only ever allows once.** It returns no permission update, so nothing is
  remembered, no mode changes and "always allow" is never chosen for you.
- **It waits at most 120 seconds**, the `timeout` in [`hooks/hooks.json`](chickenmicro-permissions/hooks/hooks.json).
  After that Claude Code stops it and the dialog stays for you to answer.
- **It forwards less than it is given.** The tool, the session, the directory and a
  trimmed copy of the input; never the transcript path or Claude's own suggestions.

The wire format and the safety rules are in
[`protocol/PROTOCOL.md`](../protocol/PROTOCOL.md#10-the-hook-channel-mac-local) and
[`SECURITY.md`](../SECURITY.md).

## Check that it works

Make Claude ask for something it has to ask for, and watch the daemon:

```
[HOOK] request h1: Bash
[SESSION] permission h1 (shell) in chickenMicro
[SERIAL] Received approve h1
[HOOK] approve h1 — answered through the hook
```

A request that shows up as `s1.req-N` instead of `hN` came through the dialog, which
means no hook claimed it: the plugin is not installed, not loaded yet, or `node` is
not on its `PATH`.

## Remove it

```bash
claude plugin uninstall chickenmicro-permissions
```

or delete the `hooks` block you merged.
