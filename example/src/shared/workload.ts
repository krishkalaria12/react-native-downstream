import type { Fixture } from './fixtures';

export type WorkloadSize = 'fixture' | 10 | 50 | 100;
export type WorkloadShape = 'repeat' | 'open-fence';

export interface Workload {
  source: string;
  manifestChunks: string[];
  bytes: number;
  hash: string;
  size: WorkloadSize;
  shape: WorkloadShape;
}

function utf8Bytes(point: number): number[] {
  if (point < 0x80) return [point];
  if (point < 0x800)
    return [0xc0 + Math.floor(point / 64), 0x80 + (point % 64)];
  if (point < 0x10000) {
    return [
      0xe0 + Math.floor(point / 4096),
      0x80 + (Math.floor(point / 64) % 64),
      0x80 + (point % 64),
    ];
  }
  return [
    0xf0 + Math.floor(point / 262144),
    0x80 + (Math.floor(point / 4096) % 64),
    0x80 + (Math.floor(point / 64) % 64),
    0x80 + (point % 64),
  ];
}

function sourceInfo(source: string): { bytes: number; hash: string } {
  let bytes = 0;
  let a = 1;
  let b = 0;
  for (const character of source) {
    const point = character.codePointAt(0) ?? 0;
    for (const byte of utf8Bytes(point)) {
      bytes += 1;
      a = (a + byte) % 65521;
      b = (b + a) % 65521;
    }
  }
  const hash = b * 65536 + a;
  return { bytes, hash: `adler32:${hash.toString(16).padStart(8, '0')}` };
}

function splitManifestPattern(source: string, fixture: Fixture): string[] {
  const points = Array.from(source);
  const lengths = fixture.chunks.map((chunk) => Array.from(chunk).length);
  const chunks: string[] = [];
  for (let cursor = 0, index = 0; cursor < points.length; index += 1) {
    const length = lengths[index % lengths.length] ?? 1;
    chunks.push(points.slice(cursor, cursor + length).join(''));
    cursor += length;
  }
  return chunks;
}

function splitFixed(source: string, length: number): string[] {
  const points = Array.from(source);
  const chunks: string[] = [];
  for (let cursor = 0; cursor < points.length; cursor += length) {
    chunks.push(points.slice(cursor, cursor + length).join(''));
  }
  return chunks;
}

export function createWorkload(
  fixture: Fixture,
  size: WorkloadSize,
  shape: WorkloadShape = 'repeat'
): Workload {
  if (shape === 'open-fence') {
    if (size === 'fixture') throw new Error('Open fence requires a byte size');
    const target = size * 1024;
    const prefix = '```text\n';
    const unit = 'const value = compute(nextInput); // the fence stays open\n';
    const bodyBytes = target - prefix.length;
    const source =
      prefix +
      unit.repeat(Math.floor(bodyBytes / unit.length)) +
      'x'.repeat(bodyBytes % unit.length);
    return {
      source,
      manifestChunks: splitFixed(source, 16),
      ...sourceInfo(source),
      size,
      shape,
    };
  }
  if (size === 'fixture') {
    const source = fixture.chunks.join('');
    return {
      source,
      manifestChunks: fixture.chunks,
      ...sourceInfo(source),
      size,
      shape,
    };
  }

  const target = size * 1024;
  const unit = `${fixture.chunks.join('')}\n\n`;
  const unitBytes = sourceInfo(unit).bytes;
  const repetitions = Math.floor(target / unitBytes);
  let source = unit.repeat(repetitions);
  let remaining = target - repetitions * unitBytes;
  for (const character of unit) {
    const bytes = sourceInfo(character).bytes;
    if (bytes > remaining) break;
    source += character;
    remaining -= bytes;
  }
  source += ' '.repeat(remaining);
  return {
    source,
    manifestChunks: splitManifestPattern(source, fixture),
    ...sourceInfo(source),
    size,
    shape,
  };
}
