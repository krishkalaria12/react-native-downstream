// Run through fence-renderers.mjs: synthetic native slots, real production
// React DOM reconciliation. No parser subprocess or native layout runs here.
import React from 'react';
import { flushSync } from 'react-dom';
import { createRoot } from 'react-dom/client';
import { expect, it, vi } from 'vitest';
import {
  FenceSlots,
  resolveRenderers,
  useFenceSlots,
} from '../src/fenceRenderers';
import type { FenceSlot } from '../src/NativeDownstreamSession';
import type { CustomRendererProps } from '../src/types';

const native = vi.hoisted(() => ({
  slots: [] as readonly FenceSlot[],
  queries: 0,
  slotRenders: 0,
}));
vi.mock('../src/NativeDownstreamSession', () => ({
  default: {
    sessionFenceSlots() {
      native.queries++;
      return native.slots;
    },
  },
}));
vi.mock('../src/DownstreamFenceSlotNativeComponent', async () => {
  const { createElement } = await import('react');
  return {
    default: ({
      ordinal,
      children,
    }: {
      ordinal: number;
      children?: React.ReactNode;
    }) => {
      native.slotRenders++;
      return createElement('div', { 'data-ordinal': ordinal }, children);
    },
  };
});

let renders = 0;
const Chart = (props: CustomRendererProps) => {
  renders++;
  return <span>{props.code}</span>;
};
const resolved = resolveRenderers({
  renderers: [{ language: 'chart', component: Chart }],
});
const noFailure = () => {};

function View({ sequence }: { sequence: number }) {
  const slots = useFenceSlots({
    sessionId: 'bench',
    generation: 0,
    sequence,
    revision: sequence,
    parseFlags: 0,
    resolved,
  });
  return <FenceSlots slots={slots} scope="bench:0" onFailure={noFailure} />;
}

function rawSlots(count: number, frame: number): FenceSlot[] {
  return Array.from({ length: count + 1 }, (_, index) => ({
    ordinal: index,
    identity: index < count ? index + 1 : 0,
    language: 'chart',
    meta: '',
    code: index < count ? `[${index},2,3]` : `[${frame},`,
    incomplete: index === count,
  }));
}

it('measures settled fences and a changing tail', async () => {
  const validateOnly = process.env.DOWNSTREAM_BENCH_CHECK === '1';
  const frames = 32;
  const rounds = 4;
  const batchSize = 16;
  const yieldTask = () => new Promise<void>((done) => setTimeout(done, 0));
  const results = [];
  for (const count of [1, 64, 256]) {
    for (const changing of [false, true]) {
      const host = document.body.appendChild(document.createElement('div'));
      const root = createRoot(host);
      let serial = 0;
      const commit = (frame: number) => {
        native.slots = rawSlots(count, frame);
        flushSync(() =>
          root.render(<View sequence={changing ? ++serial : 0} />)
        );
      };
      try {
        renders = 0;
        native.queries = 0;
        commit(0);
        expect(host.querySelectorAll('[data-ordinal]')).toHaveLength(count + 1);
        const warmRenders = renders;
        // Verify memoization against fresh native objects with stable props.
        commit(1);
        expect(renders - warmRenders).toBe(changing ? 1 : 0);
        const run = async () => {
          let total = 0;
          for (let round = 0; round < rounds; round++) {
            for (let from = 0; from < frames; from += batchSize) {
              const start = performance.now();
              for (let frame = from; frame < from + batchSize; frame++)
                commit(frame);
              total += performance.now() - start;
              // A stream delivers later snapshots in later tasks. Yielding
              // also lets React reset its nested-update guard; the baseline's
              // key-change effect schedules state resets during flushSync.
              // Task waits are outside the summed commit-batch intervals.
              await yieldTask();
            }
          }
          return total;
        };
        await yieldTask();
        await run();
        const samples: number[] = [];
        const rendersBefore = renders;
        const queriesBefore = native.queries;
        const slotRendersBefore = native.slotRenders;
        if (!validateOnly) {
          for (let sample = 0; sample < 9; sample++) {
            samples.push(await run());
          }
        }
        const rendererCallsDuringSamples = renders - rendersBefore;
        const nativeQueriesDuringSamples = native.queries - queriesBefore;
        const slotComponentCallsDuringSamples =
          native.slotRenders - slotRendersBefore;
        // One deterministic final commit makes output checks independent of
        // warmup/sample counts. Stable mode intentionally retains frame zero.
        commit(42);
        const expectedText = rawSlots(count, changing ? 42 : 0)
          .map((slot) => slot.code)
          .join('');
        expect(host.textContent).toBe(expectedText);
        const sorted = [...samples].sort((a, b) => a - b);
        results.push({
          name: `${
            changing ? 'changing_tail' : 'unchanged_rerender'
          }_${count}_settled`,
          settledFences: count,
          commitsPerSample: validateOnly ? 0 : rounds * frames,
          commitsPerBatch: batchSize,
          fingerprint: expectedText,
          median_total_ms: sorted.length ? sorted[4] : 0,
          samples_total_ms: samples,
          rendererCallsDuringSamples,
          nativeQueriesDuringSamples,
          slotComponentCallsDuringSamples,
        });
      } finally {
        flushSync(() => root.unmount());
        host.remove();
      }
    }
  }
  console.log(`DOWNSTREAM_FENCE_RESULTS=${JSON.stringify(results)}`);
});
