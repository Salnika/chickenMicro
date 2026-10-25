/**
 * Decides whether the daemon reads Claude Desktop's windows.
 *
 * Reading them takes the macOS Accessibility grant and a native module. A daemon
 * that only answers Claude Code hooks needs neither, and a terminal or VS Code
 * user has no window for it to read anyway. So the choice is made once, at
 * start-up, and the daemon says why:
 *
 *   - `off` (--no-accessibility) never loads the native module at all;
 *   - otherwise the module is loaded, and if it will not load, or the grant is
 *     missing, the daemon runs hook-only instead of refusing to start.
 *
 * Whether to insist on the grant (--accessibility, the old behaviour) is the
 * caller's call: this reports the facts and never opens a settings pane itself.
 *
 * @param {{off?: boolean, load: () => Promise<any>}} options
 * @returns {Promise<{module: any, trusted: boolean, why: string}>}
 *   `module` is the loaded Accessibility module even when it is not trusted, so a
 *   caller that insists can use its own explanation; `why` is empty when trusted.
 */
export async function chooseAccessibility({ off = false, load }) {
  if (off) {
    return { module: null, trusted: false, why: 'it was switched off with --no-accessibility' };
  }

  let module;
  try {
    module = await load();
  } catch (error) {
    return {
      module: null,
      trusted: false,
      why: `its native module did not load (${error.message})`,
    };
  }

  if (!module.isTrusted()) {
    return {
      module,
      trusted: false,
      why: 'it has not been granted to the app that launched the daemon',
    };
  }
  return { module, trusted: true, why: '' };
}
