/**
 * How deep to walk Claude Desktop's Accessibility tree.
 *
 * Electron nests heavily: measured on a real permission dialog, the verdict
 * buttons sit at depth ~34 and Claude's own stop control at depth 32, under a
 * chain of roughly twenty nested AXGroups below the AXWebArea.
 *
 * This started at 24 for the daemon and 40 for `dump`, which meant the dump
 * showed every control while the daemon silently saw none of them — it reported
 * "no stop control available" and "permission: none" for a dialog that was
 * plainly on screen. One constant now, with real headroom, and `readTree` warns
 * when a walk actually reaches the limit.
 */
export const DEFAULT_TREE_DEPTH = 60;

/** The Accessibility action name for a press. */
export const AX_PRESS = 'AXPress';

/**
 * Roles belonging to the application menu.
 *
 * They support AXPress like any button, which is exactly the danger: the menu
 * carries generic verbs that collide with dialog labels. "Annuler" is *Cancel*
 * in a dialog but *Undo* in the Edit menu, and binding STOP to it fired
 * Edit > Undo on a live conversation. Nothing the daemon presses should ever
 * come from a menu.
 */
export const MENU_ROLES = new Set([
  'AXMenuBar',
  'AXMenuBarItem',
  'AXMenu',
  'AXMenuItem',
]);

/**
 * Opaque back-reference to a live UI element.
 *
 * The interpreter works on AXNode *snapshots*, but pressing a button needs the
 * real element. AXRef pairs the element with a readable marker; tests and the
 * simulator construct refs with a marker only, so the whole interpretation path
 * runs with no UI at all.
 */
export class AXRef {
  constructor(marker, element = null) {
    this.marker = marker;
    this.element = element;
  }
}

/** Immutable snapshot of one Accessibility element and its subtree. */
export class AXNode {
  constructor({
    role,
    subrole = null,
    title = null,
    roleDescription = null,
    help = null,
    value = null,
    identifier = null,
    enabled = null,
    actions = [],
    children = [],
    ref = null,
  }) {
    this.role = role;
    this.subrole = subrole;
    this.title = title;
    this.roleDescription = roleDescription;
    this.help = help;
    this.value = value;
    this.identifier = identifier;
    this.enabled = enabled;
    this.actions = actions;
    this.children = children;
    this.ref = ref;
  }

  /**
   * Best available human label. Claude Desktop is an Electron app, so a
   * button's text sometimes lands in AXTitle, sometimes in AXDescription,
   * sometimes only in AXValue.
   */
  get label() {
    for (const candidate of [this.title, this.roleDescription, this.help, this.value]) {
      if (typeof candidate === 'string' && candidate.trim() !== '') return candidate.trim();
    }
    return null;
  }

  /** Whether the element exposes AXPress at all. Used by `dump`. */
  get canPress() {
    return this.actions.includes(AX_PRESS);
  }

  /**
   * Whether the daemon may press this element.
   *
   * Narrower than `canPress` on purpose: supporting AXPress is not enough,
   * because menu items do too — see MENU_ROLES.
   */
  get isActuatable() {
    return this.canPress && !MENU_ROLES.has(this.role);
  }

  /** Depth-first pre-order walk over the subtree, `this` included. */
  walk(visit, depth = 0) {
    visit(this, depth);
    for (const child of this.children) child.walk(visit, depth + 1);
  }

  /** All nodes in the subtree matching `predicate`, in pre-order. */
  findAll(predicate) {
    const out = [];
    this.walk((node) => { if (predicate(node)) out.push(node); });
    return out;
  }
}
