import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useState,
  useSyncExternalStore,
} from 'react';
import type { NativeSyntheticEvent } from 'react-native';

import {
  acceptControlledSource,
  initialControlledSource,
} from './controlledSource';
import { resolveControls } from './controls';
import NativeDownstreamMarkdownView from './DownstreamMarkdownViewNativeComponent';
import { getStreamState } from './createMarkdownStream';
import type {
  MarkdownControls,
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownLinkPressEvent,
  MarkdownTranslations,
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
type NativeControlAction = Readonly<{
  block: string;
  action: string;
  format: string;
  language: string;
  filename: string;
  wrapped: boolean;
}>;

const ERROR_CODES: ReadonlySet<string> = new Set<MarkdownErrorCode>([
  'invalid_input',
  'input_limit',
  'invalid_state',
  'session_bound',
  'parse_failed',
  'render_failed',
  'control_failed',
]);

const CONTROL_FORMATS = new Set(['text', 'markdown', 'csv', 'tsv']);

/** Native reports a control failure as a code plus "code copy" in the text. */
const CONTROL_FAILURE = /\b(code|table) (copy|download|wrap)\b/;

/**
 * Turns native's flat event into the public shape, or null when it names a
 * control this build does not have.
 */
function toControlEvent(action: NativeControlAction) {
  if (action.block !== 'code' && action.block !== 'table') return null;
  if (
    action.action !== 'copy' &&
    action.action !== 'download' &&
    action.action !== 'wrap'
  ) {
    return null;
  }
  const event: {
    block: 'code' | 'table';
    action: 'copy' | 'download' | 'wrap';
    language: string;
    format?: 'text' | 'markdown' | 'csv' | 'tsv';
    wrapped?: boolean;
    filename?: string;
  } = { block: action.block, action: action.action, language: action.language };
  if (action.action === 'wrap') {
    event.wrapped = action.wrapped;
  } else {
    if (CONTROL_FORMATS.has(action.format)) {
      event.format = action.format as 'text' | 'markdown' | 'csv' | 'tsv';
    }
    if (action.action === 'download' && action.filename !== '') {
      event.filename = action.filename;
    }
  }
  return event;
}

/**
 * Resolves the shared control props and the callbacks that route native events
 * back to the caller's handlers. `generation` is read through a ref so a
 * callback identity does not change on every snapshot, and a control action
 * from a superseded document is still recognised and dropped.
 */
function useControls(
  controls: MarkdownControls | undefined,
  translations: Partial<MarkdownTranslations> | undefined,
  onError: ((error: MarkdownErrorEvent) => void) | undefined,
  generationRef: { current: number }
) {
  const resolved = useMemo(
    () => resolveControls(controls, translations),
    [controls, translations]
  );

  const handleControlAction = useCallback(
    (native: NativeSyntheticEvent<NativeControlAction>) => {
      const event = toControlEvent(native.nativeEvent);
      if (event === null) return;
      const { handlers } = resolved;
      if (event.action === 'wrap') {
        handlers.codeWrap?.(event);
      } else if (event.action === 'copy') {
        (event.block === 'code' ? handlers.codeCopy : handlers.tableCopy)?.(
          event
        );
      } else {
        (event.block === 'code'
          ? handlers.codeDownload
          : handlers.tableDownload)?.(event);
      }
    },
    [resolved]
  );

  /**
   * A control failure reaches that control's own onError, and the view's
   * onError when the control has none, so an app can watch every failure in
   * one place without giving up per-control handling.
   */
  const controlError = useCallback(
    (error: MarkdownErrorEvent) => {
      const { handlers } = resolved;
      const match = CONTROL_FAILURE.exec(error.message);
      if (match === null) {
        onError?.(error);
        return;
      }
      const code = match[1] === 'code';
      const handler =
        match[2] === 'copy'
          ? code
            ? handlers.codeCopyError
            : handlers.tableCopyError
          : match[2] === 'download'
          ? code
            ? handlers.codeDownloadError
            : handlers.tableDownloadError
          : undefined;
      if (handler) {
        handler(error);
        return;
      }
      onError?.(error);
    },
    [onError, resolved]
  );

  return useMemo(
    () => ({
      resolved,
      controlError,
      handleControlAction,
      /** True when a native event still describes the mounted document. */
      current: () => generationRef.current,
    }),
    [controlError, generationRef, handleControlAction, resolved]
  );
}

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
  selectable = true,
  syntaxHighlighting = true,
  codeBlockWrap = false,
  controls,
  translations,
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

  const generationRef = useMemo(() => ({ current: generation }), [generation]);
  const { resolved, controlError, handleControlAction } = useControls(
    controls,
    translations,
    onError,
    generationRef
  );

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
      if (payload.code === 'control_failed') {
        controlError(payload);
        return;
      }
      onError?.(payload);
    },
    [controlError, generation, onError]
  );

  return (
    <NativeDownstreamMarkdownView
      style={style}
      markdown={current.source}
      generation={generation}
      selectable={selectable}
      syntaxHighlighting={syntaxHighlighting}
      codeBlockWrap={codeBlockWrap}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
      onControlAction={handleControlAction}
    />
  );
}

function StreamMarkdown({
  stream,
  style,
  selectable = true,
  syntaxHighlighting = true,
  codeBlockWrap = false,
  controls,
  translations,
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

  const generationRef = useMemo(
    () => ({ current: binding.generation }),
    [binding.generation]
  );
  const { resolved, controlError, handleControlAction } = useControls(
    controls,
    translations,
    onError,
    generationRef
  );

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
      const payload: MarkdownErrorEvent = {
        ...nativeError,
        code: ERROR_CODES.has(nativeError.code)
          ? (nativeError.code as MarkdownErrorCode)
          : 'render_failed',
      };
      if (payload.code === 'control_failed') {
        controlError(payload);
        return;
      }
      onError?.(payload);
    },
    [controlError, onError, state]
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
      codeBlockWrap={codeBlockWrap}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
      onControlAction={handleControlAction}
    />
  );
}
