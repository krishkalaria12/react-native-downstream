// Type-checks the C++ this package adds to React Native's own build.
//
//     node tests/fabric/syntax-check.mjs
//
// Three files reach a device and are compiled by no test in this repository:
//
//   - `src/cpp/fabric/DownstreamMarkdownShadowNode.cpp`, which measures and
//     places the React children a view mounts into fenced blocks
//   - `src/cpp/fabric/DownstreamTextLayoutManager.cpp`, which turns a document
//     and those children's heights into a measurement
//   - `src/cpp/session/DownstreamSessionModule.cpp`, the TurboModule that
//     answers renderer questions
//
// `pnpm test:native` compiles the engine for the host and `pnpm test:kotlin` runs
// the mirrors' tables, but the engine they build is `src/cpp/downstream`,
// `src/cpp/theme` and their neighbours: the fabric and session directories are
// built by Xcode and Gradle inside React Native's build, against headers that only
// exist there. A stale signature in one of these files passes every other suite
// and fails the first device build, which is the worst place to find it.
//
// So this runs the compiler in `-fsyntax-only` mode over those three files
// against:
//
//   - React Native's *real* headers from `node_modules`, which is the same tree
//     Xcode and Gradle compile
//   - React Native's *real* codegen output, generated here by running its own
//     codegen over this package's spec, so `DownstreamSpecJSI.h` and the component
//     descriptors are the generated ones rather than hand-written approximations
//   - its bundled Yoga, which React Native's layout headers include
//   - three stubs for headers that belong to dependencies React Native vendors
//     rather than to this package: `folly/dynamic.h`, `glog/logging.h` and
//     `jsi/JSIDynamic.h`. See `stubs/`
//
// Each source is checked once per platform, because React Native's `Float` is
// not one type: Android's `platform/android` headers make it `float`, and
// iOS's `platform/ios` make it `CGFloat`, a `double` on every device. A
// conversion that is exact on Android is a narrowing error in Xcode, so checking
// against one platform's headers proves nothing about the other. Off macOS,
// where there is no CoreGraphics, `stubs-apple/` supplies the one typedef the
// iOS header reads.
//
// What it deliberately does not do is link or run anything. It proves the sources
// parse and type-check against the versions an app will resolve, which is the part
// that catches a signature React Native's bridging cannot call; behaviour is the
// mirror tables' job, the harness's job, and a device's job.
//
// `src/ios/DownstreamMarkdownView.mm` is the one file this cannot reach. It
// includes `<React/RCTViewComponentView.h>`, whose umbrella header a pod install
// builds from headers that are not all in the npm tarball, so checking it here
// would mean writing stand-ins for React Native's own classes — the one thing a
// syntax check must not do. Xcode compiles it on every device build.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const reactNative = join(repo, 'node_modules', 'react-native');
const common = join(reactNative, 'ReactCommon');
const stubs = join(repo, 'tests', 'fabric', 'stubs');
const appleStubs = join(repo, 'tests', 'fabric', 'stubs-apple');

/**
 * The platform header roots each platform's build puts first. `Float.h` and
 * `HostPlatformViewEventEmitter.h` live under `platform/<os>`, and whichever
 * root comes first is the one every include resolves to.
 */
const platforms = {
  ios: {
    renderer: [
      'renderer/graphics/platform/ios',
      'renderer/components/view/platform/cxx',
    ],
    extra: process.platform === 'darwin' ? [] : [appleStubs],
  },
  android: {
    renderer: [
      'renderer/graphics/platform/android',
      'renderer/components/view/platform/android',
    ],
    extra: [],
  },
};

/** The sources this repository's own builds never compile. */
const sources = [
  join(repo, 'src', 'cpp', 'fabric', 'DownstreamMarkdownShadowNode.cpp'),
  join(repo, 'src', 'cpp', 'fabric', 'DownstreamTextLayoutManager.cpp'),
  join(repo, 'src', 'cpp', 'session', 'DownstreamSessionModule.cpp'),
];

/** Says why a check is not running, so a miss is a message rather than a failure. */
function skip(reason) {
  console.log(`SKIP fabric syntax check: ${reason}`);
  process.exit(0);
}

function compiler() {
  for (const candidate of ['clang++', 'g++', 'c++']) {
    const probe = spawnSync(candidate, ['--version'], { encoding: 'utf8' });
    if (probe.status === 0) {
      return candidate;
    }
  }
  return skip('no C++ compiler on PATH');
}

/**
 * Runs React Native's own codegen over this package's spec.
 *
 * The generated headers are the whole point: the module's promise signature, the
 * slot structure's member types and the component descriptors all come from the
 * spec rather than from anything this package writes.
 */
function generateCodegen(output) {
  const script = join(reactNative, 'scripts', 'generate-codegen-artifacts.js');
  if (!existsSync(script)) {
    return skip(`no codegen script at ${script}`);
  }
  // The script finishes with a step that wants an app's `.xcodeproj` to resolve
  // spec paths against, and a library has none: it reports that and exits
  // nonzero, having already written every header this check reads. So the exit
  // status is ignored and the artifacts are what decide.
  spawnSync(
    'node',
    [script, '--path', repo, '--outputPath', output, '--targetPlatform', 'ios'],
    {
      cwd: repo,
      stdio: 'ignore',
    }
  );
  const root = join(output, 'build', 'generated', 'ios');
  if (!existsSync(join(root, 'ReactCodegen', 'DownstreamSpecJSI.h'))) {
    return skip(`codegen produced no DownstreamSpecJSI.h under ${output}`);
  }
  return root;
}

function includes(generatedRoot, platform) {
  // Every directory the engine's own headers include each other by, the React
  // Native trees, and the platform's own header roots ahead of the shared ones.
  const engine = [
    '',
    'downstream',
    'fabric',
    'highlight',
    'math',
    'md4c',
    'remend',
    'session',
    'theme',
  ].map((directory) => join(repo, 'src', 'cpp', directory));
  const renderer = [
    ...platform.renderer,
    'renderer',
    'renderer/components',
    'renderer/components/view',
    'renderer/core',
    'renderer/graphics',
    'renderer/imagemanager',
    'renderer/mounting',
  ].map((path) => join(common, 'react', path));
  const rest = [
    common,
    join(common, 'jsi'),
    join(common, 'callinvoker'),
    join(common, 'react'),
    join(common, 'react', 'nativemodule', 'core'),
    join(common, 'react', 'utils'),
    join(common, 'yoga'),
    join(common, 'yoga', 'yoga'),
    stubs,
    ...platform.extra,
    generatedRoot,
    join(generatedRoot, 'ReactCodegen'),
  ];
  return [...new Set([...engine, ...renderer, ...rest])].flatMap((path) => [
    '-I',
    path,
  ]);
}

const cc = compiler();
const output = mkdtempSync(join(tmpdir(), 'downstream-fabric-'));
try {
  const generatedRoot = generateCodegen(output);
  let failures = 0;
  for (const [name, platform] of Object.entries(platforms)) {
    const flags = [
      '-std=c++20',
      '-fsyntax-only',
      '-fexceptions',
      '-Wall',
      ...includes(generatedRoot, platform),
    ];
    for (const source of sources) {
      const relative = source.slice(repo.length + 1);
      const result = spawnSync(cc, [...flags, source], {
        cwd: repo,
        encoding: 'utf8',
      });
      if (result.status === 0) {
        console.log(`ok ${name} ${relative}`);
        continue;
      }
      failures += 1;
      console.log(`FAIL ${name} ${relative}`);
      console.log((result.stderr ?? '').trim());
    }
    const binary = join(output, `host-rects-${name}`);
    const build = spawnSync(
      cc,
      [
        '-std=c++20',
        '-O2',
        ...includes(generatedRoot, platform),
        join(repo, 'tests/fabric/host-rects.cpp'),
        '-o',
        binary,
      ],
      { cwd: repo, encoding: 'utf8' }
    );
    const run =
      build.status === 0 ? spawnSync(binary, [], { encoding: 'utf8' }) : build;
    if (run.status !== 0) {
      failures += 1;
      console.log(`FAIL ${name} host rectangle lookup`);
      console.log((run.stderr ?? '').trim());
    } else {
      console.log(`ok ${name} ${(run.stdout ?? '').trim()}`);
    }
  }
  if (failures > 0) {
    console.log(`\n${failures} file(s) failed to parse`);
    process.exit(1);
  }
  console.log('fabric syntax checks passed');
} finally {
  rmSync(output, { recursive: true, force: true });
}
