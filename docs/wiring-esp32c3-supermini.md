# Wiring — ESP32-C3 SuperMini + MCP23008

The smallest build: an **ESP32-C3 SuperMini** (22 × 18 mm, USB-C) instead of the
DevKit, with the five buttons moved onto an **MCP23008** I²C I/O expander.

Build target: `pio run -e esp32-c3-supermini`. Every pin below is defined in
[`esp32/include/Config.h`](../esp32/include/Config.h), in the
`CM_BOARD_ESP32C3_SUPERMINI` branches and the `CM_INPUTS_ON_MCP23008` block.

> **3.3 V only.** Panel, joystick and MCP23008 all run from the SuperMini's
> `3V3` pin. The expander's I²C lines connect straight to the C3's pins, so
> powering it from 5 V would put 5 V on them.

> **All grounds common.** Panel, joystick, expander, every button's return leg
> and the SuperMini's `GND` are one net.

---

## Why an expander

The SuperMini exposes 13 GPIO. The panel takes 6, the joystick's two axes take
2 (they must be ADC pins, so they cannot move to the expander), and three of the
remaining pins are *strapping* pins that decide how the chip boots. Five more
buttons, each on its own pin, do not fit — and would have to sit on strapping
pins, which this project avoids for inputs.

The MCP23008 takes all five on two wires, with room for three more.

It costs nothing in responsiveness. The GPIO build catches each press with an
interrupt, so a tap during a blocking e-ink refresh is not lost. The MCP23008
does the same in hardware: it latches the first change on any pin until it is
read, and the firmware reads it every 3 ms, including all through a refresh. So
**its INT pin is not needed** and is left unconnected.

---

## Pin map

```
                     ESP32-C3 SuperMini
                   +---------------------+
  joystick VRX ----| GPIO0        GPIO4  |---- e-ink CLK
  joystick VRY ----| GPIO1        GPIO6  |---- e-ink DIN
  MCP23008 SDA ----| GPIO3        GPIO7  |---- e-ink CS
  MCP23008 SCL ----| GPIO5        GPIO10 |---- e-ink DC
                   |              GPIO20 |---- e-ink BUSY   (RX)
                   |              GPIO21 |---- e-ink RST    (TX)
                   |                     |
                   | 3V3 ------------------- panel, joystick, MCP23008 VDD
                   | GND ------------------- every ground
                   |                     |
                   | GPIO2         keycap lights data (10k pull-up)
                   | GPIO9         free (strapping; it is BOOT)
                   | GPIO8         onboard blue LED
                   +---------------------+
```

This is a connection diagram, not a picture of the board: the SuperMini's
physical pin order varies between clones.
Wire by the labels printed next to each pin.

| GPIO | Connected to | Why this pin |
| ---- | ------------ | ------------ |
| 0 | joystick X | ADC1 channel 0 |
| 1 | joystick Y | ADC1 channel 1 |
| 2 | keycap lights (WS2812B data, via 74AHCT125) | strapping pin, output only; 10 kΩ pull-up keeps flashing working — see Keycap lights |
| 3 | MCP23008 **SDA** | |
| 4 | e-ink **CLK** | the C3's default SPI clock |
| 5 | MCP23008 **SCL** | |
| 6 | e-ink **DIN** | the C3's default SPI MOSI |
| 7 | e-ink **CS** | the C3's default SPI CS |
| 8 | onboard blue LED | strapping pin, output only after boot — safe |
| 9 | — | strapping pin, wired to the BOOT button: left free |
| 10 | e-ink **DC** | |
| 20 | e-ink **BUSY** | UART RX: an input at boot as well |
| 21 | e-ink **RST** | UART TX: the ROM toggles it while printing its boot log, which only resets the panel — the firmware initialises it right after |

No input sits on a strapping pin (2, 8, 9), so nothing wired to the box can stop
it from booting. GPIO2 carries an output only — the keycap lights' data — and its
pull-up holds it at the level flashing needs.

## E-ink

Same GDEY0213Z98 module as the DevKit build, same driver.

```
EINK VCC  -> 3V3
EINK GND  -> GND
EINK DIN  -> GPIO6
EINK CLK  -> GPIO4
EINK CS   -> GPIO7
EINK DC   -> GPIO10
EINK RST  -> GPIO21
EINK BUSY -> GPIO20
```

## Joystick

```
JOYSTICK VCC -> 3V3        (not 5V: the axes would exceed the ADC's range)
JOYSTICK GND -> GND
JOYSTICK VRX -> GPIO0
JOYSTICK VRY -> GPIO1
JOYSTICK SW  -> MCP23008 GP5   (the click lives on the expander)
```

Leave the stick alone while the box boots: its resting centre is measured then,
never assumed. The C3's ADC reads a centred stick higher than the DevKit does,
which is why the centre is measured rather than fixed at mid-scale.

## MCP23008

Pin numbers are for the DIP-18 / SOIC-18 package. On a breakout board, go by
the silkscreen names.

```
                        MCP23008  (DIP-18 / SOIC-18)
                       +------------------+
  SuperMini GPIO5 -----| 1  SCL    VDD 18 |----- 3V3
  SuperMini GPIO3 -----| 2  SDA    GP7 17 |----- spare
              GND -----| 3  A2     GP6 16 |----- spare
              GND -----| 4  A1     GP5 15 |----- joystick SW  --+
              GND -----| 5  A0     GP4 14 |----- spare          |
              3V3 -----| 6  RESET  GP3 13 |----- [<] ACTION   --+
         no wire       | 7  NC     GP2 12 |----- [v] APPROVE  --+  other leg of
         no wire       | 8  INT    GP1 11 |----- [#] STOP     --+  every button
              GND -----| 9  VSS    GP0 10 |----- [x] DENY     --+  to GND
                       +------------------+
```

Three connections are easy to get wrong, and each makes the expander vanish:

- **RESET (pin 6) to 3V3.** It has no internal pull-up. Left floating, the chip
  resets at random.
- **A0, A1, A2 to GND.** That sets address `0x20`. Floating address pins make
  the address wander. To use another address, tie them differently and change
  `kExpanderAddress` in `Config.h`.
- **VDD to 3V3, not 5V** — see the top of this page.

The buttons need no resistors: the firmware turns on the MCP23008's own 100 kΩ
pull-ups, so each input reads high until its button pulls it to GND. The three
spare pins are pulled up too, so nothing on the chip floats.

### I²C pull-ups

Fit **4.7 kΩ from SDA to 3V3 and from SCL to 3V3**. The C3's internal pull-ups
(~45 kΩ) are enough at the default 100 kHz on short wires, which is why the
firmware defaults to 100 kHz — but real resistors make the bus robust, and let
you raise `kI2cClockHz` to 400 kHz. Many MCP23008 breakouts already have them;
check before adding a second pair.

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
  GPIO2 (SuperMini)
    |
    +---- 10 kOhm ---- 3V3        (strapping pin: see below)
    |
    v
  74AHCT125, powered from 5V
    pin 2   1A   <- GPIO2
    pin 3   1Y   -> 330 Ohm -> DIN of LED1
    pin 1   1OE  -> GND
    pin 7   GND  -> GND
    pin 14  VCC  -> 5V

CHAIN, in keycap order left to right
  DIN [LED1 under X] DOUT -> DIN [LED2 under #] DOUT -> DIN [LED3 under v] DOUT -> DIN [LED4 under <-]

POWER, 5V taken from the SuperMini's 5V pin (USB 5V)
  5V   -> VDD of all four LEDs, VCC of the 74AHCT125 (pin 14),
          and the + of a 470 uF capacitor at LED1
  GND  -> GND of all four LEDs, the 74AHCT125 (pin 7),
          the capacitor's -, and the SuperMini's GND
```

- **Chain in keycap order**, left to right: the ESP32 feeds LED1 under ✕, its
  `DOUT` feeds LED2 under ■, and so on to LED4 under ↵. Wire it this way round and
  `kKeyLightChain` in `Config.h` must be `{0, 1, 2, 3}`; it **ships as
  `{3, 2, 1, 0}`**, matching the board in use, whose chain is fed from the ↵ end.
  Either way the fix is that one constant, never moving wires.
- **5 V for the LEDs**, from the 5V pin (USB 5 V). Their ground and the
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
- **GPIO2, with a 10 kΩ pull-up to 3V3.** Every other pin is taken, and GPIO2 is
  a strapping pin — but only when the chip is put into download mode to be
  flashed, where it must read high. The pull-up guarantees that whatever the
  '125's input does; a normal boot ignores GPIO2 (`soc/boot_mode.h`: GPIO9 high
  means a flash boot, whatever the other straps read). The firmware only drives
  it once running, and writes the chain dark first thing.

### Current

At the brightness set in `Config.h` (`kKeyLightLevel` 40 out of 255) the four
draw around 15 mA; a press flash on every key at once peaks near 120 mA for
120 ms. USB supplies that comfortably. The levels, colours and flash length are
all in the "Keycap lights" block of `Config.h`.

### First power-up

1. The boot log says `[lights] 4 keycap LEDs on GPIO2`.
2. On the Home screen at rest, all four are dark.
3. Press each key: it flashes white. A key that stays dark when pressed is a
   break in the chain at or before it — every LED after a bad joint goes dark too.
4. With a permission on screen: ✕ red, ■ amber, ✓ green.

## Keys

The keycap order, left to right, is the same as on the DevKit build, and so is
the legend drawn along the bottom of every screen:

| Cap | Function | MCP23008 pin |
| --- | -------- | ------------ |
| ✕ cross | DENY | GP0 |
| ■ square | STOP | GP1 |
| ✓ check | APPROVE | GP2 |
| ↵ return | ACTION | GP3 |
| joystick click | select / hold for the menu | GP5 |

If your caps end up in another order, change the `kExpander…` pins in
`Config.h`; nothing else moves.

---

## Bring-up

1. **Flash.** Plug the SuperMini in over USB-C:

   ```bash
   cd esp32 && pio run -e esp32-c3-supermini --target upload
   ```

   If the upload cannot find or sync with the board, put it in download mode by
   hand: hold **BOOT**, tap **RESET**, release **BOOT**, and upload again.

2. **Read the boot log** (`pio device monitor -e esp32-c3-supermini`). The line
   that matters:

   ```
   [boot] keys on MCP23008 0x20: X=GP0 deny, #=GP1 stop, v=GP2 approve, <-=GP3 action, joystick click=GP5
   ```

   If it ends in `(NOT ANSWERING)`, or the log says `no MCP23008 at 0x20`, the
   expander is not on the bus: check RESET, A0–A2, VDD and the two bus wires.
   The firmware keeps looking for it once a second, so a fixed wire is picked up
   without a reboot.

3. **Press each key**, left to right, on the Home screen, with the daemon
   running. Its log must read `deny`, `stop`, `approve`, `context` in that
   order before you trust the box with a verdict.

## The Mac side

Nothing to configure. The SuperMini has no USB-UART bridge: its serial port is
the C3's native USB, which macOS lists as `/dev/cu.usbmodem…`, and the daemon
already looks for it there. With several USB-serial devices plugged in, pass
`--port /dev/cu.usbmodem…` to pick this one.

## When the expander misbehaves

The firmware only trusts a read of the MCP23008 if the transfer succeeded *and*
the chip still shows the configuration it was given. Anything else — a loose
wire, a brown-out that reset the chip to its defaults — is thrown away whole,
and the keys read as **released** until the chip answers properly again.

That matters because a pressed key reads 0: a bus returning zeros, or a reset
chip whose inputs float low, would otherwise look like every key down at once,
APPROVE included. The rules are tested on the host: `pio test -e native`.
