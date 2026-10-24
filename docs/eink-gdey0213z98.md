# The GDEY0213Z98 panel

GoodDisplay GDEY0213Z98: 2.13", 122x250, SSD1680 controller, three pigments
(black / white / red). The datasheet is not redistributed here —
[`reference/README.md`](reference/README.md) says where to get it, and every
register this page relies on is quoted below.

This page records what was measured on the actual panel before the firmware was
written, because it decides how the UI is driven. The bring-up sketch that
produced these numbers is kept verbatim at
[`reference/eink-hybrid-experiment.cpp`](reference/eink-hybrid-experiment.cpp).

## The measurement that matters

| Path | Driver | Refresh |
| ---- | ------ | ------- |
| Tri-colour, OTP waveform | `GxEPD2_213_Z98c` | **~14 s** full, no partial |
| Monochrome, MCU-supplied LUT | `GxEPD2_213_BN` | **~300 ms** partial |

14 seconds is not a UI. 300 ms is. That is the whole reason
`EINK_MODEL_GDEY0213Z98_FAST` — the same panel driven as monochrome through
`GxEPD2_213_BN` — is the default in
[`esp32/include/DisplayModel.h`](../esp32/include/DisplayModel.h).

## Why the mono driver is so much faster on a tri-colour panel

The SSD1680 can take its waveform from on-chip OTP *or* from the MCU
(datasheet p.4, and reference program 14.2: "Command 0x32 Write 224bytes LUT").
GxEPD2's monochrome driver does exactly that:

```cpp
void GxEPD2_213_BN::_Init_Part() {
  _writeCommand(0x32);
  _writeDataPGM(lut_partial, sizeof(lut_partial));
}
```

So it never uses the panel's tri-colour waveform. The price is that its LUT only
drives black↔white transitions — it cannot move the red pigment at all. For a
control terminal that is a good trade: no red, but a usable refresh rate.

Note that the meaning of the two RAM planes flips with the loaded waveform:

| Waveform | `0x24` | `0x26` |
| -------- | ------ | ------ |
| tri-colour (OTP) | black plane | **red** plane |
| monochrome (MCU LUT) | new image | **previous** image |

That is a trap when mixing the two drivers: a full mono refresh right after a
tri-colour pass sends the old red plane through the mono pipeline and produces
garbage. `switchToFast()` in the reference sketch works around it with
`init(0, false, …)` plus `writeScreenBufferAgain(0xFF)`.

## Why red erodes under partial refresh

Observed: with red bands painted by the tri-colour driver, each subsequent mono
partial refresh faded them slightly, cumulatively.

The window commands (`0x44`/`0x45`) do not protect anything — on the SSD1680
they bound RAM *writes*, not which gates get driven. A partial refresh does not
skip pixels outside the window; it applies the LUT's "unchanged" transition to
them. And GxEPD2's LUT does not make that a no-op:

```
LUT0 (0->0, white->white) : 0x00, 0x40, 0, 0, ...   0x40 = VSH1
LUT3 (1->1, black->black) : 0x00, 0x80, 0, 0, ...   0x80 = VSL
```

One pulse frame on every unchanged pixel, fifty refreshes in a row, is enough to
move red pigment.

The mitigation is a softened LUT with `LUT0` and `LUT3` zeroed, so an unchanged
pixel receives no voltage at all. Only real black↔white transitions are driven.

**The firmware now ships it** (`EINK_PANEL_GENTLE_LUT`, on by default for
`EINK_MODEL_GDEY0213Z98_FAST`). The earlier reasoning — "nothing is painted red,
so there is nothing to erode" — missed the *other* direction: the stock LUT's
non-zero unchanged-pixel pulses do not just erode existing red, they slowly
*create* a pink cast on settled areas by nudging the red pigment, and the more
partials you do the faster it builds. A UI that redraws often — and a little
walking chicken in the title bar redraws once a second — hits this quickly, so
the tint kept coming back between the periodic white flushes. Leaving unchanged
pixels alone removes the cause.

`installLut(lutFor(waveform))` in
[`../esp32/src/DisplayDriver.cpp`](../esp32/src/DisplayDriver.cpp) writes the
table by hand (command `0x32`) right after boot's first partial, because GxEPD2
keeps its own LUT private. It persists for the life of the run: GxEPD2 only
reloads its LUT when it re-enters partial mode, which happens once at boot (and
once more after the deep clean, where the same routine runs again).

Which waveform suits the glass is a judgement made by eye, so the firmware
carries several (`include/Waveform.h`) and the on-screen menu's **Waveform**
entry switches between them at runtime:

| Waveform | Black↔white drive | Unchanged pixels | Frames per refresh |
| -------- | ----------------- | ---------------- | ------------------ |
| `gentle` | 31 frames | no voltage | 32 |
| `stock` (GxEPD2's table verbatim) | 31 frames | 1-frame kick | 32 |
| `deep` (the default) | 61 frames | 1-frame kick | 62 |

`deep` exists because GxEPD2's table was written for a black-and-white panel.
This glass holds three pigments in a slower fluid, and 31 frames of VSL leave
its black a dark grey. `deep` repeats the 30-frame drive once more, for black
and white alike, so a pixel switched back and forth stays DC-balanced. The
voltages are unchanged — the datasheet's reference program sets VSH1 `0x41` and
VSL `0x32`, ±15 V, and no waveform here goes past them — so the price is time:
each refresh lasts about twice as long as with `stock`.

`kDefaultWaveform` in `Config.h` fixes the one loaded at boot; the boot log
prints `mono LUT '<name>' installed`. Set `EINK_PANEL_GENTLE_LUT` to 0 in
`DisplayModel.h` to leave GxEPD2's own table untouched and drop the selector.
The originally tested softened waveform lives in the reference sketch.

Any red already burned into the glass from an earlier tri-colour session is a
different matter: the mono LUT cannot move red pigment, so it cannot clear it.

The firmware can now run that cycle itself — **menu → Clean the screen**
(`GxEPD2Driver::clean()` in `DisplayDriver.cpp`). It constructs the tri-colour
panel class for the three passes, then calls `enterMonoMode()`, which is the
documented way back: `init(initial=false)` to clear both initial flags,
`writeScreenBufferAgain(0xFF)` to blank the plane the tri-colour pass left in
RAM `0x26`, a white partial, and the softened LUT reinstalled on top. Only the
low-level panel class is instantiated, so no second framebuffer is allocated.

The standalone `esp32dev_clear_panel` tool runs the identical three passes and
stays around for a panel whose firmware will not boot.

## The mono driver must never do a full update on this glass

This is the trap that actually bites, and it is worth being precise about
because GxEPD2 triggers it in two places that are easy to miss.

`GxEPD2_213_BN` has two update paths:

```cpp
void GxEPD2_213_BN::_Update_Full() {   // 0x22 / 0xF4  -> OTP waveform
  _writeCommand(0x22); _writeData(0xf4); _writeCommand(0x20);
}
void GxEPD2_213_BN::_Update_Part() {   // 0x22 / 0xCC  -> MCU LUT from 0x32
  if (!_using_partial_mode) _Init_Part();
  _writeCommand(0x22); _writeData(0xcc); _writeCommand(0x20);
}
```

Only `_Update_Part` loads the monochrome LUT. `_Update_Full` uses whatever sits
in the panel's OTP — and on the GDEY0213Z98 that is the **tri-colour**
waveform. Under it RAM `0x26` is the *red plane*, not "previous image". The BN
driver never meant `0x26` as red, so whatever it holds gets painted red:
**the panel comes up solid red.**

Two things reach `_Update_Full` without asking:

```cpp
void GxEPD2_EPD::init(uint32_t, bool initial, uint16_t, bool) {
  _initial_write = initial;
  _initial_refresh = initial;     // <- both
  ...
}
void GxEPD2_213_BN::refresh(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (_initial_refresh) return refresh(false);   // <- promotes to FULL
}
```

So `init(0, true, …)` does one full update immediately, **and** the first
partial refresh afterwards is silently promoted to a full one. Passing
`initial = false` clears both flags, which is why the firmware does:

```cpp
display_.init(0, /*initial=*/false, 50, false);
display_.epd2.writeScreenBufferAgain(0xFF);   // prime 0x24 and 0x26 to white
```

`writeScreenBufferAgain` only writes RAM (`_writeScreenBuffer` does no refresh
and loads no waveform), so it is safe to call before anything is displayed.

`EINK_PANEL_OTP_IS_3C` in `DisplayModel.h` is the flag that expresses "the glass
is 3-colour but I am driving it in mono". When it is set, `DisplayDriver.cpp`:

- passes `initial = false` and primes both RAM planes to white;
- clears to white at boot, then draws content in black;
- serves a *Full* refresh request as an all-white partial frame followed by the
  content, since de-ghosting must also stay on the mono LUT;
- never calls `setFullWindow()`.

A compile-time `#error` rejects `EINK_PANEL_OTP_IS_3C` without
`EINK_PANEL_SUPPORTS_PARTIAL`, because that combination leaves no way to reach
the panel safely at all.

On genuinely monochrome glass set `EINK_PANEL_OTP_IS_3C` to 0 and full updates
are correct and preferable.

## Mono mode cannot clear red

This follows directly from the LUT above and is worth stating on its own,
because it surprises people: the monochrome waveform has **no phase capable of
moving the red pigment**. That is precisely why the tri-colour waveform needs
14 s while the mono one needs 300 ms.

So in `EINK_MODEL_GDEY0213Z98_FAST`, red already on the glass is permanent. No
number of full refreshes will shift it — the driver is not merely choosing not
to repaint it, it physically cannot. It shows through underneath the UI.

The fix is a single pass with the tri-colour waveform, which is what
`src/tools/ClearPanel.cpp` does:

```bash
cd esp32
pio run -e esp32dev_clear_panel --target upload
pio device monitor                        # ~45 s
pio run -e esp32dev --target upload       # back to the firmware
```

Run it once after any tri-colour experiment. A panel that has only ever been
driven in mono has no red to clear.

## Pigment conditioning

A panel that has been idle shows weak, muddy colour on its first tri-colour
refresh. Three full clears — all black, all red, all white — restore
saturation, and take about 45 s together. The reference sketch does this behind
`CONDITION_ON_BOOT`.

The shipped firmware does not, because the monochrome path does not use the red
pigment. If you switch to the tri-colour model and the first screens look washed
out, run the three clears once.

## Practical limits of this panel

- **No greyscale.** Black or white per pixel in the fast path. Hierarchy has to
  come from font weight, size and rules, which is what `Display.cpp` does.
- **122 px of height** is about 6 lines of the built-in 6x8 font with a header
  and footer, or 4 lines of `FreeMonoBold9pt7b`. Longer content is paged with
  the joystick rather than shrunk.
- **Partial refresh is visible.** It flashes the changed area. The firmware
  therefore redraws only when the content hash changes, and rate-limits to
  `kMinRedrawIntervalMs` (400 ms).
- **Ghosting accumulates.** A full refresh every 20 partials clears it; holding
  ACTION forces one.

## Using a different panel

One macro in `DisplayModel.h`, and the geometry constants if the size differs.
Nothing else in the firmware names a panel class — everything draws through
`DisplayDriver`. The file documents the procedure inline.

A monochrome panel with fast partial refresh is the best fit for this project.
Tri-colour looks nicer in photographs and is painful to use.
