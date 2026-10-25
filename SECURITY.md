# Security

This project does two things that deserve care: it **authorises actions on your
Mac from a physical button**, and it **stores TOTP secrets on a microcontroller**.
Both have a written threat model, and both have limits that are deliberate
rather than oversights.

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** on this repository
(*Security → Report a vulnerability*) rather than opening a public issue, for
anything that would let someone:

- get an `approve` to Claude Desktop without a deliberate press on the box,
- read a TOTP secret off the box over USB, over the setup Wi-Fi, or out of a log,
- make the box produce a code without a press on its keys.

Anything else — a crash, a wrong screen, a parsing bug with no privilege
attached — is a normal issue, and a public one is more useful.

There is no SLA on this. It is a personal project, not a product.

## What is already known and documented

These are not vulnerabilities. They are the project's stated limits, and a
report that restates one will be closed with a link here.

**Secrets are stored unencrypted in the ESP32's flash.** With the board in hand
and esptool, they can be read out. ESP32 flash encryption would close this, but
it is a one-way eFuse change that can brick a board when misconfigured, so this
project does not turn it on. Treat the box like an unlocked phone running an
authenticator app. Full model: [`docs/otp.md`](docs/otp.md#how-it-is-kept-safe).

**The box trusts the Mac's clock.** It has no clock of its own. Software in
control of the serial port could send a future time and keep the code you then
make — it still needs your press, and it is the same limit a YubiKey's TOTP has.

**The Accessibility grant is broader than this tool.** macOS attaches it to the
application that launched the daemon — normally your terminal — so everything
that terminal runs inherits the ability to read any app's UI and synthesise
presses. A LaunchAgent scopes it to the binary instead, which is why
[`docs/setup-macos.md`](docs/setup-macos.md#running-it-as-a-login-agent-optional)
prefers one for anything long-running.

**A press Claude did not act on is repeated, once.** `AXPress` can report
success for a button that did nothing, so when the same request is still on
screen 1.5 s after a verdict, the daemon presses it again — after reading the
tree again, and through the same gate as any press. It is your one press, not a
second verdict, but if Claude closed the dialog and asked an identical question
in that interval, the repeat lands on the new one. Same text, same request id:
it is the decision you already made. It never repeats twice, and never presses
for a different request.

**The log can contain the text of permissions and messages.** At `--log debug`
the file holds every frame in both directions. One-time codes are the exception
and are masked everywhere, including in debug traces. `--no-log-file` turns the
file off.

**Deleted secrets can persist in flash.** NVS erases logically; the bytes can
stay until the page is reused. `pio run -e <board> -t erase` is the only
certain removal, and it removes every account.

## What the approve path guarantees

APPROVE authorises a real action, so it is guarded on both sides independently
([PROTOCOL.md §5](protocol/PROTOCOL.md)). Nothing is ever auto-approved; every
verdict is bound to a request id that both sides check; the Accessibility tree
is re-read immediately before the press, so a dialog swapped between display and
press cannot be approved by the press meant for the previous one.

If you find a sequence that defeats any of those, that is exactly the report
this file is asking for.
