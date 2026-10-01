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
    chunks: ['Do not open ', '[trap](', 'javascript:alert', '(1))', '.\n'], // eslint-disable-line no-script-url -- the target a renderer must refuse
  },
  {
    id: 'code-streaming',
    description: 'A long fence arriving in small chunks, the highlighting case',
    chunks: codeChunks(),
  },
  {
    id: 'code-chrome',
    description:
      'Fence meta arriving a few bytes at a time: a title, a start line, and the numbers a gutter draws',
    chunks: chromeChunks(),
  },
  {
    id: 'code-capped',
    description:
      'A fence past codeBlockMaxHeight, so its body scrolls inside the card',
    chunks: cappedChunks(),
  },
  {
    id: 'code-languages',
    description: 'One small fence per language the first release colors',
    chunks: languageChunks(),
  },
  {
    id: 'code-diff',
    description: 'A unified diff and a shell transcript in one document',
    chunks: [
      '```diff\n',
      '--- a/file.ts\n',
      '+++ b/file.ts\n',
      '@@ -1,4 +1,4 @@\n',
      '-const a = 1;\n',
      '+const a = 2;\n',
      '```\n',
      '\n',
      '```bash\n',
      'set -euo pipefail\n',
      'for f in *.txt; do echo "$f"; done\n',
      '```\n',
    ],
  },
] satisfies readonly Omit<Sample, 'source'>[];

/**
 * A code block split into small chunks. The tail is re-colored on every one, so
 * this is the sample that shows what resuming from the last complete line buys
 * against tokenizing the whole block again.
 */
function codeChunks(): string[] {
  const lines = [
    'export interface User {',
    '  id: number;',
    '  name: string;',
    '}',
    '',
    'export async function loadUser(id: number): Promise<User | null> {',
    '  const res = await fetch(`/api/users/${id}?v=${VERSION}`);',
    '  if (!res.ok) {',
    "    throw new Error('failed: ' + res.status);",
    '  }',
    '  return (await res.json()) as User;',
    '}',
    '',
    'const total = items',
    '  .filter((item) => item.active)',
    '  .reduce((sum, item) => sum + item.price, 0);',
  ];
  const chunks = ['```ts\n'];
  for (const line of lines) {
    // A few bytes at a time, so the tail is nearly always incomplete.
    for (let at = 0; at < line.length; at += 7) {
      chunks.push(line.slice(at, at + 7));
    }
    chunks.push('\n');
  }
  chunks.push('```\n');
  return chunks;
}

/**
 * A fence whose meta arrives a few bytes at a time, so every snapshot reparses a
 * partially typed info string. This is the sample that shows what parsing the meta
 * once, where the block is reported, costs against scanning it again per snapshot.
 */
function chromeChunks(): string[] {
  const lines = [
    'const start = line.offset;',
    'const end = line.start + line.length;',
    '',
    'if (start < 0 || end > text.length) {',
    '  throw new RangeError("out of range");',
    '}',
  ];
  // The opener is typed a few bytes at a time, which is the worst case for the
  // parser: every prefix of a half-written quote and brace.
  const opener = '```ts title="src/segments/lines.ts" startLine=120 {1,3-5}';
  const chunks: string[] = [opener.slice(0, 7)];
  for (let at = 7; at < opener.length; at += 6) {
    chunks.push(opener.slice(at, at + 6));
  }
  chunks.push('\n');
  for (const line of lines) {
    chunks.push(line + '\n');
  }
  chunks.push('```\n');
  return chunks;
}

/**
 * A fence long enough to pass the 400-point cap, so a card scrolls inside.
 *
 * The lines arrive in batches rather than one at a time: the benches replay every
 * earlier operation on each flush, so a few hundred single-line appends makes
 * them quadratic and measures the harness rather than the engine.
 */
function cappedChunks(): string[] {
  const chunks = ['```ts title="a/tall/block.ts"\n'];
  let batch = '';
  for (let index = 1; index <= 300; index++) {
    batch += `const value${index} = ${index} * 2;\n`;
    if (index % 20 === 0) {
      chunks.push(batch);
      batch = '';
    }
  }
  if (batch !== '') {
    chunks.push(batch);
  }
  chunks.push('```\n');
  return chunks;
}

/** One small fence per alias group, so a language table change is visible. */
function languageChunks(): string[] {
  const snippets: [string, string][] = [
    ['python', 'def greet(name: str) -> str:\n    return f"hi {name}"\n'],
    ['bash', 'echo "$HOME" # comment\n'],
    ['json', '{\n  "name": "x",\n  "n": 1\n}\n'],
    ['html', '<div class="a"><p>text</p></div>\n'],
    ['css', '.a > .b { color: red; }\n'],
    ['sql', 'SELECT a FROM t WHERE b = 1;\n'],
    ['rust', 'pub fn main() { println!("{}", 1); }\n'],
    ['yaml', 'key: value\n'],
    ['toml', 'a = "b"\n'],
    ['ruby', 'def hi(x)\n  puts x\nend\n'],
    ['lua', 'local function f() return 1 end\n'],
    ['diff', '--- a\n+++ b\n-a\n+b\n'],
    ['go', 'func main() { fmt.Println("hi") }\n'],
    ['md', '# Title\n\n*em* and `code`\n'],
  ];
  const chunks: string[] = [];
  for (const [info, code] of snippets) {
    chunks.push('```' + info + '\n');
    for (const line of code.split('\n').slice(0, -1)) {
      chunks.push(line + '\n');
    }
    chunks.push('```\n\n');
  }
  return chunks;
}

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
