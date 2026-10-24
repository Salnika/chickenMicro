#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Waveform.h"

// ---------------------------------------------------------------------------
// ChickenMicro firmware configuration — the ONLY place pins and tunables live.
//
// Wiring reference: docs/wiring.md
// Display model selection: include/DisplayModel.h  (separate on purpose)
// ---------------------------------------------------------------------------

// Board variant. `esp32dev` (ESP32-WROOM-32 DevKit V1) is the reference target
// and gets the pinout below. The ESP32-S3 block exists because this repo's
// earlier display bring-up ran on an S3 DevKitC-1; select it by building
// `[env:esp32-s3-devkitc-1]`, which defines CM_BOARD_ESP32S3. The ESP32-C3
// SuperMini is `[env:esp32-c3-supermini]`, which defines
// CM_BOARD_ESP32C3_SUPERMINI — wiring in docs/wiring-esp32c3-supermini.md.
#if defined(CM_BOARD_ESP32S3) && defined(CM_BOARD_ESP32C3_SUPERMINI)
#error "Select one board: CM_BOARD_ESP32S3 or CM_BOARD_ESP32C3_SUPERMINI, not both"
#endif
#if !defined(CM_BOARD_ESP32S3) && !defined(CM_BOARD_ESP32C3_SUPERMINI)
#define CM_BOARD_ESP32_WROOM32 1
#endif

// How the five buttons reach the MCU. The C3 SuperMini has too few pins to give
// each its own GPIO, so there they sit on an MCP23008 I2C expander instead —
// see include/ButtonBank.h for how both wirings behave the same.
#if defined(CM_BOARD_ESP32C3_SUPERMINI)
#define CM_INPUTS_ON_MCP23008 1
#else
#define CM_INPUTS_ON_MCP23008 0
#endif

namespace config {

// ---------------------------------------------------------------------------
// E-ink display — SPI (VSPI on the classic ESP32)
// ---------------------------------------------------------------------------
//   Display      ESP32 GPIO    Role
//   -----------------------------------------------------------------
//   VCC                3V3           !! 3.3 V ONLY — never 5 V on a 3.3 V panel
//   GND                GND
//   DIN / MOSI / SDA   23            SPI data
//   CLK / SCK  / SCL   18            SPI clock
//   CS                 5             chip select
//   DC                 17            data / command
//   RST                16            reset (active low)
//   BUSY               4             busy (display output -> ESP32 input)
#if defined(CM_BOARD_ESP32_WROOM32)
constexpr int8_t kEpdMosi = 23;
constexpr int8_t kEpdSck = 18;
constexpr int8_t kEpdCs = 5;
constexpr int8_t kEpdDc = 17;
constexpr int8_t kEpdRst = 16;
constexpr int8_t kEpdBusy = 4;
#elif defined(CM_BOARD_ESP32C3_SUPERMINI)
// SCK/MOSI/CS are the C3's default SPI pins. BUSY is an input and sits on the
// UART RX pin, which is an input at boot too. RST sits on UART TX: the ROM boot
// log toggles it, which only resets the panel — initialised after boot anyway.
// No strapping pin (2, 8, 9) is used. docs/wiring-esp32c3-supermini.md.
constexpr int8_t kEpdSck = 4;
constexpr int8_t kEpdMosi = 6;
constexpr int8_t kEpdCs = 7;
constexpr int8_t kEpdDc = 10;
constexpr int8_t kEpdRst = 21;
constexpr int8_t kEpdBusy = 20;
#else
// ESP32-S3 DevKitC-1: 23/18/5/17/16 are not all free, so the S3 build keeps
// the pinout the display was originally brought up on.
constexpr int8_t kEpdMosi = 11;
constexpr int8_t kEpdSck = 12;
constexpr int8_t kEpdCs = 10;
constexpr int8_t kEpdDc = 6;
constexpr int8_t kEpdRst = 5;
constexpr int8_t kEpdBusy = 4;
#endif

// ---------------------------------------------------------------------------
// Joystick — 2 analog axes + integrated push button
// ---------------------------------------------------------------------------
//   VCC -> 3V3 (mandatory: on 5 V the axes would exceed the ADC's range)
//   GND -> GND
//   X   -> GPIO32   (ADC1)
//   Y   -> GPIO33   (ADC1)
//   SW  -> GPIO25, other leg to GND, INPUT_PULLUP
//
// GPIO32/33 are on ADC1 deliberately: ADC2 becomes unusable once Wi-Fi is
// enabled, and this project may gain Wi-Fi later.
#if defined(CM_BOARD_ESP32_WROOM32)
constexpr uint8_t kJoystickX = 32;
constexpr uint8_t kJoystickY = 33;
constexpr uint8_t kJoystickSwitch = 25;
#elif defined(CM_BOARD_ESP32C3_SUPERMINI)
// ADC1 channels 0 and 1. The click is on the expander (kExpanderJoystick).
constexpr uint8_t kJoystickX = 0;
constexpr uint8_t kJoystickY = 1;
#else
constexpr uint8_t kJoystickX = 1;
constexpr uint8_t kJoystickY = 2;
constexpr uint8_t kJoystickSwitch = 3;
#endif

// ---------------------------------------------------------------------------
// Buttons — each wired GPIO -> button -> GND, INPUT_PULLUP (pressed = LOW)
// ---------------------------------------------------------------------------
// Boot/strapping pins 0, 2, 12 and 15 are avoided on purpose.
//
// The order below is the order the caps read left to right on the box, and the
// key legend at the bottom of every screen draws them in that same order:
//
//     1st   X   cross     DENY      GPIO13   refuse the request on screen
//     2nd   #   square    STOP      GPIO14   interrupt the running turn
//     3rd   v   check     APPROVE   GPIO27   authorise the request on screen
//     4th   <-  return    ACTION    GPIO26   open / back / next page
//
// !! If your caps are in another order, change the four pins here — nothing
// else in the firmware needs to move. Verify it before trusting the box with a
// verdict: on the Home screen press each key in turn and watch the daemon log
// for `[INPUT] deny`, `stop`, `approve`, `context`. Approve and deny are inert
// unless a permission is on screen, so the check costs nothing.
#if defined(CM_BOARD_ESP32_WROOM32)
// Pins run 13, 14, 27, 26 left to right on this box: the first assignment had
// the whole row backwards, confirmed on the hardware.
constexpr uint8_t kButtonDeny = 13;      // 1st cap: X
constexpr uint8_t kButtonStop = 14;      // 2nd cap: square
constexpr uint8_t kButtonApprove = 27;   // 3rd cap: check
constexpr uint8_t kButtonAction = 26;    // 4th cap: return
#elif defined(CM_BOARD_ESP32S3)
constexpr uint8_t kButtonDeny = 15;
constexpr uint8_t kButtonStop = 16;
constexpr uint8_t kButtonAction = 17;
constexpr uint8_t kButtonApprove = 18;
#endif

#if CM_INPUTS_ON_MCP23008
// ---------------------------------------------------------------------------
// Buttons on an MCP23008 I2C expander — ESP32-C3 SuperMini
// ---------------------------------------------------------------------------
//   MCP23008   SuperMini
//   VDD        3V3      !! 3.3 V: the bus is wired straight to the C3's pins
//   VSS        GND
//   SDA        GPIO3    4.7 kΩ to 3V3 recommended (the C3's own are ~45 kΩ)
//   SCL        GPIO5    4.7 kΩ to 3V3 recommended
//   RESET      3V3      !! mandatory: it has no internal pull-up, and a
//                          floating RESET makes the chip vanish at random
//   A0 A1 A2   GND      address 0x20 — never leave them floating
//   INT        —        not needed: see src/ButtonBankExpander.cpp
//
// Each input: GPn -> button -> GND. The pull-ups are the chip's own (100 kΩ),
// so a pressed key reads 0. Keycap order, left to right, then the joystick:
//
//     GP0   X   cross     DENY
//     GP1   #   square    STOP
//     GP2   v   check     APPROVE
//     GP3   <-  return    ACTION
//     GP5       joystick click
//     GP4, GP6-7  spare (pulled up, unused)
//
// Same check as the GPIO boards before trusting the box with a verdict: press
// the keys left to right on the Home screen and watch for deny, stop, approve,
// context in the daemon log.
constexpr uint8_t kI2cSda = 3;
constexpr uint8_t kI2cScl = 5;
/// 100 kHz tolerates the C3's weak internal pull-ups on short wires. With
/// 4.7 kΩ fitted, 400 kHz works and shortens each read.
constexpr uint32_t kI2cClockHz = 100000;
/// Per transaction. Keeps a missing expander from stalling the loop.
constexpr uint16_t kI2cTimeoutMs = 10;
constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderDeny = 0;       // 1st cap: X
constexpr uint8_t kExpanderStop = 1;       // 2nd cap: square
constexpr uint8_t kExpanderApprove = 2;    // 3rd cap: check
constexpr uint8_t kExpanderAction = 3;     // 4th cap: return
constexpr uint8_t kExpanderJoystick = 5;   // joystick click
/// Read interval. The chip latches the first change between two reads, so this
/// only has to be shorter than the debounce window, not than a press.
constexpr uint32_t kExpanderPollMs = 3;
/// How often a missing expander is looked for again.
constexpr uint32_t kExpanderRetryMs = 1000;
#endif

// ---------------------------------------------------------------------------
// Onboard status LED
// ---------------------------------------------------------------------------
// Solid on while all is calm, blinking while a permission is waiting for a
// verdict — a glance across the room says whether the box needs you.
//
// GPIO2 is the blue LED on the ESP32-WROOM-32 DevKit V1. It is a strapping pin,
// but only its level *during reset* chooses the boot mode; driving it as an
// output afterwards to light an LED is standard and safe. Set kLedPin to -1 to
// leave the LED alone.
#if defined(CM_BOARD_ESP32_WROOM32)
constexpr int8_t kLedPin = 2;
constexpr bool kLedActiveHigh = true;   // HIGH lights the DevKit's blue LED
#elif defined(CM_BOARD_ESP32C3_SUPERMINI)
// The SuperMini's blue LED sits between GPIO8 and 3V3, so LOW lights it. GPIO8
// is a strapping pin, but only its level during reset counts, and the LED and
// its resistor hold it high then — the level it wants.
constexpr int8_t kLedPin = 8;
constexpr bool kLedActiveHigh = false;
#else
// The S3 DevKitC-1's onboard LED is an addressable RGB on GPIO48, which a plain
// digital write cannot drive — so the status LED is disabled on that board
// rather than pretending. Point kLedPin at a plain LED if you wire one up.
constexpr int8_t kLedPin = -1;
constexpr bool kLedActiveHigh = true;
#endif

// ---------------------------------------------------------------------------
// Keycap lights — four WS2812B, one under each key, in one chain
// ---------------------------------------------------------------------------
// A key lights up when, and only when, pressing it does something on the screen
// being shown — the rule the key legend along the bottom of the panel follows,
// computed by the same function (ui::keyLegendFor), so the two cannot disagree.
// The colour says what a press will do: red refuses, green authorises, amber
// stops Claude, faint white moves around the box. Every press also flashes its
// key white, so a press the box registered is one you can see.
//
// The LEDs run on 5 V and take their data through a 74AHCT125 level shifter.
// Wiring: docs/wiring.md, docs/wiring-esp32c3-supermini.md. -1 leaves them out.
#if defined(CM_BOARD_ESP32_WROOM32)
constexpr int8_t kKeyLightPin = 21;
#elif defined(CM_BOARD_ESP32C3_SUPERMINI)
// Every other pin is taken. GPIO2 is a strapping pin, but it only counts when
// the chip enters download mode to be flashed — where it must read high, which
// a 10 kΩ pull-up to 3V3 guarantees. A normal boot ignores it (soc/boot_mode.h:
// GPIO9 high means a flash boot, whatever the other straps read).
constexpr int8_t kKeyLightPin = 2;
#else
constexpr int8_t kKeyLightPin = -1;   // S3 DevKitC-1: not wired
#endif

/// Chain position of the LED under each keycap, in keycap order (cross, square,
/// check, return). Chained left to right from the ESP32 side: 0, 1, 2, 3. Chained
/// the other way round: 3, 2, 1, 0.
///
/// Reversed here: on the board in use the data line reaches the rightmost keycap
/// first, so the chain walks back towards the ESP32. Nothing else in the firmware
/// knows — `show()` in KeyLights.cpp is the only place this is applied, so the
/// legend, the press flash and the boot self-test all follow from this one line.
constexpr uint8_t kKeyLightChain[4] = {3, 2, 1, 0};

/// Colours at full scale, as R, G, B. The levels below set how bright they are.
constexpr uint8_t kKeyColourDeny[3] = {255, 0, 0};
constexpr uint8_t kKeyColourApprove[3] = {0, 255, 0};
constexpr uint8_t kKeyColourStop[3] = {255, 80, 0};
constexpr uint8_t kKeyColourNavigate[3] = {255, 255, 255};
constexpr uint8_t kKeyColourFlash[3] = {255, 255, 255};

/// Brightness out of 255. WS2812B are bright: 40 reads clearly through a keycap
/// without glaring, and keeps all four around 15 mA.
constexpr uint8_t kKeyLightLevel = 40;

/// Brightness of the self-test, out of 255.
///
/// Three times the running brightness: this one is read through a keycap by
/// someone who wants to know whether the chain works at all, often with a
/// marginal data line, so it has to be unmistakable rather than comfortable.
/// All four white at this level draw about 110 mA from the 5 V rail.
constexpr uint8_t kKeyLightTestLevel = 120;

/// How long each step of the keycap self-test holds, at boot.
///
/// The keys are dark whenever a press would do nothing, so a chain that was
/// never wired looks exactly like one with nothing to say. Two seconds at boot
/// tell the two apart, and show which LED sits under which key — which is what
/// kKeyLightChain has to match. 0 turns the self-test off.
constexpr uint32_t kKeyLightTestStepMs = 320;
/// Keys that only move around the box stay faint, so the colours that matter
/// stand out. 0 keeps them dark.
constexpr uint8_t kKeyLightNavigateLevel = 8;
/// The press flash. All four flashing at once peaks near 120 mA, for 120 ms.
constexpr uint8_t kKeyLightFlashLevel = 120;
constexpr uint32_t kKeyLightFlashMs = 120;

// ---------------------------------------------------------------------------
// Input timing
// ---------------------------------------------------------------------------

/// Contact bounce window. A level must hold this long to count as a change.
constexpr uint32_t kButtonDebounceMs = 25;

/// A press longer than this reports `held`, used for secondary functions only.
/// APPROVE and DENY never act on a hold.
constexpr uint32_t kButtonHoldMs = 700;

/// Hold the joystick click at least this long to open (or close) the on-screen
/// menu. Just under the button-hold threshold, so the menu feels prompt.
constexpr uint32_t kMenuHoldMs = 600;

/// Joystick: reads below this distance from the calibrated centre are ignored.
constexpr int kJoystickDeadZone = 700;

/// Samples averaged at boot to find the resting centre. The centre is measured,
/// never assumed to be 2048.
constexpr int kJoystickCalibrationSamples = 64;

/// Minimum gap between two joystick events, so one flick is one event.
constexpr uint32_t kJoystickRepeatDelayMs = 450;

/// Gap between repeats once the stick is held over.
constexpr uint32_t kJoystickRepeatRateMs = 180;

/// Axis orientation, in the order it is applied.
///
/// The module used here sits a quarter turn from the board's X/Y labels, so the
/// two channels are swapped onto the logical axes first: without this, pushing
/// up read as Right and pushing right read as Up.
constexpr bool kJoystickSwapAxes = true;

/// Then each logical axis is flipped, both of them for this module: once the
/// channels line up, right still read as Left and up still read as Down. Set
/// either back to false if you swap in a module wired the other way.
constexpr bool kJoystickInvertX = true;
constexpr bool kJoystickInvertY = true;

// ---------------------------------------------------------------------------
// Link and protocol
// ---------------------------------------------------------------------------

constexpr uint32_t kSerialBaud = 115200;

/// UART receive buffer. Must outlast a display refresh, which blocks the loop
/// and stops anything draining the port: the default 256 bytes holds only
/// ~22 ms at 115200, so frames arriving mid-refresh were lost.
constexpr size_t kSerialRxBufferBytes = 4096;

/// Native USB only (ESP32-C3, ESP32-S3): transmit ring, sized for one whole
/// frame (the protocol's 768-byte maximum) with room to spare, so a write never
/// has to wait for the host mid-frame.
constexpr size_t kSerialTxBufferBytes = 1024;

/// Native USB only: how long a write may wait for room before giving up. A
/// connected host drains 1 KB in well under this; with no host listening it
/// caps each stall, so the box stays responsive with no daemon attached.
constexpr uint32_t kSerialTxTimeoutMs = 20;

/// No inbound frame for this long and the device shows itself as disconnected.
constexpr uint32_t kLinkTimeoutMs = 10000;

/// Interval between device-originated pings.
constexpr uint32_t kPingIntervalMs = 4000;

/// Fallback arming delay before APPROVE/DENY count, overridden by the Mac's
/// `hello.approveDelayMs`. See protocol/PROTOCOL.md §5.
constexpr uint32_t kApproveArmingDelayMs = 200;

/// How long a verdict is assumed to have landed before the request comes back.
///
/// The screen leaves the request the moment a verdict goes out, so the press
/// feels immediate rather than waiting on the Mac's read-press-confirm round
/// trip. That trip takes a few hundred milliseconds; past this, silence means
/// the decision did not land, and a decision the operator believes they made
/// must not be the one that quietly went missing.
constexpr uint32_t kVerdictWaitMs = 3000;

// ---------------------------------------------------------------------------
// Display refresh policy (e-ink: redraw only when something changed)
// ---------------------------------------------------------------------------

/// Partial refreshes allowed before forcing a full one, to clear ghosting.
constexpr uint16_t kPartialsBeforeFullRefresh = 20;

/// "Add OTP" closes its Wi-Fi setup page on its own after this long without a
/// request, so an access point is never left open and forgotten.
constexpr uint32_t kOtpSetupTimeoutMs = 5ul * 60ul * 1000ul;

/// Mono refresh waveform loaded at boot. The menu's "Waveform" entry switches
/// between them at runtime so they can be compared on the glass; once one wins,
/// set it here. See include/Waveform.h for what each one does. Deep gives the
/// strongest black; Stock refreshes in half the time.
constexpr ui::Waveform kDefaultWaveform = ui::Waveform::Deep;

/// How often the "connecting" indicator steps. E-ink cannot animate, so this is
/// a slow tick rather than a spinner: one cell per step.
constexpr uint32_t kConnectingStepMs = 1200;

/// Half-period of the status-LED blink while a permission is waiting: on for
/// this long, then off for this long. Slow enough to read as "attention", not
/// as a fault.
constexpr uint32_t kLedBlinkIntervalMs = 400;

/// Full breathe cycle (dark -> bright -> dark) while waiting for the Mac. Long
/// and calm, like a sleep-indicator, so it reads as "connecting" not "alarm".
constexpr uint32_t kLedBreathePeriodMs = 3000;

/// How long it keeps stepping before settling into a still screen.
///
/// A box left plugged in with no daemon running would otherwise refresh the
/// panel every second forever. The screen still says what to do once it stops,
/// and any key press starts it again.
constexpr uint32_t kConnectingAnimateMs = 120000;

// --- The chicken in the title bar ------------------------------------------
//
// Every step is a panel refresh, so the walk is slow on purpose and bounded: the
// chicken paces while a session is working and for a while after anything
// happens, then sits down. It never moves on the permission screen.

/// Time between two steps. A partial refresh takes ~300 ms on this panel.
constexpr uint32_t kChickenStepMs = 1000;

/// Distance covered per step.
constexpr int16_t kChickenStepPx = 6;

/// How long it keeps walking after the last key press or screen change, when
/// no session is working.
constexpr uint32_t kChickenWalkMs = 60000;

/// Steps that count as one partial refresh toward kPartialsBeforeFullRefresh.
/// Counted in full, a walking chicken would flash the whole panel white every
/// 20 seconds; a step only moves a 12x12 sprite, so its ghosting builds slowly.
constexpr uint16_t kChickenStepsPerPartial = 10;

/// Ignore redraw requests that arrive faster than this. A partial refresh on
/// this panel takes ~300 ms and blocks the loop anyway, so this only needs to be
/// large enough to coalesce a burst of frames — not to pace the panel, which
/// paces itself. Kept low so a state change reaches the glass promptly.
constexpr uint32_t kMinRedrawIntervalMs = 120;

}  // namespace config
