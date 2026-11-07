import type { MarkdownStreamController } from '@react-native-downstream/core';

export function resetSessionControllers(
  controllers: readonly Pick<MarkdownStreamController, 'reset'>[]
): void {
  controllers.forEach((controller) => controller.reset());
}

export function allFinalSnapshotsReady(
  readySequences: readonly number[],
  finalSequence: number
): boolean {
  return readySequences.every((sequence) => sequence >= finalSequence);
}

export function afterTwoAnimationFrames(callback: () => void): () => void {
  let cancelled = false;
  let frame = requestAnimationFrame(() => {
    if (cancelled) return;
    frame = requestAnimationFrame(() => {
      if (!cancelled) callback();
    });
  });
  return () => {
    cancelled = true;
    cancelAnimationFrame(frame);
  };
}
