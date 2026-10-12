// Executes production document/paragraph caching and hosted fallback with
// host counters. Android rasterization and DynamicLayout shaping are excluded.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const sources = join(here, '../../src/android/src/main/java/com/downstream');
function method(file, name, indentation) {
  const source = readFileSync(join(sources, file), 'utf8');
  const pattern = new RegExp(
    `^${' '.repeat(indentation)}(?:private )?fun ${name}\\([\\s\\S]*?^${' '.repeat(indentation)}}`,
    'm'
  );
  const found = source.match(pattern)?.[0];
  if (!found) throw new Error(`Missing production ${name}`);
  return found;
}
let fixture = readFileSync(join(here, 'TextStreamingCheck.kt'), 'utf8');
for (const [marker, value] of [
  ['PRODUCTION_OBTAIN', method('RenderedDocument.kt', 'obtain', 4)],
  [
    'PRODUCTION_ELIGIBILITY',
    method('NativeDocument.kt', 'isPlainParagraph', 2),
  ],
  [
    'PRODUCTION_PARAGRAPH_KEY',
    method('RenderedDocument.kt', 'plainParagraphKey', 4),
  ],
  [
    'PRODUCTION_STREAM_ELIGIBILITY',
    method('RenderedDocument.kt', 'streamingParagraphEligible', 4),
  ],
  ['PRODUCTION_HOSTED', method('CodeSegmentView.kt', 'setHosted', 2)],
  ['PRODUCTION_BIND_FALLBACK', method('CodeSegmentView.kt', 'bindFallback', 2)],
])
  fixture = fixture.replace(`/* ${marker} */`, value);
const lookup = readFileSync(join(sources, 'RenderedDocument.kt'), 'utf8').match(
  /      val plainKey = plainParagraphKey[\s\S]*?(?=\n      if \(traceId)/
)?.[0];
if (!lookup) throw new Error('Missing production paragraph cache lookup');
fixture = fixture.replace('/* PRODUCTION_PARAGRAPH_LOOKUP */', lookup);
const lanes = readFileSync(join(sources, 'RenderedDocument.kt'), 'utf8').match(
  /^    private val plainParagraphLanes[\s\S]*?^    private fun cachePlainParagraph\([\s\S]*?^    }/m
)?.[0];
if (!lanes) throw new Error('Missing production paragraph lanes');
fixture = fixture.replace('/* PRODUCTION_PARAGRAPH_LANES */', lanes);
const streamPreparation = readFileSync(
  join(sources, 'RenderedDocument.kt'),
  'utf8'
).match(
  /      val streamingParagraph = if \(streamingParagraphEligible[\s\S]*?(?=\n      if \(isCode\))/
)?.[0];
if (!streamPreparation)
  throw new Error('Missing production streaming paragraph preparation');
fixture = fixture.replace('/* PRODUCTION_STREAM_PREPARE */', streamPreparation);
const inlineGate = readFileSync(
  join(sources, 'RenderedDocument.kt'),
  'utf8'
).match(
  /        val styles = document.runStyles\(run\)[\s\S]*?(?=\n        val start = document.runStart\(run\))/
)?.[0];
if (!inlineGate) throw new Error('Missing production inline run gate');
fixture = fixture.replace('/* PRODUCTION_INLINE_GATE */', inlineGate);
const tokenColors = readFileSync(
  join(sources, 'RenderedDocument.kt'),
  'utf8'
).match(
  /^      if \(syntaxHighlighting\) \{\n        for \(token[\s\S]*?(?=\n\n      \/\/ Each laid-out inline formula)/m
)?.[0];
if (!tokenColors) throw new Error('Missing production token color application');
fixture = fixture.replace('/* PRODUCTION_TOKEN_COLORS */', tokenColors);
const output = mkdtempSync(join(tmpdir(), 'downstream-text-'));
try {
  const check = join(output, 'TextStreamingCheck.kt');
  writeFileSync(check, fixture);
  writeFileSync(
    join(output, 'Graphics.kt'),
    'package android.graphics\nobject Color { const val TRANSPARENT = 0 }\n'
  );
  const compiler = process.env.KOTLIN_HOME
    ? join(process.env.KOTLIN_HOME, 'bin/kotlinc')
    : 'kotlinc';
  const jar = join(output, 'text.jar');
  execFileSync(
    compiler,
    [
      '-include-runtime',
      '-nowarn',
      '-d',
      jar,
      join(sources, 'PlainParagraphKey.kt'),
      join(sources, 'StreamingParagraphSeeds.kt'),
      join(sources, 'RenderingCache.kt'),
      join(output, 'Graphics.kt'),
      check,
    ],
    { stdio: 'inherit' }
  );
  execFileSync('java', ['-cp', jar, 'com.downstream.TextStreamingCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}
