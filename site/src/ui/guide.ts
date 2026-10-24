import { SCREENS, type KeyId, type Screen, type ScreenLine } from '../screens.ts';

/** Keycap order, left to right, matching the board and the on-screen legend. */
const KEYS: ReadonlyArray<{ id: KeyId; glyph: string; name: string }> = [
  { id: 'deny', glyph: '✕', name: 'DENY' },
  { id: 'stop', glyph: '■', name: 'STOP' },
  { id: 'approve', glyph: '✓', name: 'APPROVE' },
  { id: 'action', glyph: '↵', name: 'MORE' },
];

export interface GuideEls {
  host: HTMLElement;
  counter: HTMLElement;
  prev: HTMLButtonElement;
  next: HTMLButtonElement;
}

/**
 * One screen at a time, because the whole set has to live inside a panel on a
 * page that never scrolls.
 */
export function mountGuide(els: GuideEls): void {
  let i = 0;

  const show = (next: number): void => {
    i = Math.max(0, Math.min(next, SCREENS.length - 1));
    const screen = SCREENS[i];
    if (!screen) return;
    els.host.replaceChildren(card(screen));
    els.counter.textContent = `${String(i + 1).padStart(2, '0')} / ${String(SCREENS.length).padStart(2, '0')} · ${screen.name}`;
    els.prev.disabled = i === 0;
    els.next.disabled = i === SCREENS.length - 1;
  };

  els.prev.addEventListener('click', () => show(i - 1));
  els.next.addEventListener('click', () => show(i + 1));

  window.addEventListener('keydown', (e) => {
    if (document.body.dataset['mode'] !== 'screens') return;
    if (e.key === 'ArrowRight') show(i + 1);
    if (e.key === 'ArrowLeft') show(i - 1);
  });

  show(0);
}

function card(screen: Screen): HTMLElement {
  const el = document.createElement('article');
  el.className = 'screen-card';

  const head = document.createElement('div');
  head.className = 'screen-head';
  const h3 = document.createElement('h3');
  h3.textContent = screen.name;
  const tag = document.createElement('span');
  tag.className = 'tag';
  tag.textContent = liveCount(screen);
  head.append(h3, tag);

  const panel = renderPanel(screen);
  const keys = renderKeys(screen, panel);

  const caption = document.createElement('p');
  caption.textContent = screen.caption;

  el.append(head, panel, keys, caption);

  if (screen.note) {
    const note = document.createElement('p');
    note.className = 'screen-note';
    note.textContent = screen.note;
    el.append(note);
  }
  return el;
}

function liveCount(s: Screen): string {
  const n = Object.keys(s.legend).length;
  if (n === 0) return 'no keys';
  return n === 1 ? '1 key live' : `${n} keys live`;
}

function renderPanel(screen: Screen): HTMLElement {
  const panel = document.createElement('div');
  panel.className = 'panel-mock';
  panel.setAttribute('role', 'img');
  panel.setAttribute('aria-label', `The ${screen.name} screen`);

  if (screen.wordmark) {
    const mark = document.createElement('div');
    mark.className = 'p-wordmark';
    mark.textContent = 'CHICKEN MICRO';
    panel.append(mark);
  } else {
    const title = document.createElement('div');
    title.className = 'p-title';
    const left = document.createElement('span');
    left.textContent = 'CHICKEN MICRO';
    const right = document.createElement('span');
    right.textContent = screen.titleRight ?? '';
    title.append(left, right);
    panel.append(title);
  }

  const body = document.createElement('div');
  body.className = 'p-body';

  if (screen.loader) {
    const label = document.createElement('div');
    label.style.textAlign = 'center';
    label.textContent = 'Connecting';
    const loader = document.createElement('div');
    loader.className = 'loader';
    for (let i = 0; i < 5; i++) {
      const box = document.createElement('i');
      box.style.setProperty('--i', String(i));
      loader.append(box);
    }
    body.append(label, loader);
  }

  for (const line of screen.lines) body.append(renderLine(line));
  panel.append(body);

  const legend = document.createElement('div');
  legend.className = 'p-legend';
  for (const key of KEYS) {
    const slot = document.createElement('span');
    slot.dataset['key'] = key.id;
    const label = screen.legend[key.id];
    // A key with nothing to do is left blank, never filled with the next
    // action along — that is what keeps the legend aligned with the keycaps.
    slot.textContent = label ? `${key.glyph} ${label}` : '';
    legend.append(slot);
  }
  panel.append(legend);

  return panel;
}

function renderLine(line: ScreenLine): HTMLElement {
  if (line.right !== undefined) {
    const row = document.createElement('div');
    row.className = line.selected ? 'l-row sel' : 'l-row';
    const a = document.createElement('span');
    a.textContent = line.text;
    const b = document.createElement('span');
    b.textContent = line.right;
    row.append(a, b);
    return row;
  }

  const el = document.createElement('div');
  const classes: string[] = [];
  if (line.size === 'big') classes.push('l-big');
  if (line.size === 'small') classes.push('l-small');
  if (line.quote) classes.push('l-quote');
  if (classes.length) el.className = classes.join(' ');

  if (line.badge) {
    const badge = document.createElement('span');
    badge.className = 'badge';
    badge.textContent = line.badge;
    el.append(badge);
  }
  el.append(document.createTextNode(line.cursor ? `> ${line.text}` : line.text));
  return el;
}

/**
 * The keycaps under each screen. Hovering one lights its slot in the legend
 * above, which is the whole point: the legend and the lights come from the
 * same place in the firmware, so they cannot disagree.
 */
function renderKeys(screen: Screen, panel: HTMLElement): HTMLElement {
  const row = document.createElement('div');
  row.className = 'keyrow';

  for (const key of KEYS) {
    const label = screen.legend[key.id];
    const cap = document.createElement('div');
    cap.className = `keycap k-${key.id}${label ? ' live' : ''}`;

    const glyph = document.createElement('span');
    glyph.className = 'k-glyph';
    glyph.textContent = key.glyph;

    const name = document.createElement('span');
    name.textContent = label ?? '—';

    cap.append(glyph, name);
    cap.title = label
      ? `${key.name}: ${label} on this screen`
      : `${key.name} does nothing on this screen`;

    if (label) {
      const slot = panel.querySelector<HTMLElement>(`.p-legend [data-key="${key.id}"]`);
      cap.addEventListener('mouseenter', () => slot?.classList.add('lit'));
      cap.addEventListener('mouseleave', () => slot?.classList.remove('lit'));
    }
    row.append(cap);
  }
  return row;
}
