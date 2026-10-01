/**
 * Whole documents chosen because appended text can change how the tail lexes.
 *
 * Where `remend-corpus.ts` holds small fragments a stream can stop on, these are
 * complete documents built from the shapes that make a chunk boundary dangerous:
 * a setext underline that reaches back over lines that look blank, a table whose
 * delimiter row arrives late, a fence that never closes, HTML that spans blank
 * lines, a heading marker that interrupts a list, and blocks that end only
 * because the next line takes them over.
 *
 * What they support is a single invariant: every prefix of a document must
 * render exactly as a full parse of that same prefix would. A chunk boundary is
 * a delivery detail and must never be visible.
 *
 * The line vocabulary and the seeded generator produce the awkward
 * combinations nobody wrote down by hand. The seed is fixed, so a failure names
 * the same document every time.
 *
 * Provenance: github.com/vercel/streamdown, the `parse-blocks-incremental`
 * suite in `packages/streamdown/__tests__`, Apache-2.0.
 */

/** Documents whose tail reclassifies when more text arrives. */
export const CURATED_DOCUMENTS: readonly string[] = [
  '# Heading\n\nParagraph one.\n\nParagraph two with **bold** and _em_.\n',
  'Setext heading\n===\n\nAnother\n---\n\nNot a heading\n\n===\n',
  '- item one\n- item two\n\n  continued paragraph\n\n- item three\n  - nested\n\n    nested paragraph\n',
  '1. first\n2. second\nlazy line\n\n3) third\n',
  '> quote\nlazy quote line\n\n> another\n>\n> - list in quote\n',
  'Paragraph\n    not code, lazy\n\n    indented code\n    more code\n\nafter\n',
  '```ts\nconst a = 1;\n\nconst b = 2;\n```\n\ntext\n\n~~~\nunclosed tilde fence\n',
  '```js\nconst x = 1;\n```\n\n```python\ny = 2\n```\n',
  'Some text\n\n```\nnever closed\n\nstill code\n',
  '<div>\n\ntext inside\n\n<div>nested</div>\n\n</div>\n\nafter div\n',
  '<details>\n<summary>Title</summary>\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n</details>\n',
  '<ai-thinking>\n\nthinking text\n\n</ai-thinking>\n\nreply\n',
  '<img src=x>\n\n<br/>\n\n<hr />\n\nparagraph\n',
  '$$\nx = 1\n$$\n\ntext\n\n$$\ny = 2\n=\n$$\n',
  'Math with split delimiters\n\n$$\na = b\n\n= c\n$$\n\nafter\n',
  '```sh\necho $$\n```\n\n$$\nz\n$$\n',
  '| a | b |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |\n\ntext after table\n',
  '| a | b |\ntext that makes it a paragraph\n\n| c | d |\n|---|---|\n',
  '* * *\n\n---\n\n___\n\n- - -\n',
  '[ref]: https://example.com\n\nSee [ref] and [^1].\n\n[^1]: footnote\n',
  'Line with trailing spaces  \nhard break\n\n\n\nmany blank lines\n\n\ntext\n',
  '\n\n\nleading blank lines\n\ntext\n',
  'Tabs\there\n\n\tindented with tab\n\n- item\n\n\ttab continued\n',
  'Windows\r\nline endings\r\n\r\n- list\r\n- items\r\n\r\n```\r\ncode\r\n```\r\n',
  'Old mac\rline endings\r\rparagraph\r',
  '# H1\n## H2\n### H3\nparagraph\n#not heading\n',
  'Term\n: not a definition list\n\n<!-- comment\nspanning\n-->\n\ntext\n',
  '<script>\nlet a = 1;\n</script>\n\n<pre>\npre text\n</pre>\n',
  'Intro\n\n1. step\n\n   ```\n   code in list\n   ```\n\n2. step two\n',
  'Emoji and unicode: 日本語のテキスト\n\n中文段落\n\n한국어 문단\n',
  'para\n#x\n\npara\n# heading\n\npara\n=\n\npara\n-x\n\npara\n- item\n',
  'para\n> quote\n#x\n\npara\n- item\n#x\n\n- item\n> quote\n#x\n',
  'para\n```\nfence after para\n```\npara\n***\npara\n<div>\nhtml\n',
  '- a\n- b\n1. c\n2) d\n+ e\n* f\n\n- g\n\n  h\n- i\n',
  '| a | b |\n|---|---|\n| 1 | 2 |\n#x\n| 3 | 4 |\n\n# real\n',
  '> a\n> b\n\n> c\nd\n=\n\n> e\n- f\n',
  'para\nfoo\n***\nbar\n=\n',
  'a\n***\nb\n***\nc\n***\nd\n=\n',
  'see [x]\n***\n[x]: /u\nz\n=\n',
  '- \n\ntext\n',
  'a\n\nb\n\npara\n***\nfoo\n=\nx\n\ny\n\nz\n',
];

/** Lines that interact awkwardly when they follow each other. */
export const LINE_SNIPPETS: readonly string[] = [
  'para',
  '***',
  '=',
  '-',
  '--',
  '# h',
  '#x',
  '- item',
  '1. one',
  '> q',
  '  ind',
  '    code',
  '\tx',
  '',
  '```',
  '~~~',
  '$$',
  '<div>',
  '</div>',
  '<br/>',
  '[x]: /u',
  'see [x]',
  '| a | b |',
  '|---|---|',
  '<!--',
  '-->',
];

/**
 * Builds `count` documents by drawing lines from `LINE_SNIPPETS`.
 *
 * A small LCG rather than `Math.random`, so a failure names the same document
 * every time. The seed is the one the ported suite used, which keeps the
 * generated set identical.
 */
export function generatedDocuments(count: number): string[] {
  let seed = 12_345;
  const next = () => {
    seed = (seed * 1_103_515_245 + 12_345) % 2_147_483_648;
    return seed;
  };
  const result: string[] = [];
  for (let i = 0; i < count; i += 1) {
    const lineCount = 2 + (next() % 8);
    const lines: string[] = [];
    for (let j = 0; j < lineCount; j += 1) {
      // The modulo keeps the index in range; a fallback would hide it drifting.
      lines.push(LINE_SNIPPETS[next() % LINE_SNIPPETS.length]!);
    }
    result.push(`${lines.join('\n')}\n`);
  }
  return result;
}

/** How a document is cut into the chunks a stream would deliver. */
export const CHUNKINGS: readonly {
  readonly name: string;
  readonly sizes: (doc: string) => number[];
}[] = [
  {
    name: 'one character',
    sizes: (doc) => Array.from({ length: doc.length }, () => 1),
  },
  {
    name: 'fixed size',
    sizes: (doc) => Array.from({ length: Math.ceil(doc.length / 5) }, () => 5),
  },
  {
    name: 'varying size',
    sizes: (doc) => {
      const sizes: number[] = [];
      let total = 0;
      let i = 0;
      while (total < doc.length) {
        const size = 1 + ((i * 7) % 11);
        sizes.push(size);
        total += size;
        i += 1;
      }
      return sizes;
    },
  },
];

/** Cuts a document into chunks of the given sizes, with any remainder last. */
export function chunk(doc: string, sizes: number[]): string[] {
  const chunks: string[] = [];
  let at = 0;
  for (const size of sizes) {
    if (at >= doc.length) break;
    chunks.push(doc.slice(at, at + size));
    at += size;
  }
  if (at < doc.length) chunks.push(doc.slice(at));
  return chunks;
}
