/**
 * The annotated tour of the photo.
 *
 * Coordinates are percentages of public/board.jpg (1494x996, 3:2), which is
 * cropped so the board's centre is the image centre. `dot` is the marker,
 * `chip` the far end of the arrow out on the cutting mat, and `zone` the
 * rectangle the spotlight keeps lit while that part is being described.
 *
 * Replacing the photo means re-reading these, the board band percentages in
 * style.css, and `Y` in ui/tour.ts. Nothing else.
 */
export interface Hotspot {
  id: string;
  /** Short name on the tag at the far end of the arrow. */
  label: string;
  /** The glyph on the physical keycap, where there is one. */
  glyph?: string;
  title: string;
  body: string;
  /** Pin in Config.h, named so a rewired build can be corrected. */
  pin?: string;
  dot: { x: number; y: number };
  chip: { x: number; y: number };
  zone: { x: number; y: number; w: number; h: number };
  align: 'left' | 'right';
}

export const HOTSPOTS: Hotspot[] = [
  {
    id: 'panel',
    label: 'E-ink panel',
    title: 'The panel',
    body:
      'A 2.13" GoodDisplay GDEY0213Z98, 250×122, driven as monochrome for a ~300 ms partial refresh. It is showing the Connecting screen right here — the box has power, but no Mac daemon is talking to it yet.',
    dot: { x: 22.7, y: 31.1 },
    chip: { x: 5, y: 20 },
    zone: { x: 18, y: 21, w: 38, h: 29 },
    align: 'left',
  },
  {
    id: 'joystick',
    label: 'Joystick',
    title: 'The joystick',
    body:
      'Moves through the session list and pages a message; left and right swap screens. Held for about 0.6 s it opens the menu, where the one-time codes and the panel cleaner live.',
    pin: 'GPIO32 · GPIO33 · click GPIO25',
    dot: { x: 69.1, y: 35.4 },
    chip: { x: 92, y: 18 },
    zone: { x: 61.2, y: 22, w: 15.7, h: 27 },
    align: 'right',
  },
  {
    id: 'deny',
    label: 'Deny',
    glyph: '✕',
    title: 'Refuse it',
    body:
      'A dedicated key, acting on one deliberate press — never a hold, a repeat or a double click. It is never reused for navigation, so muscle memory built on a list cannot turn into a verdict.',
    pin: 'GPIO13',
    dot: { x: 29.3, y: 63 },
    chip: { x: 5, y: 69 },
    zone: { x: 21.7, y: 52.4, w: 15.1, h: 21.1 },
    align: 'left',
  },
  {
    id: 'stop',
    label: 'Stop',
    glyph: '■',
    title: 'Stop the turn',
    body:
      'Interrupts whatever Claude is doing in the session the screen is pointing at — not the whole app, and not the other windows.',
    pin: 'GPIO14',
    dot: { x: 42.8, y: 63 },
    chip: { x: 5, y: 48 },
    zone: { x: 35.4, y: 52.4, w: 14.8, h: 21.1 },
    align: 'left',
  },
  {
    id: 'approve',
    label: 'Approve',
    glyph: '✓',
    title: 'Authorise it',
    body:
      'Guarded on both sides independently: the verdict carries the request id, the daemon re-reads the Accessibility tree immediately before pressing, and an arming delay absorbs a press already in flight when the screen changed.',
    pin: 'GPIO27',
    dot: { x: 55.9, y: 63 },
    chip: { x: 94, y: 51 },
    zone: { x: 48.5, y: 52.4, w: 14.7, h: 21.1 },
    align: 'right',
  },
  {
    id: 'action',
    label: 'More',
    glyph: '↵',
    title: 'Open, back, next',
    body:
      'The navigation key: it opens the highlighted session, pages through a long message, and goes back. Held, it forces a full redraw of the panel.',
    pin: 'GPIO26',
    dot: { x: 69.2, y: 63 },
    chip: { x: 94, y: 71 },
    zone: { x: 61.9, y: 52.4, w: 14.7, h: 21.1 },
    align: 'right',
  },
];
