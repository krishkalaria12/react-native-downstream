/**
 * The controller is the JS half of the render path. Appending to a stream
 * schedules a native parse, so this measures the cost the JavaScript thread
 * pays per chunk: validation, batching, and the native call. It is not a
 * measure of layout or paint, which happen elsewhere.
 */
import { describe, test } from 'vitest';

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
