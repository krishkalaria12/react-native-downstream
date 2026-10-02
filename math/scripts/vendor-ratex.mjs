// Fetches the RaTeX release pinned in ratex-version.json into vendor/.
//
//     node math/scripts/vendor-ratex.mjs
//
// Run at prepack, so the npm tarball carries the binary and no install script
// downloads anything: pnpm, `--ignore-scripts` and offline installs all work.
// Both downloads are checked against their pinned sha256 before anything is
// unpacked.
//
// What it writes:
//   vendor/RaTeX.xcframework   the iOS device and simulator slices, for the pod
//   vendor/fonts/*.ttf         the KaTeX fonts, from the Android release
//   vendor/host/               the macOS slice and its header, for the engine's
//                              host tests; not shipped

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const VENDOR = join(ROOT, 'vendor');
const pin = JSON.parse(readFileSync(join(ROOT, 'ratex-version.json'), 'utf8'));
const STAMP = join(VENDOR, '.version');

async function download(url, sha256) {
  const response = await fetch(url, { redirect: 'follow' });
  if (!response.ok) {
    throw new Error(`${url}: HTTP ${response.status}`);
  }
  const bytes = Buffer.from(await response.arrayBuffer());
  const actual = createHash('sha256').update(bytes).digest('hex');
  if (actual !== sha256) {
    throw new Error(`${url}: sha256 ${actual}, expected ${sha256}`);
  }
  return bytes;
}

function unzip(bytes, into) {
  const work = mkdtempSync(join(tmpdir(), 'ratex-'));
  const archive = join(work, 'archive.zip');
  writeFileSync(archive, bytes);
  mkdirSync(into, { recursive: true });
  execFileSync('unzip', ['-q', '-o', archive, '-d', into]);
  rmSync(work, { recursive: true, force: true });
}

/** Drops the macOS slice from the framework's manifest, which CocoaPods reads. */
function iosOnlyPlist(plist) {
  return plist.replace(
    /\s*<dict>(?:(?!<\/dict>)[\s\S])*?<string>macos-arm64_x86_64<\/string>[\s\S]*?<\/dict>/,
    ''
  );
}

async function main() {
  if (
    existsSync(STAMP) &&
    readFileSync(STAMP, 'utf8').trim() === pin.version &&
    !process.argv.includes('--force')
  ) {
    console.log(`RaTeX ${pin.version} is already vendored.`);
    return;
  }
  rmSync(VENDOR, { recursive: true, force: true });
  const work = mkdtempSync(join(tmpdir(), 'ratex-vendor-'));

  const framework = await download(pin.xcframework.url, pin.xcframework.sha256);
  unzip(framework, work);
  const source = join(work, 'RaTeX.xcframework');
  const target = join(VENDOR, 'RaTeX.xcframework');
  mkdirSync(target, { recursive: true });
  for (const slice of ['ios-arm64', 'ios-arm64_x86_64-simulator']) {
    cpSync(join(source, slice), join(target, slice), { recursive: true });
  }
  writeFileSync(
    join(target, 'Info.plist'),
    iosOnlyPlist(readFileSync(join(source, 'Info.plist'), 'utf8'))
  );
  cpSync(join(source, 'macos-arm64_x86_64'), join(VENDOR, 'host'), {
    recursive: true,
  });

  const aar = await download(pin.fonts.url, pin.fonts.sha256);
  const unpacked = join(work, 'aar');
  unzip(aar, unpacked);
  const fonts = join(VENDOR, 'fonts');
  mkdirSync(fonts, { recursive: true });
  for (const name of readdirSync(join(unpacked, 'assets', 'fonts'))) {
    if (name.endsWith('.ttf')) {
      cpSync(join(unpacked, 'assets', 'fonts', name), join(fonts, name));
    }
  }

  rmSync(work, { recursive: true, force: true });
  writeFileSync(STAMP, `${pin.version}\n`);
  console.log(`Vendored RaTeX ${pin.version} into ${VENDOR}.`);
}

main().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
