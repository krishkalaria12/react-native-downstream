import {
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
