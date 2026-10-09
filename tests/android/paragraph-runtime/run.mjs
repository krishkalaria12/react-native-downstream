// An isolated instrumentation APK: real Android text/layout APIs, no Gradle or host stubs.
// Set ANDROID_SERIAL for an existing device; DOWNSTREAM_PARAGRAPH_AVD starts a named emulator.
import { execFileSync, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir, homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '../../..');
const sdk = process.env.ANDROID_HOME ?? join(homedir(), 'Library/Android/sdk');
const build = join(
  sdk,
  'build-tools',
  readdirSync(join(sdk, 'build-tools')).sort().at(-1)
);
const platform = join(
  sdk,
  'platforms',
  readdirSync(join(sdk, 'platforms')).sort().at(-1),
  'android.jar'
);
const adb = join(sdk, 'platform-tools/adb');
const stdlibRoot = join(
  homedir(),
  '.gradle/caches/modules-2/files-2.1/org.jetbrains.kotlin/kotlin-stdlib/2.2.10'
);
const stdlib =
  process.env.DOWNSTREAM_KOTLIN_STDLIB ??
  readdirSync(stdlibRoot)
    .map((hash) => join(stdlibRoot, hash, 'kotlin-stdlib-2.2.10.jar'))
    .find(existsSync);
if (!stdlib)
  throw new Error(
    'Set DOWNSTREAM_KOTLIN_STDLIB to a Kotlin 2.2-compatible stdlib jar.'
  );
const temporary = mkdtempSync(join(tmpdir(), 'downstream-paragraph-runtime-'));
const run = (command, args, options = {}) =>
  execFileSync(command, args, { stdio: 'inherit', ...options });
const adbArgs = process.env.ANDROID_SERIAL
  ? ['-s', process.env.ANDROID_SERIAL]
  : [];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let emulator;
let installed = false;
try {
  const devices = run(adb, ['devices'], { encoding: 'utf8', stdio: 'pipe' });
  if (!/\tdevice\b/.test(devices)) {
    const avd = process.env.DOWNSTREAM_PARAGRAPH_AVD;
    if (!avd)
      throw new Error(
        'No Android device online. Set DOWNSTREAM_PARAGRAPH_AVD=downstream-test to launch the test emulator.'
      );
    emulator = spawn(
      join(sdk, 'emulator/emulator'),
      [
        '-avd',
        avd,
        '-no-window',
        '-no-audio',
        '-no-snapshot',
        '-gpu',
        'swiftshader_indirect',
      ],
      { stdio: 'ignore' }
    );
  }
  for (let attempt = 0; ; attempt++) {
    let ready = false;
    try {
      ready =
        run(adb, [...adbArgs, 'shell', 'getprop', 'sys.boot_completed'], {
          encoding: 'utf8',
          stdio: 'pipe',
        }).trim() === '1';
    } catch {}
    if (ready) break;
    if (attempt === 180)
      throw new Error('Android device did not boot within three minutes.');
    await sleep(1000);
  }
  const apk = join(temporary, 'paragraph.apk');
  const jar = join(temporary, 'paragraph.jar');
  const dex = join(temporary, 'dex');
  mkdirSync(dex);
  run(join(build, 'aapt2'), [
    'link',
    '-I',
    platform,
    '--manifest',
    join(here, 'AndroidManifest.xml'),
    '-o',
    apk,
  ]);
  const productionRoot = join(repo, 'src/android/src/main/java/com/downstream');
  const productionSources = [
    'StreamingParagraphLayout.kt',
    'StreamingParagraphSeeds.kt',
    'TextSegmentView.kt',
    'MarkdownAccessibility.kt',
    'Caret.kt',
    'Reveal.kt',
    'RevealCurve.kt',
  ].map((name) => join(productionRoot, name));
  const segmentSource = readFileSync(
    join(productionRoot, 'Segments.kt'),
    'utf8'
  );
  const textSegment = segmentSource.match(
    /^internal class TextSegment\([\s\S]*?^\}/m
  )?.[0];
  const taskBlock = segmentSource.match(
    /^internal class TaskBlock\([\s\S]*?^\)/m
  )?.[0];
  const detailsBlock = segmentSource.match(
    /^internal class DetailsBlock\([\s\S]*?^\)/m
  )?.[0];
  const precomputed = readFileSync(
    join(productionRoot, 'RenderedDocument.kt'),
    'utf8'
  ).match(/^internal object PrecomputedTexts \{[\s\S]*?^\}/m)?.[0];
  const linkAt = readFileSync(
    join(productionRoot, 'Controls.kt'),
    'utf8'
  ).match(/^internal fun linkAt\([\s\S]*?^\}/m)?.[0];
  if (!textSegment || !taskBlock || !detailsBlock || !precomputed || !linkAt)
    throw new Error(
      'Missing production TextSegment/PrecomputedTexts/linkAt declaration.'
    );
  const extracted = join(temporary, 'ProductionTextSegment.kt');
  writeFileSync(
    extracted,
    `package com.downstream\nimport android.text.*\nimport android.widget.TextView\n${textSegment}\n${taskBlock}\n${detailsBlock}\n${precomputed}\n${linkAt}\n`
  );
  const hash = createHash('sha256');
  for (const path of [
    ...productionSources,
    extracted,
    ...readdirSync(here)
      .filter((name) => name.endsWith('.kt'))
      .map((name) => join(here, name)),
  ])
    hash.update(readFileSync(path));
  const sourceSha256 = hash.digest('hex');
  run('kotlinc', [
    '-language-version',
    '2.2',
    '-api-version',
    '2.2',
    '-no-stdlib',
    '-classpath',
    `${platform}:${stdlib}`,
    '-d',
    jar,
    ...productionSources,
    extracted,
    join(here, 'FixtureDependencies.kt'),
    join(here, 'AccessibilityCompat.kt'),
    join(here, 'RequiresApi.kt'),
    join(here, 'ReactResources.kt'),
    join(here, 'ParagraphChecks.kt'),
    join(here, 'ParagraphInstrumentation.kt'),
  ]);
  run(join(build, 'd8'), [
    '--lib',
    platform,
    '--min-api',
    '28',
    '--output',
    dex,
    jar,
    stdlib,
  ]);
  run('zip', [
    '-q',
    '-j',
    apk,
    ...readdirSync(dex)
      .filter((name) => name.endsWith('.dex'))
      .map((name) => join(dex, name)),
  ]);
  run(join(build, 'apksigner'), [
    'sign',
    '--ks',
    join(homedir(), '.android/debug.keystore'),
    '--ks-key-alias',
    'androiddebugkey',
    '--ks-pass',
    'pass:android',
    '--key-pass',
    'pass:android',
    apk,
  ]);
  run(adb, [...adbArgs, 'install', '-r', '-t', apk]);
  installed = true;
  const output = run(
    adb,
    [
      ...adbArgs,
      'shell',
      'am',
      'instrument',
      '-w',
      '-r',
      'com.downstream.paragraphruntime/com.downstream.ParagraphInstrumentation',
    ],
    { encoding: 'utf8', stdio: 'pipe' }
  );
  console.log(output);
  if (/shortMsg=Process crashed|INSTRUMENTATION_FAILED/.test(output)) {
    console.error(
      run(adb, [...adbArgs, 'logcat', '-d', '-b', 'crash'], {
        encoding: 'utf8',
        stdio: 'pipe',
      })
    );
    throw new Error('Paragraph instrumentation process crashed.');
  }
  if (!/INSTRUMENTATION_CODE: 0\b/.test(output))
    throw new Error('Paragraph runtime checks failed.');
  const destination = process.env.DOWNSTREAM_PARAGRAPH_RESULTS;
  if (destination) {
    const json = output.match(
      /INSTRUMENTATION_RESULT: stream=(\{[\s\S]*?\})\s*INSTRUMENTATION_CODE: 0/
    )?.[1];
    if (!json) throw new Error('Missing paragraph runtime JSON results.');
    const result = JSON.parse(json);
    result.sourceSha256 = sourceSha256;
    result.recordedAt = new Date().toISOString();
    writeFileSync(destination, `${JSON.stringify(result, null, 2)}\n`);
  }
} finally {
  if (installed)
    run(adb, [...adbArgs, 'uninstall', 'com.downstream.paragraphruntime'], {
      stdio: 'ignore',
    });
  if (emulator) emulator.kill('SIGTERM');
  rmSync(temporary, { recursive: true, force: true });
}
