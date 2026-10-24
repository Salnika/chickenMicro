// ---------------------------------------------------------------------------
// Direct AXUIElement access from Node, via koffi FFI.
//
// There is no pure-JavaScript way to read another app's UI on macOS. The three
// options are a native addon (C++ you compile), JXA/System Events (an Apple
// Event round trip per attribute, and the permission attaches to `osascript`
// rather than to this tool), or calling the C API directly — which is what this
// does. koffi ships prebuilt binaries, so nothing here needs a compiler.
//
// The one thing the C API gives that this cannot use is AXObserver: its
// callbacks are delivered on a CFRunLoop, and Node's loop is libuv. See
// watcher.js for what is done instead.
//
// Memory: every Copy* call returns a +1 reference that must be released.
// `withReleasePool` makes that mechanical rather than a thing to remember.
// ---------------------------------------------------------------------------

import koffi from 'koffi';

const CORE_FOUNDATION =
  '/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation';
const APPLICATION_SERVICES =
  '/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices';

const CF = koffi.load(CORE_FOUNDATION);
const AX = koffi.load(APPLICATION_SERVICES);

const kCFStringEncodingUTF8 = 0x08000100;

// --- CoreFoundation ---------------------------------------------------------

const CFStringCreateWithCString = CF.func(
  'void *CFStringCreateWithCString(void *alloc, const char *cStr, uint32_t encoding)');
const CFStringGetLength = CF.func('long CFStringGetLength(void *theString)');
const CFStringGetCString = CF.func(
  'bool CFStringGetCString(void *theString, _Out_ char *buffer, long bufferSize, uint32_t encoding)');
const CFGetTypeID = CF.func('unsigned long CFGetTypeID(void *cf)');
const CFStringGetTypeID = CF.func('unsigned long CFStringGetTypeID()');
const CFArrayGetTypeID = CF.func('unsigned long CFArrayGetTypeID()');
const CFBooleanGetTypeID = CF.func('unsigned long CFBooleanGetTypeID()');
const CFNumberGetTypeID = CF.func('unsigned long CFNumberGetTypeID()');
const CFArrayGetCount = CF.func('long CFArrayGetCount(void *theArray)');
const CFArrayGetValueAtIndex = CF.func('void *CFArrayGetValueAtIndex(void *theArray, long idx)');
const CFBooleanGetValue = CF.func('bool CFBooleanGetValue(void *boolean)');
const CFNumberGetValue = CF.func('bool CFNumberGetValue(void *number, int theType, _Out_ void *valuePtr)');
const CFHash = CF.func('unsigned long CFHash(void *cf)');
const CFRetain = CF.func('void *CFRetain(void *cf)');
const CFRelease = CF.func('void CFRelease(void *cf)');

const kCFNumberDoubleType = 13;

/**
 * CoreFoundation's `true` singleton.
 *
 * `symbol` returns the *address* of the exported variable, and decoding that
 * address reads the CFBooleanRef stored in it. Handing the address itself to a
 * CF function reads garbage as an object and takes the process down.
 */
const kCFBooleanTrue = koffi.decode(CF.symbol('kCFBooleanTrue', 'void *'), 'void *');

// --- Accessibility ----------------------------------------------------------

const AXIsProcessTrusted = AX.func('bool AXIsProcessTrusted()');
const AXUIElementCreateApplication = AX.func('void *AXUIElementCreateApplication(int32_t pid)');
const AXUIElementCopyAttributeValue = AX.func(
  'int AXUIElementCopyAttributeValue(void *element, void *attribute, _Out_ void **value)');
const AXUIElementCopyActionNames = AX.func(
  'int AXUIElementCopyActionNames(void *element, _Out_ void **names)');
const AXUIElementPerformAction = AX.func(
  'int AXUIElementPerformAction(void *element, void *action)');
const AXUIElementSetAttributeValue = AX.func(
  'int AXUIElementSetAttributeValue(void *element, void *attribute, void *value)');

/** AXError values worth naming. */
export const AXError = Object.freeze({
  success: 0,
  invalidUIElement: -25202,
  cannotComplete: -25204,
  attributeUnsupported: -25205,
  actionUnsupported: -25206,
  notImplemented: -25208,
  apiDisabled: -25211,
  noValue: -25212,
});

export function axErrorName(code) {
  const found = Object.entries(AXError).find(([, value]) => value === code);
  return found ? found[0] : `AXError ${code}`;
}

// --- CFString helpers -------------------------------------------------------

/**
 * Cache of attribute-name CFStrings.
 *
 * These are created once and never released: they live for the process, and
 * re-creating one per attribute per node would dominate the cost of a tree
 * walk.
 */
const attributeCache = new Map();

export function cfString(text) {
  let cached = attributeCache.get(text);
  if (cached === undefined) {
    cached = CFStringCreateWithCString(null, text, kCFStringEncodingUTF8);
    attributeCache.set(text, cached);
  }
  return cached;
}

/** Reads a CFStringRef into a JS string, or null when it is not a string. */
function readCFString(ref) {
  if (!ref) return null;
  const length = CFStringGetLength(ref);
  // Worst case 4 UTF-8 bytes per UTF-16 unit, plus the terminator.
  const buffer = Buffer.alloc(Number(length) * 4 + 1);
  if (!CFStringGetCString(ref, buffer, buffer.length, kCFStringEncodingUTF8)) return null;
  const end = buffer.indexOf(0);
  return buffer.toString('utf8', 0, end === -1 ? buffer.length : end);
}

// --- Release pool -----------------------------------------------------------

/**
 * Runs `body` with a pool that releases every CFTypeRef handed to it.
 *
 * Every AX `Copy*` call returns an owned reference. Tracking them in a pool
 * means a tree walk cannot leak just because an early return was added later.
 */
export function withReleasePool(body) {
  const owned = [];
  const pool = {
    own(ref) {
      if (ref) owned.push(ref);
      return ref;
    },
    /** Keeps `ref` alive beyond the pool, for elements stored for later pressing. */
    keep(ref) {
      if (!ref) return null;
      return CFRetain(ref);
    },
  };
  try {
    return body(pool);
  } finally {
    for (const ref of owned) {
      try { CFRelease(ref); } catch { /* already gone */ }
    }
  }
}

export function release(ref) {
  if (ref) CFRelease(ref);
}


// --- Typed attribute reads --------------------------------------------------

export function isTrusted() {
  return AXIsProcessTrusted();
}

/**
 * Whether an element still refers to something that exists.
 *
 * A window that has moved to another Space is perfectly alive — its AXRole
 * still comes back — so this is what tells "off-Space" apart from "closed". A
 * closed window, or a dead process, answers with `cannotComplete` /
 * `invalidUIElement` instead. Any successful read (even one with no value)
 * counts as alive: the point is that the element still answers.
 */
export function isElementValid(element) {
  if (!element) return false;
  const out = [null];
  const code = AXUIElementCopyAttributeValue(element, cfString('AXRole'), out);
  if (out[0]) CFRelease(out[0]);
  return code === AXError.success || code === AXError.noValue;
}

export function applicationElement(pid) {
  return AXUIElementCreateApplication(pid);
}

/** Raw attribute read. The returned ref is owned by the caller. */
function copyAttribute(element, attribute) {
  const out = [null];
  const code = AXUIElementCopyAttributeValue(element, cfString(attribute), out);
  return code === AXError.success ? out[0] : null;
}

/** Reads a string attribute, trimmed; null when absent or empty. */
export function stringAttribute(pool, element, attribute) {
  const ref = pool.own(copyAttribute(element, attribute));
  if (!ref) return null;
  if (CFGetTypeID(ref) !== CFStringGetTypeID()) return null;
  const text = readCFString(ref)?.trim();
  return text ? text : null;
}

/** Reads a boolean attribute; null when absent or not a boolean. */
export function boolAttribute(pool, element, attribute) {
  const ref = pool.own(copyAttribute(element, attribute));
  if (!ref) return null;
  return CFGetTypeID(ref) === CFBooleanGetTypeID() ? CFBooleanGetValue(ref) : null;
}

/**
 * Reads AXValue, which is `Any`: text for labels and fields, numbers for
 * sliders and progress indicators. Only representations that make sense on a
 * 250x122 display are kept.
 */
export function valueAttribute(pool, element) {
  const ref = pool.own(copyAttribute(element, 'AXValue'));
  if (!ref) return null;
  const typeId = CFGetTypeID(ref);
  if (typeId === CFStringGetTypeID()) {
    const text = readCFString(ref)?.trim();
    return text ? text : null;
  }
  if (typeId === CFNumberGetTypeID()) {
    const out = Buffer.alloc(8);
    if (!CFNumberGetValue(ref, kCFNumberDoubleType, out)) return null;
    const number = out.readDoubleLE(0);
    return Number.isInteger(number) ? String(number) : number.toFixed(2);
  }
  return null;
}

/**
 * Identity of a live element, stable for as long as it exists.
 *
 * AXUIElement is a CFType whose equality is the underlying UI element, and
 * CFHash is consistent with CFEqual — so this is what lets the daemon recognise
 * the same window across two reads of the tree. Nothing else can: Electron
 * exposes no identifier, window titles are all "Claude", and the order of
 * AXWindows follows z-order, so it changes the moment someone clicks.
 */
export function elementHash(element) {
  if (!element) return 0;
  return Number(CFHash(element)) >>> 0;
}

/**
 * The AXError a read of `attribute` returns, the value itself released.
 *
 * `childElements` answers an empty list both for an app with no window and for
 * one refusing to answer at all; this tells the two apart.
 */
export function attributeError(element, attribute) {
  const out = [null];
  const code = AXUIElementCopyAttributeValue(element, cfString(attribute), out);
  if (out[0]) CFRelease(out[0]);
  return code;
}

/** Child elements. Each returned ref is retained for the caller to release. */
export function childElements(pool, element, limit, attribute = 'AXChildren') {
  const array = pool.own(copyAttribute(element, attribute));
  if (!array || CFGetTypeID(array) !== CFArrayGetTypeID()) return [];
  const count = Math.min(Number(CFArrayGetCount(array)), limit);
  const children = [];
  for (let index = 0; index < count; index += 1) {
    // CFArrayGetValueAtIndex does not transfer ownership, so retain what we
    // keep past the pool's lifetime.
    const child = CFArrayGetValueAtIndex(array, index);
    if (child) children.push(CFRetain(child));
  }
  return children;
}

/** Action names the element supports, e.g. ["AXPress"]. */
export function actionNames(pool, element) {
  const out = [null];
  if (AXUIElementCopyActionNames(element, out) !== AXError.success) return [];
  const array = pool.own(out[0]);
  if (!array || CFGetTypeID(array) !== CFArrayGetTypeID()) return [];
  const count = Number(CFArrayGetCount(array));
  const names = [];
  for (let index = 0; index < count; index += 1) {
    const name = readCFString(CFArrayGetValueAtIndex(array, index));
    if (name) names.push(name);
  }
  return names;
}

/**
 * The application's windows.
 *
 * `AXWindows` rather than filtering `AXChildren`: it excludes the menu bar and
 * is the attribute AppKit keeps ordered. Each returned ref is retained for the
 * caller to release.
 */
export function windowElements(pool, application, limit) {
  const windows = childElements(pool, application, limit, 'AXWindows');
  if (windows.length > 0) return windows;
  // Some Electron builds answer AXWindows with nothing while the windows are
  // plainly in AXChildren; falling back costs one extra read and keeps the
  // daemon working through it.
  return childElements(pool, application, limit).filter((child) => {
    const role = withReleasePool((inner) => stringAttribute(inner, child, 'AXRole'));
    if (role === 'AXWindow') return true;
    release(child);
    return false;
  });
}

/**
 * Asks an Electron app to build the accessibility tree for its web content.
 *
 * Electron keeps that tree switched off until an assistive client asks, and it
 * does so per process. `AXManualAccessibility` is the attribute Electron
 * documents for tools that are not VoiceOver. Returns the AXError code.
 */
export function enableWebAccessibility(application) {
  return AXUIElementSetAttributeValue(
    application, cfString('AXManualAccessibility'), kCFBooleanTrue);
}

/** Performs AXPress. Returns the AXError code (0 on success). */
export function press(element) {
  return AXUIElementPerformAction(element, cfString('AXPress'));
}
