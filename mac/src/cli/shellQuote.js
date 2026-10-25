/**
 * Quotes a word for a POSIX shell.
 *
 * Single quotes expand nothing, so the only character that needs care is a single
 * quote itself: it closes the quoting, is written escaped, and the quoting opens
 * again. Double quotes would still expand `$name` and backticks, and a `"` or `\`
 * in a path would break out of them.
 */
export function shellQuote(word) {
  return `'${word.replaceAll("'", "'\\''")}'`;
}
