/**
 * Every string used to recognise things inside Claude Desktop's Accessibility
 * tree, gathered in one place.
 *
 * Claude Desktop is an Electron app: its tree has no stable identifiers and
 * changes between releases. When a release breaks recognition, run
 * `claude-remote dump --filter button`, see what the labels actually are, and
 * edit the lists below — nothing else in the daemon needs to change.
 */
export const selectors = {
  /** Process names tried when locating the app. */
  processNames: ['Claude'],

  /** Bundle identifiers, matched against the executable path. */
  bundleIdentifiers: ['com.anthropic.claudefordesktop', 'com.anthropic.claude'],

  /**
   * Button labels that authorise the pending request, **most restrictive
   * first**.
   *
   * Order is load-bearing: when a dialog offers several, the interpreter binds
   * the earliest entry here rather than whichever happens to come first in the
   * tree, so "Autoriser une fois" always wins over a broader "Autoriser".
   */
  approveLabels: [
    'allow once', 'autoriser une fois',
    'allow', 'autoriser',
    'approve', 'accept', 'confirm', 'yes', 'continue', 'run',
    'accepter', 'confirmer', 'oui',
  ],

  /**
   * Labels that grant a *standing* permission rather than authorising this one
   * request. Never bound to any button.
   *
   * A box on a desk must not be able to hand over a whole category of actions
   * permanently: the operator reading one command on a 250x122 panel has not
   * consented to every future one. Observed in the wild as "Toujours autoriser"
   * sitting directly beside "Autoriser une fois".
   *
   * A dialog offering only these still shows on the device — with DENY
   * available and APPROVE simply absent.
   */
  standingPermissionLabels: [
    'allow always', 'always allow', 'toujours autoriser',
    'allow for this chat', 'autoriser pour ce chat',
    'always allow this', 'toujours autoriser ceci',
    // The title a later release gives that button, description included:
    // "Toujours autoriser , S’applique uniquement à cette session." Matching is
    // exact, so the whole string is needed — without it the dialog has no
    // allow-ish button at all and goes undetected.
    "toujours autoriser s'applique uniquement a cette session",
  ],

  /** Button labels that refuse the pending request. */
  denyLabels: [
    'deny', "don't allow", 'dont allow', 'do not allow',
    'reject', 'decline', 'no',
    'refuser', 'refuse', 'rejeter', 'non', 'ne pas autoriser',
  ],

  /**
   * Button labels that abort the current turn.
   *
   * Deliberately narrow. "Cancel" and its French "Annuler" used to be here and
   * had to go: they are generic verbs that appear on unrelated dialogs, and
   * "Annuler" is *Undo* in the Edit menu — STOP was binding to it and firing
   * Edit > Undo on a live conversation. Only unambiguous stop wording belongs
   * here.
   */
  stopLabels: [
    'stop', 'stop generating', 'stop generation', 'stop response',
    'arreter', 'arreter la generation',
  ],

  /** Static-text values that mark the start of an assistant turn. */
  assistantMarkers: ['claude', 'assistant'],

  /** Static-text values that mark the start of a user turn. */
  userMarkers: ['user', 'you', 'vous'],

  /**
   * Keyword to `kind` tag shown in the device's permission header, tried in
   * order. Shell markers come first because `curl https://...` is a shell
   * request even though it mentions a URL.
   */
  kindKeywords: [
    // Written without accents: the haystack is folded before matching, so
    // "ecriture" is what catches "Écriture".
    ['bash', 'shell'], ['shell', 'shell'], ['terminal', 'shell'],
    ['command', 'shell'], ['commande', 'shell'],
    ['git ', 'shell'], ['npm ', 'shell'], ['pnpm ', 'shell'], ['yarn ', 'shell'],
    ['cargo ', 'shell'], ['docker ', 'shell'], ['make ', 'shell'], ['sudo ', 'shell'],
    ['brew ', 'shell'], ['pip ', 'shell'], ['curl ', 'shell'], ['chmod ', 'shell'],
    ['python', 'shell'], ['node ', 'shell'], ['swift ', 'shell'], ['pio ', 'shell'],
    ['write', 'write'], ['create file', 'write'], ['edit', 'write'],
    ['modifier', 'write'], ['ecriture', 'write'], ['creer', 'write'],
    ['read', 'read'], ['lecture', 'read'], ['lire', 'read'],
    ['fetch', 'fetch'], ['http', 'fetch'], ['url', 'fetch'],
    ['search', 'search'], ['recherche', 'search'],
    ['executer', 'shell'], ['lancer', 'shell'],
  ],

  defaultKind: 'tool',

  /**
   * Window titles that identify the app rather than the session in it.
   *
   * Claude Desktop titles every window "Claude", so the title alone cannot tell
   * two windows apart — the session name comes from the web area's document
   * title instead. The same list strips the app's name off the end of that
   * title, which reads "Migration React 19 - Claude Code". Compared after
   * `normalise`.
   */
  genericWindowTitles: [
    'claude', 'claude desktop', 'claude beta', 'claude dev', 'claude code',
  ],

  /**
   * Window subroles that are not a conversation: the quick-entry overlay and
   * system panels. Listing one here only hides it from the session list.
   */
  ignoredWindowSubroles: ['AXFloatingWindow', 'AXSystemFloatingWindow', 'AXSystemDialog'],
};

/**
 * Case- and punctuation-insensitive normalisation.
 *
 * Electron labels pick up trailing ellipses, non-breaking spaces and stray
 * punctuation, so comparison normalises before matching.
 */
export function normalise(label) {
  if (typeof label !== 'string') return null;
  const collapsed = label
    .replace(/ /g, ' ')
    .replace(/’/g, "'")
    .toLowerCase()
    // Fold diacritics before the charset filter below turns them into
    // spaces: without this "Arreter" with its circumflex normalised to
    // "arr ter" and matched nothing.
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[^a-z0-9 ']/g, ' ')
    .split(' ')
    .filter((part) => part !== '')
    .join(' ')
    .trim()
    // Claude Desktop appends a keyboard-shortcut number to each choice
    // ("Refuser 1", "Autoriser une fois 3"). It is an affordance, not part of
    // the label — and because matching is exact, it made every one of them
    // fail to match.
    .replace(/ \d+$/, '');
  return collapsed === '' ? null : collapsed;
}

/**
 * Exact membership after normalisation.
 *
 * Deliberately not a substring test: "Disallowed" must not read as "allow", or
 * a stray control could end up wired to APPROVE.
 */
export function matches(label, candidates) {
  return matchIndex(label, candidates) >= 0;
}

/**
 * Position of the matching candidate, or -1.
 *
 * Lets a caller rank matches by how restrictive the label is, rather than
 * taking whichever element the tree happens to yield first.
 */
export function matchIndex(label, candidates) {
  const normalised = normalise(label);
  if (normalised === null) return -1;
  return candidates.findIndex((candidate) => normalised === normalise(candidate));
}
