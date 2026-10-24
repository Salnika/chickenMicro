#!/usr/bin/env bash
# Render every device screen to an image, with no hardware.
#
#   esp32/tools/preview/render.sh        -> esp32/tools/preview/out/screens.png
#
# Compiles the real layout code (Display.cpp, State.cpp, TextLayout.cpp, and the
# setup screen's QR encoder) against a stub driver and the Adafruit GFX and
# qrcodegen libraries PlatformIO already downloaded, so the picture matches the
# panel.

set -euo pipefail
cd "$(dirname "$0")/../.."          # esp32/

GFX=".pio/libdeps/esp32dev/Adafruit GFX Library"
QR=".pio/libdeps/esp32dev/qrcode generator/c"
if [ ! -d "$GFX" ] || [ ! -d "$QR" ]; then
  echo "Libraries not found. Run \`pio run -e esp32dev\` once to fetch them." >&2
  exit 1
fi

mkdir -p tools/preview/out

clang -std=c99 -O1 -c "$QR/qrcodegen.c" -o tools/preview/out/qrcodegen.o

clang++ -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -O1 -DARDUINO=10819 \
  -I include -I tools/preview/shim -I "$GFX" -I "$QR" \
  -o tools/preview/out/preview \
  tools/preview/preview.cpp \
  src/Display.cpp src/KeyLegend.cpp src/State.cpp src/TextLayout.cpp src/WifiQr.cpp \
  "$GFX/Adafruit_GFX.cpp" tools/preview/out/qrcodegen.o

rtk proxy "tools/preview/out/preview" 2>&1 || tools/preview/out/preview

sips -s format png tools/preview/out/screens.bmp \
     --out tools/preview/out/screens.png >/dev/null
rm -f tools/preview/out/screens.bmp
echo "-> esp32/tools/preview/out/screens.png"
