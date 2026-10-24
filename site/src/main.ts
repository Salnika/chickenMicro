import './style.css';
import { mountTour } from './ui/tour.ts';
import { mountFlashPanel } from './ui/flashPanel.ts';
import { mountGuide } from './ui/guide.ts';

type Mode = 'tour' | 'flash' | 'screens';

function need<T extends Element>(selector: string): T {
  const el = document.querySelector<T>(selector);
  if (!el) throw new Error(`missing element: ${selector}`);
  return el;
}

mountTour({
  frame: need<HTMLElement>('#frame'),
  spotlight: need<HTMLElement>('#spotlight'),
  word: need<HTMLElement>('#word'),
  callHead: need<HTMLElement>('#call-head'),
  intro: need<HTMLElement>('#intro'),
  callBody: need<HTMLElement>('#call-body'),
  list: need<HTMLElement>('#spot-list'),
});

mountGuide({
  host: need<HTMLElement>('#screen-host'),
  counter: need<HTMLElement>('#s-count'),
  prev: need<HTMLButtonElement>('#s-prev'),
  next: need<HTMLButtonElement>('#s-next'),
});

mountFlashPanel({
  boards: need<HTMLElement>('#boards'),
  tools: need<HTMLElement>('#boards-tools'),
  detector: need<HTMLElement>('#detector'),
  button: need<HTMLButtonElement>('#flash-btn'),
  erase: need<HTMLInputElement>('#erase-all'),
  progress: need<HTMLElement>('#progress'),
  error: need<HTMLElement>('#flash-error'),
  logWrap: need<HTMLElement>('#log-wrap'),
  log: need<HTMLElement>('#log'),
  heroSupport: need<HTMLElement>('#hero-support'),
});

/**
 * The whole page is one screen; `data-mode` on <body> is the only state. The
 * panel slides in for flash and screens, and the stage shifts so the board
 * stays centred in whatever width is left.
 */
const panel = need<HTMLElement>('#panel');

function setMode(mode: Mode): void {
  document.body.dataset['mode'] = mode;
  panel.setAttribute('aria-hidden', String(mode === 'tour'));
  for (const b of document.querySelectorAll<HTMLElement>('.tabs [data-go]')) {
    b.classList.toggle('on', b.dataset['go'] === mode);
  }
}

for (const el of document.querySelectorAll<HTMLElement>('[data-go]')) {
  el.addEventListener('click', (e) => {
    const go = el.dataset['go'];
    if (!go) return;
    e.preventDefault();
    setMode(go as Mode);
  });
}

window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') setMode('tour');
});

setMode('tour');
