/**
 * Downstream against Streamdown RN, on the same documents.
 *
 * What this measures: the work each renderer does on the JavaScript thread
 * while a document grows. Downstream validates and batches a chunk, then hands
 * it to a native session; Streamdown reparses the whole document in JavaScript
 * on every update. That difference is the design, and it is what the numbers
 * below show.
 *
 * What this does NOT measure, and what the ratio must not be read as:
 *
 * - Downstream's native parse. The TurboModule is a stub, so its append is a
 *   no-op. The C++ engine that does the real work is absent from this number.
 * - Either renderer's layout or paint. Neither runs in Node.
 *
 * So the gap here is a JavaScript-thread gap, not a total-cost gap. A full
 * answer needs the native parse, the layout, and the frame timings, which the
 * example app's Bench and Testing screens measure on a device. Read these rows
 * as "how much JavaScript each renderer asks of the thread that also has to
 * stay responsive", and nothing stronger.
 */
import { describe, test } from 'vitest';

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
  const render = renderGrowth(() => <StreamdownText markdown={source} flavor="github" />);
  let source = '';
  for (const chunk of chunks) {
    source += chunk;
    render();
  }
}

describe('sample documents', () => {
  for (const sample of samples) {
    test(`${sample.id}: ${sample.description}`, async ({ bench }) => {
      await bench(
        `downstream, ${sample.chunks.length} chunks`,
        () => {
          downstreamGrow(sample.chunks);
        }
      ).run();
      await bench(
        `streamdown, ${sample.chunks.length} chunks`,
        () => {
          streamdownGrow(sample.chunks);
        }
      ).run();
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
