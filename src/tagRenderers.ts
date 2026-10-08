/**
 * Custom-tag renderers: the app's React components inside a custom container.
 *
 * `plugins.tags[name]` draws the blocks between an allowed tag's pair, the way
 * `plugins.renderers` draws a fenced block: the view's shadow node measures the
 * child at the container's column, asks the engine for a rectangle, and places
 * it there in the same commit that draws the message. One view, one commit, no
 * second frame.
 *
 * The rules mirror the fence ones, because the failure modes are the same:
 *
 * - A tag is only a slot if a visible slot child is mounted for it. A tag with
 *   no entry draws its children natively, and `fallbackComponent` is what an
 *   allowed tag with no entry of its own uses — so all three reduce to "no
 *   child", and the native text draws underneath.
 * - The renderer list is resolved by value, so an inline `plugins` literal
 *   sends nothing and re-queries nothing when it has not changed.
 * - The engine decides which tags exist and JavaScript only asks. The slot
 *   carries the container id, the tag's name and attributes, and the inner
 *   Markdown exactly as written — not re-serialized, because the writer sent it
 *   as the tag's content rather than as Markdown to parse again.
 * - A settling tag's component does not re-render for every later snapshot.
 *   The subtree is memoized on its inputs, which are the only things that can
 *   change what it draws.
 */
import { createElement, memo, useMemo, useRef, type ReactNode } from 'react';

import DownstreamFenceSlot from './DownstreamFenceSlotNativeComponent';
import NativeDownstreamSession, {
  type TagSlot,
} from './NativeDownstreamSession';
import type { MarkdownPlugins, TagRenderer, TagRendererProps } from './types';

/** A tag name, normalized the one way the engine compares them. */
function normalizeTagName(name: string): string {
  return name.toLowerCase();
}

/** What `useTagSlots` resolved the plugins to, by value. */
export type ResolvedTagRenderers = Readonly<{
  /** Tag names with a component, lowercased, for the engine query. */
  names: ReadonlyArray<string>;
  /** Name to component, for joining. */
  components: ReadonlyMap<string, TagRenderer['component']>;
  /** The fallback for an allowed tag with no entry of its own. */
  fallback: MarkdownPlugins['fallbackComponent'];
}>;

export function resolveTagRenderers(
  plugins: MarkdownViewPropsPlugins
): ResolvedTagRenderers {
  const tags = plugins?.tags ?? [];
  const components = new Map<string, TagRenderer['component']>();
  for (const entry of tags) {
    const names = Array.isArray(entry.name) ? entry.name : [entry.name];
    for (const name of names) {
      const normalized = normalizeTagName(name);
      if (!components.has(normalized)) {
        components.set(normalized, entry.component);
      }
    }
  }
  return {
    names: [...components.keys()],
    components,
    fallback: plugins?.fallbackComponent,
  };
}

type MarkdownViewPropsPlugins = MarkdownPlugins | undefined;

/** Inline plugin objects reuse the query when their effective entries agree. */
export function useStableTagRenderers(
  resolved: ResolvedTagRenderers
): ResolvedTagRenderers {
  const stable = useRef(resolved);
  const previous = stable.current;
  if (
    previous.fallback !== resolved.fallback ||
    previous.names.length !== resolved.names.length ||
    previous.names.some(
      (name, index) =>
        name !== resolved.names[index] ||
        previous.components.get(name) !== resolved.components.get(name)
    )
  ) {
    stable.current = resolved;
  }
  return stable.current;
}

/** A mounted tag slot: the engine's slot joined with its component. */
export type MountedTagSlot = Readonly<{
  /** The custom container's own id, which is also what the host entry is keyed by. */
  container: number;
  /** The tag's name, lowercased, as the app named it in `allowedTags`. */
  name: string;
  component: TagRenderer['component'];
  props: TagRendererProps;
}>;

/**
 * A fallback as a tag component.
 *
 * A fallback accepts either prop shape, so it accepts a tag's: the cast says
 * what the union already means, and the element below passes tag props either
 * way.
 */
function asTagComponent(
  fallback: NonNullable<MarkdownPlugins['fallbackComponent']>
): TagRenderer['component'] {
  return fallback as unknown as TagRenderer['component'];
}

/** The props a tag renderer sees, taken straight from the engine's slot. */
function tagRendererProps(slot: TagSlot): TagRendererProps {
  return {
    name: slot.name,
    attributes: slot.attributes.map(
      (pair) => [pair[0] ?? '', pair[1] ?? ''] as const
    ),
    markdown: slot.markdown,
    isIncomplete: slot.incomplete,
  };
}

/**
 * The engine's slots joined with their components.
 *
 * A slot whose tag resolved to nothing — and with no fallback — is dropped
 * rather than mounted as a slot with no renderer: a mounted empty slot would
 * be a hosted container with nothing in it, and the native children are the
 * right drawing for a tag this build cannot render.
 */
function joinTagComponents(
  slots: readonly TagSlot[],
  resolved: ResolvedTagRenderers
): readonly MountedTagSlot[] {
  if (slots.length === 0) {
    return [];
  }
  const joined: MountedTagSlot[] = [];
  for (const slot of slots) {
    const entry = resolved.components.get(normalizeTagName(slot.name));
    const component =
      entry ??
      (resolved.fallback === undefined
        ? undefined
        : asTagComponent(resolved.fallback));
    if (component === undefined) {
      continue;
    }
    joined.push({
      container: slot.container,
      name: slot.name,
      component,
      props: tagRendererProps(slot),
    });
  }
  return joined;
}

/** The whole slot can skip reconciliation when its inputs are unchanged. */
function sameTagSlot(previous: MountedTagSlot, next: MountedTagSlot): boolean {
  return (
    previous.container === next.container &&
    previous.component === next.component &&
    previous.name === next.name &&
    previous.props.markdown === next.props.markdown &&
    previous.props.isIncomplete === next.props.isIncomplete &&
    previous.props.attributes.length === next.props.attributes.length &&
    previous.props.attributes.every(
      (pair, index) =>
        pair[0] === next.props.attributes[index]?.[0] &&
        pair[1] === next.props.attributes[index]?.[1]
    )
  );
}

/** Refreshing the boundary's error handler does not rerender an unchanged tag. */
const TagRendererView = memo(
  function MountedTag({ slot }: Readonly<{ slot: MountedTagSlot }>) {
    return createElement(slot.component, slot.props);
  },
  (previous, next) => sameTagSlot(previous.slot, next.slot)
);

/** What `useTagSlots` needs to know about the document being read. */
export type TagQuery = Readonly<{
  /** The controlled source, or undefined for a stream. */
  source?: string;
  /** The session a stream is bound to, which replaces `source` when present. */
  sessionId?: string;
  generation: number;
  /** The snapshot a stream view is mounting; 0 for a controlled source. */
  sequence?: number;
  revision?: number;
  parseFlags: number;
  allowedTags: ReadonlyArray<string>;
  resolved: ResolvedTagRenderers;
}>;

function readTagSlots(query: TagQuery): readonly MountedTagSlot[] {
  const { source, sessionId, generation, sequence, allowedTags, resolved } =
    query;
  // With a fallback every allowed tag is a candidate; without one, only the
  // registered names are. Either way there is nothing to ask when the answer
  // is known to be empty.
  const names =
    resolved.fallback === undefined
      ? resolved.names
      : allowedTagNames(allowedTags);
  if (names.length === 0) {
    return [];
  }
  const module = NativeDownstreamSession;
  if (module === null) {
    return [];
  }
  try {
    if (sessionId !== undefined) {
      return joinTagComponents(
        module.sessionTagSlots(sessionId, generation, sequence ?? 0, names),
        resolved
      );
    }
    if (source === undefined) {
      return [];
    }
    // The pre-filter: a message that never names a candidate tag costs one
    // substring search per name and no engine call at all.
    const lowerSource = source.toLowerCase();
    if (!names.some((name) => lowerSource.includes(`<${name}`))) {
      return [];
    }
    return joinTagComponents(
      module.tagSlots(source, query.parseFlags, names, allowedTags),
      resolved
    );
  } catch {
    return [];
  }
}

/** The policy wire format is `-1,name,attribute,...`, not a name list. */
function allowedTagNames(allowedTags: ReadonlyArray<string>): string[] {
  const names: string[] = [];
  for (let index = 0; index < allowedTags.length; index++) {
    if (allowedTags[index] === '-1') {
      const name = allowedTags[++index];
      if (name !== undefined) names.push(name);
    }
  }
  return names;
}

/**
 * The tag slots in one snapshot, read from the engine.
 *
 * The controlled path asks synchronously, so the slot children and the
 * document they belong to are in the same commit. A stream's answer never
 * parses: it reads the snapshot the view is mounting, so it is safe to call
 * on every published snapshot.
 */
export function useTagSlots(query: TagQuery): readonly MountedTagSlot[] {
  const {
    source,
    sessionId,
    generation,
    sequence,
    revision,
    parseFlags,
    allowedTags,
    resolved,
  } = query;
  const current = useMemo(
    () => ({
      slots: readTagSlots({
        source,
        sessionId,
        generation,
        sequence,
        revision,
        parseFlags,
        allowedTags,
        resolved,
      }),
    }),
    // `revision` names a settled snapshot even when its sequence is unchanged.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [
      source,
      sessionId,
      generation,
      sequence,
      revision,
      parseFlags,
      allowedTags,
      resolved,
    ]
  );
  return current.slots;
}

/**
 * The slot children for a document. A renderer that throws is caught by the
 * caller's boundary, which is what leaves the native children visible: the
 * tag's own text is always underneath, so a broken component degrades to
 * prose rather than to nothing.
 */
export function TagSlots(props: {
  slots: readonly MountedTagSlot[];
  scope: string;
}): ReactNode {
  const { slots, scope } = props;
  return slots.map((slot) =>
    createElement(DownstreamFenceSlot, {
      // A new session or generation mounts fresh renderer state.
      key: `${scope}\u0000tag\u0000${slot.container}`,
      ordinal: -1,
      containerId: slot.container,
      children: createElement(TagRendererView, { slot }),
    })
  );
}
