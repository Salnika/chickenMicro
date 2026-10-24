/**
 * Minimal option parser.
 *
 * Hand-rolled rather than pulled from a CLI framework: there are five commands
 * and eight flags, and keeping the dependency list to koffi and serialport
 * means the thing macOS grants Accessibility to has very little in it.
 */
const VALUE_OPTIONS = new Set([
  '--serial', '--baud', '--filter', '--depth', '--log', '--approve-delay', '--poll',
  '--log-file', '--log-file-level',
]);

export function parseArguments(argv) {
  const tokens = [...argv];
  let command;
  if (tokens.length > 0 && !tokens[0].startsWith('-')) {
    command = tokens.shift();
  }

  const flags = new Set();
  const values = new Map();
  const positionals = [];

  for (let index = 0; index < tokens.length; index += 1) {
    const token = tokens[index];
    if (VALUE_OPTIONS.has(token)) {
      values.set(token, tokens[index + 1] ?? '');
      index += 1;
    } else if (token.startsWith('--') && token.includes('=')) {
      const split = token.indexOf('=');
      values.set(token.slice(0, split), token.slice(split + 1));
    } else if (token.startsWith('-')) {
      flags.add(token);
    } else {
      positionals.push(token);
    }
  }

  return {
    command,
    positionals,
    has: (flag) => flags.has(flag),
    value: (option) => {
      const raw = values.get(option);
      return raw === undefined || raw === '' ? undefined : raw;
    },
    int: (option) => {
      const raw = values.get(option);
      if (raw === undefined || raw === '') return undefined;
      const parsed = Number.parseInt(raw, 10);
      return Number.isInteger(parsed) ? parsed : undefined;
    },
  };
}
