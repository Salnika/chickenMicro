# Wiring

Reference board: **ESP32 DevKit V1 (ESP32-WROOM-32)**, powered and programmed
over USB.

Every pin below is defined in one place: [`esp32/include/Config.h`](../esp32/include/Config.h).
Change it there and nothing else needs touching.

> Building the small version, on an **ESP32-C3 SuperMini** with the buttons on an
> MCP23008 expander? Its wiring is on its own page:
> [`wiring-esp32c3-supermini.md`](wiring-esp32c3-supermini.md).

> **3.3 V only.** The e-ink panel and the joystick both run from `3V3`. On 5 V
> the joystick's analog outputs would exceed what the ESP32's ADC can accept,
> and a 3.3 V panel can be damaged outright.

> **All grounds must be common.** Display GND, joystick GND, every button's
> return leg and the ESP32 GND are the same net.

---

## Bill of materials

Everything is through-hole or a breakout board, and nothing needs a PCB — a
breadboard is enough to get to a working box.

### Core — the box does not work without these

| Qty | Part | Notes |
| --- | ---- | ----- |
| 1 | **ESP32 DevKit V1** (ESP32-WROOM-32) | powered and programmed over USB; see [wiring-esp32c3-supermini.md](wiring-esp32c3-supermini.md) for the smaller build |
| 1 | **GoodDisplay GDEY0213Z98** 2.13" e-ink, 250×122, SPI | a 3-colour panel, which the firmware drives as monochrome — [why](eink-gdey0213z98.md) |
| 4 | **momentary push button** | one per keycap: ✕ ■ ✓ ↵ |
| 1 | **2-axis analog joystick with click** | KY-023-style module |
| 1 | **USB data cable** | micro-USB for the DevKit V1 — a charge-only cable will not enumerate |
| — | **jumper wires**, breadboard or perfboard | 15 connections in total |

The panel is normally sold with the FPC adapter it needs; buy them together if
the listing separates them.

### Keycap lights — optional, and the box is fully usable without them

| Qty | Part | Notes |
| --- | ---- | ----- |
| 4 | **WS2812B** | bare 5050 parts, or breakout boards |
| 1 | **74AHCT125** level shifter | **AHCT or HCT only** — a plain 74HC125 has the same threshold as the LEDs and fixes nothing |
| 1 | **330 Ω** resistor | in the data line, close to the first LED |
| 1 | **470 µF** electrolytic capacitor | anything from 100 µF, across 5 V and GND at the start of the chain |
| 4 | **100 nF** ceramic capacitor | only for bare 5050 parts; breakout boards already carry it |

No level shifter to hand? One silicon diode (1N4148, 1N400x) in the LEDs' 5 V
rail is the one-component substitute — details under
[Keycap lights](#keycap-lights-ws2812b).

### Small build — ESP32-C3 SuperMini instead of the DevKit

| Qty | Part | Notes |
| --- | ---- | ----- |
| 1 | **ESP32-C3 SuperMini** | replaces the DevKit; native USB, no UART bridge |
| 1 | **MCP23008** I²C I/O expander | DIP-18 or SOIC-18; the C3 has too few pins for a button each |
| 1 | **10 kΩ** resistor | pull-up on GPIO2, so flashing keeps working |

Wiring and bring-up for this one:
[`wiring-esp32c3-supermini.md`](wiring-esp32c3-supermini.md).

Set `kLedPin` to -1 in [`Config.h`](../esp32/include/Config.h) if your board has
no usable onboard LED, and `kKeyLightPin` to -1 to leave the lights out
entirely. Neither changes anything else.

---

## E-ink

```
EINK VCC  -> ESP32 3V3
EINK GND  -> ESP32 GND
EINK DIN  -> GPIO23
EINK CLK  -> GPIO18
EINK CS   -> GPIO5
EINK DC   -> GPIO17
EINK RST  -> GPIO16
EINK BUSY -> GPIO4
```

GPIO23 / GPIO18 are the classic ESP32's **VSPI** MOSI and SCK. `DIN` is
sometimes silkscreened `SDA` or `MOSI`, `CLK` as `SCL` or `SCK`, and `RST` as
`RES` — same signals.

E-ink is write-only over SPI, so MISO is unused (`SPI.begin(..., -1, ...)`).

## Joystick

```
JOYSTICK VCC       -> ESP32 3V3
JOYSTICK GND       -> ESP32 GND
JOYSTICK X  (VRX)  -> GPIO32
JOYSTICK Y  (VRY)  -> GPIO33
JOYSTICK SWITCH    -> GPIO25
other SWITCH leg   -> GND
```

GPIO32 and GPIO33 are on **ADC1**, chosen deliberately: ADC2 stops working once
Wi-Fi is enabled, and this project may gain Wi-Fi later.

GPIO25 uses `INPUT_PULLUP`, so the switch reads **LOW when pressed**.

A gamepad-style stick often breaks out more pins than this. Only three matter:
one ADC line per axis and one digital line for the click.

Which way each axis points is a firmware setting, not a wiring one: the module
here sits a quarter turn from its VRX/VRY labels, so `kJoystickSwapAxes` and both
`kJoystickInvertX/Y` are on in `Config.h`. If the directions come out rotated or
mirrored, see
[`docs/troubleshooting.md`](troubleshooting.md#updown-or-leftright-are-swapped).

## Buttons

Each button sits between its GPIO and GND, with `INPUT_PULLUP` on the pin —
**pressed = LOW**. No external resistors needed.

The order below is the order the **keycaps** read left to right, and the key
legend along the bottom of every screen draws them in that same order — so what
the screen offers is always what the finger above it does.

```
1st cap  ✕  cross     DENY       GPIO13 -> button -> GND
2nd cap  ■  square    STOP       GPIO14 -> button -> GND
3rd cap  ✓  check     APPROVE    GPIO27 -> button -> GND
4th cap  ↵  return    ACTION     GPIO26 -> button -> GND
```

> **If your caps sit in another order, change the four pins in
> `esp32/include/Config.h`** — nothing else in the firmware moves.
>
> Verify it before trusting the box with a verdict: on the Home screen press
> each key in turn and watch the daemon log for `[INPUT] deny`, `stop`,
> `approve`, `context`. APPROVE and DENY are inert unless a permission is on
> screen, so the check costs nothing.

GPIO 0, 2, 12 and 15 are avoided: they are boot/strapping pins, and holding one
at the wrong level during reset changes how the chip boots.

---

## Keycap lights (WS2812B)

One WS2812B under each of the four keycaps, all four in **one chain**. A key
lights up when, and only when, pressing it does something on the screen being
shown — the same rule as the key legend at the bottom of the panel, computed by
the same code, so the two never disagree. The colour says what a press will do:

| Colour | Means |
| ------ | ----- |
| red under ✕ | refuse the request on screen |
| green under ✓ | authorise it |
| amber under ■ | stop Claude's running turn |
| faint white | move around the box (open, back, select, next page) |
| dark | pressing does nothing here |
| brief white flash | the box registered your press |

At rest — one session, nothing running — all four are dark.

### At boot they all light

Power-up runs a short self-test: all four together, then ✕ red, ■ amber,
✓ green, ↵ white one at a time, then dark. It takes about two seconds and it is
the only thing that answers "are they wired?", since a chain nobody connected
looks exactly like one with nothing to say. The walk also says which LED sits
under which key: if the colours come out in the wrong order, the chain runs the
other way — flip `kKeyLightChain` in `Config.h` rather than moving wires. It
ships as `{3, 2, 1, 0}`, for a chain whose data line arrives at the ↵ end;
`{0, 1, 2, 3}` is the value for one fed from the ✕ end.
`kKeyLightTestStepMs = 0` turns the self-test off.

Nothing at all at boot, with `[lights] 4 keycap LEDs on GPIO…` in the log, is
wiring:

- **5 V on the LEDs.** A WS2812B wants 3.5 V at the very least and 5 V in
  practice; on 3.3 V most stay dark whatever you send them.
- **1OE of the 74AHCT125 to GND.** Left floating, the buffer drives nothing.
- **Data into DIN of the first LED**, from the buffer's output, not from the
  ESP32 directly.
- **One ground**, shared by the LEDs, the buffer and the board.

### Wiring

```
DATA
  GPIO21 (DevKit)
    |
    v
  74AHCT125, powered from 5V
    pin 2   1A   <- GPIO21
    pin 3   1Y   -> 330 Ohm -> DIN of LED1
    pin 1   1OE  -> GND
    pin 7   GND  -> GND
    pin 14  VCC  -> 5V

CHAIN, in keycap order left to right
  DIN [LED1 under X] DOUT -> DIN [LED2 under #] DOUT -> DIN [LED3 under v] DOUT -> DIN [LED4 under <-]

POWER, 5V taken from the DevKit's VIN pin (USB 5V)
  5V   -> VDD of all four LEDs, VCC of the 74AHCT125 (pin 14),
          and the + of a 470 uF capacitor at LED1
  GND  -> GND of all four LEDs, the 74AHCT125 (pin 7),
          the capacitor's -, and the DevKit's GND
```

- **Chain in keycap order**, left to right: the ESP32 feeds LED1 under ✕, its
  `DOUT` feeds LED2 under ■, and so on to LED4 under ↵. Wire it this way round and
  `kKeyLightChain` in `Config.h` must be `{0, 1, 2, 3}`; it **ships as
  `{3, 2, 1, 0}`**, matching the board in use, whose chain is fed from the ↵ end.
  Either way the fix is that one constant, never moving wires.
- **5 V for the LEDs**, from the DevKit's VIN pin (USB 5 V). Their ground and the
  ESP32's are the same net.
- **The 74AHCT125 is not optional in spirit.** A WS2812B on 5 V wants at least
  3.5 V to read a logic high; the ESP32 gives 3.3 V. The '125 reads anything above
  2 V as high and drives a full 5 V out. It must be the **AHCT** (or HCT) family:
  a plain 74**HC**125 has the same 3.5 V threshold as the LEDs and fixes nothing.
  Without it the chain often works on short wires, but out of spec — the tell is
  the first LED flickering or showing the wrong colour.

- **No buffer on the board?** One silicon diode (1N4148, 1N400x) in series with
  the LEDs' 5 V rail is the one-component answer: it drops their supply to about
  4.3 V, which brings the logic threshold down to roughly 3.0 V, under the
  3.3 V the ESP32 drives. The LEDs lose a little brightness and nothing else.
  Straight 5 V with a 3.3 V data line is the case that looks dead: the chain
  reads no high at all and stays dark.
- **Unused '125 gates:** tie their inputs (pins 5, 9, 12) to GND and their enables
  (pins 4, 10, 13) to 5 V.
- **330 Ω** in the data line, close to LED1: it damps ringing on the edge.
- **470 µF** (anything from 100 µF) across 5 V and GND at the start of the chain,
  and **100 nF** beside each LED if they are bare 5050 parts — WS2812B breakout
  boards already carry it.
- **GPIO21** is free on the DevKit and is not a strapping pin.

### Current

At the brightness set in `Config.h` (`kKeyLightLevel` 40 out of 255) the four
draw around 15 mA; a press flash on every key at once peaks near 120 mA for
120 ms. USB supplies that comfortably. The levels, colours and flash length are
all in the "Keycap lights" block of `Config.h`.

### First power-up

1. The boot log says `[lights] 4 keycap LEDs on GPIO21`.
2. On the Home screen at rest, all four are dark.
3. Press each key: it flashes white. A key that stays dark when pressed is a
   break in the chain at or before it — every LED after a bad joint goes dark too.
4. With a permission on screen: ✕ red, ■ amber, ✓ green.

## Complete diagram

```
                        ESP32 DevKit V1 (ESP32-WROOM-32)
                     ┌────────────────────────────────────┐
                     │                                    │
       ┌─────────────┤ 3V3                           GND ├──────────────┬────┐
       │             │                                    │             │    │
       │   ┌─────────┤ GPIO23  (VSPI MOSI)                │             │    │
       │   │ ┌───────┤ GPIO18  (VSPI SCK)                 │             │    │
       │   │ │ ┌─────┤ GPIO5   (CS)                       │             │    │
       │   │ │ │ ┌───┤ GPIO17  (DC)                       │             │    │
       │   │ │ │ │ ┌─┤ GPIO16  (RST)                      │             │    │
       │   │ │ │ │ │ ┌┤ GPIO4  (BUSY)                     │             │    │
       │   │ │ │ │ │ ││                                   │             │    │
       │   │ │ │ │ │ ││ GPIO32 (ADC1) ├───────────────────┼──── X       │    │
       │   │ │ │ │ │ ││ GPIO33 (ADC1) ├───────────────────┼──── Y       │    │
       │   │ │ │ │ │ ││ GPIO25        ├───────────────┐   │             │    │
       │   │ │ │ │ │ ││                                   │             │    │
       │   │ │ │ │ │ ││ GPIO26 ├──────────────────┐       │             │    │
       │   │ │ │ │ │ ││ GPIO27 ├────────────────┐ │       │             │    │
       │   │ │ │ │ │ ││ GPIO14 ├──────────────┐ │ │       │             │    │
       │   │ │ │ │ │ ││ GPIO13 ├────────────┐ │ │ │       │             │    │
       │   │ │ │ │ │ ││                     │ │ │ │       │             │    │
       │   │ │ │ │ │ ││ USB  <── to Mac     │ │ │ │       │             │    │
       │   │ │ │ │ │ │└─────────────────────┼─┼─┼─┼───────┘             │    │
       │   │ │ │ │ │ │                      │ │ │ │                     │    │
       │   │ │ │ │ │ │                      │ │ │ │                     │    │
       │  DIN│ │ │ │BUSY                 DENY│ │ │RET                   │    │
       │   │CLK│ │RST                       ││ │                        │    │
       │   │ │ CS DC                      STOP│APPR                     │    │
       │   │ │ │ │ │ │                      │ │ │ │                     │    │
  ┌────┴───┴─┴─┴─┴─┴─┴────┐        ┌────────┴─┴─┴─┴────────┐            │    │
  │  VCC DIN CLK CS DC    │        │  ┌───┐ ┌───┐ ┌───┐ ┌───┐           │    │
  │      RST BUSY  GND    │        │  │ 4 │ │ 3 │ │ 2 │ │ 1 │  buttons  │    │
  │                       │        │  └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘           │    │
  │   E-INK 250 x 122     │        │    └─────┴─────┴─────┴─────────────┴────┤
  │   (GDEY0213Z98)       │        │              all to GND                 │
  └───────────────────────┘        └─────────────────────────────────────────┘

       ┌──────────────────────────┐
       │  JOYSTICK                │
       │   VCC ──────── 3V3       │        1 = ✕ DENY     GPIO13
       │   GND ──────── GND ──────┼───┐    2 = ■ STOP     GPIO14
       │   VRX ──────── GPIO32    │   │    3 = ✓ APPROVE  GPIO27
       │   VRY ──────── GPIO33    │   │    4 = ↵ ACTION   GPIO26
       │   SW  ──────── GPIO25    │   │
       │   SW  ──────── GND ──────┼───┘    pressed = LOW (INPUT_PULLUP)
       └──────────────────────────┘
```

Ground is a single shared net: display GND, joystick GND, both joystick-switch
returns, all four button returns, and ESP32 GND.

---

## Quick continuity check before powering up

| Check | Expected |
| ----- | -------- |
| 3V3 to GND | no continuity (no short) |
| each button pin to GND, released | open |
| each button pin to GND, pressed | closed |
| joystick VRX/VRY to 3V3 and GND | ~half the pot resistance either way |
| display VCC | 3.3 V, never 5 V |

With power on and the firmware flashed, the boot log reports the measured
joystick centre:

```json
{"v":1,"type":"log","level":"info","message":"[input] joystick centre x=1893 y=2044, dead zone 700"}
```

A centre far from mid-scale (below ~1200 or above ~2900) means the stick was
held during boot, or that VCC is on 5 V — the firmware warns about this
explicitly.

---

## Pin map summary

| Function | GPIO | Mode | Notes |
| -------- | ---- | ---- | ----- |
| E-ink DIN/MOSI | 23 | output | VSPI MOSI |
| E-ink CLK/SCK | 18 | output | VSPI SCK |
| E-ink CS | 5 | output | |
| E-ink DC | 17 | output | |
| E-ink RST | 16 | output | active low |
| E-ink BUSY | 4 | input | driven by the panel |
| Joystick X | 32 | ADC1 | usable with Wi-Fi on |
| Joystick Y | 33 | ADC1 | usable with Wi-Fi on |
| Joystick click | 25 | INPUT_PULLUP | same as the return key |
| Button 1 ✕ DENY | 13 | INPUT_PULLUP | critical, dedicated |
| Button 2 ■ STOP | 14 | INPUT_PULLUP | interrupts the turn |
| Button 3 ✓ APPROVE | 27 | INPUT_PULLUP | critical, dedicated |
| Button 4 ↵ ACTION | 26 | INPUT_PULLUP | open / back / next page; hold = full redraw |
| Onboard LED | 2 | output (PWM) | off when idle; breathes while connecting; blinks while a permission waits |

## ESP32-S3 variant

This repo's display bring-up originally ran on an **ESP32-S3 DevKitC-1**, where
GPIO 23/18/5/17/16 are not all free. That board stays buildable with
`pio run -e esp32-s3-devkitc-1`, which selects the `CM_BOARD_ESP32S3` pin block
in `Config.h`:

| Function | GPIO (S3) |
| -------- | --------- |
| E-ink DIN | 11 |
| E-ink CLK | 12 |
| E-ink CS | 10 |
| E-ink DC | 6 |
| E-ink RST | 5 |
| E-ink BUSY | 4 |
| Joystick X / Y / click | 1 / 2 / 3 |
| DENY / STOP / ACTION / APPROVE | 15 / 16 / 17 / 18 |

The ESP32-WROOM-32 pinout at the top of this page is the reference; the S3 block
exists only so the older hardware still builds.
