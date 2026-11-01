#!/usr/bin/env node
// Public string/controller APIs and explicit plugin profiles, on Android/Perfetto.
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runBenchmarkCLI } from './bench.mjs';
import { FEATURE_ORDERS, WORKLOADS } from './workloads.mjs';

export const BENCHMARK_VERSION = 'api-plugins-v1';
export const PROFILES = {
  'core': {
    syntaxHighlighting: false,
    math: false,
    preview: true,
    animated: false,
  },
  'code': {
    syntaxHighlighting: true,
    math: false,
    preview: true,
    animated: false,
  },
  'math': {
    syntaxHighlighting: false,
    math: true,
    preview: true,
    animated: false,
  },
  'all': {
    syntaxHighlighting: true,
    math: true,
    preview: true,
    animated: false,
  },
  'all-animated': {
    syntaxHighlighting: true,
    math: true,
    preview: true,
    animated: true,
  },
  'all-literal': {
    syntaxHighlighting: true,
    math: true,
    preview: false,
    animated: false,
  },
};

const CANDIDATES = {
  'downstream-string': 'Downstream string',
  'downstream-controller': 'Downstream controller',
  'streamdown': 'RN Streamdown',
  'enriched': 'Enriched Markdown',
  'markdown-display': 'Markdown Display',
  'marked': 'Marked',
  'markdown-renderer': 'Markdown Renderer',
};

/** Require a receipt from the committed renderer, rather than echoed request flags. */
export function configurationErrors(config, result) {
  const errors = [];
  if (result.benchmarkVersion !== BENCHMARK_VERSION)
    errors.push(
      'API benchmark needs a rebuilt APK with api-plugins-v1 support'
    );
  const downstream = config.candidate.startsWith('downstream-');
  const expected = {
    sourceMode: downstream
      ? config.candidate === 'downstream-string'
        ? 'string'
        : 'controller'
      : 'peer',
    plugins: downstream
      ? [
          ...(config.syntaxHighlighting ? ['code'] : []),
          ...(config.math ? ['math'] : []),
        ]
      : [],
    preview: downstream ? config.preview : null,
  };
  const check = (receipt) => {
    if (
      !receipt ||
      receipt.sourceMode !== expected.sourceMode ||
      receipt.preview !== expected.preview ||
      JSON.stringify(receipt.plugins) !== JSON.stringify(expected.plugins)
    )
      return false;
    return (
      !downstream ||
      ((!config.syntaxHighlighting || receipt.engines?.code === true) &&
        (!config.math || receipt.engines?.math === true))
    );
  };
  if (!check(result.apiConfig))
    errors.push(
      'renderer source, plugins, preview or required native engines differ from the requested profile'
    );
  if (
    !Number.isInteger(result.threadCount) ||
    result.threadCount < 1 ||
    result.threadConfigurations?.length !== result.threadCount ||
    !Array.from(
      { length: result.threadCount },
      (_, thread) =>
        result.threadConfigurations?.filter(
          (entry) => entry.thread === thread && check(entry)
        ).length === 1
    ).every(Boolean)
  )
    errors.push(
      'missing or inconsistent per-thread renderer configuration receipts'
    );
  return errors;
}

export function validateSavedRun(session, run) {
  const profile = PROFILES[run.config.profile];
  const scheduled = session.order?.find(
    (item) => item.index === run.config.index
  );
  if (
    !profile ||
    run.config.benchmarkVersion !== BENCHMARK_VERSION ||
    !session.candidates.includes(run.config.candidate) ||
    !session.profiles.includes(run.config.profile) ||
    !session.workloadIds.includes(run.config.workload) ||
    !scheduled ||
    ['profile', 'candidate', 'workload', 'block', 'warmup'].some(
      (key) => scheduled[key] !== run.config[key]
    ) ||
    Object.entries(profile).some(([key, value]) => run.config[key] !== value)
  )
    throw new Error('Saved run does not match the API benchmark profile.');
  const errors = configurationErrors(run.config, run.app);
  if (errors.length)
    throw new Error(`Invalid saved API benchmark run: ${errors.join('; ')}`);
}

const number = (value, digits = 2) =>
  Number.isFinite(value) ? value.toFixed(digits) : '—';
const cell = (group, key, digits = 2) => {
  const metric = group.metrics[key];
  if (!metric?.n) return '—';
  return (
    number(metric.median, digits) +
    (metric.ci95
      ? ` [${metric.ci95.map((n) => number(n, digits)).join(', ')}]`
      : '')
  );
};

export function renderApiReport(session, runs, stats) {
  const measured = runs.filter((run) => !run.config.warmup);
  const lines = [
    '# Downstream API and plugin phone benchmark',
    '',
    `${session.device.model} · Android ${session.device.release} · arm64 Hermes release · ${session.startedAt}.`,
    '',
    `Suite: \`${BENCHMARK_VERSION}\`. Input: replay from \`${session.input.folder}\` (${session.input.model}). ` +
      `${measured.length} successful measured runs; ${runs.length - measured.length} warmups excluded.`,
    '',
    'The controller is the baseline. Both Downstream candidates receive the same recorded deltas and finish markers. ' +
      'The string candidate passes cumulative children and per-thread isAnimating; the controller candidate appends through createMarkdownStream without parent React source updates.',
    '',
    '| Profile | Explicit plugins | Preview | Animation |',
    '|---|---|---|---|',
    ...session.profiles.map((id) => {
      const p = PROFILES[id];
      return `| ${id} | ${[p.syntaxHighlighting && 'code', p.math && 'math'].filter(Boolean).join(', ') || 'none'} | ${p.preview} | ${p.animated} |`;
    }),
    '',
    'Values are medians with 95% bootstrap intervals in brackets. The n column is the actual measured sample count. ' +
      'Huge workloads have one sample and no warmup; a single sample cannot establish a performance difference.',
    '',
    '| Profile / workload | Candidate | n | App CPU (s) | JS CPU (s) | Peak memory increase (MiB) | Frame p95 (ms) | Settle after finish (ms) |',
    '|---|---|---:|---:|---:|---:|---:|---:|',
    ...Object.entries(stats.groups).map(([key, group]) => {
      const [profile, workload, candidate] = key.split('/');
      return `| ${profile} / ${workload} | ${CANDIDATES[candidate]} | ${group.runs} | ${cell(group, 'cpuTotalS')} | ${cell(group, 'cpuJsS')} | ${cell(group, 'memPeakIncreaseMiB', 1)} | ${cell(group, 'frameP95Ms', 1)} | ${cell(group, 'settleAfterFinishMs', 0)} |`;
    }),
    '',
    'All raw metrics and controller-relative ratios are in summary.json. Failed runs are excluded and listed there. ' +
      `Finish markers are delayed ${session.finishDelayMs ?? 0} ms per thread after its last chunk; the common settle observation budget is ${session.settleMs} ms.`,
    '',
    '## Interpretation',
    '',
    '- Every accepted run verifies the mounted renderer’s source mode, plugin names, preview setting and required native engines, for every thread. Source equality checks delivery only; inspect screenshots and accessibility output for visual correctness.',
    '- All optional Downstream native packages remain linked in the same APK. These profiles measure runtime plugin selection, not core-only binary size or the cost of omitting native dependencies.',
    '- Plugins are disabled intentionally in core/code/math profiles. Math syntax is literal without math; unhighlighted code remains visible without code. Compare the two Downstream APIs within the same profile.',
    '- Optional peer candidates retain their own rendering and repair behavior. Downstream preview/plugin settings do not establish feature equivalence with peers. Animation effects also differ.',
    '- Frame tails can be censored by the observation window. Inspect tailWindowCensored in raw metrics and retained traces; increase --settle-ms in a new session when necessary.',
    ...(session.diagnosticTrace
      ? [
          '- Native diagnostic tracing is enabled: timings include tracing overhead and must be compared only with other diagnostic runs.',
        ]
      : []),
    '- Synthetic fixtures use a fixed cadence, not model delivery timing. Use the same real capture across runs when measuring model-paced workloads.',
    '',
    '## APKs',
    '',
    '| Profile | Revision | Dirty | APK SHA-256 |',
    '|---|---|---|---|',
    ...session.builds.map(
      (build) =>
        `| ${build.profile} | ${build.revision} | ${build.dirty} | ${build.apkSha256} |`
    ),
    '',
  ];
  return lines.join('\n');
}

export const API_SUITE = {
  id: BENCHMARK_VERSION,
  profiles: PROFILES,
  candidates: CANDIDATES,
  baseline: 'downstream-controller',
  animatedCandidates: [
    'downstream-string',
    'downstream-controller',
    'streamdown',
    'enriched',
  ],
  defaultCandidates: ['downstream-string', 'downstream-controller'],
  defaultProfiles: ['core', 'code', 'math', 'all'],
  defaultWorkloads: [
    'ai-single',
    'ai-feature-matrix',
    'ai-concurrent-5',
    'ai-huge-single',
  ],
  appOptionErrors: configurationErrors,
  validateSavedRun,
  renderReport: renderApiReport,
  validateOptions(command, options) {
    if (['all', 'run'].includes(command) && !options.replay)
      throw new Error(
        'Use --replay <recording-dir>. Generate fixtures with `bench-api.mjs fixtures <dir>` first.'
      );
    if (
      options.diagnosticTrace &&
      command !== 'resume' &&
      options.candidates.includes('downstream-string')
    )
      throw new Error(
        '--diagnostic-trace needs --candidates downstream-controller; string-owned native tracing is unavailable.'
      );
    if (
      new Set(options.candidates).size !== options.candidates.length ||
      new Set(options.profiles).size !== options.profiles.length ||
      new Set(options.workloads).size !== options.workloads.length
    )
      throw new Error(
        'Candidates, profiles and workloads must not contain duplicates.'
      );
  },
};

const sha = (value) => createHash('sha256').update(value).digest('hex');
const FEATURES = {
  math: 'Inline $x^2 + y^2 = z^2$.\n\n$$\n\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2}\n$$',
  code: '```typescript title="reply.ts"\nconst literal = "$5 | `code`";\nexport const square = (n: number) => n * n;\n```',
  table:
    '| Item | Value |\n|:---|---:|\n| **Bold** and `code` | 42 |\n| [Link](https://example.com) | café 😀 |',
};

/** Reuses the ten recording slots understood by the existing replay scheduler. */
export function syntheticRecordings() {
  const section = (order, index) =>
    `## ${index + 1}. ${order.join(' then ') || 'prose only'}\n\n` +
    '**Streaming** café 清水寺 😀 with *emphasis*, ~~removed~~ text and `inline code`.\n\n' +
    '- First item\n  - Nested item\n- [x] A completed task\n\n' +
    order.map((feature) => FEATURES[feature]).join('\n\n') +
    '\n\n';
  const matrix = FEATURE_ORDERS.map(section).join('');
  return Array.from({ length: 10 }, (_, index) => {
    const source =
      `# Synthetic replay ${index}\n\n` +
      (index === 4
        ? Array.from({ length: 14 }, (unused, i) => section(['code'], i)).join(
            ''
          )
        : matrix
      ).repeat(index < 5 ? 1 : 12);
    const codepoints = Array.from(source);
    const chunks = [];
    for (let offset = 0; offset < codepoints.length; offset += 128)
      chunks.push({
        text: codepoints.slice(offset, offset + 128).join(''),
        dueMs: chunks.length * 16,
      });
    return {
      source,
      sha256: sha(source),
      chunks,
      firstArrivalMs: index * 35,
      model: 'synthetic-api-plugins-v1',
      client: 'deterministic fixtures; 128 code points every 16 ms',
      synthetic: true,
      request: { reasoningEffort: 'none' },
    };
  });
}

export function writeFixtures(folder) {
  const output = resolve(folder);
  // Require a new directory so existing captures are never partially overwritten.
  mkdirSync(dirname(output), { recursive: true });
  mkdirSync(output);
  for (const [index, recording] of syntheticRecordings().entries())
    writeFileSync(
      join(output, `response-${index}.json`),
      JSON.stringify(recording) + '\n',
      { flag: 'wx' }
    );
  writeFileSync(
    join(output, 'manifest.json'),
    JSON.stringify(
      {
        benchmarkVersion: BENCHMARK_VERSION,
        synthetic: true,
        workloads: Object.keys(WORKLOADS),
      },
      null,
      2
    ) + '\n',
    { flag: 'wx' }
  );
  return output;
}

export const HELP = `Android phone benchmark for the public Downstream APIs and explicit plugins.

  node benchmarks/phone/bench-api.mjs fixtures <new-recording-dir>
  node benchmarks/phone/bench-api.mjs capture --replay <recording-dir> [--model <id>]
  node benchmarks/phone/bench-api.mjs plan [options]
  node benchmarks/phone/bench-api.mjs all --replay <dir> --output <new-result-dir>
  node benchmarks/phone/bench-api.mjs build --output <build-dir> [options]
  node benchmarks/phone/bench-api.mjs run --replay <dir> --output <build-dir> [options]
  node benchmarks/phone/bench-api.mjs resume <result-dir>
  node benchmarks/phone/bench-api.mjs report <result-dir>
  node benchmarks/phone/bench-api.mjs selftest

Profiles: core,code,math,all (default); all-animated,all-literal (opt in).
Candidates: downstream-string,downstream-controller (default); ${Object.keys(CANDIDATES).slice(2).join(',')} (opt in).
Options: --profiles <ids> --candidates <ids> --workloads <ids> --repeats 5 --seed 1
         --device <adb-serial> --settle-ms 2000 --finish-delay-ms 0 --keep-traces
         --diagnostic-trace (controller only) --no-device-setup
See benchmarks/phone/API_BENCHMARK.md for prerequisites, methodology and limits.
`;

export async function main(args = process.argv.slice(2)) {
  if (args.includes('--help') || args[0] === 'help') {
    console.log(HELP);
    return;
  }
  if (args[0] === 'fixtures') {
    if (args.length !== 2)
      throw new Error('Usage: bench-api.mjs fixtures <new-recording-dir>');
    console.log(`Replay fixtures: ${writeFixtures(args[1])}`);
    return;
  }
  if (args[0] === 'selftest') {
    if (args.length !== 1) throw new Error('Usage: bench-api.mjs selftest');
    const result = spawnSync(
      process.execPath,
      ['--test', fileURLToPath(new URL('./api.test.mjs', import.meta.url))],
      { stdio: 'inherit' }
    );
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error('API benchmark selftest failed.');
    return;
  }
  // Capture uses the existing Vercel AI SDK path, without activating run profiles.
  if (args[0] === 'capture') return runBenchmarkCLI(undefined, args);
  return runBenchmarkCLI(API_SUITE, args);
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
)
  main().catch((error) => {
    console.error(error.stack ?? error);
    process.exitCode = 1;
  });
