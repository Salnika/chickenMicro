import { HOTSPOTS, type Hotspot } from '../hotspots.ts';

/** The photo is 3:2 and the overlay viewBox is 100 wide, so y scales by 2/3. */
const Y = 2 / 3;
const SVG_NS = 'http://www.w3.org/2000/svg';

export interface TourEls {
  frame: HTMLElement;
  spotlight: HTMLElement;
  word: HTMLElement;
  callHead: HTMLElement;
  intro: HTMLElement;
  callBody: HTMLElement;
}

/**
 * Every call-out is drawn at once and stays drawn: six arrows out to the
 * cutting mat, each with its name. Pointing at one brings up what that part
 * does, in the band under the board; moving away puts the masthead back.
 */
export function mountTour(els: TourEls): void {
  const svg = els.frame.querySelector<SVGSVGElement>('.arrows');
  const markers = els.frame.querySelector<HTMLElement>('.markers');
  if (!svg || !markers) throw new Error('tour: stage not found');

  svg.append(arrowhead());

  const lines: SVGLineElement[] = [];
  const dots: HTMLElement[] = [];
  const tags: HTMLElement[] = [];

  HOTSPOTS.forEach((h, i) => {
    const line = document.createElementNS(SVG_NS, 'line');
    line.setAttribute('x1', String(h.chip.x));
    line.setAttribute('y1', String(h.chip.y * Y));
    const end = pullBack(h);
    line.setAttribute('x2', String(end.x));
    line.setAttribute('y2', String(end.y * Y));
    line.setAttribute('marker-end', 'url(#head)');
    // Drawn in sequence on load, then left alone.
    line.style.animationDelay = `${0.15 + i * 0.07}s`;
    svg.append(line);
    lines.push(line);

    const dot = document.createElement('div');
    dot.className = 'marker';
    dot.style.left = `${h.dot.x}%`;
    dot.style.top = `${h.dot.y}%`;
    dot.style.animationDelay = `${0.15 + i * 0.07}s`;
    dot.tabIndex = 0;
    dot.setAttribute('role', 'button');
    dot.setAttribute('aria-label', `${h.label}: ${h.title}`);
    markers.append(dot);
    dots.push(dot);

    const tag = document.createElement('div');
    tag.className = 'tag-pin';
    tag.style.left = `${h.chip.x}%`;
    tag.style.top = `${h.chip.y}%`;
    tag.style.animationDelay = `${0.15 + i * 0.07}s`;
    tag.dataset['align'] = h.align;
    tag.textContent = h.glyph ? `${h.glyph} ${h.label}` : h.label;
    markers.append(tag);
    tags.push(tag);

    const enter = (): void => hover(i);
    const leave = (): void => hover(-1);
    for (const el of [dot, tag]) {
      el.addEventListener('mouseenter', enter);
      el.addEventListener('mouseleave', leave);
    }
    dot.addEventListener('focus', enter);
    dot.addEventListener('blur', leave);
  });

  let current = -2;

  const hover = (index: number): void => {
    if (index === current) return;
    current = index;

    const h = HOTSPOTS[index];
    document.body.classList.toggle('calling', h !== undefined);

    lines.forEach((l, i) => l.classList.toggle('hot', i === index));
    dots.forEach((d, i) => d.classList.toggle('hot', i === index));
    tags.forEach((t, i) => t.classList.toggle('hot', i === index));

    const showing = h !== undefined;
    els.word.hidden = showing;
    els.intro.hidden = showing;
    els.callHead.hidden = !showing;
    els.callBody.hidden = !showing;

    if (!h) return;

    // The spotlight is a hole, not a highlight: the part being described keeps
    // the photo's own brightness and everything around it takes the wash.
    els.spotlight.style.left = `${h.zone.x}%`;
    els.spotlight.style.top = `${h.zone.y}%`;
    els.spotlight.style.width = `${h.zone.w}%`;
    els.spotlight.style.height = `${h.zone.h}%`;

    els.callHead.replaceChildren(...head(h, index));
    els.callBody.replaceChildren(...body(h));
  };

  hover(-1);
}

/** Stop the arrow short of the marker so the head is not hidden under it. */
function pullBack(h: Hotspot): { x: number; y: number } {
  const dx = h.dot.x - h.chip.x;
  const dy = (h.dot.y - h.chip.y) * Y;
  const len = Math.hypot(dx, dy) || 1;
  const back = 3.2;
  return { x: h.dot.x - (dx / len) * back, y: h.dot.y - (dy / len) * (back / Y) };
}

function arrowhead(): SVGDefsElement {
  const defs = document.createElementNS(SVG_NS, 'defs');
  const marker = document.createElementNS(SVG_NS, 'marker');
  marker.setAttribute('id', 'head');
  marker.setAttribute('viewBox', '0 0 10 10');
  marker.setAttribute('refX', '9');
  marker.setAttribute('refY', '5');
  marker.setAttribute('markerWidth', '4');
  marker.setAttribute('markerHeight', '4');
  marker.setAttribute('orient', 'auto-start-reverse');
  const path = document.createElementNS(SVG_NS, 'path');
  path.setAttribute('d', 'M0,0 L10,5 L0,10 z');
  path.setAttribute('fill', 'currentColor');
  marker.append(path);
  defs.append(marker);
  return defs;
}

function head(h: Hotspot, index: number): Node[] {
  const num = document.createElement('span');
  num.className = 'call-num';
  num.textContent = String(index + 1).padStart(2, '0');

  const title = document.createElement('h1');
  title.className = 'word';
  if (h.glyph) {
    const g = document.createElement('span');
    g.className = 'call-glyph';
    g.textContent = h.glyph;
    title.append(g);
  }
  title.append(document.createTextNode(h.title));

  return [num, title];
}

function body(h: Hotspot): Node[] {
  const text = document.createElement('p');
  text.textContent = h.body;
  const out: Node[] = [text];

  if (h.pin) {
    const pin = document.createElement('span');
    pin.className = 'call-pin';
    pin.textContent = h.pin;
    out.push(pin);
  }
  return out;
}
