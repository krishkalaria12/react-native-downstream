// Compiles and runs the Kotlin mirrors against their C++ tables.
//
// `CodeLayout.kt` mirrors `src/cpp/downstream/CodeLayout.h`,
// `ImageLayout.kt` mirrors `src/cpp/downstream/ImageLayout.h`,
// `HostLayout.kt` mirrors `src/cpp/downstream/HostLayout.h`, and `MathLayout.kt`
// mirrors the sizing in `src/cpp/math/MathLayout.h`. The C++ tables in
// `src/cpp/tests/test_export.cpp`, `test_images.cpp`, and `test_math.cpp` are the
// contracts those copies answer. `RevealCurve.kt` mirrors the per-frame half of
// `src/cpp/downstream/Reveal.h`, and its contract is the curve table
// `src/cpp/tests/dump_reveal_curve.cpp` prints. This runs all of those tables
// against the Kotlin side, so a drift between them fails here rather than on a
// device.
// MathRenderCheck also records math drawing/style operations using host stubs;
// it checks redraw allocation avoidance and state changes, not raster output.
//
// The reveal table compiles against `android.jar` but does not need a device:
// `PathInterpolator`'s methods are stubs on a host JVM, so the mirror's own solver
// answers there, and the check bounds it against the engine's numbers.
//
// It needs a Kotlin compiler, which `pnpm test:native` does not assume, so it is a
// separate command. Point KOTLIN_HOME at an unpacked kotlin-compiler, or put
// `kotlinc` on the PATH.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { revealCurveCases } from '../../tests/engine.ts';

const here = dirname(fileURLToPath(import.meta.url));
const kotlin = (...names) =>
  names.map((name) =>
    join(
      here,
      '..',
      '..',
      'src',
      'android',
      'src',
      'main',
      'java',
      'com',
      'downstream',
      name
    )
  );

/** Each mirror, its table, and the `main` that runs the table. */
const suites = [
  {
    jar: 'rendering_cache',
    sources: [
      ...kotlin('RenderingCache.kt'),
      join(here, 'RenderingCacheCheck.kt'),
    ],
  },
  {
    jar: 'reveal_frame',
    sources: [
      ...kotlin('RevealCurve.kt', 'Reveal.kt'),
      join(here, 'RevealFrameCheck.kt'),
      ...readdirSync(join(here, 'reveal-stubs'))
        .filter((name) => name.endsWith('.kt'))
        .map((name) => join(here, 'reveal-stubs', name)),
    ],
  },
  {
    jar: 'code_layout',
    sources: [kotlin('CodeLayout.kt'), join(here, 'CodeLayoutCheck.kt')],
  },
  {
    jar: 'image_layout',
    sources: [kotlin('ImageLayout.kt'), join(here, 'ImageLayoutCheck.kt')],
  },
  {
    jar: 'math_layout',
    sources: [kotlin('MathLayout.kt'), join(here, 'MathLayoutCheck.kt')],
  },
  {
    jar: 'host_layout',
    sources: [kotlin('HostLayout.kt'), join(here, 'HostLayoutCheck.kt')],
  },
  {
    // The platform's own files rather than a copy: `CodeLayout.kt` for
    // `ReaderChoices` and `hasChoice`, `ContainerDecoration.kt` for the frame the
    // rules read, and `DetailsLayout.kt` for the rules themselves.
    jar: 'details_layout',
    sources: [
      ...kotlin('CodeLayout.kt'),
      ...kotlin('ContainerDecoration.kt'),
      ...kotlin('DetailsLayout.kt'),
      join(here, 'DetailsLayoutCheck.kt'),
    ],
  },
  {
    jar: 'text_height',
    sources: [
      ...kotlin(
        'Segments.kt',
        'CodeLayout.kt',
        'ContainerDecoration.kt',
        'DetailsLayout.kt'
      ),
      join(here, 'TextHeightCheck.kt'),
      ...readdirSync(join(here, 'text-height-stubs'))
        .filter((name) => name.endsWith('.kt'))
        .map((name) => join(here, 'text-height-stubs', name)),
    ],
  },
  {
    jar: 'math_render',
    sources: [
      ...kotlin('MathLayout.kt', 'MathDisplayList.kt', 'MathSegmentView.kt'),
      join(here, 'MathRenderCheck.kt'),
      ...readdirSync(join(here, 'math-stubs'))
        .filter((name) => name.endsWith('.kt'))
        .map((name) => join(here, 'math-stubs', name)),
    ],
  },
];

/** `code_layout` to `CodeLayoutCheckKt`. */
function camel(snake) {
  return snake
    .split('_')
    .map((part) => part[0].toUpperCase() + part.slice(1))
    .join('');
}

function kotlinc() {
  const home = process.env.KOTLIN_HOME;
  if (home && existsSync(join(home, 'bin', 'kotlinc'))) {
    return join(home, 'bin', 'kotlinc');
  }
  return 'kotlinc';
}

const output = mkdtempSync(join(tmpdir(), 'downstream-kotlin-'));
try {
  for (const suite of suites) {
    const jar = join(output, `${suite.jar}.jar`);
    execFileSync(
      kotlinc(),
      ['-include-runtime', '-nowarn', '-d', jar, ...suite.sources],
      {
        stdio: 'inherit',
      }
    );
    execFileSync(
      'java',
      ['-cp', jar, `com.downstream.${camel(suite.jar)}CheckKt`],
      { stdio: 'inherit' }
    );
  }

  // The reveal check is the one table that is piped in rather than compiled in,
  // so it stays outside the loop above.
  //
  // Only `RevealCurve.kt`, which is the half with no platform dependency. The
  // platform half lives in `Reveal.kt` and needs `android.jar`, and `android.jar`
  // bundles the whole `java.*` API, so a Kotlin compile against it loses the
  // stdlib's operator declarations and reports errors about destructuring and
  // comparisons that have nothing to do with this code. Gradle compiles the
  // platform half against the real android.jar; this checks the half that decides
  // what a reader sees.
  execFileSync(
    kotlinc(),
    [
      '-nowarn',
      '-include-runtime',
      '-d',
      join(output, 'reveal.jar'),
      kotlin('RevealCurve.kt'),
      join(here, 'RevealCheck.kt'),
    ],
    { stdio: 'inherit' }
  );
  // The engine's own numbers, piped in rather than read from a file, so the check
  // and the binary it checks cannot be built from different sources.
  const table = revealCurveCases()
    .map((entry) =>
      [
        entry.x1,
        entry.y1,
        entry.x2,
        entry.y2,
        entry.durationMs,
        entry.offsetMs,
        entry.alpha,
        entry.rise,
      ].join(' ')
    )
    .join('\n');
  execFileSync(
    'java',
    ['-cp', join(output, 'reveal.jar'), 'com.downstream.RevealCheckKt'],
    {
      input: table + '\n',
      stdio: ['pipe', 'inherit', 'inherit'],
    }
  );
  execFileSync(process.execPath, [join(here, 'state-application.mjs')], {
    stdio: 'inherit',
  });
  execFileSync(process.execPath, [join(here, 'native-document.mjs')], {
    stdio: 'inherit',
  });
  execFileSync(process.execPath, [join(here, 'table-streaming.mjs')], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}
