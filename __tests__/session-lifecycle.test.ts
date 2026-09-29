import {
  afterTwoAnimationFrames,
  allFinalSnapshotsReady,
  resetSessionControllers,
} from './sessionLifecycle';

test('reset clears pending sessions without disposing them', () => {
  const first = { reset: jest.fn(), dispose: jest.fn() };
  const second = { reset: jest.fn(), dispose: jest.fn() };

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
  const request = jest
    .spyOn(globalThis, 'requestAnimationFrame')
    .mockImplementation((callback) => {
      const frame = nextFrame++;
      frames.set(frame, callback);
      return frame;
    });
  const cancel = jest
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
    const completed = jest.fn();
    afterTwoAnimationFrames(completed);
    advanceFrame();
    expect(completed).not.toHaveBeenCalled();
    advanceFrame();
    expect(completed).toHaveBeenCalledTimes(1);

    const cancelled = jest.fn();
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
