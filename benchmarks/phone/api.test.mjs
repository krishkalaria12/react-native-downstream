import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  mkdtempSync,
  readFileSync,
  mkdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import {
  API_SUITE,
  BENCHMARK_VERSION,
  PROFILES,
  configurationErrors,
  main,
  renderApiReport,
  syntheticRecordings,
  validateSavedRun,
  writeFixtures,
} from './bench-api.mjs';
import { buildWorkload, profileConfig, runBenchmarkCLI } from './bench.mjs';

async function quiet(action) {
  const log = console.log;
  console.log = () => {};
  try {
    return await action();
  } finally {
    console.log = log;
  }
}

const config = (candidate = 'downstream-string') => ({
  benchmarkVersion: BENCHMARK_VERSION,
  candidate,
  ...PROFILES.all,
  profile: 'all',
  workload: 'ai-single',
  index: 0,
  block: 0,
  warmup: false,
});
const receipt = (candidate = 'downstream-string') => ({
  sourceMode: candidate === 'downstream-string' ? 'string' : 'controller',
  plugins: ['code', 'math'],
  preview: true,
  engines: { code: true, math: true },
});
const app = (candidate = 'downstream-string') => ({
  benchmarkVersion: BENCHMARK_VERSION,
  threadCount: 1,
  apiConfig: receipt(candidate),
  threadConfigurations: [{ thread: 0, ...receipt(candidate) }],
});
const session = () => ({
  schemaVersion: 2,
  benchmarkVersion: BENCHMARK_VERSION,
  candidates: ['downstream-string', 'downstream-controller'],
  profiles: ['all'],
  workloadIds: ['ai-single'],
  order: [config()],
  device: { model: 'test', release: '16' },
  startedAt: '2026-11-01',
  builds: [],
  input: {
    mode: 'replay',
    folder: 'fixtures',
    model: 'synthetic-api-plugins-v1',
  },
  settleMs: 2000,
  seed: 1,
});

test('plan schedules both public APIs with explicit profiles and reproducible order', async () => {
  const args = [
    'plan',
    '--profiles',
    'core,all-animated',
    '--workloads',
    'ai-single,ai-huge-single',
    '--repeats',
    '2',
  ];
  const a = await quiet(() => main(args));
  const b = await quiet(() => main(args));
  assert.deepEqual(a, b);
  assert.equal(a.benchmarkVersion, BENCHMARK_VERSION);
  assert.equal(a.order.length, 16); // 2 profiles * 2 APIs * (warmup + 2 regular + 1 huge).
  assert.deepEqual(a.profiles.core, PROFILES.core);
  assert.equal(
    a.order.filter((item) => item.workload === 'ai-huge-single' && item.warmup)
      .length,
    0
  );
  // The historical CLI's exported helpers retain their original definitions.
  assert.deepEqual(profileConfig('plain'), {
    syntaxHighlighting: false,
    animated: false,
  });
  assert.throws(() => profileConfig('core'), /Unknown profile/);
});

test('invalid runs fail before phone setup or model requests', async () => {
  await assert.rejects(main(['all']), /Use --replay/);
  // Validate capture arguments before any model request, through the shared SDK CLI.
  await assert.rejects(
    main(['capture', '--repeats', '0']),
    /integer of at least 2/
  );
  await assert.rejects(
    main(['plan', '--profiles', 'highlighted']),
    /Unknown profile/
  );
  await assert.rejects(
    main(['plan', '--candidates', 'downstream-string,downstream-string']),
    /duplicates/
  );
  await assert.rejects(main(['plan', '--diagnostic-trace']), /controller/);
  await assert.rejects(
    main(['plan', '--candidates', 'misspelled']),
    /Unknown candidate/
  );
});

test('SDK capture uses the existing capture path and refuses to overwrite a recording', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'downstream-api-capture-'));
  try {
    // The shared capture path rejects this before loading the SDK or requesting a model.
    await assert.rejects(
      main(['capture', '--replay', dir]),
      /already exists; pass a new --replay folder/
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('stale APKs, ignored plugins, missing engines and per-thread mismatches fail validation', () => {
  assert.deepEqual(configurationErrors(config(), app()), []);
  const mutate = (edit) => {
    const result = app();
    edit(result);
    return configurationErrors(config(), result);
  };
  assert.ok(mutate((result) => delete result.benchmarkVersion).length);
  assert.ok(
    mutate((result) => (result.apiConfig.sourceMode = 'controller')).length
  );
  assert.ok(mutate((result) => (result.apiConfig.preview = false)).length);
  assert.ok(mutate((result) => (result.apiConfig.plugins = [])).length);
  assert.ok(mutate((result) => (result.apiConfig.engines.code = false)).length);
  assert.ok(mutate((result) => (result.apiConfig.engines.math = false)).length);
  assert.ok(mutate((result) => (result.threadConfigurations = [])).length);
  assert.ok(
    mutate((result) => (result.threadConfigurations[0].plugins = [])).length
  );
  assert.ok(
    mutate((result) =>
      result.threadConfigurations.push(result.threadConfigurations[0])
    ).length
  );
  const plain = {
    sourceMode: 'string',
    plugins: [],
    preview: true,
    engines: { code: false, math: false },
  };
  assert.deepEqual(
    configurationErrors(
      { ...config(), ...PROFILES.core },
      {
        benchmarkVersion: BENCHMARK_VERSION,
        apiConfig: plain,
        threadCount: 1,
        threadConfigurations: [{ thread: 0, ...plain }],
      }
    ),
    []
  );
});

test('saved successful runs must match their profile, schedule and renderer receipts', () => {
  const run = { config: config(), app: app() };
  assert.doesNotThrow(() => validateSavedRun(session(), run));
  assert.throws(
    () =>
      validateSavedRun(session(), {
        ...run,
        config: { ...run.config, preview: false },
      }),
    /profile/
  );
  assert.throws(
    () =>
      validateSavedRun(session(), {
        ...run,
        config: { ...run.config, index: 4 },
      }),
    /profile/
  );
  assert.throws(
    () =>
      validateSavedRun(session(), {
        ...run,
        app: { ...run.app, benchmarkVersion: undefined },
      }),
    /Invalid saved/
  );
});

test('deterministic synthetic fixtures preserve Unicode, chunk timing and workload slots', () => {
  const records = syntheticRecordings();
  assert.deepEqual(records, syntheticRecordings());
  assert.equal(records.length, 10);
  for (const record of records) {
    assert.equal(
      record.chunks.map((chunk) => chunk.text).join(''),
      record.source
    );
    assert.equal(
      createHash('sha256').update(record.source).digest('hex'),
      record.sha256
    );
    assert.ok(record.source.includes('清水寺 😀'));
    for (const [index, chunk] of record.chunks.entries()) {
      assert.equal(chunk.dueMs, index * 16);
      assert.equal(chunk.text.isWellFormed(), true);
    }
  }
  assert.ok(Buffer.byteLength(records[5].source) > 65536);
  const workload = buildWorkload(
    records.map((record, i) => ({ ...record, file: `response-${i}.json` })),
    [0, 1, 2, 3, 4]
  );
  assert.equal(workload.threads.length, 5);
  assert.ok(
    workload.chunks.every(
      (chunk, i) => !i || chunk.dueMs >= workload.chunks[i - 1].dueMs
    )
  );
  const dir = mkdtempSync(join(tmpdir(), 'downstream-api-fixtures-'));
  try {
    const folder = writeFixtures(join(dir, 'new'));
    assert.equal(
      JSON.parse(readFileSync(join(folder, 'response-0.json'))).sha256,
      records[0].sha256
    );
    assert.throws(() => writeFixtures(folder), /EEXIST/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('reports use only valid API suite runs and never publish over the historical report', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'downstream-api-report-'));
  const historicalReport = new URL('./REPORT.md', import.meta.url);
  const before = readFileSync(historicalReport, 'utf8');
  try {
    const plan = await quiet(() =>
      main(['plan', '--profiles', 'all', '--workloads', 'ai-single'])
    );
    const saved = { ...session(), suiteFingerprint: plan.suiteFingerprint };
    mkdirSync(join(dir, 'runs'));
    writeFileSync(join(dir, 'session.json'), JSON.stringify(saved));
    writeFileSync(
      join(dir, 'runs', 'one.json'),
      JSON.stringify({
        config: config(),
        app: app(),
        status: 'ok',
        metrics: { cpuTotalS: 0.5, cpuJsS: 0.1 },
      })
    );
    const markdown = await quiet(() => main(['report', dir]));
    assert.ok(markdown.includes('Downstream string | 1 | 0.50'));
    assert.ok(markdown.includes('runtime plugin selection'));
    assert.equal(readFileSync(historicalReport, 'utf8'), before);
    await assert.rejects(
      quiet(() => runBenchmarkCLI(undefined, ['report', dir])),
      /different benchmark/
    );
    saved.benchmarkVersion = 'another-suite';
    writeFileSync(join(dir, 'session.json'), JSON.stringify(saved));
    await assert.rejects(
      quiet(() => main(['report', dir])),
      /different benchmark/
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('a failed CLI does not leak suite definitions into the historical CLI', async () => {
  await assert.rejects(
    main(['plan', '--profiles', 'unknown']),
    /Unknown profile/
  );
  const oldPlan = await quiet(() =>
    runBenchmarkCLI(undefined, [
      'plan',
      '--profiles',
      'plain',
      '--workloads',
      'ai-single',
    ])
  );
  assert.equal(oldPlan.benchmarkVersion, null);
  assert.equal(oldPlan.candidates[0], 'downstream-default');
  assert.equal(API_SUITE.baseline, 'downstream-controller');
  assert.ok(
    renderApiReport(session(), [], { groups: {} }).includes(
      'synthetic-api-plugins-v1'
    )
  );
});

test('resume rejects a legacy APK and changed replay timing before touching a phone', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'downstream-api-resume-'));
  try {
    const plan = await quiet(() =>
      main(['plan', '--profiles', 'all', '--workloads', 'ai-single'])
    );
    const replay = writeFixtures(join(dir, 'fixtures'));
    const bytes = Buffer.from('test APK');
    const apkSha256 = createHash('sha256').update(bytes).digest('hex');
    const build = {
      profile: 'all',
      apkSha256,
      benchmarkVersion: BENCHMARK_VERSION,
      suiteFingerprint: plan.suiteFingerprint,
    };
    const saved = {
      ...session(),
      suiteFingerprint: plan.suiteFingerprint,
      builds: [build],
      input: { ...session().input, folder: replay },
      workloads: { 'ai-single': { replaySha256: 'original-cadence-hash' } },
    };
    mkdirSync(join(dir, 'runs'));
    mkdirSync(join(dir, 'builds'));
    writeFileSync(join(dir, 'builds', 'all.apk'), bytes);
    writeFileSync(join(dir, 'session.json'), JSON.stringify(saved));
    await assert.rejects(
      quiet(() => main(['resume', dir])),
      /Replay chunks or cadence changed/
    );
    delete build.benchmarkVersion;
    writeFileSync(join(dir, 'session.json'), JSON.stringify(saved));
    await assert.rejects(
      quiet(() => main(['resume', dir])),
      /Saved APK belongs to a different benchmark/
    );
    saved.benchmarkVersion = undefined;
    writeFileSync(join(dir, 'session.json'), JSON.stringify(saved));
    await assert.rejects(
      quiet(() => main(['resume', dir])),
      /different benchmark entrypoint/
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
