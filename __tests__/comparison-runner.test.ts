import { afterEach, beforeEach, expect, vi } from 'vitest';

import { fixtures } from '../example/src/shared/fixtures';
import NativeDownstreamSession from '../src/NativeDownstreamSession';

import {
  ComparisonRunner,
  type ComparisonView,
} from '../example/src/testing/comparisonRunner';

vi.mock('../src/NativeDownstreamSession', () => {
  const listeners = { snapshot: new Set<Function>() };
  return {
    __esModule: true,
    default: {
      create: vi.fn(),
      // The real worker parses asynchronously, so a snapshot follows the
      // append instead of arriving in the same call.
      append: vi.fn(
        (
          sessionId: string,
          _chunk: string,
          generation: number,
          sequence: number
        ) => {
          setTimeout(() => {
            for (const listener of listeners.snapshot) {
              listener({ sessionId, generation, sequence });
            }
          }, 0);
        }
      ),
      reset: vi.fn(),
      finish: vi.fn(),
      dispose: vi.fn(),
      setTracing: vi.fn(),
      onSnapshotReady: vi.fn((callback: (event: object) => void) => {
        listeners.snapshot.add(callback);
        return { remove: () => listeners.snapshot.delete(callback) };
      }),
      onSessionError: vi.fn(() => ({ remove: () => {} })),
      onTrace: vi.fn(() => ({ remove: () => {} })),
    },
  };
});

const fixture = fixtures[0]!;
const source = fixture.chunks.join('');

function drain(runner: ComparisonRunner): ComparisonView {
  return advanceUntil(
    runner,
    (view) => view.status === 'complete' || view.status === 'failed'
  );
}

function advanceUntil(
  runner: ComparisonRunner,
  ready: (view: ComparisonView) => boolean
): ComparisonView {
  for (let step = 0; step < 5000; step += 1) {
    const view = runner.getView();
    if (ready(view)) return view;
    vi.advanceTimersByTime(20);
  }
  return runner.getView();
}

beforeEach(() => {
  vi.useFakeTimers();
  // The mock factory is evaluated once, so calls carry over between tests.
  vi.clearAllMocks();
});

afterEach(() => {
  vi.useRealTimers();
});

test('both candidates run in order over the same source', () => {
  const runner = new ComparisonRunner();
  runner.run({
    fixture,
    workloadSize: 'fixture',
    workloadShape: 'repeat',
    schedule: 'manifest',
    seed: 7,
    parseMode: 'full',
  });

  const view = drain(runner);

  expect(view.status).toBe('complete');
  expect(view.runs[0]?.candidate).toBe('streamdown');
  expect(view.runs[1]?.candidate).toBe('downstream');
  expect(view.runs[0]?.error).toBeNull();
  expect(view.runs[1]?.error).toBeNull();
  expect(view.runs[0]?.metrics.source).toBe(source);
  expect(view.runs[1]?.metrics.source).toBe(source);
  expect(view.runs[1]?.stream).not.toBeNull();
  expect(view.active).toBe(-1);
  runner.dispose();
});

test('the Downstream candidate hands the source to one native session', () => {
  const runner = new ComparisonRunner();
  runner.run({
    fixture,
    workloadSize: 'fixture',
    workloadShape: 'repeat',
    schedule: 'manifest',
    seed: 7,
    parseMode: 'incremental',
  });

  const view = drain(runner);
  const native = vi.mocked(NativeDownstreamSession);
  const modes = native.create.mock.calls.map((call) => call[1]);
  const appended = native.append.mock.calls.map((call) => call[1]).join('');

  expect(view.status).toBe('complete');
  expect(modes).toEqual(['incremental']);
  expect(appended).toBe(source);
  runner.dispose();
});

test('a reset mid-comparison frees the live session and stops the run', () => {
  const runner = new ComparisonRunner();
  runner.run({
    fixture,
    workloadSize: 'fixture',
    workloadShape: 'repeat',
    schedule: 'manifest',
    seed: 7,
    parseMode: 'full',
  });
  // Stop while Downstream is the candidate under test.
  advanceUntil(runner, (view) => view.active === 1);
  const native = vi.mocked(NativeDownstreamSession);
  expect(native.create).toHaveBeenCalledTimes(1);

  runner.reset();
  const view = runner.getView();

  expect(view.status).toBe('idle');
  expect(view.runs).toEqual([null, null]);
  expect(native.dispose).toHaveBeenCalledTimes(1);

  // Advancing past the run must not resurrect either pane.
  vi.advanceTimersByTime(5000);
  expect(runner.getView()).toEqual(view);
  runner.dispose();
});
