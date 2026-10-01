/**
 * Downstream against Streamdown RN, on the same documents.
 *
 * What this measures: the cost each renderer pays to show a document that is
 * still arriving. Both sides run for real here. Downstream's TurboModule is the
 * engine-backed one in `tests/real-engine-module.ts`, so its row includes
 * the actual C++ parse; Streamdown renders through a real React root, so its
 * row includes its whole-document reparse and commit on every update.
 *
 * What this does NOT measure: native layout, paint, or the frame budget. Those
 * need a device, and the example app's Bench and Testing screens measure them.
 * The engine here runs in a subprocess, which is not how it runs on a phone:
 * in the app it is an in-process session with a warm parser. So a win or a loss
 * here is a signal about this harness, not a verdict about the library.
 *
 * Read it as: on the same document, in the same process, how much time does
 * each renderer spend before the text is ready to show.
 */
// The TurboModule is backed by the real C++ engine, so a Downstream number
// includes the parse it would really do.
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('../tests/real-engine-module')).realEngineModule
);

import { describe, test, vi } from 'vitest';

import { flushSync } from 'react-dom';
import { createRoot } from 'react-dom/client';
import { StreamdownText } from 'react-native-streamdown';

import { createMarkdownStream } from '../src/createMarkdownStream';
import { samples, syntheticDocument } from './samples';

/**
 * Renders a growing document through a real React root, the way a chat view
 * would. Calling the component directly would not work: it uses hooks, and it
 * only does its work on commit.
 */
function renderGrowth(node: () => React.ReactElement): () => void {
  const host = document.createElement('div');
  document.body.append(host);
  const root = createRoot(host);
  return () => {
    flushSync(() => {
      root.render(node());
    });
  };
}

/** Feeds chunks to a Downstream session, which parses only the new tail. */
function downstreamGrow(chunks: readonly string[]): void {
  const stream = createMarkdownStream();
  try {
    for (const chunk of chunks) stream.append(chunk);
    stream.finish();
  } finally {
    stream.dispose();
  }
}

/** Re-renders Streamdown over the growing document, as a chat view would. */
function streamdownGrow(chunks: readonly string[]): void {
  const render = renderGrowth(() => (
    <StreamdownText markdown={source} flavor="github" />
  ));
  let source = '';
  for (const chunk of chunks) {
    source += chunk;
    render();
  }
}

describe('sample documents', () => {
  for (const sample of samples) {
    test(`${sample.id}: ${sample.description}`, async ({ bench }) => {
      await bench(`downstream, ${sample.chunks.length} chunks`, () => {
        downstreamGrow(sample.chunks);
      }).run();
      await bench(`streamdown, ${sample.chunks.length} chunks`, () => {
        streamdownGrow(sample.chunks);
      }).run();
    });
  }
});

describe('synthetic documents', () => {
  // Built rather than stored, so the sizes here are the point.
  const cases = [
    ['~10 KiB', 10],
    ['~50 KiB', 50],
    ['~200 KiB', 200],
  ] as const;

  for (const [label, sections] of cases) {
    const document = syntheticDocument(sections);
    // One chunk per section: the shape a real answer arrives in.
    const chunks = document.split(/(?=## )/u).filter(Boolean);
    test(label, async ({ bench }) => {
      await bench(`downstream, ${chunks.length} chunks`, () => {
        downstreamGrow(chunks);
      }).run();
      await bench(`streamdown, ${chunks.length} chunks`, () => {
        streamdownGrow(chunks);
      }).run();
    });
  }
});

describe('a document that arrives complete', () => {
  // The non-streaming path: no incremental work to amortise.
  const document = syntheticDocument(50);
  test('single render', async ({ bench }) => {
    await bench('downstream', () => {
      const stream = createMarkdownStream();
      try {
        stream.append(document);
        stream.finish();
      } finally {
        stream.dispose();
      }
    }).run();
    await bench('streamdown', () => {
      renderGrowth(() => (
        <StreamdownText markdown={document} flavor="github" />
      ))();
    }).run();
  });
});
