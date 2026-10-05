/**
 * Fence-language renderers: the app's React components inside the native
 * document.
 *
 * Streamdown's `plugins.renderers` API, brought to React Native. A fenced block
 * whose language is registered stops being a code card and becomes a real
 * React child of the Markdown view: the view's shadow node measures it at the
 * block's column, asks the engine for a rectangle, and places it there in the
 * same commit that draws the message. One view, one commit, no second frame.
 *
 * The rules this module exists to keep:
 *
 * - A fence is only a slot if a visible slot child is mounted for it. React
 *   hides a suspended subtree with `display: none`, a boundary that caught a
 *   throw renders nothing, and a fence the query never returned has no child at
 *   all — so all three reduce to "no child", and the native code card draws
 *   underneath. The card is always there and always readable, which is why the
 *   fallback needs no code of its own.
 * - The renderer list is resolved by value. `plugins` is an inline literal in
 *   every real call, so an unchanged list must send nothing and re-query
 *   nothing.
 * - The engine decides which fences exist and JavaScript only asks. The
 *   pre-filter below may say "maybe" about a source that holds no fence, and
 *   never says "no" about one that does: it looks for the registered name in
 *   the raw source, which is exactly where the engine reads a fence's language
 *   from.
 * - A settled fence's component does not re-render for every later snapshot of
 *   a streaming message. The subtree is memoized on the four props, which are
 *   the only things that can change what a renderer draws.
 */
import {
  Component,
  createElement,
  memo,
  Suspense,
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ComponentType,
  type ReactNode,
} from 'react';

import DownstreamFenceSlot from './DownstreamFenceSlotNativeComponent';
import NativeDownstreamSession, {
  type FenceSlot,
  type FenceSlotsResult,
} from './NativeDownstreamSession';
import type {
  CustomRenderer,
  CustomRendererProps,
  MarkdownErrorEvent,
  MarkdownPlugins,
} from './types';

/**
 * A fence language, normalized the one way a fence's own language is.
 *
 * Deliberately the same normalization the engine applies — ASCII lower case,
 * cut at the first space and at `:` — rather than Streamdown's exact
 * case-sensitive match. Models write ```` ```Mermaid ````, and a rule that
 * misses it is a rule every app works around. Doing it here means the list that
 * crosses to native can stay as written and C++ stays the only place the rule
 * exists for a fence.
 */
export function normalizeLanguage(language: string): string {
  const word = language.split(/\s/, 1)[0] ?? '';
  const cut = word.indexOf(':');
  return (cut === -1 ? word : word.slice(0, cut)).toLowerCase();
}

/** The registered renderers, resolved to a language map plus the flat list. */
export type ResolvedRenderers = Readonly<{
  /** Every registered language, normalized and deduplicated. Empty means off. */
  languages: readonly string[];
  /** The component for each normalized language; the first entry wins. */
  components: ReadonlyMap<string, CustomRenderer['component']>;
}>;

const NO_RENDERERS: ResolvedRenderers = {
  languages: [],
  components: new Map(),
};

/** Whether a value can be rendered as a component by React. */
function isRenderable(
  value: unknown
): value is ComponentType<CustomRendererProps> {
  if (typeof value === 'function') {
    return true;
  }
  return typeof value === 'object' && value !== null && '$$typeof' in value;
}

/**
 * `plugins.renderers` resolved, first match winning.
 *
 * Streamdown's rule is `renderers.find(...)` over the language as written, so
 * an app that registers `mermaid` can override a Mermaid plugin with its own.
 * Downstream has no built-in renderer yet and the override story is the same
 * one: the first entry that names a language owns it.
 */
export function resolveRenderers(
  plugins: MarkdownPlugins | undefined
): ResolvedRenderers {
  const renderers = plugins?.renderers;
  if (!renderers || renderers.length === 0) {
    return NO_RENDERERS;
  }
  const languages: string[] = [];
  const components = new Map<string, CustomRenderer['component']>();
  for (const renderer of renderers) {
    // A function or an object with a `$$typeof`, which is what `lazy`, `memo`,
    // and `forwardRef` hand back: those are three of the ordinary ways to write
    // a renderer, and rejecting them would leave the fence drawing its code card
    // for a renderer the app did register. Anything else is React's own error to
    // raise, and the boundary catches it into the same card.
    if (!renderer || !isRenderable(renderer.component)) {
      continue;
    }
    const declared: readonly string[] = Array.isArray(renderer.language)
      ? (renderer.language as readonly string[])
      : [renderer.language as string];
    for (const language of declared) {
      if (typeof language !== 'string') {
        continue;
      }
      const key = normalizeLanguage(language);
      if (key === '' || components.has(key)) {
        continue;
      }
      components.set(key, renderer.component);
      languages.push(key);
    }
  }
  return languages.length === 0 ? NO_RENDERERS : { languages, components };
}

/**
 * Whether a source could hold any of these languages, as one case-insensitive
 * substring search per language.
 *
 * A pre-filter and never a decision: it looks for the registered name anywhere
 * in the raw source, which is the same text the engine reads a fence's language
 * from, so a real slot always passes. A message that merely mentions the name
 * in prose pays one search and then one engine query that returns nothing.
 */
export function mayHoldRenderers(
  source: string,
  languages: readonly string[]
): boolean {
  if (source === '' || languages.length === 0) {
    return false;
  }
  const haystack = source.toLowerCase();
  return languages.some((language) => haystack.includes(language));
}

/** The native module, or null on a host with no TurboModule at all. */
function sessionModule(): typeof NativeDownstreamSession | null {
  try {
    return NativeDownstreamSession;
  } catch {
    return null;
  }
}

/**
 * Two empty lists, so a view with no renderers reuses one value at each of the
 * two stages: the engine's answer, and the joined list its children are built
 * from.
 */
const NO_RAW: readonly FenceSlot[] = Object.freeze([]);
const NO_SLOTS: readonly MountedSlot[] = Object.freeze([]);
/** An answer native gave in full: nothing to wait for. */
const ANSWERED: Readonly<{ slots: readonly MountedSlot[]; pending: boolean }> =
  {
    slots: NO_SLOTS,
    pending: false,
  };
/** An answer native declined to give, and the reason to ask again. */
const UNANSWERED: Readonly<{
  slots: readonly MountedSlot[];
  pending: boolean;
}> = {
  slots: NO_SLOTS,
  pending: true,
};

/**
 * Catches a renderer that throws and lets the code card show.
 *
 * A fence is drawn by the app's own component over text the app's own model
 * wrote, so a throw is a plausible outcome and must not take the message with
 * it. The boundary renders nothing, which is what makes the fence draw as its
 * card: the child that failed is not mounted, and native draws the fallback for
 * a fence with no child.
 *
 * It resets when the fence's text or its completeness changes, so a renderer
 * that failed on a half-streamed spec gets another chance on the next snapshot.
 * The reset is also what stops it firing per token: once the spec settles, a
 * renderer that throws again throws against the same text and is reported once.
 */
type FenceBoundaryProps = Readonly<{
  resetKey: string;
  onFailure: (error: unknown) => void;
  children?: ReactNode;
}>;

class FenceBoundary extends Component<FenceBoundaryProps, { failed: boolean }> {
  constructor(props: FenceBoundaryProps) {
    super(props);
    this.state = { failed: false };
  }

  static getDerivedStateFromError(): Readonly<{ failed: boolean }> {
    return { failed: true };
  }

  override componentDidUpdate(previous: FenceBoundaryProps): void {
    if (this.state.failed && previous.resetKey !== this.props.resetKey) {
      this.setState({ failed: false });
    }
  }

  override componentDidCatch(error: unknown): void {
    this.props.onFailure(error);
  }

  override render(): ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

/** A slot joined with the component that renders it. */
type MountedSlot = Readonly<{
  ordinal: number;
  language: string;
  code: string;
  meta: string | undefined;
  incomplete: boolean;
  /** Memoized: the renderer sees only these four, so only they re-render it. */
  props: CustomRendererProps;
  render: (props: CustomRendererProps) => ReactNode;
}>;

/** The four props a renderer sees, taken straight from the engine's slot. */
function rendererProps(slot: FenceSlot): CustomRendererProps {
  return {
    code: slot.code,
    language: slot.language,
    meta: slot.meta === '' || slot.meta === undefined ? undefined : slot.meta,
    isIncomplete: slot.incomplete,
  };
}

function sameProps(
  previous: Readonly<{ slot: MountedSlot }>,
  next: Readonly<{ slot: MountedSlot }>
): boolean {
  return (
    previous.slot.code === next.slot.code &&
    previous.slot.language === next.slot.language &&
    previous.slot.meta === next.slot.meta &&
    previous.slot.incomplete === next.slot.incomplete
  );
}

/**
 * One mounted renderer, named for React's devtools as the thing it is.
 *
 * The memo compares the four props and nothing else, which is what keeps a
 * settled fence's component from re-rendering for every later snapshot of a
 * streaming message: the spec did not change, so React skips the render even
 * though the snapshot did.
 */
const Renderer = memo(function MountedRenderer({
  slot,
}: Readonly<{ slot: MountedSlot }>) {
  return slot.render(slot.props);
},
sameProps);

/** What `useFenceSlots` needs to know about the document being read. */
export type FenceQuery = Readonly<{
  /** The controlled source, or undefined for a stream. */
  source?: string;
  /** The session a stream is bound to, which replaces `source` when present. */
  sessionId?: string;
  generation: number;
  /** The snapshot a stream view is mounting; 0 for a controlled source. */
  sequence?: number;
  mathFlags: number;
  resolved: ResolvedRenderers;
}>;

/**
 * The fences in one snapshot, read from the engine.
 *
 * The controlled path asks synchronously, so the slot children and the document
 * they belong to are in the same commit: no frame without the chart and no
 * frame where one appears late. A source larger than the synchronous bound
 * answers `pending`, and the same answer arrives later from the worker pool — a
 * code card until then, which is what a large source already shows while its
 * own preparation runs.
 *
 * A stream's answer never parses: it reads the snapshot the view is mounting,
 * so it is safe to call on every published snapshot, which is every render the
 * stream already does.
 */
export function useFenceSlots(query: FenceQuery): readonly MountedSlot[] {
  const { source, sessionId, generation, sequence, mathFlags, resolved } =
    query;
  const languages = resolved.languages;
  // One value per source and registered list, so a re-render of an unchanged
  // message does not ask again. Keyed by the languages as well as the source,
  // because the same message with a new renderer is a different question.
  const key = `${
    sessionId ?? ''
  }\u0000${generation}\u0000${sequence}\u0000${mathFlags}\u0000${
    source ?? ''
  }\u0000${languages.join('\u0000')}`;
  const [asyncSlots, setAsyncSlots] = useState<readonly FenceSlot[]>(NO_RAW);

  // Read once per document rather than once per render: the answer is a pure
  // read of an engine that does not change under it, and a message that
  // re-renders for an unrelated reason has nothing to ask again. `resolved` is
  // stable by value, so the object it hands back is too.
  const answer = useMemo(
    () =>
      readSlots({
        source,
        sessionId,
        generation,
        sequence,
        mathFlags,
        resolved,
      }),
    [source, sessionId, generation, sequence, mathFlags, resolved]
  );
  const slots = answer.slots;

  useEffect(() => {
    setAsyncSlots(NO_RAW);
  }, [key]);

  // Only an answer that said `pending` is worth asking again for: a source the
  // synchronous path already read has its answer, and asking the worker pool for
  // a second one would parse the same source twice for nothing. Until the
  // asynchronous answer lands the fences draw as cards, which is the same thing
  // a large source already shows while its own preparation runs.
  useEffect(() => {
    if (!answer.pending || source === undefined) {
      return;
    }
    const module = sessionModule();
    if (module === null) {
      return;
    }
    let live = true;
    module
      .fenceSlotsAsync(source, mathFlags, languages)
      .then((raw) => {
        if (live) {
          setAsyncSlots(raw);
        }
      })
      .catch(() => undefined);
    return () => {
      live = false;
    };
    // `answer.pending` rather than the key: the key is the whole document, and
    // this asks about one question inside it.
  }, [answer.pending, languages, mathFlags, source]);

  return slots.length > 0 ? slots : joinComponents(asyncSlots, resolved);
}

/**
 * The synchronous answer: the slots native reports for this document, and
 * whether it declined to answer.
 *
 * A thrown module call answers with no slots, which draws every fence as its
 * code card rather than failing the render: the view is still perfectly
 * readable without any renderer.
 */
function readSlots(
  query: FenceQuery
): Readonly<{ slots: readonly MountedSlot[]; pending: boolean }> {
  const { source, sessionId, generation, sequence, mathFlags, resolved } =
    query;
  const languages = resolved.languages;
  if (languages.length === 0) {
    return ANSWERED;
  }
  const module = sessionModule();
  if (module === null) {
    return ANSWERED;
  }
  try {
    if (sessionId !== undefined) {
      return {
        slots: joinComponents(
          module.sessionFenceSlots(
            sessionId,
            generation,
            sequence ?? 0,
            languages
          ),
          resolved
        ),
        pending: false,
      };
    }
    if (source === undefined || !mayHoldRenderers(source, languages)) {
      // The pre-filter: a message that never names a registered language costs
      // one substring search per language and no engine call at all.
      return ANSWERED;
    }
    const result: FenceSlotsResult = module.fenceSlots(
      source,
      mathFlags,
      languages
    );
    if (result.pending) {
      return UNANSWERED;
    }
    return { slots: joinComponents(result.slots, resolved), pending: false };
  } catch {
    return ANSWERED;
  }
}

/**
 * The engine's slots joined with their components.
 *
 * A slot whose language resolved to nothing is dropped rather than mounted as a
 * slot with no renderer: a mounted empty slot would be a hosted fence with
 * nothing in it, and the code card is the right drawing for a fence this build
 * cannot render.
 */
function joinComponents(
  slots: readonly FenceSlot[],
  resolved: ResolvedRenderers
): readonly MountedSlot[] {
  if (slots.length === 0) {
    return NO_SLOTS;
  }
  const joined: MountedSlot[] = [];
  for (const slot of slots) {
    const component = resolved.components.get(normalizeLanguage(slot.language));
    if (component === undefined) {
      continue;
    }
    joined.push({
      ordinal: slot.ordinal,
      language: slot.language,
      code: slot.code,
      meta: slot.meta === '' ? undefined : slot.meta,
      incomplete: slot.incomplete,
      props: rendererProps(slot),
      render: (props) => createElement(component, props),
    });
  }
  return joined.length === 0 ? NO_SLOTS : joined;
}

/**
 * Reports a renderer failure to the view's `onError`, once per settled fence.
 *
 * A throw while the fence is still streaming is not a failure the app can act
 * on — a half-written chart is expected to fail — so it is not reported, which
 * is the rule `onMathError` already follows. A failure from a generation the
 * view has moved past is dropped, so a slow component cannot report into a
 * message that has been replaced.
 */
export function useRendererErrors(
  options: Readonly<{
    onError: ((error: MarkdownErrorEvent) => void) | undefined;
    currentGeneration: () => number;
  }>
): (ordinal: number, language: string, message: string) => void {
  const { onError, currentGeneration } = options;
  const reported = useRef(new Set<string>());
  return useCallback(
    (ordinal: number, language: string, message: string) => {
      if (onError === undefined) {
        return;
      }
      const generation = currentGeneration();
      const key = `${generation}:${ordinal}`;
      if (reported.current.has(key)) {
        return;
      }
      reported.current.add(key);
      onError({
        code: 'renderer_failed',
        message,
        language,
        generation,
        sequence: 0,
        recoverable: true,
      });
    },
    [currentGeneration, onError]
  );
}

/**
 * The slot children for a document, ready to hand to the native view.
 *
 * Each slot is `boundary › suspense › slot › memo(renderer)`. The `Suspense`
 * sits *outside* the slot so a lazy renderer that has not resolved mounts no
 * slot at all and its fence keeps drawing as a code card, which is the fallback
 * for every case where a component is not up yet.
 */
export function FenceSlots(
  props: Readonly<{
    slots: readonly MountedSlot[];
    generation: number;
    onFailure: (ordinal: number, language: string, message: string) => void;
  }>
): ReactNode {
  const { slots, generation, onFailure } = props;
  if (slots.length === 0) {
    return null;
  }
  return slots.map((slot) =>
    createElement(
      FenceBoundary,
      {
        // Keyed by generation and ordinal: a growing fence keeps its component
        // from snapshot to snapshot, and `reset()` — a new generation — gives
        // every renderer fresh state rather than the previous message's.
        key: `${generation}:${slot.ordinal}`,
        resetKey: `${slot.code}\u0000${slot.incomplete ? '1' : '0'}`,
        onFailure: (error: unknown) => {
          // A throw while the fence is still growing is not a failure the app
          // can act on: a half-written chart is expected to fail, and the
          // renderer gets another chance on the next snapshot. This is the rule
          // `onMathError` already follows for a formula that is still arriving.
          if (slot.incomplete) {
            return;
          }
          onFailure(
            slot.ordinal,
            slot.language,
            error instanceof Error ? error.message : String(error)
          );
        },
      },
      createElement(
        Suspense,
        { fallback: null },
        createElement(
          DownstreamFenceSlot,
          { ordinal: slot.ordinal },
          createElement(Renderer, { slot })
        )
      )
    )
  );
}
