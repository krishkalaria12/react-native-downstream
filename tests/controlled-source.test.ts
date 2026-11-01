import {
  acceptControlledSource,
  checkSource,
  checkedUtf8Bytes,
  checkStreamingSource,
  initialControlledSource,
  MAX_SOURCE_BYTES,
} from '../src/controlledSource';

describe('checkSource', () => {
  it('counts validated UTF-8 bytes across Unicode boundaries', () => {
    for (const source of [
      '',
      '\u0000\u007f',
      '\u0080\u07ff',
      '\u0800\ud7ff\ue000\uffff',
      '\ud800\udc00\udbff\udfff',
      'Hello 👩‍💻 é 漢字 שלום\r\n',
    ]) {
      expect(checkedUtf8Bytes(source)).toBe(
        new TextEncoder().encode(source).length
      );
    }
    expect(checkedUtf8Bytes('a\ud800')).toEqual(checkSource('a\ud800'));
  });

  it('accepts well-formed text, including astral characters', () => {
    expect(checkSource('Hello 👩‍💻 é 漢字 שלום\r\n')).toBeNull();
    expect(checkSource('')).toBeNull();
  });

  it('rejects unpaired surrogates', () => {
    expect(checkSource('a\ud83d')?.code).toBe('invalid_input');
    expect(checkSource('a\udc69b')?.code).toBe('invalid_input');
    expect(checkSource('\udc69\ud83d')?.message).toContain('offset 0');
  });

  it('limits UTF-8 size, not UTF-16 length', () => {
    expect(checkSource('a'.repeat(MAX_SOURCE_BYTES))).toBeNull();
    expect(checkSource('a'.repeat(MAX_SOURCE_BYTES + 1))?.code).toBe(
      'input_limit'
    );
    // Three UTF-8 bytes per unit: under the limit in length, over in bytes.
    const cjk = '漢'.repeat(Math.floor(MAX_SOURCE_BYTES / 3) + 1);
    expect(cjk.length).toBeLessThan(MAX_SOURCE_BYTES);
    expect(checkSource(cjk)?.code).toBe('input_limit');
  });
});

describe('checkStreamingSource', () => {
  it('counts only appended UTF-8 bytes and validates replacements in full', () => {
    const first = { source: '漢😀', bytes: 7 };
    expect(checkStreamingSource(first, '漢😀é', true)).toEqual({
      source: '漢😀é',
      bytes: 9,
    });
    expect(checkStreamingSource(first, 'é', true)).toEqual({
      source: 'é',
      bytes: 2,
    });
    expect(checkedUtf8Bytes('\ud800é', 1, 7)).toBe(9);
  });

  it('holds a trailing high surrogate only while streaming', () => {
    const previous = { source: 'a', bytes: 1 };
    expect(checkStreamingSource(previous, 'a\ud83d', true)).toEqual(previous);
    expect(checkStreamingSource(previous, 'a😀', true)).toEqual({
      source: 'a😀',
      bytes: 5,
    });
    expect(checkStreamingSource(previous, 'a\ud83d', false)).toMatchObject({
      code: 'invalid_input',
    });
    expect(checkStreamingSource(previous, 'a\ud83dx', true)).toMatchObject({
      code: 'invalid_input',
    });
    expect(checkStreamingSource(previous, 'a\udc00', true)).toMatchObject({
      code: 'invalid_input',
    });
  });

  it('rejects suffixes that exceed the running byte limit', () => {
    const previous = {
      source: 'a'.repeat(MAX_SOURCE_BYTES - 2),
      bytes: MAX_SOURCE_BYTES - 2,
    };
    expect(checkStreamingSource(previous, previous.source + 'é', true)).toEqual(
      {
        source: previous.source + 'é',
        bytes: MAX_SOURCE_BYTES,
      }
    );
    expect(
      checkStreamingSource(previous, previous.source + '漢', true)
    ).toMatchObject({ code: 'input_limit' });
  });
});

describe('acceptControlledSource', () => {
  it('starts at generation 1 for the first valid source', () => {
    const state = initialControlledSource('# hi');
    expect(state).toEqual({
      input: '# hi',
      source: '# hi',
      generation: 1,
      error: null,
    });
  });

  it('keeps the generation for an unchanged value', () => {
    const state = initialControlledSource('a');
    expect(acceptControlledSource(state, 'a')).toBe(state);
  });

  it('starts a new generation when the value changes', () => {
    const state = acceptControlledSource(initialControlledSource('a'), 'b');
    expect(state.source).toBe('b');
    expect(state.generation).toBe(2);
  });

  it('keeps the last valid source after a rejected value', () => {
    const valid = initialControlledSource('a');
    const rejected = acceptControlledSource(valid, 'b\ud800');
    expect(rejected.source).toBe('a');
    expect(rejected.generation).toBe(1);
    expect(rejected.error).toMatchObject({
      code: 'invalid_input',
      generation: 1,
      sequence: 0,
      recoverable: true,
    });

    const recovered = acceptControlledSource(rejected, 'c');
    expect(recovered).toEqual({
      input: 'c',
      source: 'c',
      generation: 2,
      error: null,
    });
  });

  it('clears the error without a new generation when the old value returns', () => {
    const valid = initialControlledSource('a');
    const rejected = acceptControlledSource(valid, '\ud800');
    const restored = acceptControlledSource(rejected, 'a');
    expect(restored.error).toBeNull();
    expect(restored.generation).toBe(1);
  });

  it('rejects an invalid first value with an empty source', () => {
    const state = initialControlledSource('\udfff');
    expect(state.source).toBe('');
    expect(state.generation).toBe(0);
    expect(state.error?.code).toBe('invalid_input');
  });
});
