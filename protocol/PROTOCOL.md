# ChickenMicro wire protocol

Version: **1**

Transport: USB serial (UART bridge on the ESP32 DevKit), **115200 8N1**, no flow
control.

Framing: **NDJSON** — one JSON object per line, terminated by `\n`. `\r` is
tolerated and stripped. Lines longer than `PROTO_MAX_LINE` (**768 bytes**) are
dropped with a log, not truncated-and-parsed — the tail of an oversized line is
never reparsed as a fresh frame.

The sender guarantees this: free-text fields (`text`, `content`, `title`,
`detail`, `name`) are truncated by *encoded* byte cost, not character count,
because a quote costs 2 bytes escaped, a control character 6 and a non-ASCII
character up to 4. A truncated field ends with `...` — three ASCII dots, because the panel's font
draws nothing else and an ellipsis character reached it as three pieces of
garbage. Consequently a receiver
never has to handle a legitimately oversized frame.

The Mac also folds those fields to **ASCII** before sending: the panel's fonts
cover 0x20–0x7E and nothing else, so `à` would either be dropped or drawn as two
pieces of CP437 line art. Accents are removed, typographic punctuation is
replaced with its ASCII equivalent, emoji are dropped, and anything left becomes
`?`. See `mac/src/protocol/displayText.js`.

Every frame carries `v` (protocol version, integer) and `type` (string). A frame
whose `v` does not equal the receiver's version is rejected and logged. A frame
missing `type`, or carrying an unknown `type`, is rejected and logged.

Both directions are line-oriented and stateless at the transport level: there is
no handshake requirement beyond `hello`, and either side may be restarted at any
time.

---

## 1. Mac → ESP32

### `state`

Normalised Claude Desktop state. Sent whenever the state changes.

```json
{"v":1,"type":"state","state":"thinking"}
```

| Field   | Type   | Required | Notes                          |
| ------- | ------ | -------- | ------------------------------ |
| `state` | string | yes      | one of the states in §3        |
| `detail`| string | no       | short context line, ≤ 96 chars |

```json
{"v":1,"type":"state","state":"thinking","detail":"Refactor Table.tsx"}
```

### `message`

Last assistant message text.

```json
{"v":1,"type":"message","text":"I'll inspect the component and update the tests"}
```

| Field  | Type   | Required | Notes                                      |
| ------ | ------ | -------- | ------------------------------------------ |
| `text` | string | yes      | truncated by the Mac to 512 chars, and to the frame's byte budget |

Newlines inside `text` are encoded as `\n` by JSON and are preserved by the
firmware as paragraph breaks.

### `sessions`

Every Claude Desktop window the Mac can see, and which one the `state` and
`message` frames are about. Sent whenever the list, the focus or the active
permission changes. See §8.

```json
{"v":1,"type":"sessions","focus":"s1","permission":"s2.req-1","total":2,"sessions":[{"id":"s1","name":"chickenMicro","state":"writing"},{"id":"s2","name":"docs","state":"waiting_permission"}]}
```

| Field        | Type     | Required | Notes                                                      |
| ------------ | -------- | -------- | ---------------------------------------------------------- |
| `sessions`   | object[] | yes      | ≤ 8 entries; may be empty when no window is open            |
| `focus`      | string   | no       | session the `state` / `message` frames describe             |
| `permission` | string   | no       | id of the request the Mac considers active; absent for none |
| `total`      | int      | no       | sessions the Mac has, which may exceed the listed ones      |

Each entry:

| Field   | Type   | Required | Notes                                            |
| ------- | ------ | -------- | ------------------------------------------------ |
| `id`    | string | yes      | ≤ 16 chars, `[A-Za-z0-9._:-]`, stable per window |
| `name`  | string | no       | display name, ≤ 30 chars; defaults to `id`       |
| `state` | string | yes      | one of the states in §3                          |

`permission` is **level-triggered**: the device drops any active request whose id
it does not match. A `permission_cleared` lost to line noise therefore cannot
strand a dead request on the screen.

### `permission`

A permission request is pending in Claude Desktop. **This is the priority
screen** on the device.

```json
{"v":1,"type":"permission","id":"s2.req-1","kind":"shell","session":"s2","title":"Run command","content":"git push origin master","actions":["deny","approve"]}
```

| Field     | Type     | Required | Notes                                              |
| --------- | -------- | -------- | -------------------------------------------------- |
| `id`      | string   | yes      | opaque request id, ≤ 64 chars, `[A-Za-z0-9._:-]`   |
| `kind`    | string   | yes      | free-form tag shown in the header, e.g. `shell`    |
| `session` | string   | no       | id of the session that raised it (§8)              |
| `title`   | string   | no       | human label of the request                         |
| `content` | string   | no       | the command / payload being authorised             |
| `actions` | string[] | yes      | subset of the action enum in §4, ≥ 1 entry         |

The ESP32 stores `id` as the **only** id it will quote back in an `action`.

Only one request is on the device at a time. With several windows asking at
once the Mac sends the oldest, and does not replace it while it is on screen —
the queue is visible as `+N waiting`.

### `permission_cleared`

The pending permission disappeared from Claude Desktop (user acted in the app,
dialog closed, app quit). The ESP32 must drop its active permission
immediately and stop accepting approve/deny for it.

```json
{"v":1,"type":"permission_cleared","id":"req-123"}
```

| Field | Type   | Required | Notes                                                |
| ----- | ------ | -------- | ---------------------------------------------------- |
| `id`  | string | no       | if present and it does not match the active id, ignore |

### `hello`

Sent by the Mac on connect and after every reconnect.

```json
{"v":1,"type":"hello","role":"mac","app":"claude-remote","version":"1.0.0","approveDelayMs":200}
```

| Field            | Type   | Required | Notes                                            |
| ---------------- | ------ | -------- | ------------------------------------------------ |
| `role`           | string | yes      | always `mac`                                     |
| `app`            | string | no       | daemon name                                      |
| `version`        | string | no       | daemon version                                   |
| `approveDelayMs` | int    | no       | arming delay the device must honour, see §5       |

### `ping` / `pong`

Liveness. Either side may send `ping`; the peer answers `pong` with the same
`seq`.

```json
{"v":1,"type":"ping","seq":41}
```
```json
{"v":1,"type":"pong","seq":41}
```

The Mac pings every 3 s. If three consecutive pings go unanswered it logs a
warning and reopens the port.

### `error`

Non-fatal problem the device should surface.

```json
{"v":1,"type":"error","message":"Accessibility permission missing"}
```

### `time`

The Mac's clock. The device makes one-time codes from it (§9) and has no clock
of its own that survives a power cycle. Sent on connect, whenever the device
says `hello`, and every 60 s after; the device carries it forward in between.

```json
{"v":1,"type":"time","unixMs":1726000000123}
```

| Field    | Type | Required | Notes                                                          |
| -------- | ---- | -------- | -------------------------------------------------------------- |
| `unixMs` | int  | yes      | Unix time in ms; refused outside 2023-11-14 … 2100, and as a boolean or fraction |

---

## 2. ESP32 → Mac

### `action`

A deliberate button press.

```json
{"v":1,"type":"action","action":"approve","requestId":"req-123"}
```
```json
{"v":1,"type":"action","action":"deny","requestId":"req-123"}
```
```json
{"v":1,"type":"action","action":"stop","sessionId":"s2"}
```

| Field       | Type   | Required                        | Notes                        |
| ----------- | ------ | ------------------------------- | ---------------------------- |
| `action`    | string | yes                             | enum, §4                     |
| `requestId` | string | **yes for `approve`/`deny`**    | must be the id last displayed |
| `sessionId` | string | no, and only for `stop`/`context` | session to act on (§8)     |

`sessionId` is deliberately **not** accepted on a verdict: `requestId` already
resolves to exactly one session on the Mac, and a second address could only
disagree with it.

An `approve` or `deny` without `requestId`, or with a `requestId` that is not
the Mac's currently pending request, is **rejected** — logged and dropped. See
§5.

### `navigation`

Joystick movement and select. Never carries side effects on the Mac side; it is
used for logging and for optional scroll-follow features.

```json
{"v":1,"type":"navigation","action":"up"}
```

`action` ∈ `up` | `down` | `left` | `right` | `select`.

### `focus`

Asks the Mac to report on another session. The Mac answers with a fresh
`sessions` frame plus the `state` and `message` of the session it switched to.

```json
{"v":1,"type":"focus","sessionId":"s2"}
```

A `sessionId` that is not open is logged and ignored.

### `mode`

Tells the Mac how the box wants permissions answered. Sent by the firmware on
boot, whenever it sees a Mac `hello`, and each time the setting changes in the
menu.

```json
{"v":1,"type":"mode","value":"auto"}
```

| `value` | Meaning |
| ------- | ------- |
| `auto`  | Answer through a Claude Code hook when one is listening, and fall back to pressing the buttons in Claude's window when none is. See §10. |
| `ui`    | Only ever press the buttons in Claude's window. |

Any other `value` is rejected and logged. A Mac with no hook channel notes the
frame and changes nothing, and a device that never sends one is treated as
`auto`.

### `hello`

Sent by the firmware on boot and whenever it sees a Mac `hello`.

```json
{"v":1,"type":"hello","role":"device","app":"chickenmicro-fw","version":"1.0.0"}
```

`role` is always `device`.

### `log`

Device-side log line, forwarded into the daemon's log stream.

```json
{"v":1,"type":"log","level":"info","message":"display ready 250x122"}
```

`level` ∈ `debug` | `info` | `warn` | `error`.

### `ping` / `pong`

Same shape as §1.

### `otp`

A one-time code, made because the operator pressed a key on the box (§9). The
Mac puts `code` on the clipboard, and clears it `expiresInMs` + 2 s later if the
clipboard still holds it.

```json
{"v":1,"type":"otp","label":"GitHub:alice","code":"042917","expiresInMs":18000}
```

| Field         | Type   | Required | Notes                                                   |
| ------------- | ------ | -------- | ------------------------------------------------------- |
| `code`        | string | yes      | 6 to 8 ASCII digits and nothing else; anything else is refused |
| `label`       | string | no       | the account's name, ≤ 24 chars, for the log             |
| `expiresInMs` | int    | yes      | time left in the code's window, 0 … 300000              |

---

## 3. Claude state enum

```
disconnected        no Claude Desktop process found
idle                Claude Desktop running, nothing in flight
thinking            Claude is working
writing             Claude is streaming a reply
waiting_permission  a permission request is pending
done                last turn finished
error               daemon or Accessibility failure
```

An unknown state string is rejected by the firmware; it keeps the previous
state and logs the offending value.

---

## 4. Action enum

Strict enum. Anything else is rejected.

```
approve      needs requestId
deny         needs requestId
approve_all  needs requestId — approves every queued request; the id is the one
             shown on the device, so the same arming/staleness guard applies
stop         no requestId  — may carry sessionId (§8)
context      no requestId  — contextual button
up down left right select   (sent as type "navigation")
```

`approve_all` is honoured by pressing each currently-pending window's own approve
button, oldest first. The queued requests it approves are ones the operator has
*not* seen on the device, which is inherent to a bulk approve; the firmware puts
it on a separate control (a joystick flick, not the ✓ button) so it cannot be
hit by mistake in place of a single approve.

---

## 5. Approve safety rules

`approve` is the only destructive message in the protocol. Both sides enforce:

1. **No automatic approval.** The firmware only emits `approve` from a single
   deliberate press of the dedicated APPROVE button. Never from a long press,
   a repeat, or a double click. `approve_all` is the one bulk verdict; it is a
   separate, deliberate gesture (a joystick flick, offered only when two or more
   requests are queued) so it can never be triggered in place of a single
   approve, and it is gated by rules 2, 3 and 5 exactly as `approve` is.
2. **Id binding.** `approve`/`deny` must quote the `id` of the permission
   currently on screen. The firmware refuses to send otherwise.
3. **Staleness check on the Mac.** The daemon re-reads the tree before pressing
   and compares `requestId` against the live pending request, dropping any
   mismatch — this is what stops a late-arriving press from authorising a
   *different* command. There is no exception to this: a window is read wherever
   it sits, including behind a full-screen app on another Space (§8), so a
   request the re-read cannot find is genuinely gone and the verdict is refused.
   A button remembered from when the request was displayed is never pressed.
4. **Clearing.** The firmware drops the active permission on
   `permission_cleared`, on a `sessions` frame whose `permission` is absent or
   names a different request, and when the link goes quiet. It is deliberately
   *not* dropped on a `state` frame any more: with several windows open, a state
   frame describes the followed session and says nothing about a request raised
   by another one.
5. **Arming delay.** A permission is not actionable until
   `approveDelayMs` (default 200 ms) after it was rendered. This absorbs a
   press that was already in flight when the screen changed.
6. **The screen moves on at once, and can be taken back.** The device leaves
   the permission screen the moment it sends a verdict, without waiting for the
   Mac: a key press should feel like it did something, and the round trip is a
   read, a press and a poll. The request is not dropped, though — it is held
   aside, unanswerable (one press, one verdict), and comes back on screen, armed
   afresh, if the Mac has not confirmed within `VERDICT_WAIT_MS` (3 s). A
   decision the operator believes they made must never be the one that went
   quietly missing.

   The Mac shortens that wait: when a verdict it received cannot be applied —
   refused by its own checks, no button bound, `AXPress` failing — it re-sends
   the same `permission` frame, which puts the request straight back. A device
   that never cleared it is not redrawn, since the id has not changed. A verdict
   that *did* land ends as before, with `permission_cleared` or a `sessions`
   frame that no longer names it.
7. **Strict parsing.** Serial noise never becomes an action: a frame must be
   valid JSON, carry the right `v`, a known `type`, and an enum-valid
   `action`. Partial lines are discarded on overflow rather than parsed.

---

## 6. Reconnection

The ESP32 has no way to detect the host closing the port, so it is stateless
about the link: it keeps rendering the last known state and marks itself
`disconnected` after `LINK_TIMEOUT_MS` (10 s) with no inbound frame.

The Mac owns reconnection: on read/write error or port disappearance it closes
the fd, waits `RECONNECT_DELAY` (1 s), rescans for the port and reopens. On
every successful open it re-sends `hello` followed by the full current snapshot
(`sessions`, then `state`, then `message`, then `permission` if one is pending)
so the device resynchronises without needing its own request mechanism.

Frame order within one update is load-bearing on e-ink: a permission that is
going away is cleared *before* the new state, and a new one is sent *after* it,
so each transition costs one redraw instead of two.

---

## 7. Versioning

`v` is a single integer. A receiver rejects frames whose `v` differs from its
own. Bumping `v` is therefore a hard break; add optional fields instead
whenever possible — unknown fields are ignored by both sides.

---

## 8. Sessions

A **session is one Claude Desktop window**. Several windows, and several copies
of the app running side by side with their own `--user-data-dir`, are all
sessions; the device lists them and can be pointed at any one.

Identity is the window's Accessibility element, which is stable for as long as
the window is open — Electron exposes no identifier, every window is titled
"Claude", and `AXWindows` is ordered by z-order, so it changes the moment
someone clicks. The Mac maps that to a short `s1`, `s2` id, stable across polls
and never reused once a window closes.

Names are resolved in order: the window title, then the document title of each
web area in the window, then the name the window last gave of itself, then the
profile directory name (`--user-data-dir`), then `Session N`.

The document title is the one that names the session — "Migration React 19 -
Claude Code" — so the app's own name is stripped off the end of it. A window
holds several web areas (its chrome, a side panel, the conversation), and only
one of them is titled, which is why they are all tried. The remembered name
matters because Electron drops the web accessibility tree from time to time:
without it a session renamed itself to its profile mid-conversation, and the
list on the box shuffled under the operator.

Thirty characters is what a list row holds beside its state tag, so a longer
title is clipped rather than run into `THINKING`.

`AXWindows` only lists the windows on the Space in front, so when a full-screen
app takes over, Claude's windows drop out of it. The Mac keeps the window
elements it has already seen and reads them where they are, so a session stays
listed — and its permission stays answerable — from behind a full-screen app on
another Space, and a verdict for it presses that window's own live button.

Because the window really is read on any Space, an empty result is information
rather than a blind spot: when no dialog is found the request has been answered,
and the device is told at once. Treating an off-Space window as merely unreadable
— and holding its last request on the screen — is what once left the box asking
for a decision the operator had already made in the app. A window the daemon has
never seen (already on another Space at startup) is picked up the first time it
comes to the front.

Each session is interpreted on its own subtree, so its state, its request ids
and its buttons belong to it alone. This is a safety property, not a cosmetic
one: reading every window as one tree let a verdict bind to an "Allow" button
belonging to a different window from the one whose request was on screen.

**Focus** — which session `state` and `message` describe — follows whichever was
touched last: the device's own choice (`focus` frame) wins, until the window in
front on the Mac changes, which releases it.

While a request is on the device, focus goes to the session that asked, unless
the operator picked one on the box. The screen asks about one conversation, so
everything around the question describes that same conversation — and answering
it leaves the box there. Without this the state flipped between two
conversations every time the window in front changed on the Mac.

---

## 9. One-time codes

The box can act as a TOTP authenticator ([docs/otp.md](../docs/otp.md)). The wire
follows four rules:

1. **Secrets never cross it.** Accounts are added over the box's own Wi-Fi setup
   page, never over serial, and nothing the box sends carries a secret.
2. **Only the device starts.** There is no Mac → device request for a code: an
   `otp` frame exists only because a key on the box was pressed. `time` is the
   only one-time-code frame the Mac sends.
3. **An `otp` frame carries digits.** Both sides refuse a `code` that is not 6 to
   8 ASCII digits, so the box cannot put arbitrary text on the clipboard.
4. **Codes are never logged.** Neither side writes one to a log; the Mac masks
   the `code` field even in debug traces and rejected-frame reports.

---

## 10. The hook channel (Mac-local)

A Claude Code `PermissionRequest` hook can hand the daemon a request and wait for
the box to answer it. This is **not** part of the serial protocol: it is a Unix
socket on the Mac, and the box never sees it. The box sees only the `permission`
frame the daemon builds from it, and answers with the usual `action`.

**Transport.** A Unix socket, `~/.claude-remote/hook.sock` by default
(`CLAUDE_REMOTE_SOCKET` or `--hook-socket` override it, for the hook and the daemon
alike). The directory is `0700` and the socket `0600`: only the user can connect,
which is the whole of its authentication. NDJSON, one connection per request,
lines of at most 65 536 bytes.

**Hook → daemon**, one line, then the hook waits:

```json
{"v":1,"type":"request","tool":"Bash","session":"<claude session id>","cwd":"/Users/me/proj","input":{"command":"git push"}}
```

**Daemon → hook**, one line, then the daemon closes:

```json
{"v":1,"type":"verdict","decision":"allow"}
{"v":1,"type":"verdict","decision":"deny","message":"Refused from the ChickenMicro box"}
{"v":1,"type":"cancel","reason":"answered in the app"}
```

A hook that reads `verdict` prints the matching decision to Claude Code, and one
that reads anything else, nothing, or a closed socket exits with **no decision**,
so Claude's own dialog stays in charge. Nothing here ever answers for the user:

1. **No default answer.** A hook with no verdict prints no decision.
2. **One answer, to the one that asked.** A verdict goes only to the connection
   that carried the request, once.
3. **Allow means once.** The hook never returns a permission update, so nothing
   is remembered and no mode is changed.
4. **The same gate.** A hook request reaches the box as a `permission` frame with
   an id of the form `hN`, and a verdict on it passes §5 rules 1, 2, 5 and 6 like
   any other. Rule 3 becomes: the request must still be waiting — its hook has not
   given up, and its dialog has not been answered in the app.
5. **Fallback.** When no hook claims a dialog Claude draws, the daemon offers the
   dialog as it always did, after a grace of 0.6 s if hooks have recently been
   seen. A hook that times out hands its dialog straight back.
6. **The mode.** With `mode: "ui"` every request is answered `cancel` at once.

A request and the dialog drawn for it are matched by the text they share (the
command, the file path, the URL). A match that fails costs two entries on the box,
never an approval on the wrong request.
