/**
 * Test-adapter costs on the same documents.
 *
 * Each Downstream flush starts a fresh C++ subprocess and replays every earlier
 * session operation. Streamdown commits each growing prefix through React DOM.
 * These adapters have different overhead, so their timings cannot rank the
 * shipping React Native renderers or measure a warm incremental parser.
 *
 * Use `pnpm bench:native` for an in-process engine stream. Native layout, paint,
 * and frame latency need release builds on physical devices; the example app's
 * Bench and Testing screens collect those traces.
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

import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { samples, syntheticDocument } from './samples';

/**
 * Renders a growing document through a real React root, the way a chat view
 * would. Calling the component directly would not work: it uses hooks, and it
 * only does its work on commit.
 */
function renderGrowth(node: () => React.ReactElement) {
  const host = document.createElement('div');
  document.body.append(host);
  const root = createRoot(host);
  return {
    render() {
      flushSync(() => {
        root.render(node());
      });
    },
    dispose() {
      flushSync(() => root.unmount());
      host.remove();
    },
  };
}

/** Exercises the controller through the subprocess replay adapter. */
function downstreamGrow(chunks: readonly string[]): void {
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

/** Re-renders Streamdown over the growing document, as a chat view would. */
function streamdownGrow(chunks: readonly string[]): void {
  const view = renderGrowth(() => (
    <StreamdownText markdown={source} flavor="github" />
  ));
  let source = '';
  try {
    for (const chunk of chunks) {
      source += chunk;
      view.render();
    }
  } finally {
    view.dispose();
  }
}

describe('sample document adapters', () => {
  for (const sample of samples) {
    test(`${sample.id}: ${sample.description}`, async ({ bench }) => {
      await bench(`downstream replay, ${sample.chunks.length} chunks`, () => {
        downstreamGrow(sample.chunks);
      }).run();
      await bench(`streamdown DOM, ${sample.chunks.length} chunks`, () => {
        streamdownGrow(sample.chunks);
      }).run();
    });
  }
});

describe('synthetic documents', () => {
  // Built rather than stored, so the sizes here are the point.
  const cases = [10, 50, 200] as const;

  for (const sections of cases) {
    const document = syntheticDocument(sections);
    const label = `${(document.length / 1024).toFixed(1)} KiB`;
    // One chunk per section: the shape a real answer arrives in.
    const chunks = document.split(/(?=## )/u).filter(Boolean);
    test(label, async ({ bench }) => {
      await bench(`downstream replay, ${chunks.length} chunks`, () => {
        downstreamGrow(chunks);
      }).run();
      await bench(`streamdown DOM, ${chunks.length} chunks`, () => {
        streamdownGrow(chunks);
      }).run();
    });
  }
});

describe('a document that arrives complete', () => {
  // The non-streaming path: no incremental work to amortise.
  const document = syntheticDocument(50);
  test('single render', async ({ bench }) => {
    await bench('downstream replay', () => {
      const stream = createMarkdownStream();
      try {
        stream.append(document);
        stream.finish();
      } finally {
        stream.dispose();
      }
    }).run();
    await bench('streamdown DOM', () => {
      const view = renderGrowth(() => (
        <StreamdownText markdown={document} flavor="github" />
      ));
      try {
        view.render();
      } finally {
        view.dispose();
      }
    }).run();
  });
});
