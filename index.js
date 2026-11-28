import { createConnection } from 'node:net';

export const inject = ['tools'];
export const DEFAULT_PIPE = String.raw`\\.\pipe\MemeStudio.DeepSeekHarness.v1`;

const MAX_REQUEST_BYTES = 65536;
const MAX_RESPONSE_BYTES = 1024 * 1024;
const DEFAULT_TIMEOUT_MS = 3600000;

export function callStudio(operation, args = {}, signal, { pipe = DEFAULT_PIPE, timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  return new Promise((resolve, reject) => {
    signal?.throwIfAborted();
    const payload = JSON.stringify({ version: 2, operation, arguments: args }) + '\n';
    if (Buffer.byteLength(payload) > MAX_REQUEST_BYTES) {
      throw new Error('MemeStudio request exceeds 64 KiB.');
    }

    const client = createConnection(pipe);
    let reply = '';
    let bytes = 0;
    let settled = false;

    const finish = (err, result) => {
      if (settled) return;
      settled = true;
      signal?.removeEventListener('abort', onAbort);
      clearTimeout(timer);
      client.destroy();
      if (err) reject(err);
      else resolve(result);
    };

    const parseLine = (rawLine) => {
      try {
        const parsed = JSON.parse(rawLine);
        if (parsed.ok !== true) {
          return finish(new Error(parsed.error || 'MemeStudio rejected the request.'));
        }
        finish(null, parsed.result);
      } catch (e) {
        finish(new Error(`Malformed MemeStudio response: ${e.message}`));
      }
    };

    const onAbort = () => finish(new Error('MemeStudio call cancelled.'));
    const timer = setTimeout(
      () => finish(new Error('MemeStudio call timed out. Check the MemeStudio app before retrying.')),
      timeoutMs
    );
    signal?.addEventListener('abort', onAbort, { once: true });

    client.setEncoding('utf8');
    client.on('connect', () => client.write(payload));
    client.on('error', (error) =>
      finish(
        new Error(
          error.code === 'EBUSY'
            ? 'MemeStudio is currently handling another Harness request.'
            : 'Cannot reach MemeStudio. Open MemeStudio and enable the DeepSeek Harness plugin under the same Windows account.'
        )
      )
    );
    client.on('data', (chunk) => {
      bytes += Buffer.byteLength(chunk);
      if (bytes > MAX_RESPONSE_BYTES) {
        return finish(new Error('MemeStudio response exceeds 1 MiB.'));
      }
      reply += chunk;
      const nl = reply.indexOf('\n');
      if (nl < 0) return;
      parseLine(reply.slice(0, nl));
    });
    client.on('close', () => {
      if (!settled && reply.trim()) {
        return parseLine(reply.trim());
      }
      finish(new Error('MemeStudio disconnected before completing the response.'));
    });
  });
}

const localPath = {
  type: 'string',
  description: 'Absolute Windows file path on this computer, e.g. D:/images/input.png.'
};

const instruction = {
  type: 'string',
  description: 'Prompt or editing instructions in UTF-8 (maximum 24,000 bytes).'
};

const confirmed = {
  type: 'boolean',
  description: 'Set true ONLY after the user explicitly approves image generation and potential API costs.'
};

const normalizedRect = {
  type: 'object',
  additionalProperties: false,
  required: ['x', 'y', 'width', 'height'],
  description: 'Normalized rectangle in 0..1 relative to image dimensions.',
  properties: {
    x: { type: 'number' },
    y: { type: 'number' },
    width: { type: 'number' },
    height: { type: 'number' }
  }
};

const commonGenProps = {
  instruction,
  confirmed,
  styles: { type: 'array', items: localPath, description: 'Optional up to 8 style reference images.' },
  model: { type: 'string', description: 'Optional allowlisted model ID override for this task only.' },
  count: { type: 'integer', description: 'Number of copies to generate (1..10, default 1).' },
  output_parent: { type: 'string', description: 'Optional parent directory in which a new run folder will be created.' }
};

const toolSpecs = [
  [
    'status',
    'Read the running MemeStudio bridge status, granted permissions, and remaining daily/per-task quotas.',
    {},
    []
  ],
  [
    'inspect',
    'Read width, height, frame count, and frame delays of a local image or GIF without calling any remote API.',
    { path: localPath },
    ['path']
  ],
  [
    'models',
    'List available models from the currently configured MemeStudio provider or active session override.',
    {},
    []
  ],
  [
    'replace',
    'Replace the character in a local meme image or GIF using a character identity reference image. Requires user cost confirmation.',
    {
      ...commonGenProps,
      source: localPath,
      character: localPath,
      crop: normalizedRect,
      max_frames: { type: 'integer', description: 'Optional GIF frame cap (0..600; 0 means unlimited).' }
    },
    ['source', 'character', 'confirmed']
  ],
  [
    'draw',
    'Draw a brand-new meme or sticker from a text prompt with optional style reference images. Requires user cost confirmation.',
    commonGenProps,
    ['instruction', 'confirmed']
  ],
  [
    'retouch',
    'Retouch an existing image or GIF via full-frame prompt editing (mode="prompt") or localized rectangle redraw (mode="region"). Requires user cost confirmation.',
    {
      ...commonGenProps,
      source: localPath,
      mode: { type: 'string', enum: ['prompt', 'region'], description: 'prompt (full-image edit) or region (pixel-locked outside rectangle).' },
      region: normalizedRect
    },
    ['source', 'instruction', 'confirmed']
  ]
];

export function apply(ctx) {
  for (const [operation, description, properties, required] of toolSpecs) {
    ctx.effect(() =>
      ctx.tools.register({
        name: `memestudio_${operation}`,
        description,
        timeoutMs: DEFAULT_TIMEOUT_MS,
        parameters: {
          type: 'object',
          properties,
          required,
          additionalProperties: false
        },
        output: {
          schema: { type: 'object', additionalProperties: true },
          render: (_args, result) => [{ type: 'text', text: JSON.stringify(result, null, 2) }]
        },
        execute: (args, execution) => callStudio(operation, args, execution.signal)
      })
    );
  }
}
