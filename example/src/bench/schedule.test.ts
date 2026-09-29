import { fixtureById } from './fixtures';
import { createSchedule, nextScheduledBatch } from './schedule';
import { createWorkload } from './workload';

const fixture = fixtureById('unicode-crlf');
const utf8Length = (text: string) =>
  encodeURIComponent(text).replace(/%[0-9A-F]{2}/g, 'x').length;

test('all schedules preserve the exact fixture source', () => {
  const workload = createWorkload(fixture, 'fixture');
  const original = createSchedule('manifest', 17, workload);
  expect(original.chunks.map((chunk) => chunk.text)).toEqual(fixture.chunks);

  for (const kind of ['character', 'range', 'bursty'] as const) {
    const schedule = createSchedule(kind, 17, workload);
    expect(schedule.chunks.map((chunk) => chunk.text).join('')).toBe(
      workload.source
    );
    expect(
      schedule.chunks.every((chunk) => !/[\uD800-\uDBFF]$/.test(chunk.text))
    ).toBe(true);
  }
});

test('generated workloads have exact UTF-8 byte sizes and stable schedules', () => {
  for (const size of [10, 50, 100] as const) {
    const workload = createWorkload(fixture, size);
    expect(utf8Length(workload.source)).toBe(size * 1024);
    expect(workload.bytes).toBe(size * 1024);
    expect(createWorkload(fixture, size).hash).toBe(workload.hash);
  }

  const workload = createWorkload(fixture, 10);
  const first = createSchedule('range', 20260928, workload);
  const again = createSchedule('range', 20260928, workload);
  expect(again).toEqual(first);
  expect(first.chunks.map((chunk) => chunk.text).join('')).toBe(
    workload.source
  );
  expect(
    first.chunks.every((chunk) => {
      const length = Array.from(chunk.text).length;
      return length >= 1 && length <= 20;
    })
  ).toBe(true);
});

test('normal chunk schedules use 10–50 ms arrivals at fixture and stress sizes', () => {
  for (const size of ['fixture', 10] as const) {
    const workload = createWorkload(fixture, size);
    for (const kind of ['manifest', 'character', 'range'] as const) {
      const schedule = createSchedule(kind, 20260928, workload);
      let previousDueMs = 0;
      for (const chunk of schedule.chunks) {
        const interval = chunk.dueMs - previousDueMs;
        expect(interval).toBeGreaterThanOrEqual(10);
        expect(interval).toBeLessThanOrEqual(50);
        previousDueMs = chunk.dueMs;
      }
    }
  }
});

test('open-fence workloads keep one unclosed fence at each target size', () => {
  for (const size of [10, 50, 100] as const) {
    const workload = createWorkload(fixture, size, 'open-fence');
    expect(utf8Length(workload.source)).toBe(size * 1024);
    expect(workload.source.startsWith('```text\n')).toBe(true);
    expect(workload.source.match(/```/g)).toHaveLength(1);
    expect(
      createSchedule('manifest', 17, workload)
        .chunks.map((chunk) => chunk.text)
        .join('')
    ).toBe(workload.source);
  }
});

test('late ticks still deliver each predefined batch in order', () => {
  const schedule = createSchedule(
    'character',
    17,
    createWorkload(fixture, 'fixture')
  );
  expect(schedule.batches.length).toBeGreaterThan(2);
  const overdueAt = schedule.durationMs + 1000;
  const delivered = schedule.batches.map((_, index) =>
    nextScheduledBatch(schedule.batches, index, overdueAt)
  );
  expect(delivered.map((next) => [next?.batch.start, next?.batch.end])).toEqual(
    schedule.batches.map((batch) => [batch.start, batch.end])
  );
  expect(delivered.every((next) => next && next.latenessMs > 0)).toBe(true);
  expect(schedule.batches[0]?.start).toBe(0);
  expect(schedule.batches.at(-1)?.end).toBe(schedule.chunks.length);
});
