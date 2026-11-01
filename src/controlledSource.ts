import type { MarkdownErrorEvent } from './types';

/** Contract limit for one canonical source, in UTF-8 bytes. */
export const MAX_SOURCE_BYTES = 1024 * 1024;

export type SourceProblem = Pick<MarkdownErrorEvent, 'code' | 'message'>;

/**
 * Checks well-formed UTF-16 and the UTF-8 size limit in one pass. Native
 * receives UTF-8, where a lone surrogate can no longer be detected, so the
 * check has to happen here.
 */
export function checkedUtf8Bytes(
  source: string,
  start = 0,
  initialBytes = 0
): number | SourceProblem {
  let bytes = initialBytes;
  for (let i = start; i < source.length; i++) {
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
  return bytes;
}

export type CheckedStreamingSource = Readonly<{
  source: string;
  bytes: number;
}>;

/** Validate only the suffix when a controlled response grows. */
export function checkStreamingSource(
  previous: CheckedStreamingSource,
  input: string,
  streaming: boolean
): CheckedStreamingSource | SourceProblem {
  // An SDK can split a UTF-16 pair across updates. Native must only see the
  // complete pair; a response that finishes with half a pair is still invalid.
  const last = input.charCodeAt(input.length - 1);
  const source =
    streaming && last >= 0xd800 && last <= 0xdbff ? input.slice(0, -1) : input;
  const extendsPrevious = source.startsWith(previous.source);
  const bytes = checkedUtf8Bytes(
    source,
    extendsPrevious ? previous.source.length : 0,
    extendsPrevious ? previous.bytes : 0
  );
  return typeof bytes === 'number' ? { source, bytes } : bytes;
}

export function checkSource(source: string): SourceProblem | null {
  const result = checkedUtf8Bytes(source);
  return typeof result === 'number' ? null : result;
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
