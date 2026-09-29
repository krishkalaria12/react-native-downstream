// Host tests for the shared C++ Markdown engine.
//
// Builds the vendored md4c and the render-model engine with the system C/C++
// compiler, then checks final documents against the inline oracle.
//
//     node --test src/cpp/tests/engine.test.mjs

import assert from 'node:assert/strict';
import { execFile, execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { after, before, describe, test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

import { adversarialCases, cases } from './oracle.mjs';

const execFileAsync = promisify(execFile);

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const CPP = join(ROOT, 'src', 'cpp');
const MD4C = join(CPP, 'md4c');

const KIND = { paragraph: 0, heading: 1, code: 2, html: 3, hr: 4, table: 5 };
const LIST_ITEM = 1;
const TASK = { 0: null, 1: 'unchecked', 2: 'checked' };
const [STRONG, EMPHASIS, STRIKE, CODE, LINK, IMAGE_ALT, FOOTNOTE] = [
  0, 1, 2, 3, 4, 5, 6,
].map((bit) => 1 << bit);

// Mirrors the prefixes in Downstream.podspec and src/android/src/main/jni/CMakeLists.txt.
const SYMBOL_PREFIXES = [
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
];

let buildDir;
const binary = (name) => join(buildDir, name);

before(async () => {
  buildDir = mkdtempSync(join(tmpdir(), 'downstream-engine-'));
  const cc = process.env.CC ?? 'cc';
  const cxx = process.env.CXX ?? 'c++';
  const cFlags = ['-std=c11', '-O1', '-DMD4C_USE_UTF8', ...SYMBOL_PREFIXES];
  const cxxFlags = [
    '-std=c++20',
    '-O1',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-pthread',
    '-DMD4C_USE_UTF8',
    ...SYMBOL_PREFIXES,
    `-I${MD4C}`,
    `-I${CPP}`,
    `-I${join(CPP, 'downstream')}`,
    `-I${join(CPP, 'session')}`,
    `-I${join(ROOT, 'remend')}`,
  ];
  const [md4c, entity] = await Promise.all(
    ['md4c.c', 'entity.c'].map(async (source) => {
      const object = binary(`${source}.o`);
      await execFileAsync(cc, [
        ...cFlags,
        '-c',
        join(MD4C, source),
        '-o',
        object,
      ]);
      return object;
    })
  );
  const downstream = (name) => join(CPP, 'downstream', name);
  const session = (name) => join(CPP, 'session', name);
  const remend = (...parts) => join(ROOT, 'remend', ...parts);
  const testSource = (name) => join(CPP, 'tests', name);
  const engine = [
    ...['MarkdownParser.cpp', 'Unicode.cpp', 'LinkPolicy.cpp'].map(downstream),
    // The preview pass and the shared scanners it and the parser both use.
    ...['Scan.cpp', 'Compare.cpp'].map((name) => remend(name)),
  ];
  const pool = ['WorkerPool.cpp', 'RenderPreparer.cpp'].map(downstream);
  await Promise.all([
    execFileAsync(cxx, [
      ...cxxFlags,
      testSource('dump_document.cpp'),
      ...engine,
      md4c,
      entity,
      '-o',
      binary('dump_document'),
    ]),
    execFileAsync(cxx, [
      ...cxxFlags,
      testSource('test_incremental.cpp'),
      session('IncrementalParser.cpp'),
      downstream('SegmentPlan.cpp'),
      ...engine,
      md4c,
      entity,
      '-o',
      binary('test_incremental'),
    ]),
    execFileAsync(cxx, [
      ...cxxFlags,
      testSource('test_session_registry.cpp'),
      session('SessionRegistry.cpp'),
      session('IncrementalParser.cpp'),
      ...pool,
      ...engine,
      md4c,
      entity,
      '-o',
      binary('test_session_registry'),
    ]),
    execFileAsync(cxx, [
      ...cxxFlags,
      testSource('test_worker_pool.cpp'),
      session('SessionRegistry.cpp'),
      session('IncrementalParser.cpp'),
      downstream('PreparedDocument.cpp'),
      ...pool,
      ...engine,
      md4c,
      entity,
      '-o',
      binary('test_worker_pool'),
    ]),
  ]);
});

after(() => {
  if (buildDir) rmSync(buildDir, { recursive: true, force: true });
});

/** Parses a string or byte buffer and returns dump_document's JSON. */
function parse(source) {
  const input =
    typeof source === 'string' ? Buffer.from(source, 'utf8') : source;
  return JSON.parse(
    execFileSync(binary('dump_document'), {
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
        execFileSync(binary('test_incremental'), {
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
    'md4c.c':
      'e111ee600033f11dfb31e732429741f3c1406f2a3faffc0395d5f7dbb875e086',
    'md4c.h':
      'd0c14762f1eac3cd9d1c08c3a287b61d93ee2382e5a22ee48f5a6f3ad2c2349d',
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
        const document = parse(testCase.source);
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
            const document = parse(checkpoint.prefix);
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

describe('input', () => {
  // stderr is inherited, not piped: a failing CHECK writes its case label to
  // stderr, and a piped child would swallow the reason a run failed.
  test('native session versions and lifecycle', () => {
    execFileSync(binary('test_session_registry'), { timeout: 15_000 });
  });

  test('worker pool prepares and shares sessions', () => {
    execFileSync(binary('test_worker_pool'), { timeout: 30_000 });
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
