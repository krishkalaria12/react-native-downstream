import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { archiveFailedRuns, loadResumeState } from './bench.mjs';

function fixture(t) {
  const output = mkdtempSync(join(tmpdir(), 'phone-bench-resume-'));
  t.after(() => rmSync(output, { recursive: true, force: true }));
  mkdirSync(join(output, 'runs'));
  mkdirSync(join(output, 'builds'));
  const order = [0, 1, 2].map((index) => ({
    index,
    profile: 'highlighted',
    candidate: 'downstream-default',
    workload: 'ai-single',
    block: index - 1,
    warmup: index === 0,
  }));
  const apk = Buffer.from('original APK');
  writeFileSync(join(output, 'builds', 'highlighted.apk'), apk);
  writeFileSync(
    join(output, 'session.json'),
    JSON.stringify({
      schemaVersion: 2,
      order,
      builds: [
        {
          profile: 'highlighted',
          apkSha256: createHash('sha256').update(apk).digest('hex'),
        },
      ],
    })
  );
  const save = (index, status, name = `${index}.json`, overrides = {}) => {
    writeFileSync(
      join(output, 'runs', name),
      JSON.stringify({
        config: { ...order[index], ...overrides },
        status,
        metrics: {},
      })
    );
  };
  return { output, order, save };
}

test('resume skips successful warmups and measurements, retries failures, and retains indices', (t) => {
  const { output, order, save } = fixture(t);
  save(0, 'ok');
  save(1, 'failed', 'highlighted-0001-failed.json');
  const state = loadResumeState(output);
  assert.deepEqual([...state.completed], [0]);
  assert.deepEqual(state.pending, order.slice(1));
  assert.equal(state.savedRuns.length, 2);
});

test('failed attempts are archived without changing successes or overwriting earlier attempts', (t) => {
  const { output, order, save } = fixture(t);
  save(0, 'ok');
  save(1, 'failed');
  save(1, 'failed', 'highlighted-0001-failed.json');
  const success = readFileSync(join(output, 'runs', '0.json'));
  const previous = join(output, 'failed-attempts', '1-attempt-1.json');
  mkdirSync(join(output, 'failed-attempts'));
  writeFileSync(previous, 'earlier failure');
  archiveFailedRuns(output, loadResumeState(output).savedRuns, order[1]);
  assert.equal(readFileSync(previous, 'utf8'), 'earlier failure');
  assert.ok(existsSync(join(output, 'failed-attempts', '1-attempt-2.json')));
  assert.ok(
    existsSync(
      join(output, 'failed-attempts', 'highlighted-0001-failed-attempt-1.json')
    )
  );
  assert.ok(!existsSync(join(output, 'runs', '1.json')));
  assert.deepEqual(readFileSync(join(output, 'runs', '0.json')), success);
  save(1, 'ok');
  assert.deepEqual(loadResumeState(output).pending, [order[2]]);
});

test('resume rejects a saved run with a different schedule identity', (t) => {
  const { output, save } = fixture(t);
  save(0, 'ok', '0.json', { workload: 'ai-code-heavy' });
  assert.throws(
    () => loadResumeState(output),
    /does not match the session order/
  );
});

test('resume rejects duplicate successful measurements', (t) => {
  const { output, save } = fixture(t);
  save(0, 'ok');
  save(0, 'ok', 'duplicate.json');
  assert.throws(() => loadResumeState(output), /duplicate successful run/);
});

test('resume rejects a replaced or missing APK before running anything', (t) => {
  const { output } = fixture(t);
  const apk = join(output, 'builds', 'highlighted.apk');
  writeFileSync(apk, 'different APK');
  assert.throws(() => loadResumeState(output), /APK is missing or changed/);
  rmSync(apk);
  assert.throws(() => loadResumeState(output), /APK is missing or changed/);
});

test('a completed session has no remaining runs and does not need an APK', (t) => {
  const { output, order, save } = fixture(t);
  for (const item of order) save(item.index, 'ok');
  rmSync(join(output, 'builds', 'highlighted.apk'));
  assert.deepEqual(loadResumeState(output).pending, []);
});
