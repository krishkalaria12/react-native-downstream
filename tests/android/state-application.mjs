// Runs the production state-application methods with host rendering counters.
// This checks transaction ordering and work counts, not Android drawing or JNI.
import { execFileSync } from 'node:child_process';
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const sources = join(here, '../../src/android/src/main/java/com/downstream');
const view = readFileSync(join(sources, 'DownstreamMarkdownView.kt'), 'utf8');

// These functions use the source set's two-space member indentation. Fail
// closed if a declaration or its boundary changes rather than running a copy.
function member(name) {
  const lines = view.split('\n');
  const start = new RegExp(`^  (?:internal |private )?fun ${name}\\(`);
  const matches = lines.flatMap((line, index) =>
    start.test(line) ? [index] : []
  );
  if (matches.length !== 1) throw new Error(`Expected one production ${name}`);
  const from = matches[0];
  const to = lines.findIndex((line, index) => index > from && line === '  }');
  if (to < 0) throw new Error(`Missing production ${name} boundary`);
  return lines.slice(from, to + 1).join('\n');
}

const methods = [
  'reconcileSegmentViews',
  'setDocument',
  'applyState',
  'applyDocumentState',
  'setCaret',
  'setImageLayout',
  'setHostLayout',
  'setCodeProps',
  'setCodeChoices',
  'setChrome',
]
  .map(member)
  .join('\n\n');
const manager = readFileSync(
  join(sources, 'DownstreamMarkdownViewManager.kt'),
  'utf8'
);
const state = manager.match(/  override fun updateState\([\s\S]*?\n  }/);
if (
  !state ||
  (state[0].match(/view\.applyState\(/g) ?? []).length !== 1 ||
  /view\.set\w+\(/.test(state[0])
) {
  throw new Error(
    'Fabric updateState must enter exactly one state transaction'
  );
}

const caret = readFileSync(join(sources, 'Caret.kt'), 'utf8').match(
  /internal data class PlacedCaret\([\s\S]*?\n}/
)?.[0];
if (!caret) throw new Error('Missing production caret type');
const fixture = readFileSync(join(here, 'StateApplicationCheck.kt'), 'utf8');
if (fixture.split('/* PRODUCTION_STATE_METHODS */').length !== 2) {
  throw new Error('Expected one production method insertion point');
}
const output = mkdtempSync(join(tmpdir(), 'downstream-state-'));
try {
  const check = join(output, 'StateApplicationCheck.kt');
  writeFileSync(
    check,
    fixture.replace('/* PRODUCTION_STATE_METHODS */', methods) + '\n' + caret
  );
  const home = process.env.KOTLIN_HOME;
  const compiler =
    home && existsSync(join(home, 'bin/kotlinc'))
      ? join(home, 'bin/kotlinc')
      : 'kotlinc';
  const jar = join(output, 'state.jar');
  execFileSync(
    compiler,
    [
      '-include-runtime',
      '-nowarn',
      '-d',
      jar,
      ...[
        'CodeLayout.kt',
        'ImageLayout.kt',
        'HostLayout.kt',
        'TableLayout.kt',
      ].map((name) => join(sources, name)),
      check,
    ],
    { stdio: 'inherit' }
  );
  execFileSync('java', ['-cp', jar, 'com.downstream.StateApplicationCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}
