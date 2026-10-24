# macOS setup

## Prerequisites

**Node 20 or newer** — the daemon is a Node program.

```bash
brew install node
```

```bash
node --version
```

Two dependencies, both shipping prebuilt binaries so nothing needs a compiler:

| Package | Why |
| ------- | --- |
| `koffi` | calls the C Accessibility API (`AXUIElementCopyAttributeValue`, `AXUIElementPerformAction`) directly |
| `serialport` | the USB-serial link to the ESP32 |

```bash
cd mac && npm install
```

Worth being clear about: "pure Node" still rests on native code. There is no
JavaScript path to another app's UI on macOS. The options are a native addon you
compile, JXA/System Events (an Apple Event round trip per attribute, with the
permission attaching to `osascript`), or calling the C API through FFI — which
is what this does. koffi and serialport ship prebuilt, so *you* never run a
compiler, but native code is underneath.

**PlatformIO CLI** — for the firmware.

```bash
brew install platformio
```

```bash
pio --version
```

**USB-UART driver** — most ESP32 DevKit V1 boards use a CP2102 (works out of
the box on modern macOS) or a CH340/CH9102 (may need
[WCH's driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html)). You
have the right driver when the board shows up:

```bash
ls /dev/cu.*
```

---

## Accessibility permission

`claude-remote` reads Claude Desktop's interface through the macOS
Accessibility API. macOS gates that **per binary**, and it must be granted
before the daemon can see anything.

Ask the daemon which binary to authorise:

```bash
cd mac
node bin/claude-remote.js doctor
```

It prints the exact path, for example:

```
[!!] Accessibility permission
     binary: /Users/you/.nvm/versions/node/v22.18.0/bin/node
     grant it in System Settings > Privacy & Security > Accessibility
```

### The grant attaches to the launching app, not to `node`

This is the part worth understanding before you click the switch, and it is not
what you would guess.

macOS does not key the Accessibility grant to the `node` binary. It attributes
the request to the **responsible process** — for anything started from a shell,
that is normally the terminal application that owns it. This is why granting
Accessibility to Terminal or iTerm makes command-line tools work at all.

It is directly observable. The same `node` binary, run two ways:

```
# from a terminal that has been granted
[ok] Accessibility permission
     this process: /Users/you/.nvm/versions/node/v22.18.0/bin/node
     launched by:  /Applications/iTerm.app/Contents/MacOS/iTerm2

# from an app that has not
[!!] Accessibility permission
     this process: /Users/you/.nvm/versions/node/v22.18.0/bin/node
     launched by:  …/claude-code/2.1.266/claude.app/Contents/MacOS/claude
```

Same binary, different verdict. `doctor` prints both lines, so grant whatever it
names under **launched by**.

Practical consequences:

- **Authorise your terminal**, not the node binary, when running the daemon by
  hand. Upgrading Node changes nothing; switching terminals means granting the
  new one.
- **The scope is the terminal.** Everything you ever run from that terminal
  inherits Accessibility — the ability to read any app's UI and synthesise
  presses. That is considerably broader than one purpose-built binary, and it is
  the real cost of the Node port.
- **A LaunchAgent has no parent application**, so there the grant lands on the
  executable it launches. That is the way to get a narrow grant, and it is why
  the login-agent setup below is worth preferring for anything long-running.

### Things that trip this up

**Upgrading Node invalidates the grant.** A new version is a new binary at a
new path, so the entry in Settings points at the old one. Re-add the new path —
`doctor` prints it.

**Homebrew's `node` and nvm's `node` are different binaries.** Granting one does
nothing for the other. `doctor` reports the one actually in use.

**Running through a wrapper can misattribute the request.** `npx`, or a shell
function that re-execs, may cause macOS to attribute the access to the wrapper.
Invoke the binary directly — `node bin/claude-remote.js …`.

**A rebuild does not invalidate anything here.** Unlike the Swift build, the
binary is a stable, already-signed Node install, so the grant survives code
changes to the daemon itself.

---

## Verifying the Accessibility side

With Claude Desktop open:

```bash
cd mac
node bin/claude-remote.js dump --filter button
```

You should see Claude's buttons. Ask Claude to do something that needs
permission and run it again — the dialog's buttons appear, and the
`--- interpreted ---` block at the end of a plain `dump` shows what the daemon
made of the tree:

```
state:       waiting_permission
permission:  req-1 kind=shell
  content:   git push origin master
  actions:   deny, approve
bindings:    approve=… deny=…
```

If `permission: none` while a dialog is clearly on screen, the labels have
changed. Run `claude-remote dump --filter allow` to see the real ones and add
them to `approveLabels` / `denyLabels` in
[`mac/src/accessibility/selectors.js`](../mac/src/accessibility/selectors.js).

---

## Running it as a login agent (optional)

To have the daemon start with your session, create
`~/Library/LaunchAgents/com.chickenmicro.claude-remote.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.chickenmicro.claude-remote</string>
    <key>ProgramArguments</key>
    <array>
        <string>/Users/YOU/.nvm/versions/node/v22.18.0/bin/node</string>
        <string>/Users/YOU/dev/chickenMicro/mac/bin/claude-remote.js</string>
        <string>run</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardErrorPath</key>
    <string>/tmp/claude-remote.log</string>
</dict>
</plist>
```

Replace `YOU`, then:

```bash
launchctl load ~/Library/LaunchAgents/com.chickenmicro.claude-remote.plist
```

The Accessibility grant must be given to that exact binary path.

The daemon keeps its own log in `~/Library/Logs/claude-remote/claude-remote.log`
whichever way it is started, so `StandardErrorPath` above is only worth keeping
for what crashes before the daemon gets that far — and `/tmp` is emptied at
every restart, while `~/Library/Logs` is not.

To stop it:

```bash
launchctl unload ~/Library/LaunchAgents/com.chickenmicro.claude-remote.plist
```
