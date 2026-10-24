/**
 * Board detection on top of the Web Serial API.
 *
 * Web Serial has no passive enumeration: a page sees a port only after the
 * person has picked it once in the browser's chooser. After that the port is
 * remembered, `getPorts()` returns it, and connect/disconnect events fire as
 * the board is plugged and unplugged — which is what the indicator reflects.
 * So "no board" and "no permission yet" are genuinely different states, and
 * the UI says which.
 */

export type DetectionState =
  | { kind: 'unsupported'; reason: string }
  | { kind: 'no-permission' }
  | { kind: 'detected'; port: SerialPort; bridge: Bridge }
  | { kind: 'unplugged'; bridge: Bridge }
  | { kind: 'busy'; port: SerialPort; bridge: Bridge };

export interface Bridge {
  /** What the USB ids say the board is wired through. */
  label: string;
  /** The chip family this bridge implies, when it implies one at all. */
  impliesChip?: string;
  vendorId?: number;
  productId?: number;
}

/**
 * USB-UART bridges seen on ESP32 boards, plus Espressif's own native-USB
 * vendor id. Anything else still works — it is only the label that falls back.
 */
const BRIDGES: ReadonlyArray<{
  vid: number;
  pid?: number;
  label: string;
  impliesChip?: string;
}> = [
  { vid: 0x10c4, pid: 0xea60, label: 'Silicon Labs CP2102' },
  { vid: 0x10c4, pid: 0xea70, label: 'Silicon Labs CP2105' },
  { vid: 0x1a86, pid: 0x7523, label: 'WCH CH340' },
  { vid: 0x1a86, pid: 0x55d4, label: 'WCH CH9102' },
  { vid: 0x0403, label: 'FTDI' },
  // Espressif's own vendor id: the S3 and C3 expose USB directly, with no
  // bridge chip in between. 1001 is the ROM USB-Serial/JTAG peripheral and is
  // shared by the S3, C3, C6 and H2 alike, so it names no chip — only the
  // chip esptool reports once connected can do that.
  { vid: 0x303a, pid: 0x1001, label: 'Chicken Micro board' },
  { vid: 0x303a, pid: 0x0002, label: 'Espressif USB-OTG', impliesChip: 'ESP32-S2' },
  { vid: 0x303a, label: 'Espressif native USB' },
];

function describe(port: SerialPort): Bridge {
  const info = port.getInfo();
  const vid = info.usbVendorId;
  const pid = info.usbProductId;
  if (vid === undefined) return { label: 'Serial port' };

  const exact = BRIDGES.find((b) => b.vid === vid && b.pid === pid);
  const family = BRIDGES.find((b) => b.vid === vid && b.pid === undefined);
  const match = exact ?? family;

  const bridge: Bridge = {
    label: match?.label ?? `USB ${hex(vid)}:${hex(pid ?? 0)}`,
    vendorId: vid,
  };
  if (pid !== undefined) bridge.productId = pid;
  if (match?.impliesChip !== undefined) bridge.impliesChip = match.impliesChip;
  return bridge;
}

function hex(n: number): string {
  return n.toString(16).padStart(4, '0');
}

type Listener = (state: DetectionState) => void;

export class BoardDetector {
  #state: DetectionState;
  #listeners = new Set<Listener>();
  /** Remembered so an unplug can still say what went away. */
  #lastBridge: Bridge | undefined;

  constructor() {
    this.#state = supportCheck() ?? { kind: 'no-permission' };
  }

  get state(): DetectionState {
    return this.#state;
  }

  get supported(): boolean {
    return this.#state.kind !== 'unsupported';
  }

  subscribe(fn: Listener): () => void {
    this.#listeners.add(fn);
    fn(this.#state);
    return () => this.#listeners.delete(fn);
  }

  /** Begins watching. Safe to call on an unsupported browser — it no-ops. */
  async start(): Promise<void> {
    if (!this.supported) return;
    navigator.serial.addEventListener('connect', () => void this.refresh());
    navigator.serial.addEventListener('disconnect', () => void this.refresh());
    await this.refresh();
  }

  /** Opens the browser's port chooser. Must be called from a user gesture. */
  async requestPort(): Promise<boolean> {
    if (!this.supported) return false;
    try {
      await navigator.serial.requestPort();
      await this.refresh();
      return true;
    } catch {
      // The person dismissed the chooser, which is not an error worth showing.
      return false;
    }
  }

  /** Marks the detected port as in use, so the indicator stops inviting a flash. */
  markBusy(busy: boolean): void {
    const s = this.#state;
    if (busy && s.kind === 'detected') {
      this.#set({ kind: 'busy', port: s.port, bridge: s.bridge });
    } else if (!busy && s.kind === 'busy') {
      this.#set({ kind: 'detected', port: s.port, bridge: s.bridge });
    }
  }

  async refresh(): Promise<void> {
    if (!this.supported) return;
    // A port the person granted earlier but that is not plugged in right now
    // is still listed by some builds, so opening is the only sure test — and
    // opening it here would fight the flasher. Presence in getPorts() plus a
    // readable info block is the signal the API actually offers.
    const ports = await navigator.serial.getPorts();
    const port = ports.find((p) => p.getInfo().usbVendorId !== undefined) ?? ports[0];

    if (!port) {
      this.#set(this.#lastBridge ? { kind: 'unplugged', bridge: this.#lastBridge } : { kind: 'no-permission' });
      return;
    }
    const bridge = describe(port);
    this.#lastBridge = bridge;
    if (this.#state.kind === 'busy') {
      this.#set({ kind: 'busy', port, bridge });
    } else {
      this.#set({ kind: 'detected', port, bridge });
    }
  }

  #set(next: DetectionState): void {
    this.#state = next;
    for (const fn of this.#listeners) fn(next);
  }
}

function supportCheck(): DetectionState | undefined {
  if (typeof navigator === 'undefined' || !('serial' in navigator)) {
    return {
      kind: 'unsupported',
      reason:
        'This browser has no Web Serial API. Chrome, Edge or any Chromium browser on desktop can flash from here; Firefox and Safari cannot.',
    };
  }
  if (!window.isSecureContext) {
    return {
      kind: 'unsupported',
      reason: 'Web Serial needs a secure context. Open this page over HTTPS.',
    };
  }
  return undefined;
}
