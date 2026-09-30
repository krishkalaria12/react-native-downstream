import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useState,
  useSyncExternalStore,
} from 'react';
import type { NativeSyntheticEvent } from 'react-native';

import {
  acceptControlledSource,
  initialControlledSource,
} from './controlledSource';
import NativeDownstreamMarkdownView from './DownstreamMarkdownViewNativeComponent';
import { getStreamState } from './createMarkdownStream';
import type {
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownLinkPressEvent,
  MarkdownViewProps,
} from './types';

export type DownstreamMarkdownProps = MarkdownViewProps;

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
export function DownstreamMarkdown(props: DownstreamMarkdownProps) {
  if (props.stream !== undefined)
    return <StreamMarkdown {...props} stream={props.stream} />;
  if (typeof props.markdown === 'string')
    return <ControlledMarkdown {...props} markdown={props.markdown} />;
  throw new Error(
    'DownstreamMarkdown requires exactly one of markdown or stream.'
  );
}

function ControlledMarkdown({
  markdown,
  style,
  selectable = false,
  syntaxHighlighting = true,
  onLinkPress,
  onError,
}: Extract<MarkdownViewProps, { markdown: string }>) {
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
      selectable={selectable}
      syntaxHighlighting={syntaxHighlighting}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
    />
  );
}

function StreamMarkdown({
  stream,
  style,
  selectable = false,
  syntaxHighlighting = true,
  onLinkPress,
  onError,
}: Extract<
  MarkdownViewProps,
  { stream: NonNullable<MarkdownViewProps['stream']> }
>) {
  const state = getStreamState(stream);
  const subscribe = useCallback(
    (callback: () => void) => state.subscribe(callback),
    [state]
  );
  const getRevision = useCallback(() => state.binding().revision, [state]);
  const revision = useSyncExternalStore(subscribe, getRevision, getRevision);
  const binding = state.binding();

  useLayoutEffect(() => state.bind(), [state]);

  const handleLinkPress = useCallback(
    (event: NativeSyntheticEvent<NativeLinkPress>) => {
      const { url, start, end } = event.nativeEvent;
      onLinkPress?.({ url, range: { start, end } });
    },
    [onLinkPress]
  );
  const handleError = useCallback(
    (event: NativeSyntheticEvent<NativeMarkdownError>) => {
      const nativeError = event.nativeEvent;
      if (nativeError.generation !== state.binding().generation) return;
      onError?.({
        ...nativeError,
        code: ERROR_CODES.has(nativeError.code)
          ? (nativeError.code as MarkdownErrorCode)
          : 'render_failed',
      });
    },
    [state, onError]
  );

  return (
    <NativeDownstreamMarkdownView
      style={style}
      markdown=""
      generation={binding.generation}
      sessionId={binding.sessionId}
      sessionSequence={binding.readySequence}
      sessionRevision={revision}
      selectable={selectable}
      syntaxHighlighting={syntaxHighlighting}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
    />
  );
}
