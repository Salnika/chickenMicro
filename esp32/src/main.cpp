// ---------------------------------------------------------------------------
// ChickenMicro — physical remote for Claude Desktop
//
// Everything lives in App; this file only wires it to the Arduino entry points.
//
//   Wiring .................. docs/wiring.md
//   Pins and tunables ....... include/Config.h
//   E-ink model selection ... include/DisplayModel.h
//   Wire protocol ........... protocol/PROTOCOL.md
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "App.h"

namespace {
app::App application;
}

void setup() { application.setup(); }

void loop() { application.loop(); }
