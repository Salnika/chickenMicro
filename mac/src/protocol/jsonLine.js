import { MAX_LINE_BYTES } from './constants.js';

/**
 * Bytes a character costs once JSON-escaped and UTF-8 encoded.
 *
 * A quote costs 2 escaped, a control character 6, and a non-ASCII character up
 * to 4 — so a character count is not a byte count, which is exactly the bug
 * this guards against.
 */
function escapedByteCount(codePoint) {
  switch (codePoint) {
    case 0x22: // "
    case 0x5c: // \
    case 0x0a: // \n
    case 0x0d: // \r
    case 0x09: // \t
      return 2;
    default: break;
  }
  if (codePoint < 0x20) return 6;      // \u00xx
  if (codePoint < 0x80) return 1;
  if (codePoint < 0x800) return 2;
  if (codePoint < 0x10000) return 3;
  return 4;
}

/** Takes as much of `text` as fits both limits, reporting whether all of it was consumed. */
function take(text, maxChars, byteBudget) {
  let out = '';
  let chars = 0;
  let bytes = 0;
  for (const character of text) {
    if (chars >= maxChars) return { prefix: out, complete: false };
    let cost = 0;
    for (const scalar of character) cost += escapedByteCount(scalar.codePointAt(0));
    if (bytes + cost > byteBudget) return { prefix: out, complete: false };
    out += character;
    chars += 1;
    bytes += cost;
  }
  return { prefix: out, complete: true };
}

/**
 * Truncates free text so that, once JSON-escaped, it still fits the frame's
 * remaining byte budget. A truncated value ends with `...`, and the marker is
 * counted against the limits so the result never exceeds what was budgeted.
 *
 * Three dots rather than an ellipsis character: the panel's font is ASCII, and
 * the three bytes of `…` reached it as three pieces of garbage at the end of
 * every clipped name. Same three bytes, readable this time.
 */
export function clipForWire(text, maxChars, byteBudget) {
  if (byteBudget <= 0 || maxChars <= 0) return '';

  const whole = take(text, maxChars, byteBudget);
  if (whole.complete) return whole.prefix;

  const marker = '...';
  const markerBytes = marker.length;
  if (byteBudget <= markerBytes || maxChars <= markerBytes) return '';
  return take(text, maxChars - markerBytes, byteBudget - markerBytes).prefix + marker;
}

/**
 * Serialises an object as one NDJSON line.
 *
 * `JSON.stringify` preserves insertion order for string keys and escapes
 * exactly like the firmware's encoder, so the wire bytes match what
 * esp32/src/Protocol.cpp produces.
 */
export function line(object) {
  return JSON.stringify(object) + '\n';
}

/**
 * Fits `text` into `object` under `key` while keeping the encoded line inside
 * MAX_LINE_BYTES, reserving `reserved` bytes for fields written afterwards.
 *
 * The budget is measured against the object as it stands rather than a guessed
 * envelope size.
 */
export function fit(object, key, text, maxChars, reserved = 0) {
  const probe = { ...object, [key]: '' };
  const overhead = Buffer.byteLength(line(probe), 'utf8') + reserved;
  return clipForWire(text, maxChars, MAX_LINE_BYTES - overhead);
}
