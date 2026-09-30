import { describe, test } from 'vitest';

import { checkedUtf8Bytes } from '../src/controlledSource';

// Measures validation and byte accounting only, without native subprocesses.
// Compare revisions with the same Node version and machine. This is not paint
// latency or the cost of sending a chunk through a production TurboModule.
describe('stream source validation', () => {
  for (const [name, source] of [
    ['token', 'A **streamed** token 👩‍💻'],
    ['ascii 64 KiB', 'x'.repeat(64 * 1024)],
    ['unicode 64 KiB', 'é漢👩‍💻'.repeat(4096)],
  ] as const) {
    test(name, async ({ bench }) => {
      await bench(name, () => checkedUtf8Bytes(source)).run();
    });
  }
});
