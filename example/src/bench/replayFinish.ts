/** Replay-only terminal delays never block another thread's chunk delivery. */
export function replayFinishes(
  delayMs: number,
  delay: (ms: number) => Promise<void>,
  finish: (thread: number) => void
) {
  const scheduled = new Set<number>();
  const pending: Promise<void>[] = [];
  const failures: unknown[] = [];
  return {
    schedule(thread: number) {
      if (scheduled.has(thread)) return;
      scheduled.add(thread);
      if (delayMs === 0) finish(thread);
      else
        pending.push(
          delay(delayMs)
            .then(() => finish(thread))
            .catch((error) => {
              // A thread can finish long before the input loop awaits aggregate done.
              // Observe failures immediately, then report them through that boundary.
              failures.push(error);
            })
        );
    },
    async done() {
      await Promise.all(pending);
      if (failures.length) throw failures[0];
    },
  };
}
