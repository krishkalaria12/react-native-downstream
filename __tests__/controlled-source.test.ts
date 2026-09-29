import {
  acceptControlledSource,
  checkSource,
  initialControlledSource,
  MAX_SOURCE_BYTES,
} from '../controlledSource';

describe('checkSource', () => {
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
