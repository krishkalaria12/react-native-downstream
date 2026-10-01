/**
 * Whole documents swept through the incremental parser, one prefix at a time.
 *
 * `remend-corpus.ts` checks fragments and `oracle.mjs` checks named features.
 * This file checks the property in between: for a document chosen because its
 * tail reclassifies, no chunk boundary may change what a reader sees. Each
 * document is delivered one character at a time, in fixed blocks, and in sizes
 * that vary, because a bug that only appears at one boundary is a bug.
 *
 * Everything runs through the C++ engine, so a regression fails here rather
 * than on a device.
 */
import { describe, expect, it } from 'vitest';

import { assertStreamingBatchMatchesFullParse, parse } from './engine';
import {
  CHUNKINGS,
  chunk,
  CURATED_DOCUMENTS,
  generatedDocuments,
} from './incremental-documents';

/**
 * Generated documents. The count is the ported suite's; the generator is
 * seeded, so this is a fixed set rather than a new sample every run. Drawing
 * from 26 lines a few at a time collides often, so the set is deduplicated
 * rather than counted twice.
 */
const GENERATED = [...new Set(generatedDocuments(400))];

/**
 * A long enough paragraph to be committed before the document under test.
 *
 * Without this the documents here are small enough that the parser never proves
 * a boundary, so every update takes the full-parse path and the tail parser is
 * never exercised. The oracle reaches it the same way, and the two overlap on
 * purpose: this sweeps many documents, the oracle checks named features.
 */
const PREAMBLE =
  'Streaming answers arrive in small pieces, so the renderer must never trust ' +
  'a boundary it has not proven. This paragraph is long enough to commit.\n\n';

/** Every document, with the preamble in front so the tail parser runs. */
const COMMITTED = [...CURATED_DOCUMENTS, ...GENERATED].map(
  (doc) => PREAMBLE + doc
);

describe('documents whose tail reclassifies', () => {
  it('holds a document set worth sweeping', () => {
    // A silently emptied list would leave every assertion below vacuous.
    expect(CURATED_DOCUMENTS.length).toBeGreaterThan(40);
    expect(new Set(CURATED_DOCUMENTS).size).toBe(CURATED_DOCUMENTS.length);
    expect(GENERATED.length).toBeGreaterThan(100);
    // The preamble has to clear the parser's commit thresholds, or the sweep
    // below quietly degrades into a full-parse sweep.
    expect(PREAMBLE.length).toBeGreaterThan(128);
  });

  for (const { name, sizes } of CHUNKINGS) {
    it(`reaches a full parse at every prefix when chunked ${name}`, () => {
      const documents = COMMITTED.map((doc) => chunk(doc, sizes(doc)));
      assertStreamingBatchMatchesFullParse(documents);
    });
  }

  it('reaches a full parse at every prefix of the curated documents reversed', () => {
    // Delivery order is fixed, but a caller can replace a source wholesale. A
    // document built back to front is a different input, not a different
    // boundary, so it has to agree with a full parse too.
    assertStreamingBatchMatchesFullParse(
      CURATED_DOCUMENTS.map((doc) =>
        chunk([...(PREAMBLE + doc)].reverse().join(''), [1])
      )
    );
  });

  it('reaches a full parse at every prefix without the preamble', () => {
    // The same documents on the full-parse path. Kept separate from the sweep
    // above so a regression in one path cannot hide a regression in the other.
    const documents = [...CURATED_DOCUMENTS, ...GENERATED].map((doc) =>
      chunk(doc, [1])
    );
    assertStreamingBatchMatchesFullParse(documents);
  });

  it('parses every document without reporting an error', () => {
    const failures: string[] = [];
    for (const doc of COMMITTED) {
      const model = parse(doc);
      if (model.error) failures.push(`${JSON.stringify(doc)}: ${model.error}`);
    }
    expect(failures).toEqual([]);
  });

  it('offers only allowed schemes once a document is complete', () => {
    // A finished document is the one place a target is allowed to exist, so
    // this is where the link policy has to hold across every construct the
    // corpus reaches. The prefix policy covers the same rule mid-stream, and
    // `remend-corpus.ts` covers it on fragments.
    const failures: string[] = [];
    for (const doc of COMMITTED) {
      for (const link of parse(doc).links) {
        if (!/^(?:https?|mailto):/i.test(link.url)) {
          failures.push(`${JSON.stringify(doc)} -> ${link.url}`);
        }
      }
    }
    expect(failures).toEqual([]);
  });
});
