/**
 * Renders an AXNode tree as text for `claude-remote dump`.
 *
 * This is the tool that keeps the project maintainable: when a Claude Desktop
 * release moves things around, the dump shows the new shape and selectors.js is
 * edited to match.
 */
export class AXTreeFormatter {
  constructor({ maxValueChars = 120 } = {}) {
    this.maxValueChars = maxValueChars;
  }

  /** Full tree with box-drawing connectors. */
  render(root) {
    let out = `${this.describe(root)}\n`;
    out += this.#renderChildren(root, '');
    return out;
  }

  #renderChildren(node, prefix) {
    let out = '';
    node.children.forEach((child, index) => {
      const isLast = index === node.children.length - 1;
      out += `${prefix}${isLast ? '└── ' : '├── '}${this.describe(child)}\n`;
      out += this.#renderChildren(child, prefix + (isLast ? '    ' : '│   '));
    });
    return out;
  }

  /** Flat list of matching nodes, each with its role path. */
  renderMatches(root, filter) {
    const needle = filter.toLowerCase();
    let out = '';
    let count = 0;
    const walk = (node, path) => {
      const here = [...path, node.role];
      if (AXTreeFormatter.matches(node, needle)) {
        count += 1;
        out += `${here.join(' > ')}\n    ${this.describe(node)}\n`;
      }
      for (const child of node.children) walk(child, here);
    };
    walk(root, []);
    if (count === 0) return `no element matches "${filter}"\n`;
    return `${out}\n${count} match${count === 1 ? '' : 'es'} for "${filter}"\n`;
  }

  /** True when any inspected attribute contains `needle`. */
  static matches(node, needle) {
    const haystacks = [
      node.role, node.subrole, node.title, node.roleDescription,
      node.help, node.value, node.identifier, node.actions.join(' '),
    ];
    return haystacks.some((value) => typeof value === 'string' && value.toLowerCase().includes(needle));
  }

  /** One line per element: role, then only the attributes that are present. */
  describe(node) {
    const parts = [node.role];
    if (node.subrole) parts.push(`subrole=${node.subrole}`);
    if (node.title) parts.push(`title=${this.#quote(node.title)}`);
    if (node.roleDescription) parts.push(`desc=${this.#quote(node.roleDescription)}`);
    if (node.help) parts.push(`help=${this.#quote(node.help)}`);
    if (node.value) parts.push(`value=${this.#quote(node.value)}`);
    if (node.identifier) parts.push(`id=${this.#quote(node.identifier)}`);
    if (node.enabled === false) parts.push('disabled');
    if (node.actions.length > 0) parts.push(`actions=[${node.actions.join(',')}]`);
    if (node.children.length > 0) parts.push(`children=${node.children.length}`);
    return parts.join(' ');
  }

  #quote(raw) {
    const flattened = raw.replace(/\n/g, '\\n').replace(/\t/g, '\\t');
    const clipped = flattened.length > this.maxValueChars
      ? `${flattened.slice(0, this.maxValueChars)}…`
      : flattened;
    return `"${clipped}"`;
  }
}
