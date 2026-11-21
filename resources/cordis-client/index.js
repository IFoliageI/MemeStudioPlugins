import { createConnection } from 'node:net';

export const DEFAULT_PIPE = String.raw`\\.\pipe\MemeConvertStudio.DeepSeekHarness.v1`;

export function callStudio(operation, args = {}, { pipe = DEFAULT_PIPE, signal, timeoutMs = 600000 } = {}) {
  return new Promise((resolve, reject) => {
    signal?.throwIfAborted();
    const payload = JSON.stringify({ version: 2, operation, arguments: args }) + '\n';
    if (Buffer.byteLength(payload) > 65536) {
      throw new Error('Request payload exceeds 64 KiB.');
    }
    const socket = createConnection(pipe);
    let buffer = '';
    let settled = false;

    const finish = (err, res) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.destroy();
      if (err) reject(err);
      else resolve(res);
    };

    const timer = setTimeout(() => finish(new Error('MemeStudio IPC request timed out.')), timeoutMs);
    signal?.addEventListener('abort', () => finish(new Error('MemeStudio IPC request cancelled.')), { once: true });

    socket.setEncoding('utf8');
    socket.on('connect', () => socket.write(payload));
    socket.on('error', (err) => finish(new Error(`MemeStudio IPC connection error (${err.code || 'UNKNOWN'}): ${err.message}`)));
    socket.on('close', () => finish(new Error('MemeStudio IPC closed before completing response.')));
    socket.on('data', (chunk) => {
      buffer += chunk;
      if (Buffer.byteLength(buffer) > 1024 * 1024) {
        return finish(new Error('MemeStudio IPC response exceeds 1 MiB.'));
      }
      const nl = buffer.indexOf('\n');
      if (nl < 0) return;
      try {
        const parsed = JSON.parse(buffer.slice(0, nl));
        if (parsed.ok !== true) {
          return finish(new Error(parsed.error || 'Request rejected by MemeStudio.'));
        }
        finish(null, parsed.result);
      } catch (e) {
        finish(new Error(`Malformed MemeStudio IPC JSON response: ${e.message}`));
      }
    });
  });
}
