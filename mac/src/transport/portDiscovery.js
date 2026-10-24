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

/**
 * USB ids worth naming.
 *
 * The C3 and S3 have no UART bridge: the port is the ROM USB-Serial/JTAG
 * controller, whose descriptors live in silicon. That is why `pio device list`
 * calls every one of them "USB JTAG/serial debug unit" and no build flag can
 * change it — the C3 has no USB-OTG to put a software descriptor on. So the
 * name has to come from here instead.
 *
 * These ids say Espressif, not ChickenMicro: any C3 board looks the same. The
 * only proof is the `hello` frame the firmware sends on connect, which names
 * the app — `run` reports that, and this is the best a listing can do.
 */
const USB_IDS = [
  ['303a', '1001', 'Chicken Micro board — Espressif USB-Serial/JTAG (C3 / S3 / C6)', 120],
  ['303a', '0002', 'Espressif USB-OTG (ESP32-S2)', 110],
  ['303a', null, 'Espressif native USB', 110],
  ['10c4', 'ea60', 'Silicon Labs CP2102 bridge — typical ESP32 DevKit', 105],
  ['1a86', '7523', 'WCH CH340 bridge — typical ESP32 DevKit clone', 105],
  ['1a86', '55d4', 'WCH CH9102 bridge — typical ESP32 DevKit clone', 105],
  ['0403', null, 'FTDI bridge', 105],
];

/** `/dev/tty.usbmodem2101` and `/dev/cu.usbmodem2101` share this. */
export function deviceName(path) {
  const base = String(path).split('/').pop() ?? '';
  return base.replace(/^(tty|cu)\./, '');
}

/**
 * Name a port from its USB ids. Pure, so it is testable without a device.
 * Returns null when the ids say nothing useful.
 */
export function identify(vendorId, productId) {
  if (!vendorId) return null;
  const vid = String(vendorId).toLowerCase();
  const pid = productId ? String(productId).toLowerCase() : null;
  const exact = USB_IDS.find(([v, p]) => v === vid && p === pid);
  const family = USB_IDS.find(([v, p]) => v === vid && p === null);
  const match = exact ?? family;
  if (!match) return null;
  return { hint: match[2], score: match[3] };
}

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

/**
 * The same list, with each port named from its USB ids where the system will
 * say what they are. Falls back to the filename ranking when it will not —
 * `serialport` is a native module, so it is imported only when actually used
 * and a failure here must never cost us the listing.
 */
export async function describedPorts(directory = '/dev') {
  const ports = availablePorts(directory);
  let usb = [];
  try {
    const { SerialPort } = await import('serialport');
    usb = await SerialPort.list();
  } catch {
    return ports;
  }

  const named = ports.map((port) => {
    // serialport reports the callout device as /dev/tty.*, while everything
    // here works in /dev/cu.* — the same hardware under two nodes. Matching on
    // the full path therefore never hits; the device name is what they share.
    const info = usb.find((u) => deviceName(u.path) === deviceName(port.path));
    const id = info ? identify(info.vendorId, info.productId) : null;
    if (!id) return port;
    return { ...port, hint: id.hint, score: id.score };
  });

  return named.sort((a, b) => b.score - a.score || a.path.localeCompare(b.path));
}
