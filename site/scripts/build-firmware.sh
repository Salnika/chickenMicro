#!/usr/bin/env bash
#
# Builds the firmware for every board the web flasher offers and merges each
# one into a single image written at offset 0, which is all esptool-js needs
# to push from a browser.
#
#   site/scripts/build-firmware.sh          # all boards
#   site/scripts/build-firmware.sh esp32dev # just one
#
# Output: site/public/firmware/<env>.bin plus manifest.json. Both are ignored
# by git — the Pages workflow runs this before `vite build`, and locally you
# run it once so `npm run dev` has something to flash.
#
# Needs: platformio, and esptool (pip install esptool).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
site="$(dirname "$here")"
repo="$(dirname "$site")"
esp32="$repo/esp32"
out="$site/public/firmware"

# env:chip. The chip decides where the bootloader sits, and esptool needs it
# spelled its own way. Keep in step with src/boards.ts.
BOARDS=(
  "esp32dev:esp32"
  "esp32-c3-supermini:esp32c3"
  "esp32-s3-devkitc-1:esp32s3"
  "esp32dev_mock:esp32"
  "esp32dev_clear_panel:esp32"
)

if ! command -v pio >/dev/null 2>&1; then
  echo "platformio not found. pip install platformio" >&2
  exit 1
fi

# esptool renamed its entry point between releases, and PlatformIO ships its
# own copy — which is the one most machines already have, so it is worth
# falling back to rather than asking for another install.
pio_core="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
if command -v esptool >/dev/null 2>&1; then
  ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL=(esptool.py)
elif [ -x "$pio_core/penv/bin/python3" ] && [ -f "$pio_core/packages/tool-esptoolpy/esptool.py" ]; then
  ESPTOOL=("$pio_core/penv/bin/python3" "$pio_core/packages/tool-esptoolpy/esptool.py")
else
  echo "esptool not found. pip install esptool, or build once so PlatformIO fetches it" >&2
  exit 1
fi

# Looked up after the first build, not before: on a clean machine the Arduino
# framework package does not exist until PlatformIO has fetched it, and `find`
# on a missing directory fails the whole script under `set -e` with its only
# explanation sent to /dev/null.
find_boot_app0() {
  find "${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages" \
    -name boot_app0.bin -path '*framework-arduinoespressif32*' 2>/dev/null | head -1 || true
}
boot_app0=""

# macOS still ships bash 3.2, which errors on "$@" under `set -u` when there
# are no arguments at all. CI runs bash 5 and would never have caught it, so
# the no-argument case must not expand "$@".
if [ "$#" -gt 0 ]; then
  wanted=("$@")
else
  wanted=()
  for entry in "${BOARDS[@]}"; do wanted+=("${entry%%:*}"); done
fi

mkdir -p "$out"
entries=()

for env in "${wanted[@]}"; do
  chip=""
  for entry in "${BOARDS[@]}"; do
    [ "${entry%%:*}" = "$env" ] && chip="${entry##*:}"
  done
  if [ -z "$chip" ]; then
    echo "unknown environment: $env" >&2
    exit 1
  fi

  echo "==> building $env ($chip)"
  pio run -d "$esp32" -e "$env"

  build="$esp32/.pio/build/$env"
  [ -f "$build/firmware.bin" ] || { echo "no firmware.bin for $env" >&2; exit 1; }

  # The ESP32 keeps its bootloader at 0x1000; the S3 and C3 put it at 0x0.
  case "$chip" in
    esp32) boot_offset="0x1000" ;;
    *) boot_offset="0x0" ;;
  esac

  parts=(
    "$boot_offset" "$build/bootloader.bin"
    "0x8000" "$build/partitions.bin"
    "0x10000" "$build/firmware.bin"
  )
  # boot_app0 initialises the OTA selector. Without it a board that has run
  # an OTA build before can come up pointing at a slot this image never wrote.
  [ -n "$boot_app0" ] || boot_app0="$(find_boot_app0)"
  if [ -n "$boot_app0" ] && [ -f "$boot_app0" ]; then
    parts+=("0xe000" "$boot_app0")
  else
    echo "    note: boot_app0.bin not found, skipping the OTA selector"
  fi

  "${ESPTOOL[@]}" --chip "$chip" merge_bin --format raw -o "$out/$env.bin" "${parts[@]}"

  bytes=$(wc -c <"$out/$env.bin" | tr -d ' ')
  echo "    $env.bin — $((bytes / 1024)) KiB"
  entries+=("\"$env\":{\"file\":\"$env.bin\",\"bytes\":$bytes,\"chip\":\"$chip\"}")
done

commit="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)"
built="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

{
  printf '{"builtAt":"%s","commit":"%s","images":{' "$built" "$commit"
  printf '%s' "$(IFS=,; echo "${entries[*]}")"
  printf '}}\n'
} >"$out/manifest.json"

echo "==> wrote $out/manifest.json"
