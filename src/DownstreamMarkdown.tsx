import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  useSyncExternalStore,
} from 'react';
import type { NativeSyntheticEvent } from 'react-native';

import {
  acceptControlledSource,
  initialControlledSource,
} from './controlledSource';
import { resolveControls, type ResolvedControls } from './controls';
import NativeDownstreamMarkdownView from './DownstreamMarkdownViewNativeComponent';
import { getStreamState } from './createMarkdownStream';
import { resolveTheme, type ResolvedTheme } from './theme';
import type {
  MarkdownControlAction,
  MarkdownControlKind,
  MarkdownControls,
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownLinkPressEvent,
  MarkdownTranslations,
  MarkdownViewProps,
} from './types';
import { resolveMaxHeight } from './types';

export type DownstreamMarkdownProps = MarkdownViewProps;

type NativeLinkPress = Readonly<{ url: string; start: number; end: number }>;
type NativeMarkdownError = Readonly<{
  code: string;
  message: string;
  generation: number;
  sequence: number;
  recoverable: boolean;
  block: string;
  action: string;
}>;
type NativeControlAction = Readonly<{
  block: string;
  action: string;
  format: string;
  language: string;
  filename: string;
  wrapped: boolean;
  expanded: boolean;
  generation: number;
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

const CONTROL_BLOCKS: ReadonlySet<string> = new Set<MarkdownControlKind>([
  'code',
  'table',
]);
const CONTROL_ACTIONS: ReadonlySet<string> = new Set<MarkdownControlAction>([
  'copy',
  'download',
  'wrap',
  'expand',
]);
const CONTROL_FORMATS = new Set(['text', 'markdown', 'csv', 'tsv']);

function controlKind(value: string): MarkdownControlKind | undefined {
  return CONTROL_BLOCKS.has(value) ? (value as MarkdownControlKind) : undefined;
}

function controlAction(value: string): MarkdownControlAction | undefined {
  return CONTROL_ACTIONS.has(value)
    ? (value as MarkdownControlAction)
    : undefined;
}

/**
 * Turns native's flat event into the public shape, or null when it names a
 * control this build does not have.
 */
function toControlEvent(action: NativeControlAction) {
  const block = controlKind(action.block);
  const which = controlAction(action.action);
  if (block === undefined || which === undefined) return null;
  const event: {
    block: MarkdownControlKind;
    action: MarkdownControlAction;
    language: string;
    format?: 'text' | 'markdown' | 'csv' | 'tsv';
    wrapped?: boolean;
    expanded?: boolean;
    filename?: string;
  } = { block, action: which, language: action.language };
  if (which === 'wrap') {
    event.wrapped = action.wrapped;
  } else if (which === 'expand') {
    event.expanded = action.expanded;
  } else {
    if (CONTROL_FORMATS.has(action.format)) {
      event.format = action.format as 'text' | 'markdown' | 'csv' | 'tsv';
    }
    if (which === 'download' && action.filename !== '') {
      event.filename = action.filename;
    }
  }
  return event;
}

/**
 * Native's flat error into the public shape. A `control_failed` carries the
 * block and action that failed, so the failure can be routed and reported
 * without reading the message.
 */
function toErrorEvent(nativeError: NativeMarkdownError): MarkdownErrorEvent {
  const block = controlKind(nativeError.block);
  const action = controlAction(nativeError.action);
  const base = {
    message: nativeError.message,
    generation: nativeError.generation,
    sequence: nativeError.sequence,
    recoverable: nativeError.recoverable,
  };
  if (block !== undefined && action !== undefined) {
    return {
      ...base,
      code: 'control_failed',
      block,
      action,
    };
  }
  return {
    ...base,
    code: ERROR_CODES.has(nativeError.code)
      ? (nativeError.code as MarkdownErrorCode)
      : 'render_failed',
  };
}

/**
 * The values native reads, kept by value rather than by identity.
 *
 * `controls` and `translations` are usually written as inline object literals,
 * so their identity changes on every render of the parent. Memoizing on those
 * identities would hand native a fresh `controlLabels` array every time and
 * force a props update per render for a configuration that has not changed,
 * which is the opposite of what a native display path is for. Comparing the
 * resolved values instead means an unchanged configuration sends nothing.
 */
function useStableResolved(resolved: ResolvedControls): ResolvedControls {
  const stable = useRef(resolved);
  const previous = stable.current;
  if (
    previous.flags !== resolved.flags ||
    previous.codeFilename !== resolved.codeFilename ||
    previous.tableFilename !== resolved.tableFilename ||
    previous.csvSeparator !== resolved.csvSeparator ||
    previous.labels.length !== resolved.labels.length ||
    previous.labels.some((label, index) => label !== resolved.labels[index])
  ) {
    stable.current = resolved;
  }
  return stable.current;
}

/**
 * The resolved theme, kept by value rather than by identity.
 *
 * `theme` and `markdownStyle` are inline literals for the same reason
 * `controls` is, and a theme that reached native as a new array on every render
 * would re-intern and re-render the document behind it. Comparing the three
 * resolved arrays means an unchanged theme sends nothing, which is what lets a
 * chat's messages keep their prepared renderings while an unrelated state
 * change re-renders the list.
 */
function useStableTheme(resolved: ResolvedTheme): ResolvedTheme {
  const stable = useRef(resolved);
  const previous = stable.current;
  if (
    previous.slots.length !== resolved.slots.length ||
    previous.values.length !== resolved.values.length ||
    previous.fonts.length !== resolved.fonts.length ||
    previous.slots.some((slot, index) => slot !== resolved.slots[index]) ||
    previous.values.some((value, index) => value !== resolved.values[index]) ||
    previous.fonts.some((family, index) => family !== resolved.fonts[index])
  ) {
    stable.current = resolved;
  }
  return stable.current;
}

/**
 * Resolves the shared control props and the callbacks that route native events
 * back to the caller's handlers.
 *
 * Both callbacks are created once and read the current handlers and generation
 * through a ref. A caller normally writes its handlers inline, so an identity
 * that tracked them would change on every render and re-register the native
 * event each time; reading them late means the newest closure always runs, with
 * no per-render prop update. A control action from a superseded document is
 * recognised by its generation and dropped.
 */
function useControls(
  controls: MarkdownControls | undefined,
  translations: Partial<MarkdownTranslations> | undefined,
  onError: ((error: MarkdownErrorEvent) => void) | undefined,
  generationRef: { current: number }
) {
  const fresh = useMemo(
    () => resolveControls(controls, translations),
    [controls, translations]
  );
  const resolved = useStableResolved(fresh);
  const latest = useRef({
    handlers: fresh.handlers,
    generation: generationRef,
  });
  latest.current = { handlers: fresh.handlers, generation: generationRef };

  const handleControlAction = useCallback(
    (native: NativeSyntheticEvent<NativeControlAction>) => {
      const payload = native.nativeEvent;
      // A press on a document that has since been replaced describes a block
      // the reader is no longer looking at, so it is dropped rather than
      // delivered to the handlers of the document that replaced it.
      if (payload.generation !== latest.current.generation.current) return;
      const event = toControlEvent(payload);
      if (event === null) return;
      const { handlers } = latest.current;
      if (event.action === 'wrap') {
        handlers.codeWrap?.(event);
      } else if (event.action === 'expand') {
        handlers.codeExpand?.(event);
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
    []
  );

  /**
   * A control failure reaches that control's own onError, and the view's
   * onError when the control has none, so an app can watch every failure in
   * one place without giving up per-control handling. The block and action come
   * from the event, so nothing here depends on how the message is worded.
   */
  const controlError = useCallback(
    (error: MarkdownErrorEvent) => {
      const { handlers } = latest.current;
      const block = error.block;
      const action = error.action;
      const handler =
        block === undefined || action === undefined
          ? undefined
          : action === 'copy'
          ? block === 'code'
            ? handlers.codeCopyError
            : handlers.tableCopyError
          : action === 'download'
          ? block === 'code'
            ? handlers.codeDownloadError
            : handlers.tableDownloadError
          : undefined;
      if (handler) {
        handler(error);
        return;
      }
      onError?.(error);
    },
    [onError]
  );

  return { resolved, controlError, handleControlAction };
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
  lineNumbers = true,
  codeBlockMaxHeight,
  controls,
  translations,
  theme,
  markdownStyle,
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
  const resolvedTheme = useStableTheme(
    useMemo(() => resolveTheme(theme, markdownStyle), [theme, markdownStyle])
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
      const payload = toErrorEvent(nativeError);
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
      lineNumbers={lineNumbers}
      codeBlockMaxHeight={resolveMaxHeight(codeBlockMaxHeight)}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      themeSlots={resolvedTheme.slots}
      themeValues={resolvedTheme.values}
      themeFonts={resolvedTheme.fonts}
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
  lineNumbers = true,
  codeBlockMaxHeight,
  controls,
  translations,
  theme,
  markdownStyle,
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
  // A stream keeps its theme across a rebind, so a theme that has not changed
  // sends nothing however often a snapshot arrives.
  const resolvedTheme = useStableTheme(
    useMemo(() => resolveTheme(theme, markdownStyle), [theme, markdownStyle])
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
      const payload = toErrorEvent(nativeError);
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
      lineNumbers={lineNumbers}
      codeBlockMaxHeight={resolveMaxHeight(codeBlockMaxHeight)}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      themeSlots={resolvedTheme.slots}
      themeValues={resolvedTheme.values}
      themeFonts={resolvedTheme.fonts}
      onLinkPress={onLinkPress ? handleLinkPress : undefined}
      onMarkdownError={handleError}
      onControlAction={handleControlAction}
    />
  );
}
