// Audit reproduction: reports the observed baseline defect without patching it.
import { summarizeRun } from '../../../summarize.mjs';

const event = (sequence, name, ms) => ({
  sessionId: 'audit',
  generation: 0,
  sequence,
  event: name,
  clockDomain: 'native-monotonic',
  timeNs: String(ms * 1_000_000),
});

const mismatched = summarizeRun({
  streamEvents: [event(1, 'parse_start', 1), event(2, 'parse_end', 100)],
});
console.log(JSON.stringify({
  scenario: 'different snapshot start and end',
  expectedSamples: 0,
  actual: mismatched.parseMs,
}));

const overlapping = summarizeRun({
  streamEvents: [
    event(1, 'measure_start', 1),
    event(2, 'measure_start', 2),
    event(2, 'measure_end', 3),
    event(1, 'measure_end', 11),
  ],
});
console.log(JSON.stringify({
  scenario: 'overlapping measurement snapshots',
  expectedDurationsMs: [1, 10],
  actual: overlapping.measurementMs,
}));
