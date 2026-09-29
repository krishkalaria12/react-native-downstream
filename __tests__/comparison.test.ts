import {
  buildComparison,
  buildRows,
  compareText,
  formatValue,
  LONG_FRAME_MS,
  type CandidateMetrics,
} from '../example/src/testing/comparison';

function metrics(overrides: Partial<CandidateMetrics> = {}): CandidateMetrics {
  return {
    firstFrameMs: 16,
    totalMs: 100,
    frames: 5,
    longFrames: 0,
    longestFrameMs: 18,
    maxLatenessMs: 4,
    chunks: 40,
    source: 'hello',
    ...overrides,
  };
}

test('a missing metric never becomes a win', () => {
  const rows = buildRows(
    metrics({ firstFrameMs: null }),
    metrics({ firstFrameMs: 20 })
  );
  const firstFrame = rows.find((row) => row.id === 'firstFrameMs')!;
  expect(firstFrame.downstream).toBeNull();
  expect(firstFrame.delta).toBeNull();
  expect(firstFrame.faster).toBe('unavailable');
});

test('a shorter time to the final frame wins', () => {
  const rows = buildRows(metrics({ totalMs: 80 }), metrics({ totalMs: 120 }));
  const total = rows.find((row) => row.id === 'totalMs')!;
  expect(total.delta).toBe(-40);
  expect(total.faster).toBe('downstream');

  const reversed = buildRows(
    metrics({ totalMs: 120 }),
    metrics({ totalMs: 80 })
  ).find((row) => row.id === 'totalMs')!;
  expect(reversed.faster).toBe('streamdown');
});

test('count rows report values without calling a winner', () => {
  const rows = buildRows(metrics({ frames: 4 }), metrics({ frames: 9 }));
  const frames = rows.find((row) => row.id === 'frames')!;
  expect(frames.delta).toBe(-5);
  expect(frames.faster).toBe('tie');
});

test('equal totals are a tie', () => {
  const comparison = buildComparison(
    metrics({ totalMs: 90 }),
    metrics({ totalMs: 90 })
  );
  expect(comparison.verdict).toBe('tie');
});

test('long frames are counted past the frame budget', () => {
  const rows = buildRows(
    metrics({ longFrames: 3, longestFrameMs: 41.5 }),
    metrics({ longFrames: 1, longestFrameMs: LONG_FRAME_MS })
  );
  expect(rows.find((row) => row.id === 'longFrames')!.downstream).toBe(3);
  expect(rows.find((row) => row.id === 'longestFrameMs')!.faster).toBe(
    'streamdown'
  );
});

test('text parity reports the first disagreement', () => {
  expect(compareText('hello', 'hello')).toEqual({
    match: true,
    sharedPrefix: 5,
    firstDifference: null,
  });
  expect(compareText('hello world', 'hello')).toEqual({
    match: false,
    sharedPrefix: 5,
    firstDifference: 5,
  });
  expect(compareText('hallo', 'hello')).toEqual({
    match: false,
    sharedPrefix: 1,
    firstDifference: 1,
  });
});

test('the comparison carries both timings and text parity', () => {
  const comparison = buildComparison(
    metrics({ totalMs: 70, source: '# hi' }),
    metrics({ totalMs: 130, source: '# hello' })
  );
  expect(comparison.verdict).toBe('downstream');
  expect(comparison.parity.match).toBe(false);
  expect(comparison.parity.firstDifference).toBe(3);
});

test('values render with their unit and keep the empty state visible', () => {
  expect(formatValue(12.34, 'ms')).toBe('12.3 ms');
  expect(formatValue(7, '')).toBe('7');
  expect(formatValue(null, 'ms')).toBe('—');
});
