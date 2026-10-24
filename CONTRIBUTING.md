# Contributing

Thanks for looking. This is a small hardware project, so the most useful
contributions are usually the unglamorous ones: a board that needed different
pins, a Claude Desktop release that moved the buttons, a step in the setup that
did not work on your machine.

## Before anything

```bash
cd mac && npm install
```

```bash
cd mac && npm test
```

```bash
cd esp32 && pio test -e native
```

Both suites run with no hardware and no Claude Desktop attached — the daemon
tests work against the fixtures in `mac/src/simulation/`, and the firmware tests
cover the parts with no Arduino in them. If either is red before you start, say
so in the issue rather than working around it.

CI runs exactly these two, plus a build of every board variant
(`esp32dev`, `esp32dev_mock`, `esp32-s3-devkitc-1`, `esp32-c3-supermini`,
`esp32dev_clear_panel`). A change that only builds on your board will be caught
there, so build the variants locally if you touched anything shared:

```bash
cd esp32 && pio run
```

## The three rules that are easy to miss

**1. The protocol lives in three places.** `mac/src/protocol/constants.js`,
`esp32/include/Protocol.h` and [`protocol/PROTOCOL.md`](protocol/PROTOCOL.md)
mirror each other. Change one and you must change all three, in the same commit
— the constants carry a comment saying so, and the host tests check the limits
on both sides.

**2. Look at the screens after touching `Display.cpp`.**

```bash
esp32/tools/preview/render.sh
```

It compiles the real layout code against a stub driver and writes a contact
sheet of every screen. Text that overruns its band, or a legend that collides
with the line above it, is invisible in a compiler's output and obvious in the
image.

**3. Pins live in `esp32/include/Config.h` and nowhere else.** If a change needs
a GPIO number written somewhere other than that file, it is in the wrong place.
The same goes for the panel: one `#define` in `DisplayModel.h` selects it, and
everything draws through `DisplayDriver`.

## Safety-critical paths

APPROVE authorises a real action on the user's machine. The rules in
[PROTOCOL.md §5](protocol/PROTOCOL.md) and the Safety section of the README are
not style preferences — nothing is auto-approved, every verdict is bound to a
request id, and the Accessibility tree is re-read immediately before a press.

A change that touches the permission path, the request-id rules or the approve
gate needs a test showing the guard still holds. There are existing ones to
follow in `mac/test/permission.test.js` and the request-id cases in
`esp32/test/test_logic/test_main.cpp`.

## Claude Desktop keeps moving

Claude Desktop is an Electron app with no stable identifiers in its
Accessibility tree, so recognition is heuristic and breaks on its own schedule.
If the box stops seeing permissions after a Claude update, that is this
project's most common bug and the fix is usually small:

```bash
cd mac && node bin/claude-remote.js dump --filter button
```

The real labels come out of that, and they go into `approveLabels` /
`denyLabels` in `mac/src/accessibility/selectors.js`. Please include the Claude
Desktop version in the issue or PR — the labels are version-specific and it
helps the next person.

## Style

Match the file you are in. Broadly: ES modules and JSDoc on exported symbols on
the Node side, C++17 and `-Wall -Wextra` clean on the firmware side, comments
that say *why* rather than restating the code. There is no linter and no
formatter to run.

Commit messages: a short imperative subject line, and a body explaining the
reasoning when it is not obvious from the diff.

## What to open an issue about

- A board or panel that needed different wiring, with what you changed
- A Claude Desktop version that broke detection, with the version and the
  `dump --filter button` output
- A setup step that did not work on your macOS version
- Anything in [`docs/troubleshooting.md`](docs/troubleshooting.md) that did not
  actually resolve the problem

Hardware questions are welcome. "Will it work with panel X" is a reasonable
issue, and the answer usually belongs in the docs afterwards.
