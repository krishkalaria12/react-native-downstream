#!/usr/bin/env node
// Writes src/cpp/downstream/BidiTable.cpp, the strong-direction ranges the layout
// direction rule reads. No build runs this: the output is checked in, and
// `testDirection` in `src/cpp/tests/test_export.cpp` pins the rule against the
// table rather than against this script.
//
//     node src/cpp/downstream/generate-bidi-table.mjs
//
// The source is V8's own Unicode database, through the two properties it
// exposes: `\p{Script=…}` to tell which script a code point belongs to, and
// `\p{General_Category=…}` for its class. Every code point up to the last plane
// is asked once, so the table is whatever the Node this runs under knows rather
// than a list someone retyped.
//
// # What this derives, and what it does not
//
// The rule below needs only the two strong classes, L and R-or-AL, and treats
// everything else as neutral. Node does not expose `Bidi_Class`, so this derives
// them the way a compact implementation does:
//
//   - **Right to left**: any code point in one of the right-to-left scripts below,
//     plus the presentation forms those scripts use. That is the whole of R and AL
//     for the character ranges that matter here; the two classes are counted the
//     same because the rule does not care which of them a character is, only that
//     it reads the other way.
//   - **Left to right**: any letter or letter number that no right-to-left script
//     claims, plus U+200E LEFT-TO-RIGHT MARK. Unicode also assigns L to a few
//     thousand non-letters — enclosed alphanumerics, letterlike symbols, and the
//     fullwidth forms — and those are the part of the real table this leaves out.
//     They are a rounding difference in a count of strong characters, never a
//     difference in which way a block reads, and no document in a chat is decided
//     by them.

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'BidiTable.cpp');

/**
 * The scripts Unicode gives right-to-left characters.
 *
 * Written out rather than derived, because it is the one thing this script has to
 * be right about and a reader can check it against DerivedBidiClass.txt in a
 * minute.
 *
 * Three of the right-to-left blocks Unicode defines — Arabic Supplement, Syriac
 * Supplement and the Arabic Mathematical Alphabetic Symbols — are not `Script`
 * values in V8, so they are not named here; their code point ranges are in
 * `RIGHT_TO_LEFT_EXTRA` below, which is where every presentation form and mark
 * without a script of its own is listed too.
 */
const RIGHT_TO_LEFT_SCRIPTS = [
  'Adlam',
  'Arabic',
  'Cypriot',
  'Hanifi_Rohingya',
  'Hebrew',
  'Mandaic',
  'Mende_Kikakui',
  'Nko',
  'Samaritan',
  'Syriac',
  'Thaana',
];

/** Presentation forms and marks no script property covers, by code point. */
const RIGHT_TO_LEFT_EXTRA = [
  [0x200f, 0x200f], // RIGHT-TO-LEFT MARK
  [0x202b, 0x202e], // embedding and override
  [0x2067, 0x2069], // isolates
  [0xfb1d, 0xfb4f], // Hebrew presentation forms
  [0xfb50, 0xfdff], // Arabic presentation forms A
  [0xfe70, 0xfeff], // Arabic presentation forms B
  [0x10800, 0x10cff], // Cypriot syllabary through Old Hungarian
  [0x1e800, 0x1e8df], // Mende Kikakui
  [0x1ec70, 0x1ecbf], // Indic Siyaq Numbers
  [0x1ed00, 0x1ed4f], // Ottoman Siyaq Numbers
  [0x1ee00, 0x1eeff], // Arabic Mathematical Alphabetic Symbols
];

const rtlScripts = RIGHT_TO_LEFT_SCRIPTS.map(
  (name) => new RegExp(`\\p{Script=${name}}`, 'u')
);
const rtlExtra = RIGHT_TO_LEFT_EXTRA.map(([from, to]) => [from, to]);

const isRightToLeft = (code) =>
  rtlExtra.some(([from, to]) => code >= from && code <= to) ||
  rtlScripts.some((pattern) => pattern.test(String.fromCodePoint(code)));

// V8's property escapes name exactly one property, so the two categories that
// count as a strong left-to-right character are two patterns rather than one
// alternation: `Letter` for every cased and uncased letter, `Letter_Number` for
// the four Roman-numeral-like code points Unicode classes with them.
const LETTER = [
  /\p{General_Category=Letter}/u,
  /\p{General_Category=Letter_Number}/u,
];
const LRM = 0x200e;

/** Collects code points into sorted, merged, non-overlapping ranges. */
function rangesOf(keep) {
  const ranges = [];
  let start = -1;
  let previous = -2;
  for (let code = 0; code <= 0x10ffff; code++) {
    // A surrogate is not a character a document can hold, and a range spanning one
    // would put code points in the table that nothing can ever ask about.
    const usable = !(code >= 0xd800 && code <= 0xdfff);
    // LRM is read first and only for the left-to-right table, so the two marks
    // that sit either side of each other in the bidi block cannot merge into one
    // range that belongs to both.
    const include =
      usable &&
      (keep === 'left' && code === LRM
        ? true
        : isRightToLeft(code)
          ? keep === 'right'
          : keep === 'left' &&
            LETTER.some((pattern) => pattern.test(String.fromCodePoint(code))));
    if (!include) {
      if (start >= 0) ranges.push([start, previous]);
      start = -1;
      continue;
    }
    if (start >= 0 && code === previous + 1) {
      previous = code;
      continue;
    }
    if (start >= 0) ranges.push([start, previous]);
    start = previous = code;
  }
  if (start >= 0) ranges.push([start, previous]);
  return ranges;
}

const left = rangesOf('left');
const right = rangesOf('right');

const table = (name, ranges) =>
  `const Range ${name}[] = {\n${ranges
    .map(([from, to]) => `    {0x${from.toString(16)}, 0x${to.toString(16)}},`)
    .join('\n')}\n};`;

const banner = [
  '// Generated by generate-bidi-table.mjs. Do not edit.',
  '//',
  '// The strong-direction ranges the layout direction rule reads, from V8 Unicode',
  `// ${process.versions.unicode}. Both tables are sorted and non-overlapping, which is`,
  '// what makes them a binary search rather than a scan. See the generator for',
  '// what is derived and what is approximated.',
  '',
  '#include "BidiTable.h"',
  '',
  '#include <algorithm>',
  '#include <iterator>',
  '',
  'namespace downstream {',
  '',
  'namespace {',
  '',
  '/// One inclusive range of code points, so the table is pairs rather than a',
  '/// bitmap: a bitmap of the whole space would be a megabyte of mostly zeros, and',
  '/// the search is a binary search over a few hundred pairs either way.',
  'struct Range {',
  '  uint32_t first;',
  '  uint32_t second;',
  '};',
  '',
].join('\n');

const tail = [
  '',
  '/// True when a code point is inside one of the ranges.',
  'bool inRanges(const Range *begin, const Range *end, uint32_t codePoint) {',
  '  const auto *found = std::upper_bound(',
  '      begin, end, codePoint, [](uint32_t value, const Range &range) { return value < range.first; });',
  '  return found != begin && found[-1].second >= codePoint;',
  '}',
  '',
  '} // namespace',
  '',
  'bool isStrongLeftToRight(uint32_t codePoint) {',
  '  return inRanges(std::begin(kLeftToRight), std::end(kLeftToRight), codePoint);',
  '}',
  '',
  'bool isStrongRightToLeft(uint32_t codePoint) {',
  '  return inRanges(std::begin(kRightToLeft), std::end(kRightToLeft), codePoint);',
  '}',
  '',
  '} // namespace downstream',
  '',
].join('\n');

writeFileSync(
  OUT,
  banner +
    table('kLeftToRight', left) +
    '\n\n' +
    table('kRightToLeft', right) +
    '\n' +
    tail
);
