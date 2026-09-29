import { expect, vi } from 'vitest';

import {
  afterTwoAnimationFrames,
  allFinalSnapshotsReady,
  resetSessionControllers,
} from '../example/src/shared/sessionLifecycle';

test('reset clears pending sessions without disposing them', () => {
  const first = { reset: vi.fn(), dispose: vi.fn() };
  const second = { reset: vi.fn(), dispose: vi.fn() };

  resetSessionControllers([first, second]);

  expect(first.reset).toHaveBeenCalledWith();
  expect(second.reset).toHaveBeenCalledWith();
  expect(first.dispose).not.toHaveBeenCalled();
  expect(second.dispose).not.toHaveBeenCalled();
});

test('completion waits for every native session to reach the final sequence', () => {
  expect(allFinalSnapshotsReady([8, 7, 8, 8], 8)).toBe(false);
  expect(allFinalSnapshotsReady([8, 8, 8, 8], 8)).toBe(true);
});

test('completion waits for two frames and cancellation stops the pending frame', () => {
  const frames = new Map<number, Parameters<typeof requestAnimationFrame>[0]>();
  let nextFrame = 1;
  const request = vi
    .spyOn(globalThis, 'requestAnimationFrame')
    .mockImplementation((callback) => {
      const frame = nextFrame++;
      frames.set(frame, callback);
      return frame;
    });
  const cancel = vi
    .spyOn(globalThis, 'cancelAnimationFrame')
    .mockImplementation((frame) => {
      frames.delete(frame);
    });
  const advanceFrame = () => {
    const [frame, callback] = frames.entries().next().value!;
    frames.delete(frame);
    callback(0);
  };

  try {
    const completed = vi.fn();
    afterTwoAnimationFrames(completed);
    advanceFrame();
    expect(completed).not.toHaveBeenCalled();
    advanceFrame();
    expect(completed).toHaveBeenCalledTimes(1);

    const cancelled = vi.fn();
    const stop = afterTwoAnimationFrames(cancelled);
    advanceFrame();
    stop();
    expect(frames.size).toBe(0);
    expect(cancelled).not.toHaveBeenCalled();
  } finally {
    request.mockRestore();
    cancel.mockRestore();
  }
});
