/**
 * Markdown documents the benches replay, and the example's Bench and Testing
 * screens play back in turn. Chunk boundaries are the interesting part: a
 * renderer that only handles whole documents never sees a half-open fence or a
 * table that has not reached its separator row.
 */
export interface Sample {
  readonly id: string;
  readonly description: string;
  /** The document split into the pieces a stream delivers. */
  readonly chunks: readonly string[];
  readonly source: string;
}

const playback = [
  {
    id: 'emphasis',
    description: 'Open and closing emphasis across chunk boundaries',
    chunks: ['hello ', '**', 'wor', 'ld', '**', ' and _fri', 'ends_', '.\n'],
  },
  {
    id: 'links-images',
    description: 'Partial URL actions and image alt fallback',
    chunks: [
      'Read ',
      '[docs](',
      'https://exam',
      'ple.com/guide)',
      ' then ![diagram](',
      'https://example.com/dia',
      'gram.png)',
      '.\n',
    ],
  },
  {
    id: 'fenced-code',
    description: 'Open info string, progressive code, and closing fence',
    chunks: ['```ts\n', 'const x', ' = 1;\n', 'console.log(x);\n', '```', '\n'],
  },
  {
    id: 'gfm-table',
    description: 'Paragraph changes to a GFM table after separator',
    chunks: ['| A | B |', '\n', '| --- | --- |\n', '| one |', ' two |\n'],
  },
  {
    id: 'nested-tasks',
    description: 'Task states and an open nested emphasis span',
    chunks: [
      '- [',
      ' ] first\n',
      '  - nested **it',
      'em**\n',
      '- [x] sec',
      'ond\n',
    ],
  },
  {
    id: 'reference-setext',
    description: 'Setext reclassification and later reference definition',
    chunks: [
      'Guide\n',
      '-----\n',
      '\nRead [entry][ref].\n',
      '\n',
      '[ref]: https://exam',
      'ple.com/ref\n',
    ],
  },
  {
    id: 'unicode-crlf',
    description: 'Code point safe emoji, combining mark, RTL, and split CRLF',
    chunks: [
      'Hello \ud83d\udc69',
      '\u200d',
      '\ud83d\udcbb e',
      '\u0301 \u6f22',
      '\u5b57 \u05e9\u05dc\u05d5\u05dd\r',
      '\nNext line\r',
      '\n',
    ],
  },
  {
    id: 'raw-html-literal-math',
    description: 'GFM HTML oracle versus escaped display text; math literal',
    chunks: ['Raw <', 'b>bold</b>', ' and $x', '^2$ stay literal.\n'],
  },
  {
    id: 'unsafe-url',
    description: 'Complete disallowed link never becomes actionable',
    chunks: ['Do not open ', '[trap](', 'javascript:alert', '(1))', '.\n'],
  },
] satisfies readonly Omit<Sample, 'source'>[];

export const samples: readonly Sample[] = playback.map((sample) => ({
  ...sample,
  source: sample.chunks.join(''),
}));

export function sampleById(id: string): Sample {
  const sample = samples.find((candidate) => candidate.id === id);
  if (!sample) throw new Error(`Unknown sample: ${id}`);
  return sample;
}

/** A large document, built rather than stored, for throughput benches. */
export function syntheticDocument(sections: number): string {
  return `# Synthetic document

${Array.from(
  { length: sections },
  (_, index) =>
    `## Section ${index}

A paragraph with **bold**, *italic*, and \`code\`, plus a [link](https://example.com/${index}).

- One
- Two
  - Nested

\`\`\`typescript
const value = compute(${index});
\`\`\`

`
).join('')}`;
}
