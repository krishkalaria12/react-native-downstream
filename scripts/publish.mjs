// Publishes the core package and its plugins to npm at their current versions.
//
//   NPM_TOKEN=npm_xxx node scripts/publish.mjs [--dry-run] [--tag <dist-tag>]
//   NPM_TOKEN_FILE=/path/to/token node scripts/publish.mjs
//
// The token is written to a throwaway npmrc that is removed on exit, so it never
// reaches ~/.npmrc or the project. It has to be a granular access token with
// publish rights on the @react-native-downstream org (and "bypass 2FA" if the
// account requires 2FA on publish).
//
// Each package is packed with pnpm, which rewrites `workspace:` ranges and runs
// the package's own build (bob for core, the RaTeX vendoring for math), and the
// tarball is what npm publishes. Core goes first because the plugins depend on
// it. Each published package is tagged `<tagPrefix><version>`, the tag its
// podspec's `source` points at, and the tags are pushed at the end.

import { execFileSync } from 'node:child_process';
import {
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
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

const packages = [
  { dir: '.', tagPrefix: 'core-v' },
  { dir: 'math', tagPrefix: 'math-v' },
  { dir: 'code', tagPrefix: 'code-v' },
].map((pkg) => {
  const { name, version } = JSON.parse(
    readFileSync(join(root, pkg.dir, 'package.json'), 'utf8')
  );
  return { ...pkg, name, version, gitTag: `${pkg.tagPrefix}${version}` };
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

if (read('git', ['status', '--porcelain'])) {
  fail('the working tree has uncommitted changes');
}
for (const { gitTag } of packages) {
  if (read('git', ['tag', '--list', gitTag])) {
    fail(`git tag ${gitTag} already exists; bump the version first`);
  }
}

const workDir = mkdtempSync(join(tmpdir(), 'downstream-publish-'));
process.on('exit', () => rmSync(workDir, { recursive: true, force: true }));
const npmrc = join(workDir, '.npmrc');
writeFileSync(
  npmrc,
  `registry=https://registry.npmjs.org/\n//registry.npmjs.org/:_authToken=${token}\n`,
  { mode: 0o600 }
);
const npmEnv = { env: { ...process.env, NPM_CONFIG_USERCONFIG: npmrc } };

try {
  console.log(`publish: authenticated as ${read('npm', ['whoami'], npmEnv)}`);
} catch {
  fail('npm rejected the token (npm whoami failed)');
}

for (const { name, version } of packages) {
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
}

run('pnpm', ['lint']);
run('pnpm', ['typecheck']);
run('pnpm', ['test']);

const publishedTags = [];
for (const pkg of packages) {
  const packDir = join(workDir, pkg.tagPrefix);
  mkdirSync(packDir);
  run('pnpm', ['pack', '--pack-destination', packDir], {
    cwd: join(root, pkg.dir),
  });
  const [tarball] = readdirSync(packDir).filter((f) => f.endsWith('.tgz'));
  if (!tarball) fail(`pnpm pack wrote no tarball for ${pkg.name}`);

  // The pack already ran the package's build, so npm's lifecycle run is skipped.
  try {
    run(
      'npm',
      [
        'publish',
        join(packDir, tarball),
        '--access',
        'public',
        '--tag',
        args.tag,
        '--ignore-scripts',
        ...(args['dry-run'] ? ['--dry-run'] : []),
      ],
      npmEnv
    );
  } catch {
    // Packages before this one are already live, so their tags still go out.
    if (publishedTags.length === 0) {
      fail(`${pkg.name}@${pkg.version} failed to publish`);
    }
    run('git', ['push', 'origin', ...publishedTags]);
    fail(
      `${pkg.name}@${pkg.version} failed to publish; ` +
        `already published and tagged: ${publishedTags.join(', ')}`
    );
  }

  if (!args['dry-run']) {
    run('git', ['tag', pkg.gitTag]);
    publishedTags.push(pkg.gitTag);
  }
}

if (args['dry-run']) {
  console.log('\npublish: dry run finished');
} else {
  run('git', ['push', 'origin', ...publishedTags]);
  for (const { name, version } of packages) {
    console.log(`publish: ${name}@${version} published`);
  }
}
