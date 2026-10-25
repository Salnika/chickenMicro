import { basename } from 'node:path';

import { toSingleLine } from '../protocol/displayText.js';

/** What the device is told about one hook request, and what ties it to a dialog. */
const CONTENT_MAX_CHARS = 400;
const SALIENT_MAX_CHARS = 80;

const clip = (text, limit) => (text.length <= limit ? text : `${text.slice(0, limit - 3)}...`);
const str = (value) => (typeof value === 'string' ? value : '');

/** The part of a command a dialog is sure to show: its first line. */
const firstLine = (text) => str(text).split('\n', 1)[0].trim();

/**
 * What a session is called on the box: the folder it runs in, as one line of text
 * the panel can draw. A folder with no such name — the root, an emoji — is "Claude".
 */
export function sessionNameOf(cwd) {
  return (cwd !== '' ? toSingleLine(basename(cwd)) : '') || 'Claude';
}

/**
 * Turns what a hook was handed into what the box shows, in the vocabulary the
 * interpreter already uses for a dialog it read: a `kind`, a `title`, a `content`.
 *
 * `salient` lists the strings a dialog for this request must contain, so the
 * request can be matched to the dialog Claude draws for it. Nothing here decides
 * anything: a wrong guess costs two entries on the box, never a wrong approval.
 *
 * @returns {{kind: string, title: string, content: string, salient: string[], sessionName: string}}
 */
export function summarise({ tool, input = {}, cwd = '' }) {
  const sessionName = sessionNameOf(cwd);
  const path = str(input.file_path ?? input.notebook_path ?? input.path);
  const out = (kind, title, content, salient) => ({
    kind,
    title,
    content: clip(content, CONTENT_MAX_CHARS),
    salient: salient.map((s) => clip(s, SALIENT_MAX_CHARS)).filter((s) => s.length >= 3),
    sessionName,
  });

  switch (tool) {
    case 'Bash':
      return out('shell', 'Run command', str(input.command), [firstLine(input.command)]);

    case 'Edit':
    case 'MultiEdit':
      return out('write', 'Edit file', path, [path, basename(path)]);
    case 'Write':
      return out('write', 'Write file', path, [path, basename(path)]);
    case 'NotebookEdit':
      return out('write', 'Edit notebook', path, [path, basename(path)]);

    case 'Read':
      return out('read', 'Read file', path, [path, basename(path)]);
    case 'Glob':
      return out('read', 'Find files', str(input.pattern), [str(input.pattern)]);
    case 'Grep':
      return out('read', 'Search files', str(input.pattern), [str(input.pattern)]);

    case 'WebFetch':
      return out('fetch', 'Fetch URL', str(input.url), [str(input.url)]);
    case 'WebSearch':
      return out('search', 'Search the web', str(input.query), [str(input.query)]);

    default:
      break;
  }

  if (tool.startsWith('mcp__')) {
    const [, server = '', name = ''] = tool.split('__');
    const detail = Object.values(input).filter((v) => typeof v === 'string').slice(0, 2);
    return out('tool', 'Use tool', `${server}: ${name}`.trim(), [name, ...detail]);
  }

  const strings = Object.values(input).filter((v) => typeof v === 'string');
  return out('tool', tool, strings.join(' '), strings.map(firstLine));
}
