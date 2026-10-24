#pragma once
// Minimal Arduino surface, only what Adafruit_GFX needs to compile on a Mac.
// This exists so the screens can be rendered to an image without a panel —
// see tools/preview/README.md. It is never part of a firmware build.

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WString.h"
#include "Print.h"

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const unsigned char*)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const unsigned short*)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const unsigned long*)(addr))
#endif

/// Only ever used as `const __FlashStringHelper*`, so incomplete is enough.
class __FlashStringHelper;

#define DEG_TO_RAD 0.017453292519943295
#define RAD_TO_DEG 57.29577951308232
#define radians(degrees) ((degrees) * DEG_TO_RAD)
#define degrees(radians) ((radians) * RAD_TO_DEG)

typedef bool boolean;
typedef uint8_t byte;

inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
