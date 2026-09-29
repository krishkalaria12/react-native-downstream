import type { MarkdownStreamController } from 'react-native-downstream';

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
