/**
 * Stream-controller cost through the subprocess replay adapter.
 *
 * Every flush starts a fresh native process and replays all earlier operations.
 * This includes repeated historical parsing and subprocess overhead. It cannot
 * measure a warm production session, native layout, or paint. Use bench:native
 * for engine measurements and physical devices for the complete renderer.
 */
// The TurboModule is backed by the real C++ engine, so a Downstream number
// includes the parse it would really do.
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('../tests/real-engine-module')).realEngineModule
);

import { describe, test, vi } from 'vitest';

import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { samples, syntheticDocument } from './samples';

function appendAll(chunks: readonly string[]): void {
  const stream = createMarkdownStream();
  try {
    for (const chunk of chunks) {
      stream.append(chunk);
      getStreamState(stream).flush();
    }
    stream.finish();
  } finally {
    stream.dispose();
  }
}

describe('stream appends', () => {
  for (const sample of samples) {
    test(`append ${sample.id}`, async ({ bench }) => {
      await bench(`${sample.chunks.length} chunks, ${sample.source.length} chars`, () => {
        appendAll(sample.chunks);
      }).run();
    });
  }
});

describe('source size', () => {
  // A replacement validates and parses the complete source, independently of
  // the append limit and frame batching.
  const cases = [4, 40, 200].map((sections) => {
    const source = syntheticDocument(sections);
    return [`${(source.length / 1024).toFixed(1)} KiB`, source] as const;
  });

  for (const [label, source] of cases) {
    test(`replace a whole document, ${label}`, async ({ bench }) => {
      await bench(`${source.length} chars in one reset`, () => {
        const stream = createMarkdownStream();
        try {
          stream.reset(source);
          stream.finish();
        } finally {
          stream.dispose();
        }
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
