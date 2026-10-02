/**
 * The streaming reveal, against the real engine.
 *
 * Every snapshot here is the one the app shows and every unit comes from the
 * tracker the platforms draw with, so a property stated in this file is a
 * property of the shipped engine rather than of a test double's idea of what
 * counts as new.
 */
import { describe, expect, it } from 'vitest';

import { samples } from '../benchmarks/samples';
import { replayReveal, type RevealPass, type SessionOp } from './engine';

/**
 * Appends each chunk and parses after it, the way a stream does: one snapshot
 * per chunk rather than one per document. `finished` closes the stream, which
 * publishes one more snapshot.
 */
function steps(chunks: readonly string[], finished = false): SessionOp[] {
  const ops: SessionOp[] = [];
  for (const chunk of chunks) {
    ops.push({ op: 'append', chunk }, { op: 'parse', finished: false });
  }
  if (finished) ops.push({ op: 'parse', finished: true });
  return ops;
}

/** Every character the pass's units cover, as `[block, offset]`. */
function scheduled(pass: RevealPass): [number, number][] {
  return pass.units.flatMap(([block, start, end]) =>
    Array.from(
      { length: end - start },
      (_, index) => [block, start + index] as [number, number]
    )
  );
}

const MODES = ['full', 'incremental'] as const;

describe('reveal replay properties', () => {
  // The invariant that matters: a character drawn at full alpha never fades in
  // again. The tracker reports it, because a character's identity across two
  // snapshots is the alignment's answer and no offset-based check on this side
  // can tell a real re-reveal from the legitimate one where closing a `**` makes
  // the word it was styling new text in a different place.
  for (const sample of samples) {
    const chunkings: [string, readonly string[]][] = [
      ['as delivered', sample.chunks],
    ];
    // The exhaustive chunkings are the interesting ones for an aligner, and they
    // cost one parse per character, so the long samples replay only as delivered.
    if (sample.source.length <= 1024) {
      chunkings.push(
        ['one character at a time', [...sample.source]],
        [
          'one byte at a time',
          [...Buffer.from(sample.source, 'utf8')].map((b) =>
            String.fromCharCode(b)
          ),
        ]
      );
    }
    for (const mode of MODES) {
      for (const [label, chunks] of chunkings) {
        it(`${sample.id} never re-reveals a character, ${mode}, ${label}`, () => {
          const passes = replayReveal(mode, steps(chunks, true));
          expect(passes.length).toBeGreaterThan(1);
          const rereveals = passes.at(-1)!.rereveals;
          expect(
            rereveals,
            `${sample.id}: ${rereveals} characters faded twice`
          ).toBe(0);
        });
      }
    }
  }

  for (const mode of MODES) {
    // Every character the last snapshot shows is either faded, or text that was
    // already on screen and therefore legitimately never faded at all. A
    // character that is neither was never accounted for, which would leave a
    // reader looking at text nobody ever drew.
    it(`reveals or accounts for every character, ${mode}`, () => {
      for (const sample of samples) {
        const passes = replayReveal(mode, steps(sample.chunks, true));
        const faded = new Set<string>();
        for (const pass of passes) {
          for (const [block, offset] of scheduled(pass))
            faded.add(`${block}:${offset}`);
        }
        // The characters the very first snapshot put on screen. Whitespace and a
        // space a later snapshot's alignment absorbed count as old, which is
        // exactly what they are.
        const already = new Set<string>();
        for (const [block, text] of passes[0]!.blocks.entries()) {
          for (let offset = 0; offset < text.length; offset++) {
            already.add(`${block}:${offset}`);
          }
        }
        passes.at(-1)!.blocks.forEach((text, block) => {
          for (let offset = 0; offset < text.length; offset++) {
            // Whitespace is the one exception, and it is Streamdown's exception
            // too: a space rides on the word before it, so when that word was
            // already on screen the space came with it and never needed a span of
            // its own. A space draws nothing, so this is invisible either way.
            if (/\s/.test(text[offset]!)) continue;
            const key = `${block}:${offset}`;
            expect(
              faded.has(key) || already.has(key),
              `${sample.id}: ${key} was neither revealed nor already shown`
            ).toBe(true);
          }
        });
      }
    });

    // Start times never decrease along text order within one pass, so words
    // cascade forwards rather than appearing out of sequence.
    it(`start times run forwards within a pass, ${mode}`, () => {
      const passes = replayReveal(
        mode,
        steps(
          ['one two three four five six seven eight nine ten eleven twelve\n'],
          true
        )
      );
      const units = passes.flatMap((pass) => pass.units);
      expect(units.length).toBeGreaterThan(1);
      for (let index = 1; index < units.length; index++) {
        const previous = units[index - 1]!;
        const current = units[index]!;
        const follows =
          current[0] > previous[0] ||
          (current[0] === previous[0] && current[1] >= previous[1]);
        expect(follows).toBe(true);
        expect(current[3]).toBeGreaterThanOrEqual(previous[3]);
      }
    });
  }

  // The two parser modes reach the same document, so what they reveal is a
  // property of the text rather than of which parser produced the snapshot.
  it('both parsers reveal the same documents', () => {
    for (const sample of samples) {
      const text = (mode: (typeof MODES)[number]) =>
        replayReveal(mode, steps(sample.chunks, true))
          .map((pass) => pass.blocks.join('\u0000'))
          .join('\u0001');
      expect(text('incremental'), sample.id).toBe(text('full'));
    }
  });

  it('a finished snapshot says so and an open one does not', () => {
    const open = replayReveal('incremental', steps(['hello ', 'world']));
    expect(open.every((pass) => pass.streaming)).toBe(true);
    const closed = replayReveal(
      'incremental',
      steps(['hello ', 'world'], true)
    );
    expect(closed.at(-1)!.streaming).toBe(false);
  });

  // A reset clears what has been shown, so a new generation animates from its
  // first word rather than starting from the old one's tail.
  it('a reset fades the new source from its first word', () => {
    const passes = replayReveal('incremental', [
      { op: 'append', chunk: 'first message here' },
      { op: 'parse', finished: false },
      { op: 'replace', source: 'a different message' },
      { op: 'parse', finished: false },
    ]);
    expect(passes.length).toBe(2);
    // "a different message": each word with its trailing space, from the first.
    expect(passes[1]!.units.map((unit) => [unit[1], unit[2]])).toEqual([
      [0, 2],
      [2, 12],
      [12, 19],
    ]);
  });

  // A word split across two snapshots is one word, so the rest of it joins the
  // fade already running rather than restarting a cascade on half a word.
  it('a word split across snapshots keeps one fade', () => {
    // Two passes, because the whole word has to exist before the cascade ends.
    const passes = replayReveal('incremental', steps(['wor', 'ld']));
    // The first pass fades "wor" on its own.
    expect(passes[0]!.units).toEqual([[0, 0, 3, 50]]);
    // The second pass extends that same unit to the whole word rather than
    // restarting a cascade on "ld", so its start time is unchanged.
    expect(passes[1]!.units).toEqual([[0, 0, 5, 50]]);
  });

  // A settled message that a virtualized list scrolls back into view is seeded
  // from the session's record of what was shown, so it does not fade all over
  // again. Streamdown fades everything on remount; this is the deliberate
  // difference, and the property is that nothing in the earlier snapshot is
  // scheduled.
  it('a rebind seeds from what the session already showed', () => {
    const shown = 'a message that is already on screen';
    const fresh = replayReveal('incremental', [
      { op: 'append', chunk: shown },
      { op: 'parse', finished: false },
    ]);
    // A view that binds to a stream already in flight, and has been given the
    // session's record of what it showed before it bound.
    const rebound = replayReveal('incremental', [
      { op: 'seed', source: shown },
      { op: 'append', chunk: ' and then more text' },
      { op: 'parse', finished: false },
    ]);
    // A fresh mount fades the whole message, which is Streamdown's behaviour and
    // the thing a rebound view must not do.
    expect(fresh[0]!.units[0]![1]).toBe(0);
    // The rebound view's first unit is the new text, not the first word.
    expect(rebound.at(-1)!.units[0]![1]).toBe(shown.length + 1);
  });
});
