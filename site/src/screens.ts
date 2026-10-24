/**
 * The guided tour of the device's screens.
 *
 * These are rebuilt in HTML rather than cropped out of the preview harness's
 * contact sheet, so a legend key can light up the matching keycap on the photo
 * and the loader can actually step. The wording follows what the firmware
 * draws — esp32/tools/preview/render.sh produces the real thing to check
 * against.
 */

export type KeyId = 'deny' | 'stop' | 'approve' | 'action';

export interface ScreenLine {
  text: string;
  size?: 'big' | 'small';
  /** Inverted box, the way the firmware tags a permission kind. */
  badge?: string;
  /** The quoted last instruction, drawn with a leading bar. */
  quote?: boolean;
  /** Right-aligned column, for session rows. */
  right?: string;
  /** Boxed row: the one the cursor is on. */
  selected?: boolean;
  /** Leading '>' cursor, for menu entries. */
  cursor?: boolean;
}

export interface Screen {
  id: string;
  name: string;
  /** One line on why this screen exists, shown beside it. */
  caption: string;
  titleRight?: string;
  /** Replaces the title bar with the centred wordmark. */
  wordmark?: boolean;
  /** Draws the stepping Connecting loader. */
  loader?: boolean;
  lines: ScreenLine[];
  legend: Partial<Record<KeyId, string>>;
  /** Shown as a note under the screen when there is something to warn about. */
  note?: string;
}

export const SCREENS: Screen[] = [
  {
    id: 'connecting',
    name: 'Connecting',
    caption:
      'What the box shows on power-up, and the screen in the photo above. It owns the display — nothing navigates away from it — until the Mac daemon says hello.',
    wordmark: true,
    loader: true,
    lines: [{ text: 'start the Mac daemon: claude-remote run', size: 'small' }],
    legend: {},
    note: 'Still here after flashing? The firmware is fine — the daemon is not running, or something else is holding the serial port.',
  },
  {
    id: 'home',
    name: 'Home',
    caption:
      'The session being followed: what it is doing, and the last thing asked of it. STOP is the only key that does anything, so it is the only one drawn.',
    lines: [
      { text: 'chickenMicro' },
      { text: 'Writing...', size: 'big' },
      { text: 'Improve the device interface', quote: true },
    ],
    legend: { stop: 'STOP' },
  },
  {
    id: 'home-multi',
    name: 'Home, three sessions',
    caption:
      'With several Claude windows open, the corner counts them and the joystick steps between them. A second key appears, because there is now a list worth opening.',
    titleRight: '2/3',
    lines: [
      { text: 'table-header refactor' },
      { text: 'Thinking...', size: 'big' },
      { text: 'Split the sorting logic out', quote: true },
    ],
    legend: { stop: 'STOP', action: 'LIST' },
  },
  {
    id: 'sessions',
    name: 'Sessions',
    caption:
      'Every Claude Desktop window the daemon can see, with its state. The boxed row is the one the joystick is on; the dot marks the one being followed.',
    titleRight: '3/3',
    lines: [
      { text: 'chickenMicro', right: 'IDLE' },
      { text: '• table-header refactor', right: 'THINKING' },
      { text: 'docs', right: '! WAITING', selected: true },
    ],
    legend: { stop: 'STOP', action: 'OPEN' },
  },
  {
    id: 'message',
    name: 'Message',
    caption:
      'Claude’s last reply, paginated. Up and down page through it; a long answer is several screens rather than a truncated one.',
    titleRight: 'MESSAGE',
    lines: [
      { text: 'table-header refactor', size: 'small' },
      {
        text:
          'I’ll split the sorting logic out of Table.tsx into a hook so it can be tested on its own, then update the snapshot tests.',
        size: 'small',
      },
    ],
    legend: { stop: 'STOP', action: 'BACK' },
  },
  {
    id: 'permission',
    name: 'Permission',
    caption:
      'The priority screen: it takes over the display and cannot be navigated away from. The badge says what kind of thing is being authorised, and the command is shown in full — paged if it does not fit.',
    titleRight: '1/3',
    lines: [
      { text: 'Run command', badge: 'SHELL' },
      { text: 'git push --force-with-lease origin', size: 'big' },
    ],
    legend: { deny: 'DENY', stop: 'STOP', approve: 'ALLOW', action: 'MORE' },
    note: 'All four keys are live here. This is the only screen where APPROVE does anything at all.',
  },
  {
    id: 'queue',
    name: 'Permission, queued',
    caption:
      'Two windows asking at once. The corner says how many are waiting behind this one, and a flick right on the joystick approves the whole queue — deliberately not the ✓ key, so a slip still answers only the request on screen.',
    titleRight: 'PERMISSION',
    lines: [
      { text: 'Run command', badge: 'SHELL' },
      { text: 'git push origin master', size: 'big' },
    ],
    legend: { deny: 'DENY', stop: 'STOP', approve: 'ALLOW' },
    note: '“+2 (o)>ALL” in the header is the queue count and the approve-all gesture. It approves what you have not read, so reach for it only when you know what is waiting.',
  },
  {
    id: 'menu',
    name: 'Menu',
    caption:
      'Held joystick click, from any navigable screen. Everything device-side lives here: the one-time codes, the setup network, the panel cleaner and the refresh waveforms.',
    titleRight: 'MENU',
    lines: [
      { text: 'OTP codes', cursor: true },
      { text: 'Add OTP' },
      { text: 'Clean the screen (45 s)' },
      { text: 'Waveform: gentle' },
      { text: 'Resync with the Mac' },
    ],
    legend: { action: 'BACK', approve: 'SELECT' },
  },
  {
    id: 'otp',
    name: 'One-time code',
    caption:
      'The box doubles as a hardware authenticator. The secret never leaves it: a code exists only because a key was pressed, and the daemon puts it on the Mac’s clipboard, wiping it when the window closes.',
    titleRight: 'OTP',
    lines: [
      { text: 'GitHub:alice', size: 'small' },
      { text: '042 917', size: 'big' },
      { text: 'Copied to the Mac’s clipboard with 18 s left in its window', size: 'small' },
    ],
    legend: { deny: 'BACK', approve: 'AGAIN' },
  },
];
