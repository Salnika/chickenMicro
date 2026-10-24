/**
 * The build targets offered by the flasher.
 *
 * `env` matches a PlatformIO environment in esp32/platformio.ini, and the
 * firmware image is expected at `firmware/<env>.bin` — a single merged image
 * written at offset 0, produced by scripts/build-firmware.sh or by the Pages
 * workflow. Adding a board here means adding it to that script too.
 */
export interface Board {
  env: string;
  name: string;
  /** Chip family, as esptool reports it. Used to refuse a mismatched board. */
  chip: string;
  summary: string;
  /** Shown under the name when the board needs something unusual. */
  note?: string;
  /** Tools are real builds, but not the firmware — kept out of the main list. */
  kind: 'firmware' | 'tool';
}

export const BOARDS: Board[] = [
  {
    env: 'esp32dev',
    name: 'ESP32 DevKit V1',
    chip: 'ESP32',
    summary:
      'The reference build. ESP32-WROOM-32, buttons straight on GPIO, panel on the pins in docs/wiring.md.',
    kind: 'firmware',
  },
  {
    env: 'esp32-c3-supermini',
    name: 'ESP32-C3 SuperMini',
    chip: 'ESP32-C3',
    summary:
      'The small build. Buttons move onto an MCP23008 I²C expander, because the C3 has too few pins for one each.',
    note: 'Native USB — it shows up as /dev/cu.usbmodem*, not usbserial.',
    kind: 'firmware',
  },
  {
    env: 'esp32-s3-devkitc-1',
    name: 'ESP32-S3 DevKitC-1',
    chip: 'ESP32-S3',
    summary:
      'Secondary target, kept buildable since the display bring-up ran on an S3. Different pinout — see the CM_BOARD_ESP32S3 block in Config.h.',
    kind: 'firmware',
  },
  {
    env: 'esp32dev_mock',
    name: 'DevKit V1 — no panel',
    chip: 'ESP32',
    summary:
      'Renders into RAM and reports what it drew as log frames, so the controls and the protocol can be exercised before the panel arrives.',
    kind: 'tool',
  },
  {
    env: 'esp32dev_clear_panel',
    name: 'Panel cleaner (one-shot)',
    chip: 'ESP32',
    summary:
      'Not the firmware. Cycles the pigment black → red → white with the tri-colour waveform to scrub red left by an earlier session, then stops. Flash the firmware back afterwards.',
    note: 'Takes about 45 seconds on the glass. The firmware’s own menu runs the same cycle without reflashing.',
    kind: 'tool',
  },
];

export function boardByEnv(env: string): Board | undefined {
  return BOARDS.find((b) => b.env === env);
}

/** What scripts/build-firmware.sh writes beside the images. */
export interface FirmwareManifest {
  builtAt: string;
  commit: string;
  images: Record<string, { file: string; bytes: number; chip: string }>;
}
