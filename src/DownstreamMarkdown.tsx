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
import type { ReactNode } from 'react';

import {
  NONE,
  resolveAnimate,
  type ResolvedAnimate,
  type ResolvedAnimating,
  type ResolvedCaret,
} from './animation';
import {
  acceptControlledSource,
  initialControlledSource,
} from './controlledSource';
import { resolveAllowedTags, type ResolvedAllowedTags } from './allowedTags';
import { resolveControls, type ResolvedControls } from './controls';
import NativeDownstreamMarkdownView from './DownstreamMarkdownViewNativeComponent';
import { getStreamState, type StreamState } from './createMarkdownStream';
import {
  FenceSlots,
  resolveRenderers,
  useFenceSlots,
  useRendererErrors,
  type ResolvedRenderers,
} from './fenceRenderers';
import {
  mathAvailable,
  resolveParseFlags,
  warnAboutMissingMathEngine,
} from './parseFlags';
import { openLinkSafely, resolveLinkUrl, resolvesLinks } from './linkSafety';
import { resolveIcons } from './icons';
import { resolveLinkVariants } from './linkVariants';

/**
 * The link variants as four flat arrays the native views read.
 *
 * Memoized on the style object rather than rebuilt per render, because the
 * arrays are new identities every time and a new identity is a props update the
 * shadow node would re-measure for.
 */
function useLinkVariantProps(
  markdownStyle: MarkdownViewProps['markdownStyle']
) {
  return useMemo(() => {
    // A composable style is an array; the resolver takes one style, so the last
    // entry wins the way it does in the theme resolver.
    const style = Array.isArray(markdownStyle)
      ? markdownStyle[markdownStyle.length - 1]
      : markdownStyle;
    const variants = resolveLinkVariants(
      style === false || style === null || style === undefined
        ? undefined
        : style
    );
    const prefixes: string[] = [];
    const counts: number[] = [];
    const colors: number[] = [];
    const underlines: boolean[] = [];
    for (const variant of variants) {
      counts.push(variant.match.length);
      prefixes.push(...variant.match);
      colors.push(variant.color ?? -1);
      underlines.push(variant.underline ?? false);
    }
    return { prefixes, counts, colors, underlines };
  }, [markdownStyle]);
}
import { TableFullscreen, type InheritedTableProps } from './TableFullscreen';
import { resolveTheme, type ResolvedTheme } from './theme';
import type {
  MarkdownControlAction,
  MarkdownControlKind,
  MarkdownControls,
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownImageEvent,
  MarkdownLinkPressEvent,
  MarkdownLinkSafetyOptions,
  MarkdownMathErrorEvent,
  MarkdownTableFullscreenEvent,
  MarkdownTaskPressEvent,
  MarkdownTranslations,
  MarkdownViewProps,
} from './types';
import {
  defaultTableMaxHeight,
  defaultTranslations,
  resolveMaxHeight,
} from './types';

export type DownstreamMarkdownProps = MarkdownViewProps;

type NativeLinkPress = Readonly<{ url: string; start: number; end: number }>;
type NativeImagePress = Readonly<{
  url: string;
  alt: string;
  title: string;
  start: number;
  end: number;
}>;
type NativeMathError = Readonly<{
  source: string;
  message: string;
  display: boolean;
  generation: number;
}>;
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
  url: string;
  wrapped: boolean;
  expanded: boolean;
  ordinal: number;
  /**
   * The block's own Markdown, for a full-screen table. Empty otherwise: the
   * other actions carry nothing a second view would need.
   */
  markdown: string;
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
  'image',
  'details',
]);
const CONTROL_ACTIONS: ReadonlySet<string> = new Set<MarkdownControlAction>([
  'copy',
  'download',
  'wrap',
  'expand',
  'fullscreen',
]);
const CONTROL_FORMATS = new Set(['text', 'markdown', 'csv', 'tsv']);

/**
 * rehype-harden's and Streamdown's default: any HTTPS host. A module-level
 * constant so a view that passes nothing sends the same array every time rather
 * than a fresh literal.
 */
const IMAGE_PREFIX_ALLOW_ALL: readonly string[] = ['*'];

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
    url?: string;
    markdown?: string;
    ordinal?: number;
  } = { block, action: which, language: action.language };
  if (which === 'wrap') {
    event.wrapped = action.wrapped;
  } else if (which === 'expand') {
    event.expanded = action.expanded;
    // The block's ordinal among its own kind, which is what an app that keys a
    // press on needs: code, table and image ordinals are three different counts.
    event.ordinal = action.ordinal;
  } else if (which === 'fullscreen') {
    // The table's own Markdown, so a modal holding a second view is the same
    // table rather than one rebuilt from the model. Native writes it through
    // `Export.cpp`, so the modal sees exactly what a copy would produce.
    if (action.markdown !== '') {
      event.markdown = action.markdown;
      event.ordinal = action.ordinal;
    }
  } else {
    if (CONTROL_FORMATS.has(action.format)) {
      event.format = action.format as 'text' | 'markdown' | 'csv' | 'tsv';
    }
    if (which === 'download' && action.filename !== '') {
      event.filename = action.filename;
    }
    // An image saves its own bytes, so a format is meaningless and only the
    // destination is worth reporting.
    if (block === 'image' && action.url !== '') {
      event.url = action.url;
      delete event.format;
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
 * The resolved animation, kept by value rather than by identity.
 *
 * `animated` is usually written as an inline object literal, so its identity
 * changes on every render of the parent. Sending a fresh `animationTiming` array
 * each time would rebuild the fade's state and re-render the document behind it
 * for a configuration that has not changed, which is the opposite of what the
 * animation is for. Comparing the resolved values means an unchanged animation
 * sends nothing.
 */
function useStableAnimate(resolved: ResolvedAnimate): ResolvedAnimate {
  const stable = useRef(resolved);
  const previous = stable.current;
  if (
    previous.animation !== resolved.animation ||
    previous.split !== resolved.split ||
    previous.timing.some((value, index) => value !== resolved.timing[index])
  ) {
    stable.current = resolved;
  }
  return stable.current;
}

/**
 * The animation callbacks, with Streamdown's mount rule.
 *
 * `onAnimationStart` fires when `isAnimating` turns true *including on mount*,
 * because a view that mounts mid-stream is animating and an app showing a
 * "typing" indicator needs to know. `onAnimationEnd` does not fire on mount,
 * because a view that mounts with the flag already off was never animating and
 * has nothing to report.
 *
 * A null flag is a view that does not know, which is the stream case handled by
 * the controller's own transitions rather than by a prop.
 */
function useAnimationCallbacks(
  isAnimating: boolean | undefined,
  onStart: (() => void) | undefined,
  onEnd: (() => void) | undefined
): void {
  const mounted = useRef(false);
  const handlers = useRef({ onStart, onEnd });
  handlers.current = { onStart, onEnd };
  useLayoutEffect(() => {
    if (isAnimating === undefined) return;
    if (isAnimating) handlers.current.onStart?.();
    else if (mounted.current) handlers.current.onEnd?.();
    mounted.current = true;
  }, [isAnimating]);
}

/**
 * The same callbacks for a stream, whose transitions the controller owns.
 *
 * There is no prop to watch, so the rule is the controller's: the first append of
 * a generation is a start, `finish()` is an end, and a `reset()` ends the current
 * cycle so the next append starts a new one. A view that subscribes while text is
 * already arriving reports a start, which is the controlled view's mount rule: a
 * message mounted mid-answer is animating.
 *
 * An explicit `isAnimating` takes the callbacks over along with the flag, so
 * `follows` is false then and the controller's transitions are ignored.
 */
function useStreamAnimationCallbacks(
  state: StreamState,
  follows: boolean,
  onStart: (() => void) | undefined,
  onEnd: (() => void) | undefined
): void {
  const handlers = useRef({ onStart, onEnd });
  handlers.current = { onStart, onEnd };
  useEffect(() => {
    if (!follows) return undefined;
    if (state.isStreaming()) handlers.current.onStart?.();
    return state.subscribeAnimation((animating) => {
      if (animating) handlers.current.onStart?.();
      else handlers.current.onEnd?.();
    });
  }, [state, follows]);
}

/**
 * What native reads for the animation and the caret, gathered once.
 *
 * `animating` is `auto` for a stream so the platform follows the session's own
 * flag, and an explicit `on` or `off` for a controlled view, which has nothing
 * else to go on.
 */
function useAnimationProps(
  animated: MarkdownViewProps['animated'],
  caret: MarkdownViewProps['caret'],
  animating: ResolvedAnimating
) {
  const fresh = useMemo(() => resolveAnimate(animated), [animated]);
  const resolved = useStableAnimate(fresh);
  // `none` is what the platforms' draw pass reads as "no fade", and an empty
  // timing array is what keeps the prop off native's comparison entirely.
  return {
    animation: resolved.animation,
    animationSplit: resolved.split,
    animationTiming: resolved === NONE ? undefined : resolved.timing,
    animating,
    caret: (caret ?? 'none') as ResolvedCaret,
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
    previous.imageFilename !== resolved.imageFilename ||
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
 * A ref the control routing reads the full-screen opener through.
 *
 * The modal's state lives in the component and the routing lives in `useControls`,
 * and a ref between them is what lets each own its half without either knowing
 * about the other's lifecycle.
 */
type FullscreenRef = {
  current: ((event: MarkdownTableFullscreenEvent) => void) | undefined;
};

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
  generationRef: { current: number },
  fullscreen: FullscreenRef
) {
  const fresh = useMemo(
    () => resolveControls(controls, translations),
    [controls, translations]
  );
  const resolved = useStableResolved(fresh);
  const latest = useRef({
    handlers: fresh.handlers,
    generation: generationRef,
    fullscreen: fullscreen.current,
  });
  latest.current.handlers = fresh.handlers;
  latest.current.generation = generationRef;
  latest.current.fullscreen = fullscreen.current;

  const handleControlAction = useCallback(
    (native: NativeSyntheticEvent<NativeControlAction>) => {
      const payload = native.nativeEvent;
      // A press on a document that has since been replaced describes a block
      // the reader is no longer looking at, so it is dropped rather than
      // delivered to the handlers of the document that replaced it.
      if (payload.generation !== latest.current.generation.current) return;
      const event = toControlEvent(payload);
      if (event === null) return;
      const { handlers, fullscreen: openFullscreen } = latest.current;
      if (event.action === 'wrap') {
        handlers.codeWrap?.(event);
      } else if (event.action === 'expand') {
        (event.block === 'table'
          ? handlers.tableExpand
          : event.block === 'details'
            ? handlers.detailsExpand
            : handlers.codeExpand)?.(event);
      } else if (event.action === 'fullscreen') {
        // The modal is this view's, not a control handler's: the press has to
        // open a second view, which only the component holding the first can do.
        if (event.markdown !== undefined) {
          openFullscreen?.({
            markdown: event.markdown,
            ordinal: event.ordinal ?? 0,
          });
        }
        handlers.tableFullscreen?.(event);
      } else if (event.block === 'image') {
        handlers.imageDownload?.(event);
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
                : block === 'image'
                  ? handlers.imageDownloadError
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
 * The full-screen table modal.
 *
 * Mounted only while a table is open, so the whole second view — including the
 * native one behind it — is unmounted when the reader closes it. The source is
 * held rather than derived from the document because the press carried it: that
 * is the table's own Markdown, which is what makes the modal the same table
 * rather than a re-parse of a re-export.
 */
function useTableFullscreen(
  onTableFullscreen:
    | ((event: MarkdownTableFullscreenEvent) => void)
    | undefined,
  renderTableFullscreen:
    | ((event: MarkdownTableFullscreenEvent) => ReactNode)
    | undefined
) {
  const [open, setOpen] = useState<MarkdownTableFullscreenEvent | undefined>(
    undefined
  );
  const latest = useRef({ onTableFullscreen, renderTableFullscreen });
  latest.current = { onTableFullscreen, renderTableFullscreen };

  // `renderTableFullscreen` is called during render rather than held in state,
  // so a modal an app draws is a child of this view and gets its own updates,
  // and closing it is this view's state rather than the app's.
  const request = useCallback((event: MarkdownTableFullscreenEvent) => {
    latest.current.onTableFullscreen?.(event);
    setOpen(event);
  }, []);
  const close = useCallback(() => setOpen(undefined), []);
  return { open, request, close, renderTableFullscreen };
}

/**
 * Renders the full-screen table, or nothing.
 *
 * Mounted here rather than inside the press handler so the modal is a child of
 * the view and shares its theme, and closed by the same state that opened it.
 */
function TableFullscreenHost({
  open,
  close,
  render,
  inherited,
  translations,
}: Readonly<{
  open: MarkdownTableFullscreenEvent | undefined;
  close: () => void;
  render: ((event: MarkdownTableFullscreenEvent) => ReactNode) | undefined;
  inherited: InheritedTableProps;
  translations: Partial<MarkdownTranslations> | undefined;
}>) {
  if (open === undefined) {
    return null;
  }
  const custom = render?.(open);
  if (custom !== undefined && custom !== null) {
    return <>{custom}</>;
  }
  return (
    <TableFullscreen
      markdown={open.markdown}
      closeLabel={
        translations?.exitFullscreen ?? defaultTranslations.exitFullscreen
      }
      inherited={inherited}
      onClose={close}
    />
  );
}

/**
 * Resolves the props that reach the parser and the image loader, and the
 * callbacks that route their native events back to the caller.
 *
 * The math default is settled once per process rather than per render, because
 * it is a question about the binary and not about the document: a view that
 * decided "off" and then turned "on" would lay the same source out twice with
 * two different answers, and the second one is a relayout the reader sees.
 *
 * `allowedImagePrefixes` is normalized and kept by value for the same reason
 * `controls` is: it is almost always an inline literal, and a new array on every
 * render would send a props update for a policy that has not changed. The link
 * options are the same case, which is why the origin is a trimmed string rather
 * than the object the caller passed.
 */
function useMathAndImages(
  props: {
    math?: MarkdownViewProps['math'];
    images?: boolean;
    allowedImagePrefixes?: readonly string[];
    onMathError?: (event: MarkdownMathErrorEvent) => void;
    onImagePress?: (event: MarkdownImageEvent) => void;
    inlineHtml?: boolean;
    syntax?: MarkdownViewProps['syntax'];
    cjk?: boolean;
    links?: MarkdownViewProps['links'];
    disableAutolinkProtocols?: MarkdownViewProps['disableAutolinkProtocols'];
    allowedTags?: MarkdownViewProps['allowedTags'];
    literalTagContent?: MarkdownViewProps['literalTagContent'];
  },
  currentGeneration: () => number
) {
  const {
    math,
    images,
    allowedImagePrefixes,
    onMathError,
    onImagePress,
    inlineHtml,
    syntax,
    cjk,
    links,
    disableAutolinkProtocols,
    allowedTags,
    literalTagContent,
  } = props;

  // One read of the platform's answer for the lifetime of the process. A test
  // that installs a mock engine calls the reset hook to observe the change.
  const engine = useRef<boolean | undefined>(undefined);
  if (engine.current === undefined) engine.current = mathAvailable();

  const resolved = useMemo(
    () =>
      resolveParseFlags(
        {
          math,
          inlineHtml,
          syntax,
          cjk,
          links,
          autolinkProtocols: disableAutolinkProtocols,
          literalTagContent,
        },
        engine.current
      ),
    [
      math,
      inlineHtml,
      syntax,
      cjk,
      links,
      disableAutolinkProtocols,
      literalTagContent,
    ]
  );
  const resolvedStable = useRef(resolved);
  if (resolvedStable.current.flags !== resolved.flags) {
    resolvedStable.current = resolved;
  }

  // `['*']` is the default, and a view that passes the default explicitly is the
  // same view as one that passes nothing, so both send the same array.
  const prefixes = useMemo(
    () =>
      allowedImagePrefixes === undefined || allowedImagePrefixes.length === 0
        ? IMAGE_PREFIX_ALLOW_ALL
        : allowedImagePrefixes,
    [allowedImagePrefixes]
  );
  const prefixesStable = useRef<readonly string[]>(prefixes);
  if (
    prefixesStable.current.length !== prefixes.length ||
    prefixesStable.current.some((entry, index) => entry !== prefixes[index])
  ) {
    prefixesStable.current = prefixes;
  }

  // Formulas the reader has already been told about, so `onMathError` fires once
  // per distinct source per view rather than once per snapshot that contains it.
  const reportedMathErrors = useRef(new Set<string>());
  const latest = useRef({ onImagePress, onMathError, currentGeneration });
  latest.current = { onImagePress, onMathError, currentGeneration };

  const handleImagePress = useCallback(
    (event: NativeSyntheticEvent<NativeImagePress>) => {
      const { url, alt, title, start, end } = event.nativeEvent;
      // The title is a native empty string when the writer gave none, and the
      // public shape makes it optional rather than always-present.
      const payload: MarkdownImageEvent =
        title === ''
          ? { url, alt, range: { start, end } }
          : { url, alt, title, range: { start, end } };
      latest.current.onImagePress?.(payload);
    },
    []
  );

  const handleMathError = useCallback(
    (event: NativeSyntheticEvent<NativeMathError>) => {
      const native = event.nativeEvent;
      // An error from a document that has since been replaced describes a
      // formula the reader is no longer looking at.
      if (native.generation !== latest.current.currentGeneration()) return;
      const key = `${native.display ? 'd' : 'i'}\u0000${native.source}`;
      if (reportedMathErrors.current.has(key)) return;
      reportedMathErrors.current.add(key);
      if (!engine.current && __DEV__) {
        // A formula with no engine is not an error the caller can act on beyond
        // installing the package, so it is a development warning instead of an
        // event per formula.
        warnAboutMissingMathEngine();
        return;
      }
      latest.current.onMathError?.({
        source: native.source,
        message: native.message,
        display: native.display,
        generation: native.generation,
      });
    },
    []
  );

  // The tag policy is kept by value for the same reason the prefix list is: it is
  // almost always an inline literal, and a new array on every render would send
  // a props update — and a reparse, because it is a parse option — for a policy
  // that has not changed.
  const tags = resolveAllowedTags(allowedTags);
  const tagsStable = useRef<ResolvedAllowedTags>(tags);
  if (
    tagsStable.current.tags.length !== tags.tags.length ||
    tagsStable.current.tags.some((entry, index) => entry !== tags.tags[index])
  ) {
    tagsStable.current = tags;
  }

  return {
    parseFlags: resolvedStable.current.flags,
    images: images !== false,
    allowedImagePrefixes: prefixesStable.current,
    allowedTags: tagsStable.current.tags,
    onImagePress: onImagePress ? handleImagePress : undefined,
    onMathError: handleMathError,
  };
}

/**
 * The registered fence renderers, kept by value rather than by identity.
 *
 * `plugins` is an inline object literal in every real call, so its identity
 * changes on every render of the parent. Memoizing on that identity would ask
 * the engine for the same fences again and again and would re-join every slot
 * with its component each time. Comparing the resolved language list means an
 * unchanged configuration asks nothing and re-renders nothing, which is what
 * lets a chat's messages keep their mounted renderers while an unrelated state
 * change re-renders the list.
 */
function useStableRenderers(resolved: ResolvedRenderers): ResolvedRenderers {
  const stable = useRef(resolved);
  const previous = stable.current;
  if (
    previous.languages.length !== resolved.languages.length ||
    previous.languages.some(
      (language, index) =>
        language !== resolved.languages[index] ||
        previous.components.get(language) !== resolved.components.get(language)
    )
  ) {
    stable.current = resolved;
  }
  return stable.current;
}

/**
 * Resolves `plugins`, queries the engine for this document's fences, and
 * returns the slot children for the native view.
 *
 * The query is a synchronous TurboModule call made during render, which is the
 * whole design: the slot children and the document they belong to arrive in
 * the same commit, so a fence becomes a component in the frame it should, and
 * not a frame later. A view with no registered renderers asks nothing, sends
 * nothing and renders no children, so its behaviour and its cost are exactly
 * what they were before renderers existed.
 */
function useFenceRenderers(
  plugins: MarkdownViewProps['plugins'],
  document: {
    source?: string;
    sessionId?: string;
    generation: number;
    sequence?: number;
    revision?: number;
    parseFlags: number;
  },
  onError: ((error: MarkdownErrorEvent) => void) | undefined,
  currentGeneration: () => number
): ReactNode {
  const resolved = useStableRenderers(
    useMemo(() => resolveRenderers(plugins), [plugins])
  );
  const slots = useFenceSlots({ ...document, resolved });
  // The document a slot belongs to: a generation alone is not one, because a
  // view handed a new stream numbers its generations from the same start, and a
  // renderer's state and its one report belong to one message.
  const scope = `${document.sessionId ?? ''}\u0000${document.generation}`;
  const onFailure = useRendererErrors({ onError, currentGeneration, scope });
  if (slots.length === 0) {
    return null;
  }
  return <FenceSlots slots={slots} scope={scope} onFailure={onFailure} />;
}

/**
 * What a link press does.
 *
 * With an `onLinkPress` the app decides and this is a straight report: the
 * source's own `href` and, for a relative destination, the `url` it resolves to
 * beside it. Without one, the press does nothing at all today, which no reader
 * expects from something drawn as a link, so the view opens it — behind link
 * safety, which is on by default and asks first.
 *
 * The safety config is read through a ref rather than captured, so a caller
 * writing `linkSafety={{ onLinkCheck }}` inline does not re-register the native
 * handler on every render. The handler itself is created once.
 */
function useLinkPress(
  onLinkPress: ((event: MarkdownLinkPressEvent) => void) | undefined,
  safety: {
    enabled: boolean;
    origin: string;
    onLinkCheck?: (url: string) => boolean | Promise<boolean>;
    renderModal?: MarkdownLinkSafetyOptions['renderModal'];
  },
  translations: Partial<MarkdownTranslations> | undefined
) {
  const latest = useRef({ onLinkPress, safety, translations });
  latest.current = { onLinkPress, safety, translations };

  return useCallback((event: NativeSyntheticEvent<NativeLinkPress>) => {
    const { url: href, start, end } = event.nativeEvent;
    const current = latest.current;
    if (current.onLinkPress) {
      const payload: MarkdownLinkPressEvent = {
        url: resolveLinkUrl(href, current.safety.origin),
        href,
        range: { start, end },
      };
      current.onLinkPress(payload);
      return;
    }
    void openLinkSafely(href, {
      enabled: current.safety.enabled,
      origin: current.safety.origin,
      onLinkCheck: current.safety.onLinkCheck,
      renderModal: current.safety.renderModal,
      translations: current.translations,
    });
  }, []);
}

/**
 * Resolves `linkSafety` into what a press needs.
 *
 * `true`, `undefined` and `{ enabled: true }` are the same configuration, and
 * `false` is the only way to turn the confirmation off — which is why a falsy
 * value is read as "no interception" rather than as "no options".
 */
function useLinkSafety(
  linkSafety: MarkdownViewProps['linkSafety'],
  links: MarkdownViewProps['links']
) {
  const options: MarkdownLinkSafetyOptions | undefined =
    typeof linkSafety === 'object' && linkSafety !== null
      ? linkSafety
      : undefined;
  const origin = useMemo(
    () => (resolvesLinks(links) ? (links?.defaultOrigin?.trim() ?? '') : ''),
    [links]
  );
  const enabled = linkSafety !== false && options?.enabled !== false;
  const onLinkCheck = options?.onLinkCheck;
  const renderModal = options?.renderModal;
  return useMemo(
    () => ({ enabled, origin, onLinkCheck, renderModal }),
    [enabled, origin, onLinkCheck, renderModal]
  );
}

/**
 * The full-screen table, wired into the control routing.
 *
 * One hook rather than three lines in each component: the two bodies are already
 * near-identical for everything a table's modal needs, and a table's modal that
 * worked in one and not the other would be a bug nobody would look for.
 */
function useFullscreenTable(
  onTableFullscreen: MarkdownViewProps['onTableFullscreen'],
  renderTableFullscreen: MarkdownViewProps['renderTableFullscreen'],
  translations: Partial<MarkdownTranslations> | undefined,
  inherited: InheritedTableProps
) {
  const {
    open,
    request,
    close,
    renderTableFullscreen: render,
  } = useTableFullscreen(onTableFullscreen, renderTableFullscreen);
  const ref = useMemo<FullscreenRef>(() => ({ current: request }), [request]);
  const element = (
    <TableFullscreenHost
      open={open}
      close={close}
      render={render}
      inherited={inherited}
      translations={translations}
    />
  );
  return { fullscreen: ref, element };
}

/**
 * The props a full-screen table inherits from the view around it.
 *
 * Written out rather than spread from `props`, because every prop here is a
 * decision: `style` does not travel because the modal fills the screen, and
 * `controls` does not because the modal owns its own chrome. A spread would
 * inherit both by accident and inherit whatever a later release adds without
 * anyone deciding.
 */
function useInheritedTableProps(
  props: Record<string, unknown>
): InheritedTableProps {
  const keys: ReadonlyArray<string> = [
    'selectable',
    'syntaxHighlighting',
    'codeBlockWrap',
    'lineNumbers',
    'theme',
    'markdownStyle',
    'translations',
    'math',
    'onMathError',
    'inlineHtml',
    'syntax',
    'cjk',
    'links',
    'disableAutolinkProtocols',
    'allowedTags',
    'literalTagContent',
    'images',
    'allowedImagePrefixes',
    'onLinkPress',
    'onImagePress',
    'onTagPress',
    'linkSafety',
    'onError',
  ];
  const inherited = {} as Record<string, unknown>;
  for (const key of keys) {
    if (props[key] !== undefined) {
      inherited[key] = props[key];
    }
  }
  return inherited as InheritedTableProps;
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
  plugins,
  animated,
  isAnimating,
  caret,
  onAnimationStart,
  onAnimationEnd,
  onLinkPress,
  linkSafety,
  tableMaxHeight,
  dir,
  icons,
  onTableFullscreen,
  onTaskPress,
  onLinkLongPress,
  linkPreview = false,
  renderTableFullscreen,
  onError,
  ...rest
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
  const fullscreen = useFullscreenTable(
    onTableFullscreen,
    renderTableFullscreen,
    translations,
    useInheritedTableProps(rest)
  );
  // A task checkbox is drawn, not stored: the document does not change when a
  // reader taps one, so the press only reports. A press on a document that has
  // since been replaced is dropped, which is the same generation rule every
  // other event follows.
  // A hold on a link is the app's, full stop: the library neither opens nor
  // asks, so there is no safety flow — only the URL as written, the URL resolved,
  // and where the link was.
  // Reads the handler and the generation late, through refs, so the callback
  // keeps its identity across snapshots: re-registering the native event on
  // every generation would drop a press that landed between renders. The same
  // shape `useControls` uses for its own routing, for the same reason.
  const linkLongPressLatest = useRef({ onLinkLongPress, generationRef });
  linkLongPressLatest.current = { onLinkLongPress, generationRef };
  const handleLinkLongPress = useCallback(
    (native: NativeSyntheticEvent<NativeLinkPress>) => {
      const { url: href, start, end } = native.nativeEvent;
      linkLongPressLatest.current.onLinkLongPress?.({
        url: resolveLinkUrl(href, ''),
        href,
        range: { start, end },
      });
    },
    []
  );

  const taskPressLatest = useRef({ onTaskPress, generationRef });
  taskPressLatest.current = { onTaskPress, generationRef };
  const handleTaskPress = useCallback(
    (native: NativeSyntheticEvent<MarkdownTaskPressEvent>) => {
      const payload = native.nativeEvent;
      const latest = taskPressLatest.current;
      if (payload.generation !== latest.generationRef.current) return;
      latest.onTaskPress?.({
        containerId: payload.containerId,
        checked: payload.checked,
        generation: payload.generation,
      } satisfies MarkdownTaskPressEvent);
    },
    []
  );

  const { resolved, controlError, handleControlAction } = useControls(
    controls,
    translations,
    onError,
    generationRef,
    fullscreen.fullscreen
  );
  const currentGeneration = useCallback(
    () => generationRef.current,
    [generationRef]
  );
  const images = useMathAndImages(rest, currentGeneration);
  const resolvedTheme = useStableTheme(
    useMemo(() => resolveTheme(theme, markdownStyle), [theme, markdownStyle])
  );
  const linkVariantProps = useLinkVariantProps(markdownStyle);
  // A controlled view has to say whether text is arriving; the platform has
  // nothing else to go on.
  useAnimationCallbacks(isAnimating, onAnimationStart, onAnimationEnd);
  const animation = useAnimationProps(
    animated,
    caret,
    isAnimating ? 'on' : 'off'
  );

  const handleLinkPress = useLinkPress(
    onLinkPress,
    useLinkSafety(linkSafety, rest.links),
    translations
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

  // A controlled source is the whole message, so the engine reads its fences
  // from that string: one synchronous call during this render, which is what
  // puts the components and the document they belong to in the same commit.
  const fences = useFenceRenderers(
    plugins,
    { source: current.source, generation, parseFlags: images.parseFlags },
    onError,
    currentGeneration
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
      tableMaxHeight={resolveMaxHeight(tableMaxHeight, defaultTableMaxHeight)}
      dir={dir}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      imageFilename={resolved.imageFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      controlIcons={resolveIcons(icons)}
      linkVariantPrefixes={linkVariantProps.prefixes}
      linkVariantCounts={linkVariantProps.counts}
      linkVariantColors={linkVariantProps.colors}
      linkVariantUnderline={linkVariantProps.underlines}
      onTaskPress={handleTaskPress}
      onLinkLongPress={handleLinkLongPress}
      linkPreview={linkPreview}
      parseFlags={images.parseFlags}
      images={images.images}
      allowedImagePrefixes={images.allowedImagePrefixes}
      allowedTags={images.allowedTags}
      themeSlots={resolvedTheme.slots}
      themeValues={resolvedTheme.values}
      themeFonts={resolvedTheme.fonts}
      animation={animation.animation}
      animationSplit={animation.animationSplit}
      animationTiming={animation.animationTiming}
      animating={animation.animating}
      caret={animation.caret}
      onLinkPress={handleLinkPress}
      onImagePress={images.onImagePress}
      onMathError={images.onMathError}
      onMarkdownError={handleError}
      onControlAction={handleControlAction}
    >
      {fences}
      {fullscreen.element}
    </NativeDownstreamMarkdownView>
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
  plugins,
  animated,
  isAnimating,
  caret,
  onAnimationStart,
  onAnimationEnd,
  onLinkPress,
  linkSafety,
  tableMaxHeight,
  dir,
  icons,
  onTableFullscreen,
  onTaskPress,
  onLinkLongPress,
  linkPreview = false,
  renderTableFullscreen,
  onError,
  ...rest
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
  const fullscreen = useFullscreenTable(
    onTableFullscreen,
    renderTableFullscreen,
    translations,
    useInheritedTableProps(rest)
  );
  // A task checkbox is drawn, not stored: the document does not change when a
  // reader taps one, so the press only reports. A press on a document that has
  // since been replaced is dropped, which is the same generation rule every
  // other event follows.
  // A hold on a link is the app's, full stop: the library neither opens nor
  // asks, so there is no safety flow — only the URL as written, the URL resolved,
  // and where the link was.
  // Reads the handler and the generation late, through refs, so the callback
  // keeps its identity across snapshots: re-registering the native event on
  // every generation would drop a press that landed between renders. The same
  // shape `useControls` uses for its own routing, for the same reason.
  const linkLongPressLatest = useRef({ onLinkLongPress, generationRef });
  linkLongPressLatest.current = { onLinkLongPress, generationRef };
  const handleLinkLongPress = useCallback(
    (native: NativeSyntheticEvent<NativeLinkPress>) => {
      const { url: href, start, end } = native.nativeEvent;
      linkLongPressLatest.current.onLinkLongPress?.({
        url: resolveLinkUrl(href, ''),
        href,
        range: { start, end },
      });
    },
    []
  );

  const taskPressLatest = useRef({ onTaskPress, generationRef });
  taskPressLatest.current = { onTaskPress, generationRef };
  const handleTaskPress = useCallback(
    (native: NativeSyntheticEvent<MarkdownTaskPressEvent>) => {
      const payload = native.nativeEvent;
      const latest = taskPressLatest.current;
      if (payload.generation !== latest.generationRef.current) return;
      latest.onTaskPress?.({
        containerId: payload.containerId,
        checked: payload.checked,
        generation: payload.generation,
      } satisfies MarkdownTaskPressEvent);
    },
    []
  );

  const { resolved, controlError, handleControlAction } = useControls(
    controls,
    translations,
    onError,
    generationRef,
    fullscreen.fullscreen
  );
  const currentGeneration = useCallback(
    () => state.binding().generation,
    [state]
  );
  const images = useMathAndImages(rest, currentGeneration);
  // A stream keeps its theme across a rebind, so a theme that has not changed
  // sends nothing however often a snapshot arrives.
  const resolvedTheme = useStableTheme(
    useMemo(() => resolveTheme(theme, markdownStyle), [theme, markdownStyle])
  );
  const linkVariantProps = useLinkVariantProps(markdownStyle);
  // A stream view follows its controller's own transitions, so the callbacks fire
  // on the first append of a generation and on `finish()`, unless an explicit
  // `isAnimating` overrides the flag, which takes the callbacks with it.
  useStreamAnimationCallbacks(
    state,
    isAnimating === undefined,
    onAnimationStart,
    onAnimationEnd
  );
  useAnimationCallbacks(isAnimating, onAnimationStart, onAnimationEnd);
  const animation = useAnimationProps(
    animated,
    caret,
    // `auto` follows the session's own flag, which the stream sets on every
    // snapshot it publishes before `finish()`. An explicit value overrides it,
    // which is how a controlled-looking stream view takes over.
    isAnimating === undefined ? 'auto' : isAnimating ? 'on' : 'off'
  );

  const handleLinkPress = useLinkPress(
    onLinkPress,
    useLinkSafety(linkSafety, rest.links),
    translations
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

  // The stream's answer reads the published snapshot the view is about to mount
  // and never parses, so asking on every render — which is every snapshot — is
  // one walk of blocks and nothing more.
  const fences = useFenceRenderers(
    plugins,
    {
      sessionId: binding.sessionId,
      generation: binding.generation,
      sequence: binding.readySequence,
      revision: binding.revision,
      parseFlags: images.parseFlags,
    },
    onError,
    currentGeneration
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
      tableMaxHeight={resolveMaxHeight(tableMaxHeight, defaultTableMaxHeight)}
      dir={dir}
      controls={resolved.flags}
      codeFilename={resolved.codeFilename}
      tableFilename={resolved.tableFilename}
      imageFilename={resolved.imageFilename}
      csvSeparator={resolved.csvSeparator}
      controlLabels={resolved.labels}
      controlIcons={resolveIcons(icons)}
      linkVariantPrefixes={linkVariantProps.prefixes}
      linkVariantCounts={linkVariantProps.counts}
      linkVariantColors={linkVariantProps.colors}
      linkVariantUnderline={linkVariantProps.underlines}
      onTaskPress={handleTaskPress}
      onLinkLongPress={handleLinkLongPress}
      linkPreview={linkPreview}
      parseFlags={images.parseFlags}
      images={images.images}
      allowedImagePrefixes={images.allowedImagePrefixes}
      allowedTags={images.allowedTags}
      themeSlots={resolvedTheme.slots}
      themeValues={resolvedTheme.values}
      themeFonts={resolvedTheme.fonts}
      animation={animation.animation}
      animationSplit={animation.animationSplit}
      animationTiming={animation.animationTiming}
      animating={animation.animating}
      caret={animation.caret}
      onLinkPress={handleLinkPress}
      onImagePress={images.onImagePress}
      onMathError={images.onMathError}
      onMarkdownError={handleError}
      onControlAction={handleControlAction}
    >
      {fences}
      {fullscreen.element}
    </NativeDownstreamMarkdownView>
  );
}
