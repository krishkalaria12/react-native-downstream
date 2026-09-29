#!/usr/bin/env node
// Checks fixture playback and final HTML against a fresh md4c parse.
//
//     node src/fixtures/validate.mjs --md2html /tmp/md2html-fixtures

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const FIXTURE_DIR = dirname(fileURLToPath(import.meta.url));

function validateDirectory(md2html, directory) {
  const manifest = JSON.parse(
    readFileSync(join(directory, 'manifest.json'), 'utf8')
  );
  if (manifest.schemaVersion !== 1) {
    throw new Error('unsupported manifest schema');
  }
  if (JSON.stringify(manifest.oracle.flags) !== JSON.stringify(['--gfm'])) {
    throw new Error('unexpected oracle flags');
  }

  const version = execFileSync(md2html, ['--version'], {
    encoding: 'utf8',
  }).trim();
  if (version !== manifest.oracle.version) {
    throw new Error(`md2html version '${version}' does not match manifest`);
  }

  const seen = new Set();
  for (const fixture of manifest.fixtures) {
    const id = fixture.id;
    if (seen.has(id)) {
      throw new Error(`duplicate fixture ID: ${id}`);
    }
    seen.add(id);

    const source = readFileSync(join(directory, fixture.source));
    const chunks = fixture.chunks;
    if (
      !chunks.length ||
      chunks.some((chunk) => typeof chunk !== 'string' || !chunk)
    ) {
      throw new Error(`${id}: chunks must be nonempty strings`);
    }
    if (!Buffer.from(chunks.join(''), 'utf8').equals(source)) {
      throw new Error(`${id}: chunks do not reproduce source bytes`);
    }

    const checkpoints = fixture.checkpoints;
    const positions = checkpoints.map((checkpoint) => checkpoint.afterChunk);
    const ascending = positions.every(
      (position, index) => index === 0 || position > positions[index - 1]
    );
    if (!positions.length || !ascending || positions.at(-1) !== chunks.length) {
      throw new Error(
        `${id}: checkpoints must be sorted and end at completion`
      );
    }
    for (const checkpoint of checkpoints) {
      const position = checkpoint.afterChunk;
      if (position < 1 || position > chunks.length) {
        throw new Error(`${id}: checkpoint outside chunk range`);
      }
      if (checkpoint.prefix !== chunks.slice(0, position).join('')) {
        throw new Error(`${id}: checkpoint prefix does not match chunks`);
      }
      if (
        !checkpoint.expectedPreview ||
        !Object.keys(checkpoint.expectedPreview).length
      ) {
        throw new Error(`${id}: checkpoint has no preview expectation`);
      }
    }

    const html = execFileSync(md2html, ['--gfm'], { input: source });
    if (!html.equals(readFileSync(join(directory, fixture.expectedHtml)))) {
      throw new Error(`${id}: final HTML differs from fresh md4c parse`);
    }
    console.log(
      `ok ${id}: ${chunks.length} chunks, ${checkpoints.length} checkpoints`
    );
  }
}

const { values } = parseArgs({ options: { md2html: { type: 'string' } } });
if (!values.md2html) {
  console.error(
    'usage: node src/fixtures/validate.mjs --md2html <md4c 0.6.0 md2html binary>'
  );
  process.exit(2);
}
const md2html = resolve(values.md2html);
// The adversarial set shares the schema and oracle.
for (const directory of [FIXTURE_DIR, join(FIXTURE_DIR, 'adversarial')]) {
  validateDirectory(md2html, directory);
}
