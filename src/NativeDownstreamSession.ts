import {
  TurboModuleRegistry,
  type TurboModule,
  type CodegenTypes,
} from 'react-native';

export type SnapshotReadyEvent = Readonly<{
  sessionId: string;
  generation: CodegenTypes.Int32;
  sequence: CodegenTypes.Int32;
}>;

export type SessionErrorEvent = SnapshotReadyEvent &
  Readonly<{
    code: string;
    message: string;
    recoverable: boolean;
  }>;

export type NativeTraceEvent = SnapshotReadyEvent &
  Readonly<{
    event: string;
    clockDomain: 'native-monotonic';
    timeNs: string;
  }>;

/**
 * One fenced block a registered renderer wants, as the engine found it.
 *
 * Exactly Streamdown's `CustomRendererProps` and nothing more: the text, the
 * language as written, the meta string, and whether the fence is still
 * growing. Ordinals and block indices stay native; the identity is the
 * engine's own, and is what tells JavaScript that a fence is the same fence in
 * the next snapshot.
 */
export type FenceSlot = Readonly<{
  ordinal: CodegenTypes.Int32;
  language: string;
  meta?: string;
  code: string;
  incomplete: boolean;
  identity: CodegenTypes.Double;
}>;

/**
 * The answer to a controlled query: the slots when the source could be read
 * here and now, `pending` when it was too large to parse on this thread and the
 * caller has to ask again through the asynchronous form.
 *
 * A separate shape rather than an empty array, because "this message has no
 * renderable fences" and "this message has not been read yet" lead to the same
 * drawing today — the code card — and are not the same answer.
 */
export type FenceSlotsResult = Readonly<{
  pending: boolean;
  slots: ReadonlyArray<FenceSlot>;
}>;

/**
 * One custom container the app could draw, as the slot query reports it.
 *
 * Beside `FenceSlot` because the two answer different questions: a fence slot
 * names a code block by ordinal with the code to draw, and a tag slot names a
 * container by id with the source it holds and the tag that named it.
 */
export type TagSlot = Readonly<{
  container: CodegenTypes.Int32;
  tag: CodegenTypes.Int32;
  name: string;
  attributes: ReadonlyArray<ReadonlyArray<string>>;
  markdown: string;
  incomplete: boolean;
}>;

export interface Spec extends TurboModule {
  /**
   * Creates a session. `parseFlags` is the parse option bitmask, read once here
   * and kept for the life of the session: parse options are part of a source's
   * identity, so they cannot change under a stream the way a preview policy can.
   */
  create(sessionId: string, mode: string, parseFlags: number): void;
  append(
    sessionId: string,
    chunk: string,
    generation: number,
    sequence: number
  ): void;
  reset(sessionId: string, source: string, generation: number): void;
  finish(sessionId: string, generation: number, sequence: number): void;
  dispose(sessionId: string): void;
  setTracing(sessionId: string, enabled: boolean): void;
  /**
   * Replaces the session's provisional-styling policy. `features` is a bitmask
   * of the pass's features; zero turns it off, which leaves every open span
   * literal. Takes effect on the next parse, which this queues.
   */
  setPreview(sessionId: string, features: number): void;
  /**
   * Replaces the session's HTML tag policy, keeping the flags `create` set.
   *
   * `tags` is the flat list `src/allowedTags.ts` writes: `-1,name,attribute,…`
   * in name order. Takes effect on the next parse, which this queues.
   */
  setParseTags(sessionId: string, tags: ReadonlyArray<string>): void;
  /**
   * Whether a math engine is linked into this process, so JavaScript can settle
   * the `math` prop's default during render instead of after a first pass.
   *
   * A method rather than a field because codegen only allows a property on a
   * TurboModule when it is a non-null EventEmitter; anything else has to be
   * spelled as a method, sync or not. The answer only changes when the app's
   * binary does, which is a relaunch, so nothing here is cached on the native side.
   */
  mathAvailable(): boolean;
  /**
   * Puts `text` on the system clipboard.
   *
   * React Native core has no clipboard API, and a confirmation dialog that
   * offers "Copy link" has to keep that promise on both platforms. iOS uses
   * `UIPasteboard`, Android the clipboard manager, and neither is reachable from
   * JavaScript without this.
   */
  copyText(text: string): Promise<boolean>;
  /**
   * The fence slots a controlled source holds, answered from this thread.
   *
   * A source the engine has already prepared answers from that preparation; a
   * larger one than this thread will parse says `pending` rather than blocking,
   * which is the same rule layout already follows for a big source: the view
   * draws what it has until preparation finishes.
   *
   * `languages` is the app's registered languages, unnormalized: the engine
   * normalizes them the one way a fence's language is normalized, so a
   * renderer and the highlighter can never disagree about what a fence is.
   */
  fenceSlots(
    source: string,
    parseFlags: CodegenTypes.Int32,
    languages: ReadonlyArray<string>
  ): FenceSlotsResult;
  /**
   * The same answer for a source too large to parse synchronously, parsed on
   * the worker pool. A view that gets `pending` asks again here once.
   */
  fenceSlotsAsync(
    source: string,
    parseFlags: CodegenTypes.Int32,
    languages: ReadonlyArray<string>
  ): Promise<ReadonlyArray<FenceSlot>>;
  /**
   * The fence slots of one published snapshot, which is the snapshot the view
   * is mounting for `sessionSequence`. Walks blocks and never parses, so it is
   * safe to call on every snapshot of a stream from render.
   */
  sessionFenceSlots(
    sessionId: string,
    generation: CodegenTypes.Int32,
    sequence: CodegenTypes.Int32,
    languages: ReadonlyArray<string>
  ): ReadonlyArray<FenceSlot>;
  /**
   * The tag slots a controlled source holds, answered from this thread.
   *
   * Only containers whose tag the app named are reported: a tag with no
   * renderer draws its children natively, so there is nothing to mount for it.
   */
  tagSlots(
    source: string,
    parseFlags: CodegenTypes.Int32,
    names: ReadonlyArray<string>
  ): ReadonlyArray<TagSlot>;
  /**
   * The tag slots of one published snapshot, which is the snapshot the view is
   * mounting for `sessionSequence`. Walks blocks and never parses, so it is
   * safe to call on every snapshot of a stream from render.
   */
  sessionTagSlots(
    sessionId: string,
    generation: CodegenTypes.Int32,
    sequence: CodegenTypes.Int32,
    names: ReadonlyArray<string>
  ): ReadonlyArray<TagSlot>;
  readonly onSnapshotReady: CodegenTypes.EventEmitter<SnapshotReadyEvent>;
  readonly onSessionError: CodegenTypes.EventEmitter<SessionErrorEvent>;
  readonly onTrace: CodegenTypes.EventEmitter<NativeTraceEvent>;
}

export default TurboModuleRegistry.getEnforcing<Spec>('DownstreamSession');
