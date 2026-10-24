#pragma once

// ===========================================================================
//  E-INK PANEL SELECTION — THIS IS THE ONLY LINE TO CHANGE
// ===========================================================================
//
//      #define EINK_MODEL   EINK_MODEL_GDEY0213Z98_FAST
//                           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
//
// Nothing else in the firmware refers to the panel. Everything above this file
// talks to `DisplayDriver` (include/DisplayDriver.h), so switching panels is
// this one macro plus, if the geometry differs, the two size constants at the
// bottom of this file.
//
// ---------------------------------------------------------------------------
// AVAILABLE MODELS
// ---------------------------------------------------------------------------
//
//  EINK_MODEL_GDEY0213Z98_FAST   (default)
//      GoodDisplay GDEY0213Z98, 2.13" 122x250, SSD1680.
//      Driven as MONOCHROME through GxEPD2_213_BN. Partial refresh ~300 ms,
//      which is what makes the UI usable. No red.
//      This is the panel this repo was built around. Its datasheet is not
//      redistributed here; docs/reference/README.md says where to get it.
//
//  EINK_MODEL_GDEY0213Z98_TRI
//      The same panel driven as 3-COLOUR through GxEPD2_213_Z98c, so the
//      permission screen can use red. The cost is severe: the tri-colour
//      waveform lives in the panel's OTP and a full refresh takes ~14 s, with
//      no usable partial mode. Fine for a status display that changes rarely,
//      painful for scrolling. See docs/eink-gdey0213z98.md.
//
//  EINK_MODEL_MOCK
//      No panel at all. Renders into a memory canvas and reports what it drew
//      as protocol `log` frames. Use it to develop controls, protocol and
//      layout with nothing wired up, and to keep the build green on a board
//      whose display is not connected yet.
//
// ---------------------------------------------------------------------------
// USING A DIFFERENT PANEL
// ---------------------------------------------------------------------------
//
//  1. Find your panel's class in the GxEPD2 sources:
//         esp32/.pio/libdeps/<env>/GxEPD2/src/gdey/      (mono)
//         esp32/.pio/libdeps/<env>/GxEPD2/src/gdey3c/    (3-colour)
//     or in GxEPD2's GxEPD2_display_selection_new_style.h.
//  2. Add a model block below, following EINK_MODEL_GDEY0213Z98_FAST: set
//     EINK_PANEL_CLASS, EINK_PANEL_IS_3C, EINK_PANEL_OTP_IS_3C, and the
//     width/height.
//     EINK_PANEL_OTP_IS_3C matters when you drive 3-colour GLASS with a mono
//     driver: set it to 1 and the firmware avoids every full-update path,
//     because a full update loads the panel's tri-colour OTP waveform and
//     renders RAM 0x26 as red. Set it to 0 for genuinely monochrome glass.
//  3. Point EINK_MODEL at it.
//  4. Check the pins in include/Config.h against docs/wiring.md.
//
// A mono driver gives partial refresh and a responsive UI; a 3-colour driver
// gives red and full refreshes measured in seconds. For a control terminal the
// mono path is almost always the right trade.
// ===========================================================================

#define EINK_MODEL_GDEY0213Z98_FAST 1
#define EINK_MODEL_GDEY0213Z98_TRI 2
#define EINK_MODEL_MOCK 3

// >>> SELECT YOUR PANEL HERE <<<
#ifndef EINK_MODEL
#define EINK_MODEL EINK_MODEL_GDEY0213Z98_FAST
#endif

// ---------------------------------------------------------------------------
// Per-model definitions. Nothing below needs editing to switch models.
// ---------------------------------------------------------------------------

#if EINK_MODEL == EINK_MODEL_GDEY0213Z98_FAST

#define EINK_PANEL_CLASS GxEPD2_213_BN
#define EINK_PANEL_IS_3C 0
#define EINK_PANEL_SUPPORTS_PARTIAL 1
// The glass is physically 3-colour, so its OTP waveform is the tri-colour one.
// A mono FULL update would load that waveform, under which RAM 0x26 is the RED
// plane instead of "previous image" — the panel comes up solid red. So this
// model must never take a full-update path. See DisplayDriver.cpp.
#define EINK_PANEL_OTP_IS_3C 1
// Install a softened partial-refresh LUT that leaves unchanged pixels alone, so
// repeated partials cannot slowly drag red pigment up on this tri-colour glass.
// 1 = on (recommended for this panel); set to 0 to fall back to GxEPD2's stock
// LUT if a future GDEY revision misbehaves with it. See DisplayDriver.cpp.
#define EINK_PANEL_GENTLE_LUT 1
// The glass holds red pigment the mono waveform cannot move at all, so a deep
// clean has to borrow the tri-colour driver for one black/red/white cycle.
// Naming that class here is what lets the on-screen menu run the cycle in
// place, instead of reflashing the standalone cleaner tool. ~45 s.
#define EINK_PANEL_CAN_CLEAN 1
#define EINK_PANEL_CLEANER_CLASS GxEPD2_213_Z98c
#define EINK_MODEL_NAME "GDEY0213Z98 (mono, fast partial)"

#elif EINK_MODEL == EINK_MODEL_GDEY0213Z98_TRI

#define EINK_PANEL_CLASS GxEPD2_213_Z98c
#define EINK_PANEL_IS_3C 1
#define EINK_PANEL_SUPPORTS_PARTIAL 0
// Driving the tri-colour waveform on purpose here, so full updates are correct.
#define EINK_PANEL_OTP_IS_3C 0
#define EINK_MODEL_NAME "GDEY0213Z98 (3-colour, ~14 s full refresh)"

#elif EINK_MODEL == EINK_MODEL_MOCK

#define EINK_PANEL_IS_3C 0
#define EINK_PANEL_SUPPORTS_PARTIAL 1
#define EINK_PANEL_OTP_IS_3C 0
#define EINK_MODEL_NAME "mock (no panel)"

#else
#error "Unknown EINK_MODEL — see include/DisplayModel.h for the available values"
#endif

// Only the mono-on-3C-glass model needs the softened LUT; everything else is off.
#ifndef EINK_PANEL_GENTLE_LUT
#define EINK_PANEL_GENTLE_LUT 0
#endif

// Likewise the deep clean: it only means something when mono glass is hiding
// red pigment underneath. A genuinely monochrome panel has nothing to scrub.
#ifndef EINK_PANEL_CAN_CLEAN
#define EINK_PANEL_CAN_CLEAN 0
#endif

// ---------------------------------------------------------------------------
// Geometry in the orientation the UI uses (landscape).
// Change these two if your panel is not 250x122.
// ---------------------------------------------------------------------------
#ifndef EINK_WIDTH
#define EINK_WIDTH 250
#endif
#ifndef EINK_HEIGHT
#define EINK_HEIGHT 122
#endif

/// GxEPD2 rotation that yields EINK_WIDTH x EINK_HEIGHT. The GDEY0213Z98 is
/// natively 122x250 portrait, so rotation 1 gives the 250x122 landscape used
/// by every screen in Display.cpp.
#ifndef EINK_ROTATION
#define EINK_ROTATION 1
#endif
