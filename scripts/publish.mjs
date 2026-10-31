// Publishes the package at its current version to npm.
//
//   NPM_TOKEN=npm_xxx node scripts/publish.mjs [--dry-run] [--tag <dist-tag>]
//   NPM_TOKEN_FILE=/path/to/token node scripts/publish.mjs
//
// The token is written to a throwaway npmrc that is removed on exit, so it never
// reaches ~/.npmrc or the project. It has to be a granular access token with
// publish rights (and "bypass 2FA" if the account requires 2FA on publish).
// After a real publish the version is tagged `v<version>`, which is the tag the
// podspec's `source` points at, and the tag is pushed.

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const { values: args } = parseArgs({
  options: {
    'dry-run': { type: 'boolean', default: false },
    'tag': { type: 'string', default: 'latest' },
  },
});

function run(command, commandArgs, options = {}) {
  console.log(`\n$ ${command} ${commandArgs.join(' ')}`);
  return execFileSync(command, commandArgs, {
    cwd: root,
    stdio: 'inherit',
    ...options,
  });
}

function read(command, commandArgs, options = {}) {
  return execFileSync(command, commandArgs, {
    cwd: root,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    ...options,
  }).trim();
}

function fail(message) {
  console.error(`\npublish: ${message}`);
  process.exit(1);
}

function readToken() {
  if (process.env.NPM_TOKEN) return process.env.NPM_TOKEN.trim();
  if (process.env.NPM_TOKEN_FILE) {
    return readFileSync(process.env.NPM_TOKEN_FILE, 'utf8').trim();
  }
  return '';
}

const token = readToken();
if (!token) fail('set NPM_TOKEN or NPM_TOKEN_FILE');

const { name, version } = JSON.parse(
  readFileSync(join(root, 'package.json'), 'utf8')
);
const gitTag = `v${version}`;

if (read('git', ['status', '--porcelain'])) {
  fail('the working tree has uncommitted changes');
}
if (read('git', ['tag', '--list', gitTag])) {
  fail(`git tag ${gitTag} already exists; bump the version first`);
}

const npmrcDir = mkdtempSync(join(tmpdir(), 'downstream-npmrc-'));
const npmrc = join(npmrcDir, '.npmrc');
writeFileSync(
  npmrc,
  `registry=https://registry.npmjs.org/\n//registry.npmjs.org/:_authToken=${token}\n`,
  { mode: 0o600 }
);
process.on('exit', () => rmSync(npmrcDir, { recursive: true, force: true }));
const npmEnv = { env: { ...process.env, NPM_CONFIG_USERCONFIG: npmrc } };

try {
  console.log(`publish: authenticated as ${read('npm', ['whoami'], npmEnv)}`);
} catch {
  fail('npm rejected the token (npm whoami failed)');
}

// npm prints a bare string rather than an array when only one version exists.
let publishedVersions = [];
try {
  const output = JSON.parse(
    read('npm', ['view', name, 'versions', '--json'], npmEnv)
  );
  publishedVersions = Array.isArray(output) ? output : [output];
} catch {
  // A 404 here means the package has never been published.
}
if (publishedVersions.includes(version)) {
  fail(`${name}@${version} is already on npm; bump the version first`);
}

run('pnpm', ['lint']);
run('pnpm', ['typecheck']);
run('pnpm', ['test']);
run('pnpm', ['prepare']);

// prepare already built lib/, so npm's own lifecycle run is skipped.
run(
  'npm',
  [
    'publish',
    '--access',
    'public',
    '--tag',
    args.tag,
    '--ignore-scripts',
    ...(args['dry-run'] ? ['--dry-run'] : []),
  ],
  npmEnv
);

if (args['dry-run']) {
  console.log(`\npublish: dry run of ${name}@${version} finished`);
} else {
  run('git', ['tag', gitTag]);
  run('git', ['push', 'origin', gitTag]);
  console.log(`\npublish: ${name}@${version} is live`);
}
