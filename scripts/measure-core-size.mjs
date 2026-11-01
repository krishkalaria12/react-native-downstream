// Matched native engine archives, without React Native platform glue. These are
// object/archive sizes, not IPA/APK download sizes or loaded process memory.
import { execFileSync, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import {
  mkdtempSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ENGINE, MD4C_SOURCES, CODE_SOURCES } from '../src/cpp/tests/build.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const revision = process.argv[2] ?? 'cc65ca1';
const output = process.argv[3] ?? '/tmp/downstream-core-size.json';
const work = mkdtempSync(join(tmpdir(), 'downstream-core-size-'));
const before = join(work, 'before');
mkdirSync(before);
execFileSync('tar', ['-x', '-C', before], {
  input: execFileSync('git', ['archive', revision, 'src/cpp'], {
    cwd: root,
    maxBuffer: 64 * 1024 * 1024,
  }),
});
if (
  !readFileSync(join(before, 'src/cpp/wasm/highlights.c')).equals(
    readFileSync(join(root, 'code/cpp/wasm/highlights.c'))
  )
) {
  throw new Error(
    'The generated lexer changed; its measured object cannot be reused.'
  );
}
const environment = {
  ...process.env,
  DEVELOPER_DIR:
    process.env.DEVELOPER_DIR ?? '/Applications/Xcode.app/Contents/Developer',
};
const appleSdk = execFileSync(
  'xcrun',
  ['--sdk', 'iphoneos', '--show-sdk-path'],
  { env: environment, encoding: 'utf8' }
).trim();
const appleClang = execFileSync('xcrun', ['--find', 'clang'], {
  env: environment,
  encoding: 'utf8',
}).trim();
const appleClangxx = execFileSync('xcrun', ['--find', 'clang++'], {
  env: environment,
  encoding: 'utf8',
}).trim();
const appleAr = execFileSync('xcrun', ['--find', 'ar'], {
  env: environment,
  encoding: 'utf8',
}).trim();
const androidSdk =
  process.env.ANDROID_HOME ?? join(process.env.HOME, 'Library/Android/sdk');
const ndkVersion = process.env.DOWNSTREAM_SIZE_NDK ?? '27.1.12297006';
const ndk = join(
  androidSdk,
  'ndk',
  ndkVersion,
  'toolchains/llvm/prebuilt/darwin-x86_64/bin'
);
const platforms = [
  {
    name: 'ios-arm64',
    c: appleClang,
    cpp: appleClangxx,
    ar: appleAr,
    flags: ['-target', 'arm64-apple-ios15.1', '-isysroot', appleSdk],
  },
  {
    name: 'android-arm64-v8a',
    c: join(ndk, 'aarch64-linux-android24-clang'),
    cpp: join(ndk, 'aarch64-linux-android24-clang++'),
    ar: join(ndk, 'llvm-ar'),
    flags: [],
  },
];
const common = [
  '-Oz',
  '-DNDEBUG',
  '-fvisibility=hidden',
  '-ffunction-sections',
  '-fdata-sections',
  '-DMD4C_USE_UTF8=1',
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
];
const run = promisify(execFile);
const afterCore = [
  ...ENGINE,
  ...MD4C_SOURCES,
  join(root, 'src/cpp/session/IncrementalParser.cpp'),
  ...readdirSync(join(root, 'src/cpp/highlight'))
    .filter((file) => file.endsWith('.cpp'))
    .map((file) => join(root, 'src/cpp/highlight', file)),
];
const beforeCore = [
  ...ENGINE,
  ...MD4C_SOURCES,
  join(root, 'src/cpp/session/IncrementalParser.cpp'),
].map((file) => join(before, relative(root, file)));
beforeCore.push(
  ...readdirSync(join(before, 'src/cpp/highlight'))
    .filter((file) => /\.(c|cpp)$/.test(file))
    .map((file) => join(before, 'src/cpp/highlight', file))
);

async function archive(platform, kind, sources, base, wasmObject) {
  const dir = join(work, platform.name, kind);
  mkdirSync(dir, { recursive: true });
  const cpp = join(base, 'src/cpp');
  const code =
    kind === 'before' ? join(cpp, 'highlight') : join(root, 'code/cpp');
  const wasm =
    kind === 'before' ? join(cpp, 'wasm') : join(root, 'code/cpp/wasm');
  const includes = [
    cpp,
    ...[
      'downstream',
      'session',
      'highlight',
      'math',
      'theme',
      'md4c',
      'remend',
    ].map((name) => join(cpp, name)),
    code,
    wasm,
    join(wasm, 'wasm-rt'),
    join(wasm, 'simde'),
  ].map((path) => `-I${path}`);
  const objects = Array(sources.length);
  let cursor = 0;
  await Promise.all(
    Array.from({ length: 2 }, async () => {
      while (cursor < sources.length) {
        const index = cursor++;
        const source = sources[index];
        if (wasmObject && source.endsWith('/HighlightsModule.c')) {
          // The generated module and runtime are unchanged by the move. Reuse
          // that same target's compiled object in the optional package archive.
          objects[index] = wasmObject;
          continue;
        }
        const object = join(dir, `${index}.o`);
        await run(
          source.endsWith('.c') ? platform.c : platform.cpp,
          [
            ...platform.flags,
            ...common,
            source.endsWith('.c') ? '-std=c11' : '-std=c++20',
            ...includes,
            '-c',
            source,
            '-o',
            object,
          ],
          { env: environment, maxBuffer: 8 * 1024 * 1024 }
        );
        objects[index] = object;
      }
    })
  );
  const path = join(dir, `libdownstream-${kind}.a`);
  execFileSync(platform.ar, ['rcs', path, ...objects]);
  return {
    path,
    bytes: statSync(path).size,
    wasmObject:
      objects[
        sources.findIndex((source) => source.endsWith('/HighlightsModule.c'))
      ],
  };
}
const results = [];
for (const platform of platforms) {
  console.log(`Building matched ${platform.name} engine archives…`);
  const original = await archive(platform, 'before', beforeCore, before);
  const core = await archive(platform, 'core', afterCore, root);
  const code = await archive(
    platform,
    'code',
    CODE_SOURCES,
    root,
    original.wasmObject
  );
  results.push({
    platform: platform.name,
    beforeBytes: original.bytes,
    coreBytes: core.bytes,
    codeBytes: code.bytes,
    reductionBytes: original.bytes - core.bytes,
    reductionPercent: (100 * (original.bytes - core.bytes)) / original.bytes,
    archives: [original.path, core.path, code.path],
  });
  console.log(JSON.stringify(results.at(-1)));
}
const report = {
  baseline: revision,
  method:
    'unstripped static native engine archives, no React Native platform glue; identical generated WASM object reused after move',
  optimization: common,
  iosSdk: appleSdk,
  androidNdk: ndkVersion,
  results,
};
writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
console.log(`Wrote ${output}`);
