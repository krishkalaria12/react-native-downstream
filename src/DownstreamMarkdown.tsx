import { useCallback, useEffect, useState } from 'react';
import type { NativeSyntheticEvent } from 'react-native';

import {
  acceptControlledSource,
  initialControlledSource,
} from './controlledSource';
import NativeDownstreamMarkdownView from './DownstreamMarkdownViewNativeComponent';
import type {
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownLinkPressEvent,
  MarkdownViewProps,
} from './types';

/** The controlled-string input mode. The stream mode arrives with the session. */
export type DownstreamMarkdownProps = Extract<
  MarkdownViewProps,
  { markdown: string }
>;

type NativeLinkPress = Readonly<{ url: string; start: number; end: number }>;
type NativeMarkdownError = Readonly<{
  code: string;
  message: string;
  generation: number;
  sequence: number;
  recoverable: boolean;
}>;

const ERROR_CODES: ReadonlySet<string> = new Set<MarkdownErrorCode>([
  'invalid_input',
  'input_limit',
  'invalid_state',
  'session_bound',
  'parse_failed',
  'render_failed',
]);

/**
 * Renders Markdown with native text layout. Parsing and measurement happen in
 * native code; no parse tree crosses into JavaScript.
 */
export function DownstreamMarkdown({
  markdown,
  style,
  onLinkPress,
  onError,
}: DownstreamMarkdownProps) {
  const [controlled, setControlled] = useState(() =>
    initialControlledSource(markdown)
  );
  // Derive the next accepted source during render so native never sees a
  // rejected value.
  let current = controlled;
  if (current.input !== markdown) {
    current = acceptControlledSource(current, markdown);
    setControlled(current);
  }

  const { error, generation } = current;
  useEffect(() => {
    if (error) {
      onError?.(error);
    }
    // Report each rejection once, not on every callback identity change.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [error]);

  const handleLinkPress = useCallback(
    (event: NativeSyntheticEvent<NativeLinkPress>) => {
      const { url, start, end } = event.nativeEvent;
      const payload: MarkdownLinkPressEvent = { url, range: { start, end } };
      onLinkPress?.(payload);
    },
    [onLinkPress]
  );

  const handleError = useCallback(
    (event: NativeSyntheticEvent<NativeMarkdownError>) => {
      const nativeError = event.nativeEvent;
      if (nativeError.generation !== generation) {
        return;
      }
      const payload: MarkdownErrorEvent = {
        ...nativeError,
        code: ERROR_CODES.has(nativeError.code)
          ? (nativeError.code as MarkdownErrorCode)
          : 'render_failed',
      };
      onError?.(payload);
    },
    [generation, onError]
  );

  return (
    <NativeDownstreamMarkdownView
      style={style}
      markdown={current.source}
      generation={generation}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
    />
  );
}
