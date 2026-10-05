// Host tests for the shared C++ Markdown engine.
//
// Builds the vendored md4c and the render-model engine with the system C/C++
// compiler, then checks final documents against the inline oracle.
//
//     node --test src/cpp/tests/engine.test.mjs

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { before, describe, test } from 'node:test';

import {
  CPP,
  ENGINE,
  MD4C_SOURCES,
  compile,
  prepareTargets,
} from './build.mjs';
import { fencedCases, highlightCases } from './highlight_fixtures.mjs';
import { adversarialCases, cases } from './oracle.mjs';

const MD4C = join(CPP, 'md4c');
const LEXILLA = join(CPP, 'lexilla');

const KIND = { paragraph: 0, heading: 1, code: 2, html: 3, hr: 4, table: 5 };
const LIST_ITEM = 1;
const TASK = { 0: null, 1: 'unchecked', 2: 'checked' };
const [STRONG, EMPHASIS, STRIKE, CODE, LINK, IMAGE_ALT, FOOTNOTE] = [
  0, 1, 2, 3, 4, 5, 6,
].map((bit) => 1 << bit);
/** Bits of the provisional pass, mirroring `src/cpp/remend/Preview.h`. */
const PREVIEW_HTML_TAGS = 1 << 5;
const PREVIEW_ALL = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | PREVIEW_HTML_TAGS;

/**
 * RaTeX's macOS slice, which `node math/scripts/vendor-ratex.mjs` writes. The
 * math engine test links it, and is skipped where it is not vendored: there is
 * no Linux slice in RaTeX's release.
 */
const RATEX_HOST = resolve(CPP, '..', '..', 'math', 'vendor', 'host', 'libratex_ffi.a');
const ratexVendored = process.platform === 'darwin' && existsSync(RATEX_HOST);

const targets = {};
const binary = (name) => compile(targets[name], name);

before(async () => {
  const downstream = (name) => join(CPP, 'downstream', name);
  const session = (name) => join(CPP, 'session', name);
  const testSource = (name) => join(CPP, 'tests', name);
  const engine = ENGINE;
  const pool = ['WorkerPool.cpp', 'RenderPreparer.cpp'].map(downstream);
  const incremental = session('IncrementalParser.cpp');
  const registry = session('SessionRegistry.cpp');
  const theme = ['Theme.cpp', 'ThemeLayout.cpp'].map((name) => join(CPP, 'theme', name));
  const executables = [
    ['dump_document', engine],
    [
      'test_incremental',
      [incremental, downstream('SegmentPlan.cpp'), ...engine],
    ],
    ['dump_session', [incremental, ...engine]],
    ['dump_export', [downstream('SegmentPlan.cpp'), ...engine]],
    ['test_export', [downstream('SegmentPlan.cpp'), ...engine]],
    ['test_fence_meta', [downstream('SegmentPlan.cpp'), ...engine]],
    ['test_reveal', [incremental, ...engine]],
    ['test_preview', [incremental, ...engine]],
    ['test_session_registry', [registry, incremental, ...pool, ...engine]],
    [
      'test_worker_pool',
      [
        registry,
        incremental,
        downstream('PreparedDocument.cpp'),
        ...pool,
        ...theme,
        ...engine,
      ],
    ],
    ['test_highlight', engine],
    ['dump_tokens', [downstream('Unicode.cpp')]],
    ['test_theme', theme],
    ['test_images', [downstream('ImageLayout.cpp')]],
    ['test_math', []],
    [
      'test_math_scan',
      ['MathScan.cpp', 'Scan.cpp', 'Compare.cpp'].map(downstream),
    ],
    [
      'test_math_cache',
      ['MathEngine.cpp', 'MathDisplayList.cpp', 'MathLayout.cpp'].map((name) =>
        join(CPP, 'math', name)
      ),
    ],
  ];
  if (ratexVendored) {
    executables.push([
      'test_math_engine',
      ['MathEngine.cpp', 'MathDisplayList.cpp', 'MathLayout.cpp']
        .map((name) => join(CPP, 'math', name))
        .concat(RATEX_HOST),
    ]);
  }
  for (const [name, dependencies] of executables) {
    targets[name] = [
      testSource(`${name}.cpp`),
      ...dependencies,
      ...MD4C_SOURCES,
    ];
  }
  await prepareTargets(targets);
});

/** Tokenizes one code block and returns dump_tokens' JSON. */
function tokens(info, source) {
  return JSON.parse(
    execFileSync(binary('dump_tokens'), {
      input: Buffer.from(`${info}\n${source}`, 'utf8'),
      maxBuffer: 64 << 20,
    }).toString('utf8')
  );
}

/** Parses a string or byte buffer and returns dump_document's JSON. */
function parse(source, parseFlags = 0) {
  const input =
    typeof source === 'string' ? Buffer.from(source, 'utf8') : source;
  return JSON.parse(
    execFileSync(binary('dump_document'), [String(parseFlags)], {
      input,
      maxBuffer: 64 << 20,
    }).toString('utf8')
  );
}

/** Joins non-table blocks the way the fixture manifest describes. */
function visibleText(document) {
  return document.blocks
    .filter((block) => block.kind !== KIND.table)
    .map((block) => block.text)
    .join('\n');
}

function stylesOf(block, substring) {
  const start = block.text.indexOf(substring);
  const end = start + substring.length;
  return block.runs.filter((run) => run[0] < end && run[1] > start);
}

// The prefix exceeds the minimum commit size, so the fixture streams as an
// active tail after committed prose.
const COMMITTED_PREFIX = Buffer.from(
  "A plain prefix paragraph, with punctuation! It's long enough to commit " +
    'before the fixture starts streaming, so the tail path is exercised.\n\n'
);

/** Streams each case's exact chunks through the incremental parser, which
 * checks every prefix against a full parse. */
async function assertIncrementalMatches(t, allCases) {
  for (const testCase of allCases) {
    await t.test(testCase.id, () => {
      const chunks = testCase.chunks.map((chunk) => Buffer.from(chunk, 'utf8'));
      const args = ['--batch'];
      if (testCase.parseFlags) args.push(String(testCase.parseFlags));
      for (const sequence of [chunks, [COMMITTED_PREFIX, ...chunks]]) {
        const framed = Buffer.concat(
          sequence.flatMap((chunk) => {
            const length = Buffer.alloc(4);
            length.writeUInt32LE(chunk.length);
            return [length, chunk];
          })
        );
        // stderr is inherited so a failing CHECK names its case; stdin stays
        // piped because the chunks arrive on it.
        execFileSync(binary('test_incremental'), args, {
          input: framed,
          stdio: ['pipe', 'pipe', 'inherit'],
        });
      }
    });
  }
}

describe('vendored md4c', () => {
  // The renderer is only as correct as the parser it wraps, so the vendored
  // copy is pinned by digest. A local edit to md4c is either a deliberate
  // security or bugfix patch, which must be recorded here on purpose, or an
  // accident. The revision and version live in src/cpp/md4c/README.md.
  const PINS = {
    // md4c.c and md4c.h carry two local patches, both recorded in
    // src/cpp/md4c/README.md and both behind a flag that is off by default:
    // MD_FLAG_LATEXMATHBLOCKS makes a `$$` line a fence, and
    // MD_FLAG_LATEXMATHNOSINGLE makes a single `$` never math.
    'md4c.c':
      '20ae89c5abf48e65608ed1e279f682b07d62dea1b1498c096c41d625aa3100a4',
    'md4c.h':
      '391b36c01aa734a67cc1c399c9c342913586a3112eecfb1a0f699ea2ef3b3a33',
    'entity.c':
      '7ea50d695d9210958e01b12b3e4d1db78d783d03d95b040fd7d2d9cfe1f1d39c',
    'entity.h':
      '99a8952be6629e4114947cb61a901bf4f73b10d84d1ac5e17ad8299dfbad708e',
  };

  for (const [name, digest] of Object.entries(PINS)) {
    test(`${name} is unmodified`, () => {
      assert.equal(createHash('sha256').update(readFileSync(join(MD4C, name))).digest('hex'), digest);
    });
  }

  test('no HTML renderer is vendored', () => {
    // md4c-html existed only to produce the deleted HTML oracle.
    assert.equal(existsSync(join(MD4C, 'md4c-html.c')), false);
    assert.equal(existsSync(join(MD4C, 'md4c-html.h')), false);
  });
});

describe('vendored Lexilla', () => {
  // The token colors a reader sees come out of these lexers, so a local edit is
  // either a deliberate upstream merge, which must be recorded here on purpose,
  // or an accident. The golden token fixtures above catch a change that alters
  // output; this catches one that does not.
  //
  // One digest over the whole tree rather than a hash per file: there are ~40
  // files, and a per-file table is a list nobody reads carefully. The digest is
  // taken over the sorted "path sha256" manifest, so it changes when a file is
  // edited, added, removed, or renamed. Regenerate with:
  //
  //   node -e 'const{createHash}=require("crypto"),{readdirSync,readFileSync}=require("fs");\
  //   const files=[];const walk=d=>{for(const n of readdirSync(d,{withFileTypes:true})){\
  //   const p=d+"/"+n.name;n.isDirectory()?walk(p):/\.(cxx|h)$/.test(n.name)&&files.push(p)}};\
  //   walk("src/cpp/lexilla");files.sort();\
  //   console.log(createHash("sha256").update(files.map(f=>f+" "+createHash("sha256")\
  //   .update(readFileSync(f)).digest("hex")+"\n").join("")).digest("hex"))'
  const LEXILLA_DIGEST =
    'f1da780dd34638a8933eb0f6924d337119efac15db7218ddb89c9c998f5526e8';

  function lexillaManifest() {
    const files = [];
    const walk = (dir) => {
      for (const entry of readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
        const path = join(dir, entry.name);
        if (entry.isDirectory()) {
          walk(path);
        } else if (/\.(cxx|h)$/.test(entry.name)) {
          files.push(path);
        }
      }
    };
    walk(LEXILLA);
    return files
      .map((path) => `${path.slice(LEXILLA.length + 1)} ${createHash('sha256').update(readFileSync(path)).digest('hex')}`)
      .join('\n');
  }

  test('the vendored lexers are unmodified', () => {
    assert.equal(createHash('sha256').update(lexillaManifest()).digest('hex'), LEXILLA_DIGEST);
  });

  test('the manifest is not empty', () => {
    // A digest over nothing would be stable while proving nothing.
    assert.ok(lexillaManifest().split('\n').length > 30);
  });
});

describe('parse oracle', () => {
  test('each case is internally consistent', async (t) => {
    for (const testCase of cases) {
      await t.test(testCase.id, () => {
        assert.deepEqual(
          Buffer.from(testCase.chunks.join(''), 'utf8'),
          Buffer.from(testCase.source, 'utf8')
        );
      });
    }
  });

  test('incremental matches full parse at every prefix', (t) =>
    assertIncrementalMatches(t, cases));

  test('final checkpoints', async (t) => {
    for (const testCase of cases) {
      await t.test(testCase.id, () => {
        const expected = testCase.checkpoints.at(-1);
        assert.deepEqual(
          Buffer.from(testCase.chunks.join(''), 'utf8'),
          Buffer.from(testCase.source, 'utf8')
        );
        const document = parse(testCase.source, testCase.parseFlags ?? 0);
        const blocks = document.blocks;

        if ('visibleText' in expected) {
          assert.equal(visibleText(document), expected.visibleText);
        }
        if ('enabledLinks' in expected) {
          assert.deepEqual(
            document.links.map((link) => link.url),
            expected.enabledLinks
          );
        }
        if ('imageRequests' in expected) {
          // The first release never fetches images.
          assert.deepEqual(expected.imageRequests, []);
        }
        if (expected.syntaxHighlighting === false) {
          assert.ok(blocks.every((block) => block.kind === KIND.code));
        }
        if ('provisionalStyles' in expected) {
          // Completed documents have no provisional styles.
          assert.deepEqual(expected.provisionalStyles, []);
        }
        if ('rawHtmlEscaped' in expected) {
          assert.ok(visibleText(document).includes('<b>bold</b>'));
        }
        if (expected.mathKind === 'literal') {
          assert.ok(visibleText(document).includes('$x^2$'));
        }
        if ('unicodeValid' in expected) {
          assert.ok(visibleText(document).includes('👩‍💻'));
        }

        const kind = expected.blockKind;
        if (kind === 'list') {
          assert.ok(
            blocks.every((block) =>
              block.containers.some((container) => container.kind === LIST_ITEM)
            )
          );
        } else if (kind === 'table') {
          const rows = blocks.filter((block) => block.kind === KIND.table);
          const cells = rows.map((row) =>
            row.cells.map(([start, end]) => row.text.slice(start, end))
          );
          assert.ok(rows[0].tableHeader);
          assert.deepEqual(cells[0], expected.headers);
          assert.deepEqual(cells.slice(1), expected.rows);
        } else if (kind !== undefined) {
          assert.deepEqual(
            blocks.map((block) => block.kind),
            [KIND[kind]]
          );
        }
        if ('headingLevel' in expected) {
          assert.equal(blocks[0].headingLevel, expected.headingLevel);
        }
        if ('taskStates' in expected) {
          const tasks = blocks.flatMap((block) =>
            block.containers
              .filter(
                (container) =>
                  container.kind === LIST_ITEM &&
                  container.showsMarker &&
                  container.task
              )
              .map((container) => TASK[container.task])
          );
          assert.deepEqual(tasks, expected.taskStates);
        }
      });
    }
  });
});

describe('adversarial cases', () => {
  // Streams whose later input changes, or seems to change, earlier blocks.
  test('each case is internally consistent', async (t) => {
    for (const testCase of adversarialCases) {
      await t.test(testCase.id, () => {
        assert.deepEqual(
          Buffer.from(testCase.chunks.join(''), 'utf8'),
          Buffer.from(testCase.source, 'utf8')
        );
      });
    }
  });

  test('every checkpoint', async (t) => {
    for (const testCase of adversarialCases) {
      for (const checkpoint of testCase.checkpoints) {
        await t.test(
          `${testCase.id} after chunk ${checkpoint.afterChunk}`,
          () => {
            const document = parse(checkpoint.prefix, testCase.parseFlags ?? 0);
            if ('visibleText' in checkpoint) {
              assert.equal(visibleText(document), checkpoint.visibleText);
            }
            if ('enabledLinks' in checkpoint) {
              assert.deepEqual(
                document.links.map((link) => link.url),
                checkpoint.enabledLinks
              );
            }
          }
        );
      }
    }
  });

  test('incremental matches full parse at every prefix', (t) =>
    assertIncrementalMatches(t, adversarialCases));
});

describe('render model', () => {
  test('inline styles', () => {
    const block = parse('a **b _c_** ~~d~~ `e`\n').blocks[0];
    assert.equal(block.text, 'a b c d e');
    assert.equal(stylesOf(block, 'b ')[0][2], STRONG);
    assert.equal(stylesOf(block, 'c')[0][2], STRONG | EMPHASIS);
    assert.equal(stylesOf(block, 'd')[0][2], STRIKE);
    assert.equal(stylesOf(block, 'e')[0][2], CODE);
  });

  test('hard and soft breaks', () => {
    assert.equal(parse('a\nb  \nc\\\nd\n').blocks[0].text, 'a\nb\nc\nd');
  });

  test('entities and null', () => {
    assert.equal(
      parse('&copy; &#65; &#x42; &bogus; &#0; a\0b\n').blocks[0].text,
      '© A B &bogus; � a�b'
    );
  });

  test('headings, code, and rules', () => {
    const document = parse(
      '# One\n\n###### Six\n\n---\n\n```py\nx\n\ny\n```\n\n    indented\n'
    );
    assert.deepEqual(
      document.blocks.map((block) => [block.kind, block.headingLevel]),
      [
        [1, 1],
        [1, 6],
        [4, 0],
        [2, 0],
        [2, 0],
      ]
    );
    assert.equal(document.blocks[3].text, 'x\n\ny');
    assert.equal(document.blocks[3].language, 'py');
    assert.equal(document.blocks[4].text, 'indented');
    assert.equal(document.blocks[0].spacing, 0);
  });

  test('HTML block is literal', () => {
    const block = parse('<div>\n<script>x()</script>\n</div>\n').blocks[0];
    assert.equal(block.kind, KIND.html);
    assert.equal(block.text, '<div>\n<script>x()</script>\n</div>');
  });

  test('list markers and spacing', () => {
    const blocks = parse('1. one\n2. two\n   - inner\n\n3) x\n').blocks;
    assert.deepEqual(
      blocks.map((block) => block.text),
      ['one', 'two', 'inner', 'x']
    );
    assert.equal(blocks[0].containers[0].marker, '1.');
    assert.equal(blocks[1].containers[0].marker, '2.');
    assert.equal(blocks[2].containers[0].showsMarker, false);
    assert.equal(blocks[2].containers[1].marker, '•');
    assert.equal(blocks[3].containers[0].marker, '3)');
    assert.deepEqual(
      blocks.map((block) => block.spacing),
      [0, 1, 1, 2]
    );
  });

  test('loose list and multi-paragraph item', () => {
    const blocks = parse('- a\n\n  more\n- b\n').blocks;
    assert.deepEqual(
      blocks.map((block) => block.spacing),
      [0, 2, 2]
    );
    assert.equal(blocks[0].containers[0].showsMarker, true);
    assert.equal(blocks[1].containers[0].showsMarker, false);
  });

  test('nested bullets change glyph', () => {
    const blocks = parse('- a\n  - b\n    - c\n      - d\n').blocks;
    assert.deepEqual(
      blocks.map((block) => block.containers.at(-1).marker),
      ['•', '◦', '‣', '‣']
    );
  });

  test('quotes nest', () => {
    const blocks = parse('> a\n>> b\n').blocks;
    assert.deepEqual(
      blocks.map((block) => block.containers.length),
      [1, 2]
    );
    assert.equal(blocks[0].containers[0].id, blocks[1].containers[0].id);
  });

  test('separate quotes have distinct IDs', () => {
    const blocks = parse('> a\n\n> b\n').blocks;
    assert.notEqual(blocks[0].containers[0].id, blocks[1].containers[0].id);
  });

  test('table cells and alignment', () => {
    const blocks = parse(
      '| a | b | c |\n|:--|:-:|--:|\n| 1 | **2** |\n'
    ).blocks;
    assert.equal(blocks[0].text, 'a\tb\tc');
    assert.deepEqual(
      blocks[0].cells.map((cell) => cell[2]),
      [1, 2, 3]
    );
    assert.equal(blocks[1].text, '1\t2\t');
    assert.deepEqual(
      blocks.map((block) => block.spacing),
      [0, 1]
    );
  });

  test('adjacent tables have distinct IDs', () => {
    const blocks = parse(
      '| a |\n| - |\n| 1 |\n\n| b |\n| - |\n\npara\n'
    ).blocks;
    assert.deepEqual(
      blocks.map((block) => block.table),
      [1, 1, 2, 0]
    );
  });

  test('admonition and footnotes', () => {
    const blocks = parse(
      '> [!WARNING]\n> careful\n\nA[^n].\n\n[^n]: *why*\n'
    ).blocks;
    assert.deepEqual(
      blocks.map((block) => block.text),
      ['Warning', 'careful', 'A1.', '', 'why']
    );
    assert.equal(blocks[0].containers[0].kind, 2);
    assert.deepEqual(blocks[2].runs, [[1, 2, FOOTNOTE, -1]]);
    assert.equal(blocks[3].kind, KIND.hr);
    assert.equal(blocks[4].containers[0].marker, '1.');
  });

  test('image alt text without a request', () => {
    const document = parse('![a *b*](https://x/y.png)\n');
    assert.equal(document.blocks[0].text, 'a b');
    assert.deepEqual(document.links, []);
    assert.ok(document.blocks[0].runs.every((run) => run[2] & IMAGE_ALT));
  });

  test('empty and whitespace sources', () => {
    assert.deepEqual(parse('').blocks, []);
    assert.deepEqual(parse('\n\n  \n').blocks, []);
  });
});

describe('links', () => {
  /** Ranges are UTF-16 offsets, which JavaScript strings index directly. */
  function assertLinkRanges(source, expected) {
    const document = parse(source);
    assert.deepEqual(
      document.links.map(({ range: [start, end] }) => source.slice(start, end)),
      expected
    );
    return document;
  }

  test('link policy', () => {
    const document = parse(
      '[a](https://ok) [b](HTTP://ok) [c](mailto:x@y) [d](javascript:x) [e](/rel) ' +
        '[f](data:x) [g](https:) [h](ftp://x) [i](custom:x)\n'
    );
    assert.deepEqual(
      document.links.map((link) => link.url),
      ['https://ok', 'HTTP://ok', 'mailto:x@y']
    );
    assert.equal(
      document.blocks[0].runs.filter((run) => run[2] & LINK).length,
      3
    );
  });

  test('inline link ranges', () => {
    assertLinkRanges(
      'x [a](https://a) [*b*](<https://b c> "t)") [c](https://c/(d)) y\n',
      ['[a](https://a)', '[*b*](<https://b c> "t)")', '[c](https://c/(d))']
    );
  });

  test('reference link ranges', () => {
    assertLinkRanges(
      '[full][r] [collapsed][] [r]\n\n[r]: https://r\n[collapsed]: https://c\n',
      ['[full][r]', '[collapsed][]', '[r]']
    );
  });

  test('autolink ranges', () => {
    assertLinkRanges('<https://a.b/c> www.example.com x@y.com\n', [
      '<https://a.b/c>',
      'www.example.com',
      'x@y.com',
    ]);
    assert.deepEqual(
      parse('www.example.com x@y.com\n').links.map((link) => link.url),
      ['http://www.example.com', 'mailto:x@y.com']
    );
  });

  test('nested image and escapes', () => {
    assertLinkRanges(
      '[![i](s)](https://a) [\\]x\\[](https://b) [`]`](https://c) [](https://d) ![](z)[e](https://e)\n',
      [
        '[![i](s)](https://a)',
        '[\\]x\\[](https://b)',
        '[`]`](https://c)',
        '[](https://d)',
        '[e](https://e)',
      ]
    );
  });

  test('ranges use UTF-16 offsets', () => {
    assertLinkRanges('👩‍💻 é [link](https://x) 漢\n', ['[link](https://x)']);
  });

  test('entity in URL', () => {
    assert.equal(
      parse('[a](https://x?a=1&amp;b=2)\n').links[0].url,
      'https://x?a=1&b=2'
    );
  });

  test('link in footnote is still actionable', () => {
    const document = assertLinkRanges(
      'a[^1]\n\n[^1]: see [docs](https://d)\n',
      ['[docs](https://d)']
    );
    assert.deepEqual(
      document.links.map((link) => link.url),
      ['https://d']
    );
  });
});

describe('syntax highlighting', () => {
  // The engine's own tests: the language table, the offsets, the limits, the
  // budget, and the streaming equivalence that a finished block's tokens equal
  // a one-shot pass's.
  test('engine invariants', () => {
    execFileSync(binary('test_highlight'), { timeout: 30_000, stdio: 'inherit' });
  });

  // One golden snapshot per alias group. A diff here is a diff in what a reader
  // sees, because a run's class is the only thing that decides its color.
  test('golden tokens', async (t) => {
    for (const testCase of highlightCases) {
      await t.test(testCase.id, (sub) => {
        sub.assert.snapshot(tokens(testCase.info, testCase.source).runs);
      });
    }
  });

  test('a parse colors the fenced blocks in the document', async (t) => {
    for (const testCase of fencedCases) {
      await t.test(testCase.id, (sub) => {
        sub.assert.snapshot(parse(testCase.source).blocks.map((block) => block.tokens));
      });
    }
  });

  test('a code block keeps its text and its link ranges', () => {
    // Highlighting changes colors, never text, so selection and copy are the
    // bytes the writer sent.
    const document = parse('```ts\nconst a: number = 1;\n```\n');
    assert.equal(document.blocks[0].text, 'const a: number = 1;');
    assert.ok(document.blocks[0].tokens.length > 0);
    assert.equal(document.blocks[0].runs.length, 0);
    const links = parse('```ts\n// see https://example.com\n```\n');
    assert.deepEqual(links.links, []);
  });

  test('a line comment that ends the block is colored to its last character', () => {
    // A fenced block's text never ends in a newline, so its last line is the
    // one a lexer closes at the end of the document. Rust and Bash take a line
    // comment's end from the lexer's line end, which once stopped a byte short.
    const comments = {
      rust: '// A comment',
      bash: '# A comment',
      ts: '// A comment',
      cpp: '// A comment',
      python: '# A comment',
      ruby: '# A comment',
      sql: '-- A comment',
      lua: '-- A comment',
      toml: '# A comment',
      yaml: '# A comment',
    };
    for (const [info, comment] of Object.entries(comments)) {
      const [block] = parse(`\`\`\`${info}\nlet x = 1\n${comment}\n\`\`\`\n`).blocks;
      assert.deepEqual(
        block.tokens.at(-1),
        [block.text.length - comment.length, block.text.length, 'comment', comment],
        info
      );
    }
  });

  test('an unknown language and an empty fence stay plain', () => {
    for (const info of ['brainfuck', '', '   ']) {
      assert.deepEqual(tokens(info, 'const a = 1;\n').runs, [], info);
    }
  });

  test('every run lands inside the block on a code point boundary', () => {
    for (const testCase of highlightCases) {
      const { text, runs } = tokens(testCase.info, testCase.source);
      let previous = 0;
      for (const [start, end, token, slice] of runs) {
        assert.ok(start < end && end <= text.length, `${testCase.id}: run inside the block`);
        assert.notEqual(token, 'plain', `${testCase.id}: plain text has no run`);
        assert.ok(start >= previous, `${testCase.id}: runs are sorted and do not overlap`);
        assert.equal(text.slice(start, end), slice, `${testCase.id}: the slice matches its offsets`);
        const before = start > 0 ? text.codePointAt(start - 2) : undefined;
        const after = end < text.length ? text.codePointAt(end) : undefined;
        assert.ok(
          !(before >= 0xd800 && before <= 0xdbff),
          `${testCase.id}: a run does not start between a surrogate pair`
        );
        assert.ok(
          !(after >= 0xdc00 && after <= 0xdfff),
          `${testCase.id}: a run does not end between a surrogate pair`
        );
        previous = end;
      }
    }
  });
});

describe('themes', () => {
  // The three generated halves of the slot table are checked in, so a slot added
  // to the JSON without regenerating would otherwise ship a build that cannot
  // read it. Regenerating into a scratch directory and comparing is what catches
  // that here rather than at the first themed frame.
  test('the slot table matches its source', () => {
    const scratch = mkdtempSync(join(tmpdir(), 'ds-slots-'));
    const android = join(CPP, '..', 'android', 'src', 'main', 'java', 'com', 'downstream');
    try {
      execFileSync(process.execPath, [join(CPP, 'theme', 'generate-theme-slots.mjs')], {
        env: { ...process.env, DS_THEME_SLOT_OUT: scratch },
        stdio: 'ignore',
      });
      const checkedIn = {
        'ThemeSlots.h': join(CPP, 'theme', 'ThemeSlots.h'),
        'themeSlots.ts': join(CPP, '..', 'themeSlots.ts'),
        'ThemeSlots.kt': join(android, 'ThemeSlots.kt'),
      };
      for (const [name, path] of Object.entries(checkedIn)) {
        assert.equal(
          readFileSync(join(scratch, name), 'utf8'),
          readFileSync(path, 'utf8'),
          `${name} is stale; run node src/cpp/theme/generate-theme-slots.mjs`,
        );
      }
    } finally {
      rmSync(scratch, { recursive: true, force: true });
    }
  });

  /**
   * Every slot a theme can set is read by something that draws.
   *
   * A slot nothing reads is not a harmless extra: `src/theme.ts` will happily
   * resolve it and send it, the theme will intern, the view will rebuild, and the
   * value will simply not appear. That is the worst shape a feature can fail in,
   * because it looks like it worked. So this reads the sources rather than a
   * hand-kept list, which is the only list that would be right.
   *
   * One direction on purpose: a read counted for the wrong slot is a false pass,
   * but a read missed is caught, and the readers are few and named in one place.
   */
  test('every slot is read by a renderer or by the shared rules', () => {
    const repo = resolve(CPP, '..', '..');
    const readers = [
      // The rules both platforms call, and the one token table both read.
      join(CPP, 'theme', 'ThemeLayout.cpp'),
      join(CPP, 'highlight', 'TokenPalette.h'),
      // Each platform's own drawing.
      join(repo, 'src', 'ios', 'DSRenderedDocument.mm'),
      join(repo, 'src', 'ios', 'DSSegmentViews.mm'),
      join(repo, 'src', 'ios', 'DSLayoutManager.mm'),
      join(repo, 'src', 'ios', 'DSThemeColors.mm'),
      join(repo, 'src', 'ios', 'DownstreamMarkdownView.mm'),
      join(repo, 'src', 'ios', 'DownstreamTextLayoutManager.mm'),
      join(repo, 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', 'RenderedDocument.kt'),
      join(repo, 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', 'NativeTheme.kt'),
      join(repo, 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', 'DownstreamMarkdownView.kt'),
    ].map((path) => readFileSync(path, 'utf8')).join('\n');

    // One form per way a slot can be named in the sources, so the comparison is
    // about the slot rather than about the spelling.
    const normalize = (name) =>
      name
        .replace(/^(SLOT_|kSlot|k)/, '')
        .replace(/(\d)/g, '_$1')
        .replace(/[^A-Za-z0-9]/g, '')
        .toUpperCase();
    const read = new Set();
    for (const match of readers.matchAll(/ThemeSlots\.SLOT_([A-Z0-9_]+)/g)) {
      read.add(normalize(match[1]));
    }
    for (const match of readers.matchAll(/Heading\.([A-Z_]+)/g)) {
      for (let level = 1; level <= 6; level++) {
        read.add(`HEADING${level}${match[1]}`);
      }
    }
    for (const match of readers.matchAll(/kSlot([A-Za-z0-9]+)/g)) {
      read.add(normalize(match[1]));
    }
    for (const match of readers.matchAll(/heading::(k[A-Za-z]+)/g)) {
      for (let level = 1; level <= 6; level++) {
        read.add(`HEADING${level}${match[1].slice(1).toUpperCase()}`);
      }
    }

    const source = JSON.parse(readFileSync(join(repo, 'src', 'theme', 'theme-slots.json'), 'utf8'));
    const slots = Array.isArray(source.slots)
      ? source.slots
      : Object.entries(source.slots).map(([name, value]) => ({ name, ...value }));
    const unread = slots
      .map((slot) => slot.name)
      .filter((name) => !name.endsWith('.dark'))
      .filter((name) => !read.has(normalize(name)));
    assert.deepEqual(unread, [], 'these theme slots are set but never drawn');
  });

  // The slot table's own invariants, the defaults a platform reads, margin
  // collapsing, the font scale cap, interning, and lookup by id. These are the
  // rules both renderers are built on, so they are checked in C++ rather than
  // through one platform's drawing.
  test('theme invariants', () => {
    execFileSync(binary('test_theme'), { timeout: 15_000, stdio: 'inherit' });
  });
});

describe('images', () => {
  test('finish preserves committed and tail image records', () => {
    const prefix =
      '[![Committed lake](https://example.com/lake.png "Lake")](https://example.com/page)\n\n' +
      'A settled paragraph. '.repeat(12) +
      '\n\n';
    const tail = '![Tail chart](https://example.com/chart.png "Chart")';
    const steps = [
      { op: 'append', chunk: prefix },
      { op: 'parse', finished: false },
      { op: 'append', chunk: tail },
      { op: 'parse', finished: false },
    ];
    const open = runSession('incremental', steps);
    assert.ok(open.settled > 0, 'the stream has committed its prefix');
    assert.equal(open.images.length, 2);
    const done = runSession('incremental', [
      ...steps,
      { op: 'parse', finished: true },
    ]);
    assert.deepEqual(done.images, open.images);
    assert.deepEqual(done.images, parse(prefix + tail).images);
    assert.deepEqual(
      done.blocks.map((block) => block.images),
      open.blocks.map((block) => block.images)
    );
    assert.equal(done.settled, done.blocks.length);
  });
  // The box rule, the state rule, and the URL policy. Both platforms are held to
  // this one table: `test_images` is the C++ side, and
  // `tests/android/ImageLayoutCheck.kt` runs the same cases against the Kotlin
  // mirror, so an image cannot be a different height on one platform.
  test('image box, state, and URL policy', () => {
    execFileSync(binary('test_images'), { timeout: 15_000, stdio: 'inherit' });
  });
});

describe('math', () => {
  test('bracket scanning preserves code and source offsets', () => {
    execFileSync(binary('test_math_scan'), { timeout: 15_000, stdio: 'inherit' });
  });

  test('math cache reuses successes and failures with bounded eviction', () => {
    execFileSync(binary('test_math_cache'), { timeout: 15_000, stdio: 'inherit' });
  });

  // The sizing both platforms apply once a layout exists. `test_math` is the C++
  // side, and `tests/android/MathLayoutCheck.kt` runs the same cases against the
  // Kotlin mirror, so a formula cannot be a different height on one platform.
  test('block and inline sizing', () => {
    execFileSync(binary('test_math'), { timeout: 15_000, stdio: 'inherit' });
  });

  // RaTeX itself, installed the way the math package installs it, read by the
  // decoder both platforms draw from.
  test(
    'RaTeX display lists decode',
    {
      skip: ratexVendored
        ? false
        : 'run node math/scripts/vendor-ratex.mjs on macOS',
    },
    () => {
      execFileSync(binary('test_math_engine'), {
        timeout: 30_000,
        stdio: 'inherit',
      });
    }
  );
});

// Parse option bits, mirroring `src/parseFlags.ts` and `downstream::ParseFlags`.
const INLINE_HTML = 1 << 3;
const SUPERSCRIPT_SYNTAX = 1 << 4;
const SUBSCRIPT_SYNTAX = 1 << 5;

const SUPERSCRIPT = 1 << 8;
const SUBSCRIPT = 1 << 9;

describe('inline html', () => {
  // The three tags on the allowlist, and the proof that nothing else moved.
  // Every case runs through the real parser with the inline HTML bit on, which
  // is the default a view sends, and again with it off, which must be today's
  // parse byte for byte.

  /** [source, visible text, [[substring, style mask], …]] over one paragraph. */
  const grammarCases = [
    ['H<sub>2</sub>O', 'H2O', [['2', SUBSCRIPT]]],
    ['x<sup>2</sup>', 'x2', [['2', SUPERSCRIPT]]],
    ['a<br>b', 'a\nb', []],
    ['a<br/>b', 'a\nb', []],
    ['a<br />b', 'a\nb', []],
    ['a<BR>b', 'a\nb', []],
    ['a</br>b', 'a\nb', []],
    ['a<br clear="all">b', 'a\nb', []],
    ['<sup class="x">y</sup>', 'y', [['y', SUPERSCRIPT]]],
    ['<sub id="y">z</sub>', 'z', [['z', SUBSCRIPT]]],
    // A stray end tag is swallowed, which is what an HTML parser does.
    ['a</sub>b', 'ab', []],
    ['a</sup>b', 'ab', []],
    // A self-closed script is swallowed, not opened: nothing means it.
    ['a<sub/>b', 'ab', []],
    ['a<sup />b', 'ab', []],
    // An unclosed tag runs to the end of its block.
    ['H<sub>2 open', 'H2 open', [['2 open', SUBSCRIPT]]],
    // An escaped opener leaves no open script, so the stray closer is swallowed.
    ['\\<sub>y</sub>', '<sub>y', []],
    // The innermost opened script wins.
    ['<sup>a<sub>b</sub>c</sup>d', 'abcd', [['a', SUPERSCRIPT], ['b', SUBSCRIPT], ['c', SUPERSCRIPT]]],
    // Nesting the same script twice does not shift further.
    ['<sup>a<sup>b</sup>c</sup>', 'abc', [['a', SUPERSCRIPT], ['b', SUPERSCRIPT], ['c', SUPERSCRIPT]]],
    // Markdown inside a script keeps working.
    ['<sup>**y**</sup>', 'y', [['y', SUPERSCRIPT | STRONG]]],
    ['<sub>*i*</sub>', 'i', [['i', SUBSCRIPT | EMPHASIS]]],
    // A name that merely starts with one is not one.
    ['<subtitle>n</subtitle>', '<subtitle>n</subtitle>', []],
    ['<supplier>n</supplier>', '<supplier>n</supplier>', []],
    ['<brx>a</brx>', '<brx>a</brx>', []],
    // Every other tag stays literal.
    ['<b>bold</b>', '<b>bold</b>', []],
    ['a <span>b</span>', 'a <span>b</span>', []],
    ['<!-- note -->', '<!-- note -->', []],
    // A code span is verbatim.
    ['`<sub>x</sub>`', '<sub>x</sub>', [['<sub>x</sub>', CODE]]],
  ];

  for (const [source, expected, runs] of grammarCases) {
    test(`grammar: ${JSON.stringify(source)}`, () => {
      const document = parse(source, INLINE_HTML);
      assert.equal(visibleText(document), expected);
      const block = document.blocks[0];
      assert.ok(block, 'one block');
      assert.deepEqual(
        runs.map(([substring, mask]) => [
          substring,
          stylesOf(block, substring).reduce((bits, run) => bits | run[2], 0) & mask,
        ]),
        runs
      );
    });
  }

  // The break table, one row per way a writer can put a `<br>` next to a source
  // line break. Downstream shows a soft break as a break, so a `<br>` beside one
  // must not open a blank line.
  const breakCases = [
    ['a<br>b', 'a\nb'],
    ['a<br>\nb', 'a\nb'],
    ['a\n<br>b', 'a\nb'],
    ['a\n<br>\nb', 'a\nb'],
    ['a<br><br>b', 'a\n\nb'],
    ['a<br>\n<br>\nb', 'a\n\nb'],
    ['a\n<br>\n<br>\nb', 'a\n\nb'],
    ['a  \n<br>b', 'a\n\nb'],
    ['a <br> b', 'a\nb'],
    // A final break draws no empty line; two leave one.
    ['a<br>', 'a'],
    ['a<br><br>', 'a\n'],
    ['a<br>\nb', 'a\nb'],
  ];

  for (const [source, expected] of breakCases) {
    test(`break: ${JSON.stringify(source)}`, () => {
      assert.equal(visibleText(parse(source, INLINE_HTML)), expected);
    });
  }

  test('a table cell holds a line break, and its spaces go away', () => {
    const source =
      '| Step | Notes |\n| - | - |\n| 1 | Mix <br> Then bake |\n';
    const document = parse(source, INLINE_HTML);
    const row = document.blocks.at(-1);
    assert.equal(row.kind, KIND.table);
    const cell = row.text.slice(row.cells[1][0], row.cells[1][1]);
    assert.equal(cell, 'Mix\nThen bake');
  });

  test('a block of only <br> tags is an empty paragraph, one line per tag', () => {
    const lone = parse('before\n\n<br>\n\nnext\n', INLINE_HTML);
    assert.equal(lone.blocks.length, 3);
    assert.equal(lone.blocks[1].kind, KIND.paragraph);
    // One tag is one empty line, which a paragraph with no text measures as one.
    assert.equal(lone.blocks[1].text, '');
    const pair = parse('before\n\n<br>\n<br>\n\nnext\n', INLINE_HTML);
    assert.equal(pair.blocks[1].text, '\n');
    const mixed = parse('before\n\n<br>\n<div>x</div>\n\nnext\n', INLINE_HTML);
    // One <br> beside a real tag is not a block of <br> tags, so the html
    // block keeps its literal text.
    assert.equal(mixed.blocks[1].kind, KIND.html);
    assert.ok(mixed.blocks[1].text.includes('<div>'));
  });

  test('a footnote reference keeps only its own bit inside a script', () => {
    const document = parse('<sup>[^1]</sup>\n\n[^1]: note\n', INLINE_HTML);
    const run = document.blocks[0].runs[0];
    assert.equal(run[2] & FOOTNOTE, FOOTNOTE);
    assert.equal(run[2] & SUPERSCRIPT, 0);
  });

  test('a script inside alt text keeps its text out of the alt', () => {
    const document = parse('![a<sup>2</sup>b](https://example.com/i.png)', INLINE_HTML);
    assert.equal(document.images[0].alt, 'a2b');
  });

  test('md4c script syntax is opt-in and off by default', () => {
    // `^x^` is only a superscript when the caller asked for it, because
    // `2^10 and 3^5` would otherwise pair the carets.
    assert.equal(visibleText(parse('2^10 and 3^5')), '2^10 and 3^5');
    // Probed, not imagined: with the flag on, md4c pairs the carets greedily,
    // so `2^10 and 3^5` becomes `2` plus a raised `10 and 3` plus `5`.
    assert.equal(
      visibleText(parse('2^10 and 3^5', INLINE_HTML | SUPERSCRIPT_SYNTAX)),
      '210 and 35'
    );
    // `~x~` is only a subscript when the caller asked for it, because remark
    // reads `~x~` as strikethrough.
    assert.equal(stylesOf(parse('~strike~').blocks[0], 'strike')[0][2], STRIKE);
    assert.equal(
      stylesOf(parse('~x~', INLINE_HTML | SUBSCRIPT_SYNTAX).blocks[0], 'x')[0][2],
      SUBSCRIPT
    );
  });

  test('inline HTML off reproduces the literal tags', () => {
    // The same sources the default parse handles, with the bit cleared: the
    // tags are visible text again, exactly as before this feature.
    assert.equal(visibleText(parse('a<br>b', 0)), 'a<br>b');
    assert.equal(visibleText(parse('H<sub>2</sub>O', 0)), 'H<sub>2</sub>O');
    assert.equal(visibleText(parse('a<br>b|c', 0)), 'a<br>b|c');
  });
});

describe('preview', () => {
  // The provisional pass, through the real session: a preview feature is only
  // worth having if the engine can prove it, and the proof runs the parser.
  const previewed = (source, features) =>
    runSession(
      'incremental',
      [
        { op: 'preview', features },
        { op: 'append', chunk: source },
        { op: 'parse', finished: false },
      ],
      INLINE_HTML
    );

  test('a half-arrived tag is hidden for as long as the stream stops there', () => {
    // Only the fragment itself is hidden: the complete tags before it are the
    // document, and the fragment returns the moment its `>` arrives.
    for (const [source, expected] of [
      ['Mix<br', 'Mix'],
      ['Mix<b', 'Mix'],
      ['H<sub>2</sub', 'H2'],
      ['x<sup>2</sup', 'x2'],
    ]) {
      assert.equal(visibleText(previewed(source, PREVIEW_HTML_TAGS)), expected);
    }
  });

  test('a finished stream is never previewed', () => {
    const document = runSession('incremental', [
      { op: 'preview', features: PREVIEW_ALL },
      { op: 'append', chunk: 'Mix<br' },
      { op: 'parse', finished: false },
      { op: 'parse', finished: true },
    ]);
    // The fragment comes back the moment the stream is settled, because the
    // settled document is always a plain parse of the bytes the writer sent.
    assert.equal(visibleText(document), 'Mix<br');
  });

  test('a fragment that cannot become an allowed tag is left alone', () => {
    // `<blink` runs past the name, and `<3` is a comparison; neither is a
    // prefix of `<br>`, `<sub>` or `<sup>`, so neither is ever hidden. A
    // candidate whose removal would change what precedes it is refused too: a
    // `<br>` before the fragment draws a line, and a document whose line
    // changed is not the document minus a fragment.
    for (const source of ['a<b and 2<3', 'a<blink', 'a<subtitle>x', 'a<em>']) {
      assert.equal(visibleText(previewed(source, PREVIEW_ALL)), source);
    }
    assert.equal(visibleText(previewed('a<br><su', PREVIEW_ALL)), 'a\n<su');
  });

  test('a fragment inside code is content', () => {
    // An unterminated code span keeps its content as code, and the fragment
    // lands inside that run, so the proof refuses to hide it: the reader is
    // looking at code, not at a tag.
    for (const source of ['`<br`', '`<b`', '`x<sup>2</sup`', '`<sup>1</sup`']) {
      assert.equal(
        visibleText(previewed(source, PREVIEW_HTML_TAGS)),
        source.slice(1, -1)
      );
    }
  });

  test('the feature is off unless asked for', () => {
    const document = runSession(
      'incremental',
      [
        { op: 'append', chunk: 'Mix<br' },
        { op: 'parse', finished: false },
      ],
      INLINE_HTML
    );
    assert.equal(visibleText(document), 'Mix<br');
  });
});

describe('input', () => {
  // stderr is inherited, not piped: a failing CHECK writes its case label to
  // stderr, and a piped child would swallow the reason a run failed.
  test('native session versions and lifecycle', () => {
    execFileSync(binary('test_session_registry'), { timeout: 15_000 });
  });

  test('worker pool prepares and shares sessions', () => {
    execFileSync(binary('test_worker_pool'), { timeout: 30_000 });
  });

  test('provisional styling for the unstable tail', () => {
    // Off by default, so nothing in the suites above exercises it. Its cases are
    // the ported remend corpus plus the session integration that proves a repair
    // cannot reach a committed block.
    execFileSync(binary('test_preview'), { timeout: 15_000 });
  });

  test('fence info string meta, prefixes, and identity', () => {
    execFileSync(binary('test_fence_meta'), { timeout: 30_000, stdio: 'inherit' });
  });

  test('reveal alignment, units, timeline, easing, and caret placement', () => {
    // The rules both platforms draw streamed text with, against the real parser,
    // so a fade cannot disagree between them or with what a reader sees.
    execFileSync(binary('test_reveal'), { timeout: 30_000, stdio: 'inherit' });
  });

  test('rejects malformed UTF-8', () => {
    assert.equal(parse(Buffer.from([0x61, 0xff])).error, 'invalid_input');
    assert.equal(parse(Buffer.from([0xed, 0xa0, 0x80])).error, 'invalid_input');
  });

  test('rejects oversized source', () => {
    assert.equal(
      parse(Buffer.alloc(1024 * 1024 + 1, 'a')).error,
      'input_limit'
    );
    assert.ok('blocks' in parse(Buffer.alloc(1024 * 1024, 'a')));
  });

  test('CRLF is normalized in visible text', () => {
    assert.equal(visibleText(parse('a\r\nb\r\n\r\nc\r\n')), 'a\nb\nc');
  });
});

/**
 * Replays a session through the real IncrementalParser and returns the document
 * its last pass published. `steps` mirror what a stream does, in order.
 */
function runSession(mode, steps, parseFlags = 0) {
  const frame = (op, payload) => {
    const length = Buffer.alloc(4);
    length.writeUInt32LE(payload.length);
    return Buffer.concat([Buffer.from(op, 'latin1'), length, payload]);
  };
  const frames = steps
    .map((step) => {
      switch (step.op) {
        case 'append':
          return frame('a', Buffer.from(step.chunk, 'utf8'));
        case 'replace':
          return frame('r', Buffer.from(step.source, 'utf8'));
        case 'preview': {
          const mask = Buffer.alloc(4);
          mask.writeUInt32LE(step.features >>> 0);
          return frame('p', mask);
        }
        case 'parse':
          return frame(step.finished ? 'f' : 'u', Buffer.alloc(0));
        default:
          throw new Error(`unknown step ${step.op}`);
      }
    });
  return JSON.parse(
    execFileSync(binary('dump_session'), [mode, String(parseFlags)], {
      input: Buffer.concat(frames),
      maxBuffer: 256 << 20,
    }).toString('utf8')
  );
}

describe('copy and download controls', () => {
  // stderr is inherited, not piped: a failing CHECK names the case it broke.
  test('export formats, file names, and segment plans', () => {
    execFileSync(binary('test_export'), { timeout: 30_000, stdio: 'inherit' });
  });

  /** Runs dump_export over `source` and returns its model plus its exports. */
  function exportOf(source, format, operations, separator = ',') {
    const model = JSON.parse(
      execFileSync(
        binary('dump_export'),
        [format, separator, 'snippet', 'file', ...operations],
        { input: Buffer.from(source, 'utf8'), maxBuffer: 256 << 20 }
      ).toString('utf8')
    );
    return { model, exports: model.exports ?? [] };
  }

  /**
   * The table rows in blocks [from, to), as text, alignment, and inline runs.
   * Runs are per block, so a cell's own styles are read inside it.
   */
  function tableShape(model, from = 0, to = Infinity) {
    return model.blocks
      .slice(from, to)
      .filter((block) => block.kind === KIND.table)
      .map((block) => ({
        header: block.tableHeader,
        cells: block.cells.map(([start, end, align]) => [
          block.text.slice(start, end),
          align,
        ]),
        runs: block.runs.map(([start, end, styles, link]) => [
          block.text.slice(start, end),
          styles,
          link,
        ]),
      }));
  }

  const linkUrls = (model) => model.links.map((link) => link.url);

  test('a Markdown export re-parses to the same table', async (t) => {
    // The round trip runs through the real parser, so a rule that only looks
    // right in the exporter cannot pass.
    const styled = [
      '| a | b | c | d |',
      '| :-- | :-: | --: | --- |',
      '| **x** | *y* | ~~z~~ | `code` |',
      '| [link](https://example.com) | plain | a_b | x \\| y |',
      '| <b>&amp;</b> | 1.5 | é 漢 | 👩‍💻 |',
    ].join('\n');

    const sources = {
      'a simple table': '| a | b |\n| - | - |\n| 1 | 2 |\n',
      'every alignment': '| a | b | c |\n| :-- | :-: | --: |\n| 1 | 2 | 3 |\n',
      'inline styles and a link': styled,
      // The cases a code span's fence has to be widened for. The bytes are
      // U+0060 repeated, spelled out because a template literal would read as
      // the span it is trying to describe.
      'a code span holding backticks': [
        '| a | b |',
        '| - | - |',
        `| ${'` `'.repeat(2)}${'`'}x${'`'}${'`'.repeat(2)} | ${'`'.repeat(3)}${' '.repeat(1)}${'`'.repeat(2)}y${'`'.repeat(2)}${'`'.repeat(3)} |`,
      ].join('\n'),
      'a short row': '| a | b | c |\n| - | - | - |\n| 1 |\n',
      'a wide table': `| ${'h'.repeat(40)} |\n| - |\n| ${'v'.repeat(80)} |\n`,
      'a table with a quote around it': '> | a | b |\n> | - | - |\n> | 1 | 2 |\n',
    };

    for (const [name, source] of Object.entries(sources)) {
      await t.test(name, () => {
        const { model, exports } = exportOf(source, 'markdown', ['r:0:8']);
        const round = exports[0].reparsed;
        assert.equal(round.error, undefined, 'the export re-parses');
        assert.deepEqual(tableShape(round), tableShape(model), name);
        assert.deepEqual(linkUrls(round), linkUrls(model), `${name}: link targets`);
      });
    }
  });

  test('every table in the oracle corpus round trips', async (t) => {
    for (const testCase of [...cases, ...adversarialCases]) {
      const source = testCase.source;
      if (!source.includes('| ---')) continue;
      await t.test(testCase.id, () => {
        const first = parse(source).blocks.findIndex(
          (block) => block.kind === KIND.table
        );
        assert.ok(first >= 0, `${testCase.id}: has a table`);
        const last = parse(source).blocks.findLastIndex(
          (block) => block.kind === KIND.table
        );
        const { model, exports } = exportOf(
          source,
          'markdown',
          [`r:${first}:${last + 1}`]
        );
        const round = exports[0].reparsed;
        assert.equal(round.error, undefined, `${testCase.id}: re-parses`);
        assert.deepEqual(
          tableShape(round),
          tableShape(model, first, last + 1),
          `${testCase.id}: the table survives`
        );
      });
    }
  });

  test('copy ignores layout', () => {
    // A wrap toggle and `codeBlockWrap` change how a block is laid out, never
    // what a copy puts on the clipboard, so the exporter has no layout input
    // and cannot drift from the reader's text.
    const source = '```ts\nconst a = 1;\nconst b: number = 2;\n```\n';
    const first = exportOf(source, 'text', ['e:0:1']).exports[0].text;
    const second = exportOf(source, 'text', ['e:0:1']).exports[0].text;
    assert.equal(first, second);
    assert.equal(first, 'const a = 1;\nconst b: number = 2;');
  });

  test('a fence info string is kept whole for a file name', () => {
    const source =
      '```ts title="app.ts"\nx\n```\n\n```ts:src/lib.ts\ny\n```\n\n    indented\n';
    const document = parse(source);
    assert.equal(document.blocks[0].info, 'ts title="app.ts"');
    assert.equal(document.blocks[0].language, 'ts');
    assert.equal(document.blocks[1].info, 'ts:src/lib.ts');
    // The third block is indented code, which has no language and no name.
    assert.equal(document.blocks[2].info, '');
    const { exports } = exportOf(source, 'text', [
      'd:0',
      'd:1',
      'd:2',
      't:csv',
      't:md',
    ]);
    assert.deepEqual(
      exports.map((entry) => entry.text),
      // The configured base is `snippet` and the fallback is `file`.
      ['app.ts', 'lib.ts', 'snippet.txt', 'snippet.csv', 'snippet.md']
    );
  });

  test('a controlled document settles every block', () => {
    const source = '# One\n\n| a |\n| - |\n| 1 |\n\n```ts\nx\n```\n';
    const document = parse(source);
    assert.equal(document.settled, document.blocks.length);
  });

  test('an open stream settles only what it can prove', () => {
    // The same source, one chunk at a time, through the real parser. A control
    // may act on a block inside the settled count and on nothing beyond it.
    const chunks = [
      'A first paragraph long enough to be worth committing on its own line.\n\n',
      '| a | b |\n| --- | --- |\n| 1 | 2 |\n\n',
      '```ts\nconst a = 1;\n',
      'const b = 2;\n```\n',
    ];
    let source = '';
    let settled = 0;
    for (const chunk of chunks) {
      source += chunk;
      const model = runSession('incremental', [
        { op: 'replace', source: '' },
        { op: 'append', chunk: source },
        { op: 'parse', finished: false },
      ]);
      assert.equal(model.error, undefined);
      // Settling never goes backwards within a generation.
      assert.ok(
        model.settled >= settled,
        `settled ${model.settled} >= ${settled}`
      );
      settled = model.settled;
      assert.ok(
        settled <= model.blocks.length,
        'settled never covers a block that does not exist'
      );
      for (let index = 0; index < model.blocks.length; index++) {
        if (index < model.settled) {
          continue;
        }
        // Anything past the settled prefix is the tail, so nothing there is
        // claimed to be final.
        assert.ok(index >= settled);
      }
    }
    const finished = runSession('incremental', [
      { op: 'replace', source: '' },
      { op: 'append', chunk: source },
      { op: 'parse', finished: true },
    ]);
    assert.equal(finished.settled, finished.blocks.length);
  });

  test('full mode settles nothing until the stream finishes', () => {
    const source = 'A paragraph.\n\nAnother paragraph.\n';
    const open = runSession('full', [
      { op: 'append', chunk: source },
      { op: 'parse', finished: false },
    ]);
    assert.equal(open.settled, 0);
    const done = runSession('full', [
      { op: 'append', chunk: source },
      { op: 'parse', finished: true },
    ]);
    assert.equal(done.settled, done.blocks.length);
  });
});

// Fence renderers: the slots the engine hands JavaScript, and the rule that
// decides which fenced blocks are candidates at all.
describe('fence slots', () => {
  /** Parses `source` and returns the slots for `languages`. */
  const slotsFor = (source, languages, { mathFlags = 0, live = true } = {}) => {
    const output = execFileSync(
      compile(
        [join(CPP, 'tests', 'dump_fence_slots.cpp'), ...ENGINE, ...MD4C_SOURCES],
        'downstream-fence-slots'
      ),
      [String(mathFlags), live ? '1' : '0', languages.join(',')],
      { input: Buffer.from(source, 'utf8') }
    ).toString('utf8');
    return JSON.parse(output).slots;
  };

  test('a matching fence becomes one slot with the renderer props', () => {
    const slots = slotsFor('```mermaid\ngraph TD;\nA-->B;\n```\n', ['mermaid']);
    assert.equal(slots.length, 1);
    assert.deepEqual(slots[0], {
      ordinal: 0,
      blockIndex: 0,
      language: 'mermaid',
      meta: '',
      // Exactly what the code block's copy button copies: no fence markers.
      code: 'graph TD;\nA-->B;',
      incomplete: false,
      identity: 0,
    });
  });

  test('a language matches case-insensitively, as written', () => {
    for (const written of ['mermaid', 'Mermaid', 'MERMAID']) {
      const slots = slotsFor(`\u0060\u0060\u0060${written}\nx\n\u0060\u0060\u0060\n`, [
        'mermaid',
      ]);
      assert.equal(slots.length, 1, `${written} matches mermaid`);
      assert.equal(slots[0].language, written, 'the language is as written');
    }
  });

  test('the language is cut at a colon and the meta is what follows', () => {
    const slots = slotsFor(
      '```chart title="Revenue" months=12\n{"a":1}\n```\n',
      ['chart']
    );
    assert.equal(slots[0].language, 'chart');
    assert.equal(slots[0].meta, 'title="Revenue" months=12');
    const bare = slotsFor('```chart\n{"a":1}\n```\n', ['chart']);
    assert.equal(bare[0].meta, '', 'no meta is an empty string');
  });

  test('ordinals count every code block, registered or not', () => {
    const slots = slotsFor(
      '```ts\nconst a = 1;\n```\n\n```mermaid\ngraph;\n```\n',
      ['mermaid']
    );
    assert.equal(slots.length, 1);
    // The `ts` block ahead of it is a code block whether or not a renderer was
    // registered for it, so the fence a renderer hosts is the second one: the
    // same number the wrap and expand choices are keyed by.
    assert.equal(slots[0].ordinal, 1);
    assert.equal(slots[0].blockIndex, 1);
  });

  test('fences inside containers are candidates', () => {
    for (const source of [
      '- ```mermaid\n  graph;\n  ```\n',
      '1. ```mermaid\n   graph;\n   ```\n',
      '> ```mermaid\n> graph;\n> ```\n',
      '>> ```mermaid\n>> graph;\n>> ```\n',
      '~~~mermaid\ngraph;\n~~~\n',
    ]) {
      const slots = slotsFor(source, ['mermaid']);
      assert.equal(slots.length, 1, JSON.stringify(source));
      assert.equal(slots[0].code, 'graph;', JSON.stringify(source));
      assert.equal(slots[0].incomplete, false, JSON.stringify(source));
    }
  });

  test('what is never a candidate', () => {
    assert.deepEqual(slotsFor('    indented code\n', ['mermaid']), []);
    assert.deepEqual(slotsFor('```mermaid\n```\n', ['js']), []);
    // A fence whose info line has not ended is not a fence yet: it is still
    // growing into one.
    assert.deepEqual(slotsFor('```merm', ['mermaid']), []);
    assert.deepEqual(slotsFor('```merm\n', ['mermaid']), []);
    // A ```math fence is a formula block either way, exactly as Streamdown treats
    // it: remark-math claims the language before the code component runs, so the
    // engine never reports it as a code block and no renderer can be given one.
    assert.deepEqual(slotsFor('```math\nx^2\n```\n', ['math'], { mathFlags: 1 }), []);
    assert.deepEqual(slotsFor('```math\nx^2\n```\n', ['math'], { mathFlags: 0 }), []);
    // The raw info word is matched, so an entity-encoded language is neither
    // matched nor mistaken for the decoded one.
    assert.deepEqual(
      slotsFor('```mer&#x61;id\ngraph;\n```\n', ['mermaid']),
      []
    );
  });

  test('completeness follows the fence, not the document', () => {
    const cases = [
      ['```mermaid\ngraph;\n', true, 'open at the end of the source'],
      ['```mermaid\ngraph;\n```\n', false, 'closed by its own fence'],
      ['```mermaid\ngraph;\n````\n', false, 'closed by a longer fence'],
      ['```mermaid\ngraph;\n``\n', true, 'a shorter fence does not close it'],
      ['```mermaid\ngraph;\n~~~\n```\n', false, 'a different character runs on as code'],
      ['```mermaid\ngraph;\n  ```\n', false, 'a closing fence may be indented three spaces'],
      ['- ```mermaid\n  graph;\n  ```\n', false, 'a fence ended by its list item is closed'],
    ];
    for (const [source, incomplete, label] of cases) {
      const slots = slotsFor(source, ['mermaid']);
      assert.equal(slots.length, 1, label);
      assert.equal(slots[0].incomplete, incomplete, label);
    }
  });

  test('a settled stream reads a never-closed fence as final', () => {
    const source = '```mermaid\ngraph;\n';
    assert.equal(slotsFor(source, ['mermaid'], { live: true })[0].incomplete, true);
    // The same fence after `finish()`: settled, so the renderer is told the
    // spec is complete even though the model never closed the fence.
    assert.equal(slotsFor(source, ['mermaid'], { live: false })[0].incomplete, false);
  });

  test('several fences, and one registered language', () => {
    const slots = slotsFor(
      '```js\nconst a = 1;\n```\n\n```mermaid\ngraph;\n```\n\n```ts:a.ts\nconst b = 2;\n```\n',
      ['mermaid']
    );
    assert.deepEqual(
      slots.map((slot) => slot.ordinal),
      [1]
    );
    const both = slotsFor(
      '```js\nconst a = 1;\n```\n\n```mermaid\ngraph;\n```\n',
      ['js', 'mermaid']
    );
    assert.deepEqual(
      both.map((slot) => [slot.ordinal, slot.language]),
      [
        [0, 'js'],
        [1, 'mermaid'],
      ]
    );
  });

  test('an empty language list matches nothing', () => {
    assert.deepEqual(slotsFor('```mermaid\ngraph;\n```\n', []), []);
  });

  test('the normalized language is what highlighting normalizes', () => {
    // `lang:path` and a space both cut the word, and the case does not matter.
    const slots = slotsFor('```MERMAID:src/x.mmd title="t"\ngraph;\n```\n', [
      'mermaid',
    ]);
    assert.equal(slots.length, 1);
    assert.equal(slots[0].language, 'MERMAID', 'as written, cut at the colon');
  });
});
