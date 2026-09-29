import { useCallback, useEffect, useReducer, useRef } from 'react';

import { ComparisonRunner, type ComparisonConfig } from './comparisonRunner';

/**
 * Drives a comparison run. The runner owns the timers and the native sessions,
 * so a rerender can never restart or duplicate a run in flight.
 */
export function useComparison() {
  const runner = useRef<ComparisonRunner | null>(null);
  if (runner.current === null) runner.current = new ComparisonRunner();
  const [, rerender] = useReducer((count: number) => count + 1, 0);
  useEffect(() => {
    const instance = runner.current!;
    instance.subscribe(rerender);
    return () => {
      instance.subscribe(() => {});
      instance.dispose();
    };
  }, []);

  const run = useCallback((config: ComparisonConfig) => {
    runner.current?.run(config);
  }, []);
  const reset = useCallback(() => {
    runner.current?.reset();
  }, []);

  return { view: runner.current.getView(), run, reset };
}
