/** @vitest-environment happy-dom */
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import PhoneBenchmark from '../example/src/bench/PhoneBenchmark';

const observed = vi.hoisted(() => ({
  props: [] as Record<string, unknown>[],
  controllers: [] as {
    append: ReturnType<typeof vi.fn>;
    finish: ReturnType<typeof vi.fn>;
    dispose: ReturnType<typeof vi.fn>;
  }[],
  results: [] as Record<string, unknown>[],
}));
vi.mock('@react-native-downstream/core', () => ({
  Downstream: (props: Record<string, unknown>) => {
    observed.props.push(props);
    return null;
  },
  codeAvailable: () => true,
  mathAvailable: () => true,
  createMarkdownStream: () => {
    const controller = { append: vi.fn(), finish: vi.fn(), dispose: vi.fn() };
    observed.controllers.push(controller);
    return controller;
  },
}));
// These dependencies belong to the example workspace, not the library tests.
vi.mock('../example/node_modules/expo/fetch', () => ({ fetch: vi.fn() }));
vi.mock('../example/node_modules/react-native-streamdown', () => ({
  StreamdownText: () => null,
}));
vi.mock('../example/node_modules/react-native-enriched-markdown', () => ({
  EnrichedMarkdownText: () => null,
}));
vi.mock('../example/node_modules/react-native-markdown-display', () => ({
  default: () => null,
}));
vi.mock('../example/node_modules/react-native-marked', () => ({
  default: () => null,
}));
vi.mock('../example/node_modules/react-native-markdown-renderer', () => ({
  default: () => null,
}));

let root: Root | undefined;
let host: HTMLDivElement;
let candidate: 'downstream-string' | 'downstream-controller';

beforeEach(() => {
  observed.props = [];
  observed.controllers = [];
  observed.results = [];
  candidate = 'downstream-string';
  vi.useFakeTimers({
    toFake: ['setTimeout', 'clearTimeout', 'Date', 'performance'],
  });
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal(
    'requestAnimationFrame',
    vi.fn(() => 1)
  );
  vi.stubGlobal('cancelAnimationFrame', vi.fn());
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string, init?: { body?: string }) => {
      if (url.endsWith('/config'))
        return {
          ok: true,
          json: async () => ({
            benchmarkVersion: 'api-plugins-v1',
            candidate,
            workload: 'manual',
            runId: 'test',
            mode: 'stream',
            preview: true,
            syntaxHighlighting: true,
            math: true,
            finishDelayMs: 50,
            settleMs: 20,
            source: 'firstsecond',
            chunks: [
              { text: 'first', thread: 0, dueMs: 0 },
              { text: 'second', thread: 1, dueMs: 100 },
            ],
          }),
        };
      if (url.endsWith('/result'))
        observed.results.push(JSON.parse(init!.body!));
      return { ok: true };
    })
  );
  host = document.body.appendChild(document.createElement('div'));
  root = createRoot(host);
});
afterEach(() => {
  act(() => root?.unmount());
  host.remove();
  root = undefined;
  vi.useRealTimers();
  vi.unstubAllGlobals();
});
async function advance(ms: number) {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(ms);
  });
}
async function start() {
  await act(async () => {
    root!.render(<PhoneBenchmark />);
  });
  await advance(1000);
}

describe('phone API benchmark execution', () => {
  it('finishes each string row independently after its terminal delay and records committed plugin receipts', async () => {
    await start();
    expect(observed.controllers).toHaveLength(0);
    expect(
      observed.props.findLast((props) => props.children === 'first')
        ?.isAnimating
    ).toBe(true);
    await advance(50);
    expect(
      observed.props.findLast((props) => props.children === 'first')
        ?.isAnimating
    ).toBe(false);
    expect(observed.props.at(-1)?.isAnimating).toBe(true);
    await advance(100);
    expect(
      observed.props.findLast((props) => props.children === 'second')
        ?.isAnimating
    ).toBe(false);
    await advance(20);
    expect(observed.results[0]).toMatchObject({
      benchmarkVersion: 'api-plugins-v1',
      apiConfig: {
        sourceMode: 'string',
        plugins: ['code', 'math'],
        preview: true,
        engines: { code: true, math: true },
      },
      sourceEqualsFixture: true,
      threadCount: 2,
      finishes: [
        { thread: 0, actualMs: 50 },
        { thread: 1, actualMs: 150 },
      ],
      errors: [],
    });
  });

  it('feeds public controllers without parent React source updates for every chunk', async () => {
    candidate = 'downstream-controller';
    await start();
    const renders = observed.props.length;
    await advance(170);
    expect(observed.controllers).toHaveLength(2);
    expect(observed.controllers[0]!.append).toHaveBeenCalledWith('first');
    expect(observed.controllers[1]!.append).toHaveBeenCalledWith('second');
    expect(
      observed.controllers.every(
        (controller) => controller.finish.mock.calls.length === 1
      )
    ).toBe(true);
    expect(observed.props).toHaveLength(renders);
    expect(
      observed.props.every(
        (props) => props.children === undefined && props.stream !== undefined
      )
    ).toBe(true);
    expect(observed.results[0]).toMatchObject({
      apiConfig: {
        sourceMode: 'controller',
        plugins: ['code', 'math'],
        preview: true,
      },
      revisionCount: 2,
      errors: [],
    });
  });
});
