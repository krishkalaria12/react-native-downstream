/** Deterministic sources and arrival schedules shared by every phone renderer. */
export interface PhoneChunk {
  text: string;
  dueMs: number;
}

export interface PhoneWorkload {
  source: string;
  chunks: PhoneChunk[];
}

export function phoneSourceInfo(source: string) {
  let bytes = 0;
  let a = 1;
  let b = 0;
  let codepoints = 0;
  for (const character of source) {
    codepoints += 1;
    const point = character.codePointAt(0)!;
    const encoded =
      point < 0x80
        ? [point]
        : point < 0x800
          ? [0xc0 + (point >> 6), 0x80 + (point & 63)]
          : point < 0x10000
            ? [0xe0 + (point >> 12), 0x80 + ((point >> 6) & 63), 0x80 + (point & 63)]
            : [0xf0 + (point >> 18), 0x80 + ((point >> 12) & 63), 0x80 + ((point >> 6) & 63), 0x80 + (point & 63)];
    for (const byte of encoded) {
      bytes += 1;
      a = (a + byte) % 65521;
      b = (b + a) % 65521;
    }
  }
  return {
    bytes,
    codepoints,
    hash: `adler32:${(b * 65536 + a).toString(16).padStart(8, '0')}`,
  };
}

export function phoneFixedChunks(source: string): PhoneChunk[] {
  const points = Array.from(source);
  const chunks: PhoneChunk[] = [];
  for (let cursor = 0; cursor < points.length; cursor += 128) {
    chunks.push({ text: points.slice(cursor, cursor + 128).join(''), dueMs: chunks.length * 32 });
  }
  return chunks;
}

const prose = 'A fair rendering benchmark sends the same text to every library. The document grows on a fixed schedule while the phone records CPU use, memory, and frame timing. Clear prose should remain readable as new words arrive. Each completed paragraph keeps its original content.\n\n';
const mixed = '# A deterministic document\n\nA paragraph with **bold**, *emphasis*, ~~deleted words~~, `inline code`, a [reference link][ref], and Unicode café 日本語 😀.\n\n## Lists\n\n- First item\n- Second item with **bold**\n  - Nested item\n\n1. Ordered first\n2. Ordered second\n\n> A blockquote with a [link](https://example.com).\n\n| Item | Value |\n| :--- | ---: |\n| Apples | 3 |\n| Pears | 8 |\n\n```javascript\nconst answer = 42;\nconsole.log(answer);\n```\n\n---\n\n[ref]: https://example.com "Reference title"\n\n';

function sized(unit: string, bytes: number) {
  const repetitions = Math.floor(bytes / phoneSourceInfo(unit).bytes);
  const source = unit.repeat(repetitions);
  // Keep the final Markdown block complete; pad remaining bytes with plain text.
  const remaining = bytes - phoneSourceInfo(source).bytes;
  return source + prose.repeat(Math.ceil(remaining / prose.length)).slice(0, remaining);
}

export const phoneCorrectnessSource = '# Heading\n\nPlain text, **bold**, *italic*, ~~strike~~ and `code`. Café 日本語 😀.\n\n- Alpha\n- Beta\n  - Nested\n\n1. One\n2. Two\n\n> Quoted **words**.\n\n[Inline link](https://example.com) and [reference][target].\n\n| Name | Count |\n| :--- | ---: |\n| A | 2 |\n| B | 3 |\n\n```js\nconst value = "a < b";\n```\n\n---\n\nEscaped \\*asterisks\\* and entity &amp;.\n\n[target]: https://example.com "Title"\n';

export const phoneWorkloads: Record<string, PhoneWorkload> = Object.fromEntries(
  [
    ['basic10KiB', sized(prose, 10 * 1024)],
    ['prose10KiB', sized(prose, 10 * 1024)],
    ['mixed10KiB', sized(mixed, 10 * 1024)],
    ['prose50KiB', sized(prose, 50 * 1024)],
    ['correctness', phoneCorrectnessSource],
    ['correctness-open-emphasis', 'hello **world and _friends'],
    ['correctness-open-code', '```js\nconst x = 1;\n'],
    ['correctness-reference-setext', 'Guide\n-----\n\nRead [entry][ref].\n\n[ref]: https://example.com/ref\n'],
    ['correctness-raw-html', 'Raw <b>bold</b> and $x^2$ stay literal.\n\nH<sub>2</sub>O and E=mc<sup>2</sup>.\n'],
    ['correctness-tasks', '- [ ] first\n  - nested **item**\n- [x] second\n'],
  ].map(([name, source]) => [name!, { source: source!, chunks: phoneFixedChunks(source!) }])
);
