import { BOARDS, type Board, type FirmwareManifest } from '../boards.ts';
import { BoardDetector, type DetectionState } from '../serial.ts';
import { FlashError, flash, loadManifest } from '../flasher.ts';

const BASE = import.meta.env.BASE_URL;

interface Els {
  boards: HTMLElement;
  tools: HTMLElement;
  detector: HTMLElement;
  button: HTMLButtonElement;
  erase: HTMLInputElement;
  progress: HTMLElement;
  error: HTMLElement;
  logWrap: HTMLElement;
  log: HTMLElement;
  heroSupport: HTMLElement;
}

export function mountFlashPanel(els: Els): void {
  const detector = new BoardDetector();
  let manifest: FirmwareManifest | undefined;
  let selected: Board | undefined = BOARDS.find((b) => b.kind === 'firmware');
  let flashing = false;

  // ---------------------------------------------------------- board picker
  const renderBoards = (): void => {
    for (const host of [els.boards, els.tools]) host.replaceChildren();
    for (const board of BOARDS) {
      const host = board.kind === 'firmware' ? els.boards : els.tools;
      host.append(boardCard(board, () => board === selected, () => {
        selected = board;
        renderBoards();
        syncButton();
      }, manifest));
    }
  };

  const boardCard = (
    board: Board,
    isSelected: () => boolean,
    pick: () => void,
    mf: FirmwareManifest | undefined,
  ): HTMLElement => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'board';
    b.setAttribute('aria-pressed', String(isSelected()));

    const chip = document.createElement('span');
    chip.className = 'chipname';
    chip.textContent = board.chip;

    const name = document.createElement('strong');
    name.textContent = board.name;

    const summary = document.createElement('span');
    summary.textContent = board.summary;

    b.append(chip, name, summary);

    const image = mf?.images[board.env];
    if (board.note || !image) {
      const em = document.createElement('em');
      em.textContent = image
        ? (board.note ?? '')
        : 'No image published for this build yet.';
      if (em.textContent) b.append(em);
    }

    b.addEventListener('click', pick);
    return b;
  };

  // ------------------------------------------------------------- detector
  const renderDetector = (state: DetectionState): void => {
    els.detector.className = 'detector';
    const led = document.createElement('i');
    led.className = 'led';
    const text = document.createElement('div');
    text.className = 'detector-text';
    const title = document.createElement('strong');
    const sub = document.createElement('span');

    switch (state.kind) {
      case 'unsupported':
        els.detector.classList.add('is-unsupported');
        title.textContent = 'This browser cannot flash';
        sub.textContent = state.reason;
        break;
      case 'no-permission':
        title.textContent = 'No board chosen yet';
        sub.textContent =
          'The browser only shows a port you have picked once. Use the button below to open the chooser.';
        break;
      case 'detected':
        els.detector.classList.add('is-detected');
        title.textContent = `Board detected — ${state.bridge.label}`;
        sub.textContent = state.bridge.impliesChip
          ? `Looks like an ${state.bridge.impliesChip}. Ready to flash.`
          : 'Ready to flash.';
        break;
      case 'busy':
        els.detector.classList.add('is-busy');
        title.textContent = 'Port open';
        sub.textContent = 'Talking to the board — do not unplug it.';
        break;
      case 'unplugged':
        title.textContent = 'Board unplugged';
        sub.textContent = `The ${state.bridge.label} went away. Plug it back in and it reappears on its own.`;
        break;
    }

    text.append(title, sub);
    els.detector.replaceChildren(led, text);
  };

  // --------------------------------------------------------------- button
  const syncButton = (): void => {
    const state = detector.state;
    if (flashing) {
      els.button.disabled = true;
      els.button.textContent = 'Flashing…';
      return;
    }
    if (state.kind === 'unsupported') {
      els.button.disabled = true;
      els.button.textContent = 'Flashing unavailable';
      return;
    }
    if (state.kind === 'detected') {
      els.button.disabled = !selected;
      els.button.textContent = selected ? `Flash ${selected.name}` : 'Pick a build';
      return;
    }
    els.button.disabled = false;
    els.button.textContent = 'Choose the USB port…';
  };

  // ------------------------------------------------------------ progress
  const bar = els.progress.querySelector<HTMLElement>('.bar-track i');
  const msg = els.progress.querySelector<HTMLElement>('.progress-msg');

  const setProgress = (p: { fraction?: number; message: string; phase: string }): void => {
    els.progress.hidden = false;
    els.progress.classList.toggle('indeterminate', p.fraction === undefined && p.phase !== 'done');
    els.progress.classList.toggle('done', p.phase === 'done');
    if (bar && p.fraction !== undefined) bar.style.width = `${Math.round(p.fraction * 100)}%`;
    if (bar && p.phase === 'done') bar.style.width = '100%';
    if (msg) msg.textContent = p.message;
  };

  const appendLog = (line: string): void => {
    els.logWrap.hidden = false;
    els.log.textContent += line.endsWith('\n') ? line : `${line}\n`;
    els.log.scrollTop = els.log.scrollHeight;
  };

  const showError = (err: FlashError): void => {
    els.error.hidden = false;
    els.error.replaceChildren();
    const b = document.createElement('b');
    b.textContent = err.message;
    els.error.append(b);
    if (err.hint) els.error.append(document.createTextNode(err.hint));
  };

  // ----------------------------------------------------------------- wire
  els.button.addEventListener('click', async () => {
    const state = detector.state;

    if (state.kind !== 'detected') {
      await detector.requestPort();
      return;
    }
    if (!selected) return;

    flashing = true;
    detector.markBusy(true);
    syncButton();
    els.error.hidden = true;
    els.log.textContent = '';

    try {
      await flash({
        port: state.port,
        board: selected,
        imageUrl: `${BASE}firmware/${selected.env}.bin`,
        eraseAll: els.erase.checked,
        onProgress: setProgress,
        onLog: appendLog,
      });
    } catch (err) {
      setProgress({ phase: 'error', message: 'Stopped.', fraction: 0 });
      showError(err instanceof FlashError ? err : new FlashError(String(err)));
    } finally {
      flashing = false;
      detector.markBusy(false);
      syncButton();
    }
  });

  detector.subscribe((state) => {
    renderDetector(state);
    syncButton();
    if (state.kind === 'unsupported') {
      els.heroSupport.textContent = state.reason;
      els.heroSupport.classList.add('warn');
    } else {
      els.heroSupport.textContent =
        'Flashing works in Chrome, Edge and other Chromium browsers on desktop — the board talks to the page over USB.';
    }
  });

  renderBoards();
  void detector.start();
  void loadManifest(BASE).then((m) => {
    manifest = m;
    renderBoards();
  });
}
