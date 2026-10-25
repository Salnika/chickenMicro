/**
 * Folds text down to what the device can actually draw.
 *
 * The panel's fonts are ASCII: Adafruit GFX's FreeSans and FreeMono cover
 * 0x20–0x7E and silently skip anything else, and its built-in 6x8 font renders
 * the raw bytes of a UTF-8 sequence as two pieces of CP437 line art. So on a
 * French desktop "Autoriser Claude à modifier" reached the box as
 * "Autoriser Claude ├á modifier" — the one thing on that screen that has to be
 * read exactly, made unreadable.
 *
 * Folding here rather than on the device is deliberate: the ESP32 has no
 * Unicode tables, and the Mac has to know the final text anyway to truncate it
 * to the frame's byte budget.
 */

/** Typography Electron produces that has a plain ASCII equivalent. */
const SUBSTITUTIONS = [
  [/[‘’‚‛′´`]/g, "'"],
  [/[“”„‟″«»]/g, '"'],
  [/[‐-―−]/g, '-'],
  [/…/g, '...'],
  [/[  -   　]/g, ' '],
  [/[​-‍⁠﻿]/g, ''],
  [/[•·●▪]/g, '-'],
  [/→/g, '->'],
  [/←/g, '<-'],
  [/×/g, 'x'],
  [/✓|✔/g, 'v'],
  [/✗|✘/g, 'x'],
  [/€/g, 'EUR'],
  [/©/g, '(c)'],
  [/®/g, '(r)'],
];

/** Letters that survive NFD with no ASCII form of their own. */
const LIGATURES = [
  [/œ/g, 'oe'], [/Œ/g, 'OE'],
  [/æ/g, 'ae'], [/Æ/g, 'AE'],
  [/ß/g, 'ss'],
  [/ø/g, 'o'], [/Ø/g, 'O'],
  [/ł/g, 'l'], [/Ł/g, 'L'],
  [/đ/g, 'd'], [/Đ/g, 'D'],
];

/**
 * Emoji, symbols and the joiners that hold them together.
 *
 * Dropped rather than replaced: a reply full of "?" reads worse than one with
 * the decoration removed.
 */
const PICTOGRAPHS = /[\p{Extended_Pictographic}\p{Emoji_Presentation}︎️\u{1F3FB}-\u{1F3FF}]/gu;

/**
 * ASCII the device can draw, with tabs turned into spaces and newlines kept —
 * the firmware treats `\n` as a hard break.
 *
 * Anything still unrepresentable becomes a single `?` per run, so text that
 * *was* there does not silently vanish.
 */
export function toDisplayText(text) {
  if (typeof text !== 'string' || text === '') return text;

  let out = text;
  for (const [pattern, replacement] of SUBSTITUTIONS) out = out.replace(pattern, replacement);
  for (const [pattern, replacement] of LIGATURES) out = out.replace(pattern, replacement);

  out = out
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')
    .normalize('NFC')
    .replace(PICTOGRAPHS, '')
    .replace(/\t/g, '  ')
    .replace(/\r\n?/g, '\n')
    // Every remaining control character, and everything above ASCII.
    .replace(/[^\n\x20-\x7E]+/g, '?');

  return out;
}

/**
 * One line of drawable text: for a name someone typed (a session's title, a
 * folder's), which must not carry a line break into the row of the panel it sits
 * in, nor into the daemon's log. A name that is nothing the panel can draw — an
 * emoji — comes out empty, for the caller to replace.
 */
export function toSingleLine(text) {
  if (typeof text !== 'string') return text;
  return toDisplayText(text).replace(/\s*\n+\s*/g, ' ').trim();
}
