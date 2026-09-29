/**
 * What a stream costs to feed, end to end, with the real parser.
 *
 * The TurboModule is the engine-backed one, so this includes the C++ parse that
 * a real append triggers. What it does not include is layout or paint, and the
 * engine runs in a subprocess here where it is in-process on a device, so these
 * numbers describe this harness rather than the shipping app.
 */
// The TurboModule is backed by the real C++ engine, so a Downstream number
// includes the parse it would really do.
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('../__tests__/real-engine-module')).realEngineModule
);

import { describe, test, vi } from 'vitest';

import { createMarkdownStream } from '../src/createMarkdownStream';
import { samples, syntheticDocument } from './samples';

function appendAll(chunks: readonly string[]): void {
  const stream = createMarkdownStream();
  try {
    for (const chunk of chunks) stream.append(chunk);
    stream.finish();
  } finally {
    stream.dispose();
  }
}

describe('stream appends', () => {
  for (const sample of samples) {
    test(`append ${sample.id}`, async ({ bench }) => {
      await bench(
        `${sample.chunks.length} chunks, ${sample.source.length} chars`,
        () => {
          appendAll(sample.chunks);
        }
      ).run();
    });
  }
});

describe('source size', () => {
  // A large document re-validated per render is the cost that shows up when a
  // parent re-renders during a stream.
  const cases = [
    ['~4 KiB', syntheticDocument(4)],
    ['~40 KiB', syntheticDocument(40)],
    ['~200 KiB', syntheticDocument(200)],
  ] as const;

  for (const [label, source] of cases) {
    test(`append a whole document, ${label}`, async ({ bench }) => {
      await bench(`${source.length} chars in one chunk`, () => {
        appendAll([source]);
      }).run();
    });
  }

  test('append many small chunks', async ({ bench }) => {
    const chunks = Array.from({ length: 200 }, () => 'x');
    await bench('200 single-character chunks', () => {
      appendAll(chunks);
    }).run();
  });
});
