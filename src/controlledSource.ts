import type { MarkdownErrorEvent } from './types';

/** Contract limit for one canonical source, in UTF-8 bytes. */
export const MAX_SOURCE_BYTES = 1024 * 1024;

export type SourceProblem = Pick<MarkdownErrorEvent, 'code' | 'message'>;

/**
 * Checks well-formed UTF-16 and the UTF-8 size limit in one pass. Native
 * receives UTF-8, where a lone surrogate can no longer be detected, so the
 * check has to happen here.
 */
export function checkSource(source: string): SourceProblem | null {
  let bytes = 0;
  for (let i = 0; i < source.length; i++) {
    const unit = source.charCodeAt(i);
    if (unit < 0x80) {
      bytes += 1;
    } else if (unit < 0x800) {
      bytes += 2;
    } else if (unit >= 0xd800 && unit <= 0xdbff) {
      const next = source.charCodeAt(i + 1);
      if (!(next >= 0xdc00 && next <= 0xdfff)) {
        return loneSurrogate(i);
      }
      bytes += 4;
      i++;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return loneSurrogate(i);
    } else {
      bytes += 3;
    }
    if (bytes > MAX_SOURCE_BYTES) {
      return {
        code: 'input_limit',
        message: `Markdown source exceeds ${MAX_SOURCE_BYTES} bytes of UTF-8.`,
      };
    }
  }
  return null;
}

function loneSurrogate(index: number): SourceProblem {
  return {
    code: 'invalid_input',
    message: `Markdown source has an unpaired surrogate at UTF-16 offset ${index}.`,
  };
}

/** The controlled source a view displays, and the input that produced it. */
export type ControlledSource = Readonly<{
  /** The latest `markdown` prop, accepted or not. */
  input: string;
  /** The last accepted source, which native renders. */
  source: string;
  generation: number;
  /** Set when `input` was rejected; `source` is then the previous value. */
  error: MarkdownErrorEvent | null;
}>;

export function initialControlledSource(input: string): ControlledSource {
  return acceptControlledSource(
    { input: '', source: '', generation: 0, error: null },
    input
  );
}

/**
 * Applies a new `markdown` prop. A changed, valid value starts a new
 * generation. A rejected value keeps the previous source and generation.
 */
export function acceptControlledSource(
  previous: ControlledSource,
  input: string
): ControlledSource {
  if (input === previous.source && previous.error === null) {
    return previous.input === input ? previous : { ...previous, input };
  }
  const problem = checkSource(input);
  if (problem) {
    return {
      input,
      source: previous.source,
      generation: previous.generation,
      error: {
        ...problem,
        generation: previous.generation,
        sequence: 0,
        recoverable: true,
      },
    };
  }
  if (input === previous.source) {
    return {
      input,
      source: input,
      generation: previous.generation,
      error: null,
    };
  }
  return {
    input,
    source: input,
    generation: previous.generation + 1,
    error: null,
  };
}
