import { readdirSync } from 'node:fs';

/**
 * Prefix patterns ranked by how likely they are to be the DevKit bridge.
 *
 * The ESP32 DevKit V1 has no native USB: it exposes a UART bridge, which on
 * macOS appears as /dev/cu.usbserial-* (CP210x, FTDI) or /dev/cu.wchusbserial-*
 * (CH340/CH9102). /dev/cu.* is used rather than /dev/tty.* because opening a
 * tty. device blocks waiting for carrier detect, which a USB-UART bridge never
 * asserts.
 */
const RANKED_PREFIXES = [
  ['cu.usbserial', 'USB-UART bridge (CP210x / FTDI) — typical ESP32 DevKit', 100],
  ['cu.wchusbserial', 'USB-UART bridge (CH340 / CH9102) — typical ESP32 DevKit clone', 100],
  ['cu.SLAB_USBtoUART', 'Silicon Labs CP210x driver', 95],
  ['cu.usbmodem', 'native USB CDC (ESP32-S2/S3/C3) or other USB device', 60],
];

/** Devices that are never the ESP32 and only add noise. */
const IGNORED_SUFFIXES = ['Bluetooth-Incoming-Port', 'debug-console'];

/** Pure ranking step, separated so it is testable without a /dev. */
export function rank(deviceNames, directory = '/dev') {
  const ports = [];
  for (const name of deviceNames) {
    if (!name.startsWith('cu.')) continue;
    if (IGNORED_SUFFIXES.some((suffix) => name.endsWith(suffix))) continue;
    const match = RANKED_PREFIXES.find(([prefix]) => name.startsWith(prefix));
    if (!match) continue;
    ports.push({ path: `${directory}/${name}`, hint: match[1], score: match[2] });
  }
  // Stable order: score first, then path, so output does not jitter.
  return ports.sort((a, b) => (b.score - a.score) || a.path.localeCompare(b.path));
}

/** All plausible serial ports, best candidate first. */
export function availablePorts(directory = '/dev') {
  let names = [];
  try {
    names = readdirSync(directory);
  } catch {
    return [];
  }
  return rank(names, directory);
}

/** The port auto-detection would pick, or null. */
export function bestGuess(directory = '/dev') {
  return availablePorts(directory)[0] ?? null;
}
