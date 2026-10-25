# Troubleshooting

Work top to bottom: `doctor` first, then the layer that is actually failing.

```bash
cd mac && node bin/claude-remote.js doctor
```

It checks the Accessibility grant, whether Claude Desktop is running, whether a
serial port is present, and whether the tree can actually be read.

## Where the logs are

`run` writes to the terminal **and** to
`~/Library/Logs/claude-remote/claude-remote.log`, so a warning from three hours
ago is still there. The file rotates at 2 MB, keeping `claude-remote.log.1` to
`.3`; each run of the daemon starts with a dated `=== claude-remote … ===` line.

```bash
tail -f ~/Library/Logs/claude-remote/claude-remote.log
```

`doctor` prints the path and the file's size. `--log-file <path>` puts it
elsewhere, `--no-log-file` turns it off, and `--log-file-level debug` records
every frame while the terminal stays at `info`.

The box keeps nothing of its own: its messages travel over USB as `log` frames
and appear in the same stream, tagged `[DEVICE]`. Its flash holds only the
one-time-code secrets.

---

## Serial

### `claude-remote ports` finds nothing

```bash
ls /dev/cu.*
```

- Nothing new when you plug the board in → the cable is charge-only, or the
  USB-UART driver is missing. CP2102 works natively on modern macOS; CH340 /
  CH9102 boards usually need [WCH's driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html).
- `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial-*` present but not listed →
  pass it explicitly with `--serial`, and open an issue with the device name so
  the prefix can be added to `PortDiscovery.rankedPrefixes`.

The daemon uses `/dev/cu.*`, never `/dev/tty.*`: opening a `tty.` device blocks
waiting for carrier detect, which a USB-UART bridge never asserts.

### `cannot open …: Resource busy`

Something else holds the port. Almost always `pio device monitor`. Close it —
only one process can own a serial port.

```bash
lsof | grep cu.usbserial
```

### The device shows "no link" although the daemon is running

The firmware infers the link from inbound traffic: no frame for 10 s and it
declares itself disconnected. So this means frames are not arriving.

- Baud mismatch. Both sides are 115200; if you changed `kSerialBaud` in
  `Config.h` you must pass a matching `--baud`.
- Wrong port. With two USB serial devices connected, auto-detection may pick
  the other one. Check with `claude-remote ports` and pass `--serial`.
- Run the daemon at debug level and watch the traffic:

  ```bash
  node bin/claude-remote.js run --log debug
  ```

  `-> {...}` lines are outbound, `<- {...}` inbound. Seeing `->` but never `<-`
  means the device is not transmitting: check the board is actually running
  (`pio device monitor` on its own should show its boot log).

### The device logs `connected` / `link lost` once or twice a second

Fixed in the firmware; if you see it, you are running a build from before that
fix — reflash.

The cause was an unsigned underflow. `App::loop` samples `millis()` once at the
top, then `SerialProtocol::poll` dispatches frames whose handlers stamp
`lastInbound` with a *later* `millis()`. The old check computed
`(now - lastInbound) < timeout` on two `uint32_t`, so when `lastInbound` was a
few milliseconds ahead the subtraction wrapped to ~4.29e9 and the link was
declared dead — **once per received frame**, hence flapping at the frame rate
rather than anything to do with the 10-second timeout.

`linkIsUp` in [`esp32/include/LinkTimeout.h`](../esp32/include/LinkTimeout.h)
now casts the modular difference to `int32_t`, which reads a small "negative"
elapsed time correctly and still survives the 49.7-day `millis()` rollover.

It matters beyond the noise: every flap called `markDirty()`, so the device was
redrawing the panel continuously, and a redraw blocks the loop for hundreds of
milliseconds with nothing draining the UART. That starved the receive buffer —
see the next entry.

### `rejected frame` warnings in the daemon log

The line failed validation and was dropped — which is the intended behaviour,
not a bug. The reason is in the message:

| Reason | Meaning |
| ------ | ------- |
| `not valid JSON` | serial noise, or a non-protocol print reached the wire |
| `protocol version N != 1` | firmware and daemon are from different commits — rebuild both |
| `unknown type "…"` | ditto |
| `malformed requestId "…"` | an id outside `[A-Za-z0-9._:-]`, refused on purpose |

A burst of `not valid JSON` right after connecting is normal: the ESP32 prints
its ROM bootloader banner at a different baud rate before the firmware starts.
Opening the port toggles DTR/RTS, which resets the board, so this happens on
every connect.

**A sustained stream of fragments** — short pieces like `"}`, plus
`dropped N oversized line(s)` — means frames are being spliced rather than
merely lost, and has three known causes, all fixed:

- the device stopped draining its UART during a display refresh. The default
  256-byte receive buffer holds only ~22 ms at 115200, and a refresh blocks for
  hundreds of milliseconds; `kSerialRxBufferBytes` is now 4096.
- the daemon kept the `data` listener attached to a replaced port, so two byte
  streams fed one assembler.
- the daemon could open a second reader on the same device when a retry fired
  while a port was already live.

If it persists on current builds, capture the raw bytes to see which side is
producing them:

```bash
cat /dev/cu.usbserial-0001 | head -c 4000 | xxd | head -60
```

Run that with the daemon stopped. Well-formed output is one JSON object per
line; anything else is coming from the device.

---

## Accessibility

### `Accessibility permission missing` even though it is granted

The grant attaches to the **application that launched the process**, not to the
`node` binary, so the usual cause is running the daemon from a different
terminal than the one that was authorised — or from an editor, a task runner, or
anything else that is not the granted app.

```bash
cd mac && node bin/claude-remote.js doctor
```

`doctor` prints both `this process` and `launched by`. Authorise whatever it
names under **launched by** — see
[setup-macos.md](setup-macos.md#the-grant-attaches-to-the-launching-app-not-to-node).

### `Claude Desktop is not running` while it is open

The executable name did not match. Check what it actually is:

```bash
ps -axo pid=,comm= | grep -i claude | grep 'MacOS'
```

Add the executable name to `processNames` in
[`mac/src/accessibility/selectors.js`](../mac/src/accessibility/selectors.js).
The daemon finds Claude with `ps`, matching the executable name and requiring
the path to contain `.app/Contents/MacOS/` so a helper process is never picked.

### STOP (or any button) pressed the wrong thing

Fixed; if you see it, restart the daemon.

STOP was binding to **Édition > Annuler** in the menu bar — "Annuler" is
*Cancel* in a dialog but *Undo* in the Edit menu, so pressing STOP fired Undo on
a live conversation. Menu items expose `AXPress` exactly like buttons, so
nothing structural stopped it.

Three guards now, in
[`axNode.js`](../mac/src/accessibility/axNode.js) and
[`selectors.js`](../mac/src/accessibility/selectors.js):

- `isActuatable` excludes `AXMenuBar` / `AXMenuBarItem` / `AXMenu` / `AXMenuItem`,
  so nothing in a menu is ever bound or treated as a dialog. `canPress` still
  reports the truth, so `dump` is unaffected;
- the reader no longer descends into the menu bar at all, which also removes
  hundreds of cross-process reads from every poll;
- `stopLabels` dropped the generic `cancel` / `annuler`; only unambiguous stop
  wording remains.

The general lesson when editing the label lists: a verb that is unambiguous in a
dialog may mean something else in a menu. Check with
`claude-remote dump --filter <label>` and confirm the match is an `AXButton`
inside a window.

### Accented labels never match

`normalise` folds diacritics (NFD, then strip combining marks) before its
charset filter. Without that, "Arrêter" became `arr ter` and matched nothing,
because the filter turned `ê` into a space. If you add a label with an accent,
write the *unaccented* form in `selectors.js` — `arreter`, not `arrêter`.

### One session shows its real state; the others stay idle

Electron does not build the accessibility tree for its web content until an
assistive tool asks for it, and it keeps track of that per process. Several
copies of Claude Desktop are several processes, so a window can expose nothing
but an empty web area: no stop control, no permission dialog, and a state that
reads `idle` whatever Claude is doing in it.

The daemon asks every Claude process for its web content through
`AXManualAccessibility` — the attribute Electron documents for this — when it
first sees it, and logs it:

```
[AX] asked Claude Desktop (pid 77871) to expose its web content
```

Once is not always enough: a copy asked a few seconds after it launched was
seen keeping an empty web area for minutes, until it was asked again. So while
one of a copy's windows shows none of its content, the daemon asks again every
five seconds (logged at `--log debug`), and stops as soon as the content is
there.

Chromium builds the tree asynchronously, so the first read after that can still
be empty. To check each window:

```bash
cd mac && node bin/claude-remote.js sessions
```

Every session prints a `tree:` line. A real Claude window runs to hundreds of
nodes; one reporting a few dozen with `web area yes` has not exposed its content
yet — run the command again. If a window stays small, or shows plenty of nodes
with no `stop` binding while Claude is visibly working in it, paste that output:
the `buttons:` line lists what the window offers instead.

### A copy of Claude Desktop is missing, and `doctor` says it refuses Accessibility

```
[!!] Claude Desktop (pid 36838, profile Claude-Work) refuses Accessibility: it was
     updated while running, and macOS no longer finds the executable it needs to
     check who is asking. Quit that copy and open it again.
```

The daemon logs the same line as a warning, once, while `run` is going. Every
Accessibility call to that copy comes back `kAXErrorAPIDisabled`, although the
daemon is trusted and reads the other copies fine, so its windows never reach
the box.

The cause is an update installed while the copy was open — by Claude's own
updater, or by a management tool such as Jamf, which moves the old bundle to a
temporary folder and deletes it. The copy keeps running from the deleted files.
macOS checks each Accessibility request from inside the target app, with TCC,
and that check starts by resolving the app's own executable: gone, so every
request is refused. No Accessibility client can read that copy, VoiceOver
included. Copies opened after the update are not affected.

**Fix:** quit that copy (⌘Q in its window) and open it again with the same
profile. The daemon picks it up on its next poll, and logs
`Claude Desktop (pid …) answers Accessibility again` if the same process
recovers. The system log shows the underlying failure:

```bash
/usr/bin/log show --last 10m --predicate 'process == "tccd" AND eventMessage CONTAINS "proc_pidpath"'
```

`proc_pidpath_audittoken() failed from PID[…]: No such file or directory` is
the copy in question. (`/usr/bin/log` in full: in zsh, `log` is a built-in.)

### The session list shows "Session 1" instead of a useful name

Claude Desktop titles every window "Claude", so a name has to come from
somewhere else. The daemon tries, in order: the window title, the document
title of each web area in the window (with the app's own name stripped off the
end, so "Migration React 19 - Claude Code" reads as the session), the name that
window last gave of itself, the profile directory from `--user-data-dir`, then
a number.

Check what it resolved, per window:

```bash
cd mac && node bin/claude-remote.js sessions
```

If a better name is visible in the tree, `dump` will show it — look for an
`AXWebArea` or a heading near the top of the window — and the resolution order
lives in `#nameFor` in `mac/src/session/sessionTracker.js`.

A window only carries its document title while its web content is exposed, so
the name appears a moment after the window does — and `Claude-Work` or
`Session 2` on the box means that window has never once exposed it. The
`sessions` command says whether it did: a window of a few dozen nodes has not.

Running several copies of Claude Desktop with their own `--user-data-dir`
remains the backstop: the profile name is distinctive, so a session with no
title of its own comes out as "Claude-Work" rather than "Session 2".

### The box shows a permission from the wrong window

It should be impossible: each window is read as its own subtree, request ids are
namespaced per session (`s2.req-1`), and the daemon re-reads the tree and
locates the session still showing that exact id before it presses anything.

If it happens anyway, `claude-remote sessions` with the prompt on screen shows
which window the daemon thinks each request belongs to, and the `run` log names
the session on every press:

```
[AX] Pressing button "…AXButton[Autoriser une fois]" in docs
```

### `dump` sees the buttons but the daemon reports none

Almost always the tree depth. Electron nests hard: on a live Claude Desktop the
stop control sits at **depth 32** and a permission dialog's verdict buttons at
**~34**, under roughly twenty nested `AXGroup`s below the `AXWebArea`.

The daemon used to walk 24 levels while `dump` walked 40, so the dump showed
every control and the daemon was blind to all of them — reporting
`no stop control available` and `permission: none` for a dialog plainly on
screen. Both now share `DEFAULT_TREE_DEPTH` in
[`axNode.js`](../mac/src/accessibility/axNode.js), and `readTree` warns when a
walk actually reaches the limit:

```
! [AX] tree hit the depth limit (12 node(s) at depth 60); controls below that
       are invisible — raise --depth
```

If you see that, raise it:

```bash
node bin/claude-remote.js run --depth 80
```

Depth costs time — every level is more cross-process reads — but the watcher
times each read and backs off automatically, so a deeper tree throttles itself
rather than pinning a core.

### Finding the permission dialog's real labels

`mac/tools/capture-permission.sh` diffs the Accessibility tree against itself —
once with a prompt pending, once without — and prints only what the dialog
added. That is the reliable way to find new wording, because the tree carries
hundreds of controls and the dialog only exists while it is on screen.

Captured from Claude Desktop in French:

```
AXButton title="Autoriser Claude à modifier selectors.js ?"  children=5   <- container
AXStaticText value="Permission demandée : modifier. /Users/…/selectors.js"
AXButton title="Refuser 1"                                    <- deny
AXButton title="Toujours autoriser 2"                         <- standing grant
AXButton title="Autoriser une fois 3"                         <- approve
```

Two things in there are easy to get wrong:

- **Every choice carries a trailing keyboard-shortcut digit.** Matching is exact
  after normalisation, so `Refuser 1` did not match `refuser` and detection
  failed silently. `normalise` now strips a trailing standalone number.
- **"Toujours autoriser" sits right beside "Autoriser une fois".** It is listed
  in `standingPermissionLabels` and never bound to anything: a button on a desk
  must not grant a category of actions permanently. `approveLabels` is ordered
  most-restrictive-first, and the interpreter binds the earliest match rather
  than whichever the tree yields first, so "allow once" always wins.

A dialog offering *only* a standing grant is still detected, and reaches the
device with DENY available and APPROVE absent.

### `dump` shows the tree but `permission: none` during a real prompt

The button labels changed. Find them:

```bash
node bin/claude-remote.js dump --filter button
```

A permission is recognised only when one container holds **both** an
approve-ish and a deny-ish *pressable* element. So it fails if:

- the labels are new wording → add them to `approveLabels` / `denyLabels`;
- the controls are not `AXButton` and expose no `AXPress` → the daemon cannot
  press them at all, which is a real limitation rather than a config problem;
- only one of the two is present → by design, a lone "Allow" is not treated as
  a permission dialog.

### `dump` shows almost nothing — one window and no children

Electron builds the accessibility tree lazily. It usually fills in once the
window has been interacted with, or once a screen reader API has asked for it.
Click inside Claude's window and dump again. If it stays empty, the app was
launched before the Accessibility grant existed — quit and reopen it.

### The message text is empty or wrong

Transcript extraction picks the scroll area with the most static text and
splits it by speaker markers ("Claude", "User"). When a release changes that
structure, `detail` and `lastMessage` degrade. `claude-remote dump` plus the
`--- interpreted ---` block shows what was found; `assistantMarkers` and
`userMarkers` in `mac/src/accessibility/selectors.js` are the knobs.

### Pressing works only when Claude is in the foreground

`AXUIElementPerformAction` normally works on background windows, which is why
the project uses it rather than synthetic keystrokes. If a particular control
only responds when focused, that control is handling raw key events rather than
exposing an accessibility action — nothing the daemon can do about it. Move the
permission decision to the dedicated buttons, which do work in the background.

---

## Display

### Blank screen, no reaction at all

1. `VCC` on **3V3**, not 5 V.
2. `BUSY` connected. Without it GxEPD2 waits on a pin that never changes and
   silently gives up.
3. `RST` connected — the panel needs the reset pulse to come out of sleep.
4. Check the boot log:

   ```bash
   pio device monitor
   ```

   Expect `[display] GDEY0213Z98 (mono, fast partial), 250x122`. If the
   reported size differs from `DisplayModel.h`, the firmware warns — the
   selected model does not match the panel.

### Text is drawn but rotated or clipped

`EINK_ROTATION` in
[`esp32/include/DisplayModel.h`](../esp32/include/DisplayModel.h) is the
rotation that yields `EINK_WIDTH x EINK_HEIGHT`. The GDEY0213Z98 is natively
122x250 portrait, so rotation `1` gives the 250x122 landscape the layouts
assume. Try `3` for the opposite landscape orientation.

### Ghosting — faint traces of previous screens

Expected on e-ink with partial refresh. The firmware forces a full refresh
every `kPartialsBeforeFullRefresh` (20) updates. Hold the **↵ key** (GPIO26) to
force one immediately, or lower the constant in `Config.h`.

### A pink or red cast creeps in during use, then clears at a refresh

Different from the solid-red-at-boot bug below, and different from leftover
pigment: this is a faint tint that builds on the *settled* parts of the screen
the longer the box runs, and a periodic white flush wipes it — so it comes and
goes.

Cause: GxEPD2's stock partial LUT sends a small pulse even to pixels that do not
change, and on this tri-colour glass that pulse slowly drags the red pigment up.
A UI that redraws often (the title-bar chicken alone redraws once a second)
reaches it quickly.

Fixed by the softened `gentle` waveform the firmware installs at boot
(`EINK_PANEL_GENTLE_LUT`, on by default) — it leaves unchanged pixels at zero
volts. The boot log shows `mono LUT 'gentle' installed`. If you still see the
tint, confirm that line appears; if not, you are on a stale binary, a model other
than `EINK_MODEL_GDEY0213Z98_FAST`, or someone switched the menu's **Waveform**
entry to `stock` (which brings the settle kicks back). Details:
[eink-gdey0213z98.md](eink-gdey0213z98.md).

### The whole panel goes solid red at boot

Not leftover pigment — this is a full-update reaching the panel. `GxEPD2_213_BN`
serves a full update with `0x22/0xF4`, which loads the panel's **OTP** waveform;
on the GDEY0213Z98 that is the tri-colour one, and under it RAM `0x26` is the
red plane. The mono driver never meant `0x26` as red, so the whole plane paints
red.

The firmware avoids every full-update path when `EINK_PANEL_OTP_IS_3C` is set
(it is, for the default model), so this should not happen. If it does:

1. Confirm you are on the default model in
   [`esp32/include/DisplayModel.h`](../esp32/include/DisplayModel.h):
   `EINK_MODEL_GDEY0213Z98_FAST`, with `EINK_PANEL_OTP_IS_3C 1`.
2. Confirm nothing calls `init(…, true, …)`, `clearScreen()`, `refresh(false)`
   or `setFullWindow()` on the mono driver — all four end in `_Update_Full`.
3. Rebuild and reflash; a stale binary from before this fix will still do it.

Tell-tale: leftover pigment shows as red *patches* in the shape of whatever was
drawn before, while this bug paints the panel *uniformly* red.

Details: [eink-gdey0213z98.md](eink-gdey0213z98.md).

### Red patches sit under the UI and no refresh removes them

Expected, and not a bug: the default model drives the panel in monochrome, and
the mono waveform cannot move red pigment at all. Red from an earlier
tri-colour session is stuck until it is cleared with the tri-colour waveform.

**On the box:** hold the joystick to open the menu, pick **Clean the screen**.
It runs the black → red → white cycle in place (~45 s) and returns to the UI —
no reflashing. The panel flashing black and red is the cycle working.

The standalone tool does the same three passes, for a panel whose firmware will
not boot or before the firmware is flashed at all:

```bash
cd esp32
pio run -e esp32dev_clear_panel --target upload
pio device monitor                        # ~45 s, black -> red -> white
pio run -e esp32dev --target upload       # back to the firmware
```

See [eink-gdey0213z98.md](eink-gdey0213z98.md).

### Red areas fade after a while (tri-colour model)

Real effect, measured on this panel. Partial refresh with GxEPD2's monochrome
waveform applies a pulse to *unchanged* pixels too, which slowly moves the red
pigment. See [eink-gdey0213z98.md](eink-gdey0213z98.md). The default
`EINK_MODEL_GDEY0213Z98_FAST` avoids it entirely by never painting red.

### Refresh takes ~14 seconds

You are on `EINK_MODEL_GDEY0213Z98_TRI`. The tri-colour waveform lives in the
panel's OTP and is genuinely that slow. Switch to
`EINK_MODEL_GDEY0213Z98_FAST` for a responsive UI.

---

## Controls

### A button press is missed depending on when you press it

A refresh blocks the loop for a few hundred milliseconds, and the render before
it samples nothing — so a tap that started and ended inside that window used to
be lost, seemingly at random.

Each button's **press edge is now caught by a GPIO interrupt**
([`ButtonBankGpio.cpp`](../esp32/src/ButtonBankGpio.cpp)), which fires the
instant the pin falls whatever the loop is doing, and latches the press until the
loop consumes it. On the ESP32-C3 SuperMini build the MCP23008 expander does the
same latching in hardware ([`ButtonBankExpander.cpp`](../esp32/src/ButtonBankExpander.cpp)),
and both feed the same debounce, click and hold rules
([`ButtonState.cpp`](../esp32/src/ButtonState.cpp)), tested on the host.
The interrupt is on `CHANGE` and tracks the level, so contact bounce on release
cannot register a phantom press; the debounce window is still
`kButtonDebounceMs`. Hold and release stay on the polled path, which is fine —
only "did the press happen" is time-critical. The loop also skips its idle sleep
right after a refresh, so a press caught mid-refresh is acted on immediately.

If presses are still dropped, check the wiring/debounce: a very noisy button may
need `kButtonDebounceMs` raised, and a floating pin (missing `INPUT_PULLUP`
ground) fires spurious edges.

### ESP32-C3 SuperMini: no button does anything

The buttons sit on the MCP23008, so a dead row of keys is almost always the
expander missing from the I²C bus. The boot log says so:

```
[input] no MCP23008 at 0x20 (SDA=GPIO3, SCL=GPIO5): buttons read as released until it answers.
```

or the key line ends in `(NOT ANSWERING)`. In order of likelihood:

1. **RESET not tied to 3V3.** It has no internal pull-up; left floating, the chip
   comes and goes.
2. **A0/A1/A2 floating.** Tie all three to GND for address `0x20`.
3. **SDA and SCL swapped** — SDA is GPIO3, SCL is GPIO5.
4. **VDD on 5V, or not powered.** It must be 3V3.
5. **Weak bus.** Without 4.7 kΩ pull-ups on SDA and SCL, long wires fail even at
   100 kHz. Add them.

Nothing needs a reboot once fixed: the firmware looks for the expander again
once a second and logs `MCP23008 answering` when it comes back.

If the log shows `lost the MCP23008 (bus error or chip reset)` from time to
time, the chip is browning out or the bus is marginal — the same checks apply.
While it is gone the keys read as released, never pressed: a failing expander
cannot produce a verdict. Details:
[wiring-esp32c3-supermini.md](wiring-esp32c3-supermini.md).

### The keycap lights never come on

They are dark whenever pressing the key would do nothing, so "dark" is the
normal state of a box with nothing to decide — and a press only flashes its own
key white, for 120 ms. The self-test at boot is what tells a chain that is not
wired from one with nothing to say: all four light together, then ✕ red,
■ amber, ✓ green, ↵ white in turn, then dark.

Nothing at boot? The boot log says whether the firmware even has a chain:

```
[lights] 4 keycap LEDs on GPIO2
```

With that line present, it is the wiring — and the usual causes, in order: the
LEDs on 3.3 V instead of 5 V (a WS2812B needs 3.5 V at the very least), the
74AHCT125's `1OE` left floating instead of grounded, the data line taken from
the ESP32 rather than from the buffer's output, or the first LED's `DIN` and
`DOUT` swapped. Without the line, `kKeyLightPin` is -1 for that board.

If the colours walk in the wrong order, the chain runs the other way round than
`kKeyLightChain` says in `Config.h`: flip it. It ships as `{3, 2, 1, 0}`, for a
chain whose data line arrives at the ↵ end; use `{0, 1, 2, 3}` for one fed from
the ✕ end. The constant is a permutation, so a typo that names a position twice
fails the build rather than lighting the wrong key.

### A button fires twice, or at random

Check the ground connection first — a floating return leg picks up noise. Then
raise `kButtonDebounceMs` in `Config.h` (default 25 ms).

### The joystick drifts, or one direction is always active

Look at the calibration line in the boot log:

```json
{"v":1,"type":"log","level":"info","message":"[input] joystick centre x=1893 y=2044, dead zone 700"}
```

- A centre below ~1200 or above ~2900 → the stick was held during boot, or VCC
  is on 5 V. The firmware warns in that case. Reset with the stick at rest.
- Centre plausible but a direction still latches → raise `kJoystickDeadZone`.

### Up/down or left/right are swapped

Three settings in `Config.h`, applied in that order. Module orientations vary and
there is no way to detect it, so all three are wiring-dependent.

- **Vertical comes out horizontal** (up moves left/right, left moves up/down) →
  the two channels are crossed: toggle `kJoystickSwapAxes`. It defaults to `true`
  here, for a module mounted a quarter turn from the board's X/Y labels.
- **The right axis, the wrong way round** (up reads as down) → flip
  `kJoystickInvertX` / `kJoystickInvertY`. Both default to `true` for the module
  in use; set the offending axis back to `false` if yours is wired the other way.

Fix the swap first: the inversions name the logical axes, so they only mean what
they say once the channels line up.

### APPROVE does nothing

Look at the device log, which says exactly which guard refused:

| Device log | Meaning |
| ---------- | ------- |
| `ignored: no permission on screen` | nothing pending — the device is showing status, not a decision |
| `ignored … arming delay not elapsed` | pressed within 200 ms of the screen appearing; press again |
| `ignored: Claude offers no approve` | the dialog offered only deny |
| `no word from the Mac about req-N: back on screen` | the verdict went out, nothing confirmed it within 3 s, so the request came back — press again |

And on the Mac side:

| Daemon log | Meaning |
| ---------- | ------- |
| `refused approve: stale requestId req-N, current is req-M` | the dialog changed between display and press — press again on the current screen |
| `refused approve …: the request is gone from Claude` | the dialog closed first, e.g. you clicked in the app |
| `no approve button bound for req-N` | the button was recognised as text but exposes no `AXPress` |
| `req-N is still on screen after its approve was pressed; pressing it once more` | the first press did nothing, so the daemon repeated it once — see below |
| `not pressing approve req-N again: …` | the repeat was refused by the gate (arming delay, or the request changed); press again yourself |
| `Claude still shows req-N after its approve was pressed twice` | neither press took, or the same question came back — see below |

All of these are the safety rules doing their job. Both sides validate
independently — see [PROTOCOL.md §5](../protocol/PROTOCOL.md).

### The same request comes back, or a press changes nothing

`AXPress` reports success for a button that is no longer wired to anything — in
practice the first press on a dialog is often dropped, and the next one lands.
Claude can also ask the very same question again, which the id, built from the
text on screen, cannot tell from the first.

So when the same request is still on screen 1.5 s after a verdict was pressed,
the daemon presses it once more, after reading the tree again:

```
[SESSION] s5.req-4 is still on screen after its approve was pressed; pressing it once more
```

It does this once per press, for APPROVE and DENY alike, and only through the
checks any press goes through: the gate, and a fresh read that finds this exact
request. If the second press changes nothing either, the box shows a request
that looks unanswered, and the daemon says so:

```
! [SESSION] Claude still shows s5.req-4 after its approve was pressed twice: the
  press did nothing, or the same question came back. The box can answer it again.
```

Press it again: the daemon re-reads the tree and presses the button that is
live at that moment.

Up to version 1.1.0 it could not: a press cleared the gate, nothing re-armed it
while the same request stayed on screen, and every later press was refused with
`refused approve: no permission is pending` — the box dead for that request
until Claude moved on. If you see that line in an old log, that is what it was.

---

## One-time codes

### `NO CLOCK`, refused codes, the setup Wi-Fi

The box makes codes from the Mac's clock, so `NO CLOCK` in the list's header
means the daemon has not sent the time yet: check it is running and connected.
A code the service refuses usually means the Mac's clock is off — turn on
automatic date and time. The setup network takes one device at a time and closes
after five minutes idle. If a phone will not join it, scan the QR code on the
panel instead of typing the password: the password changes every time, and a
phone that joined before keeps offering the old one. The full table is in
[otp.md](otp.md#when-something-goes-wrong).

---

## Build

### `npm install` fails to fetch a prebuilt binary

`koffi` and `serialport` ship prebuilt native binaries. If npm cannot download
one it falls back to building from source, which needs Xcode Command Line Tools:

```bash
xcode-select --install
```

### `Cannot find module 'koffi'`

Dependencies were not installed, or the command was run from the wrong
directory:

```bash
cd mac && npm install
```

`ports`, `simulate` and `help` load the Accessibility modules lazily, so they
still work if koffi is missing — `dump`, `run` and `doctor` do not.

### Running the Mac test suite

```bash
cd mac && npm test
```

The suites use Node's built-in runner, so there is nothing to install beyond
`npm install`.

### `pio run` cannot download packages

First build needs network access for the toolchain and libraries. Afterwards
everything is cached under `esp32/.pio` and `~/.platformio`, and builds work
offline.

### `pio run --target upload` fails to connect

```
A fatal error occurred: Failed to connect to ESP32: No serial data received.
```

- Close `pio device monitor` — it holds the port.
- Hold the **BOOT** button while the upload starts, release once it says
  `Connecting...`. Some boards need this; a 10 µF capacitor between EN and GND
  fixes it permanently.
- Pass the port explicitly:

  ```bash
  pio run -e esp32dev --target upload --upload-port /dev/cu.usbserial-0001
  ```

### The firmware builds but nothing appears on serial

The classic ESP32 DevKit has no native USB — it always goes through the UART
bridge, so `monitor_speed = 115200` and no `ARDUINO_USB_CDC_ON_BOOT`. If you
are on the S3 environment instead, that one *does* use native USB and needs
`-DARDUINO_USB_CDC_ON_BOOT=1`, which is already set in `platformio.ini`. Using
the S3's UART port rather than its USB port requires flipping that to `0`.
