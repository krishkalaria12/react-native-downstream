/**
 * A stand-in for `react-native-worklets` in Node.
 *
 * Streamdown repairs a partial Markdown document on a worklet runtime before
 * rendering it. Node has no such runtime, and Streamdown creates one at import
 * time, so importing it under vitest throws before any benchmark runs.
 *
 * The worklet calls run inline instead. The repair work is the same, on the
 * same thread, which is what the comparison bench is measuring; only the thread
 * boundary is gone.
 */
import { vi } from 'vitest';

export const createWorkletRuntime = vi.fn(() => ({ __runtime: 'bench' }));

export function scheduleOnRuntime(
  fn: (...args: unknown[]) => unknown,
  ...args: unknown[]
): void {
  fn(...args);
}

export function scheduleOnRN(
  fn: (...args: unknown[]) => unknown,
  ...args: unknown[]
): void {
  fn(...args);
}
