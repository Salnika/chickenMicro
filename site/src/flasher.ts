import { ESPLoader, Transport } from 'esptool-js';
import type { Board, FirmwareManifest } from './boards.ts';

export type Phase = 'idle' | 'connecting' | 'erasing' | 'writing' | 'done' | 'error';

export interface Progress {
  phase: Phase;
  /** 0..1 while writing, otherwise undefined. */
  fraction?: number;
  message: string;
}

export type ProgressSink = (p: Progress) => void;
export type LogSink = (line: string) => void;

export async function loadManifest(base: string): Promise<FirmwareManifest | undefined> {
  try {
    const res = await fetch(`${base}firmware/manifest.json`, { cache: 'no-cache' });
    if (!res.ok) return undefined;
    return (await res.json()) as FirmwareManifest;
  } catch {
    return undefined;
  }
}

export class FlashError extends Error {
  readonly hint: string | undefined;
  constructor(message: string, hint?: string) {
    super(message);
    this.name = 'FlashError';
    this.hint = hint;
  }
}

export interface FlashRequest {
  port: SerialPort;
  board: Board;
  imageUrl: string;
  eraseAll: boolean;
  onProgress: ProgressSink;
  onLog: LogSink;
}

export async function flash(req: FlashRequest): Promise<void> {
  const { port, board, imageUrl, eraseAll, onProgress, onLog } = req;

  onProgress({ phase: 'connecting', message: 'Fetching the firmware image…' });
  const image = await fetchImage(imageUrl);
  onLog(`image ${board.env}.bin, ${(image.length / 1024).toFixed(0)} KiB`);

  const transport = new Transport(port, false);
  const loader = new ESPLoader({
    transport,
    baudrate: 921600,
    romBaudrate: 115200,
    terminal: {
      clean() {},
      writeLine(data: string) {
        onLog(data);
      },
      write(data: string) {
        onLog(data);
      },
    },
  });

  try {
    onProgress({ phase: 'connecting', message: 'Putting the board into bootloader mode…' });
    const detected = await loader.main();
    onLog(`detected ${detected}`);

    const reported = loader.chip.CHIP_NAME;
    if (!chipMatches(reported, board.chip)) {
      throw new FlashError(
        `This board reports ${reported}, but ${board.name} needs ${board.chip}.`,
        'Pick the build that matches the board you plugged in, or select a different port.',
      );
    }

    if (eraseAll) {
      onProgress({ phase: 'erasing', message: 'Erasing the whole flash — this wipes stored OTP accounts…' });
      await loader.eraseFlash();
    }

    onProgress({ phase: 'writing', fraction: 0, message: 'Writing…' });
    await loader.writeFlash({
      // One merged image, written at offset 0 — build-firmware.sh has already
      // placed the bootloader, the partition table and the app inside it.
      fileArray: [{ data: image, address: 0 }],
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (_index: number, written: number, total: number) => {
        onProgress({
          phase: 'writing',
          fraction: total > 0 ? written / total : 0,
          message: `Writing ${(written / 1024).toFixed(0)} / ${(total / 1024).toFixed(0)} KiB…`,
        });
      },
    });

    await loader.after();
    onProgress({ phase: 'done', message: 'Done. The board has restarted.' });
  } catch (err) {
    if (err instanceof FlashError) throw err;
    throw new FlashError(messageOf(err), hintFor(err));
  } finally {
    try {
      await transport.disconnect();
    } catch {
      // The board resetting under us closes the port first often enough that
      // a failure here says nothing useful.
    }
  }
}

async function fetchImage(url: string): Promise<Uint8Array> {
  let res: Response;
  try {
    res = await fetch(url, { cache: 'no-cache' });
  } catch {
    throw new FlashError('Could not download the firmware image.', 'Check your connection and reload the page.');
  }
  if (!res.ok) {
    throw new FlashError(
      `The firmware image for this board is not published (HTTP ${res.status}).`,
      'The Pages workflow builds it from esp32/. If you are running the site locally, run `npm run firmware` first.',
    );
  }
  return new Uint8Array(await res.arrayBuffer());
}

/** esptool spells chips "ESP32-C3", the board table matches that spelling. */
function chipMatches(reported: string, expected: string): boolean {
  const norm = (s: string) => s.toUpperCase().replace(/[^A-Z0-9]/g, '');
  return norm(reported).startsWith(norm(expected));
}

function messageOf(err: unknown): string {
  if (err instanceof Error) return err.message;
  return String(err);
}

function hintFor(err: unknown): string | undefined {
  const text = messageOf(err).toLowerCase();
  if (text.includes('failed to connect') || text.includes('no serial data')) {
    return 'Hold BOOT, tap EN/RESET, release BOOT, then try again. A charge-only USB cable also looks exactly like this.';
  }
  if (text.includes('already open') || text.includes('busy') || text.includes('locked')) {
    return 'Something else is holding the port — quit `pio device monitor` or the claude-remote daemon and retry.';
  }
  if (text.includes('access denied') || text.includes('permission')) {
    return 'The browser could not open the port. Unplug the board, plug it back in, and pick it again.';
  }
  return undefined;
}
