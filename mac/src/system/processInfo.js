import koffi from 'koffi';

/** PROC_PIDPATHINFO_MAXSIZE in <libproc.h>. */
const PATH_CAPACITY = 4096;

let procPidPath = null;

/**
 * The executable a running process is actually running, per the kernel, or
 * null when that can no longer be resolved.
 *
 * `ps` reports the path the process was started from and keeps reporting it
 * after the file has been replaced. proc_pidpath resolves the file itself, and
 * fails once it has been deleted — the same lookup TCC makes, and fails, for an
 * app that was updated while it was open.
 */
export function executableOf(pid) {
  procPidPath ??= koffi.load('/usr/lib/libSystem.B.dylib')
    .func('int proc_pidpath(int pid, _Out_ uint8_t *buffer, uint32_t size)');
  const buffer = Buffer.alloc(PATH_CAPACITY);
  const length = procPidPath(pid, buffer, buffer.length);
  return length > 0 ? buffer.toString('utf8', 0, length) : null;
}
