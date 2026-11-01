/* eslint-disable no-bitwise -- the preview feature mask is a bitmask */
import { unstable_batchedUpdates } from 'react-native';
import NativeDownstreamSession, {
  type SessionErrorEvent,
  type SnapshotReadyEvent,
} from './NativeDownstreamSession';
import {
  checkedUtf8Bytes,
  checkStreamingSource,
  MAX_SOURCE_BYTES,
  type CheckedStreamingSource,
} from './controlledSource';
import { resolveAllowedTags } from './allowedTags';
import { mathAvailable, resolveParseFlags } from './parseFlags';
import { emitJsStreamTrace } from './streamTrace';
import type {
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownOperationError,
  MarkdownPreviewOptions,
  MarkdownStreamController,
  MarkdownStreamOptions,
  MarkdownViewProps,
} from './types';

/** Internal fixture/benchmark configuration. Public controllers only retain text. */
export type MarkdownSessionOptions = Pick<
  MarkdownViewProps,
  | 'math'
  | 'inlineHtml'
  | 'syntax'
  | 'cjk'
  | 'links'
  | 'disableAutolinkProtocols'
  | 'literalTagContent'
  | 'allowedTags'
  | 'preview'
  | 'syntaxHighlighting'
>;

const MAX_CHUNK_BYTES = 64 * 1024;
const MAX_QUEUED_BYTES = 256 * 1024;

/**
 * Bits for the native provisional-styling pass, mirroring
 * `remend::PreviewFeature`. Keep in step with `src/cpp/remend/Preview.h`: a
 * bit that the native side does not define is masked off and does nothing, so
 * adding one is a change on both sides rather than a silent no-op here.
 */
const PREVIEW_EMPHASIS = 1 << 0;
const PREVIEW_INLINE_CODE = 1 << 1;
const PREVIEW_LINKS = 1 << 2;
const PREVIEW_IMAGES = 1 << 3;
const PREVIEW_MATH = 1 << 4;
const PREVIEW_HTML_TAGS = 1 << 5;
const PREVIEW_TABLES = 1 << 6;
const PREVIEW_ALL =
  PREVIEW_EMPHASIS |
  PREVIEW_INLINE_CODE |
  PREVIEW_LINKS |
  PREVIEW_IMAGES |
  PREVIEW_MATH |
  PREVIEW_HTML_TAGS |
  PREVIEW_TABLES;

/**
 * Resolves the public `preview` option to a bitmask.
 *
 * `true` is every feature, `false` and `undefined` are none, and an object
 * enables only what it names. An option that is neither throws rather than
 * falling back to a default, because a typo here would otherwise silently
 * change what a reader sees mid-stream.
 */
function previewFeatures(options: boolean | MarkdownPreviewOptions): number {
  if (typeof options === 'boolean') {
    return options ? PREVIEW_ALL : 0;
  }
  if (typeof options !== 'object' || options === null) {
    throw operationError(
      'invalid_input',
      'preview must be a boolean or an options object.'
    );
  }
  let features = 0;
  if (options.emphasis) features |= PREVIEW_EMPHASIS;
  if (options.inlineCode) features |= PREVIEW_INLINE_CODE;
  if (options.links) features |= PREVIEW_LINKS;
  if (options.images) features |= PREVIEW_IMAGES;
  if (options.math) features |= PREVIEW_MATH;
  if (options.htmlTags) features |= PREVIEW_HTML_TAGS;
  if (options.tables) features |= PREVIEW_TABLES;
  return features;
}
const controllers = new Map<string, StreamState>();
let nextSession = 0;
let removeSnapshotListener: (() => void) | undefined;
let removeErrorListener: (() => void) | undefined;

// All sessions publish through one frame callback. Advance every dirty store
// before notifying React, so a render reads a coherent revision across streams.
const snapshotPublications = new Set<() => () => void>();
let snapshotFrame: number | undefined;

function scheduleSnapshotPublication(prepare: () => () => void): void {
  snapshotPublications.add(prepare);
  if (snapshotFrame !== undefined) return;
  const frame = requestAnimationFrame(() => {
    if (snapshotFrame !== frame) return;
    snapshotFrame = undefined;
    const pending = Array.from(snapshotPublications);
    snapshotPublications.clear();
    unstable_batchedUpdates(() => {
      const notifications = pending.map((publication) => publication());
      notifications.forEach((notify) => notify());
    }, undefined);
  });
  snapshotFrame = frame;
}

function cancelSnapshotPublication(prepare: () => () => void): void {
  snapshotPublications.delete(prepare);
  if (snapshotPublications.size === 0 && snapshotFrame !== undefined) {
    cancelAnimationFrame(snapshotFrame);
    snapshotFrame = undefined;
  }
}

export type ParseMode = 'full' | 'incremental';

export type StreamBinding = Readonly<{
  sessionId: string;
  mode: ParseMode;
  generation: number;
  sequence: number;
  readySequence: number;
  revision: number;
}>;

type PendingBatch = {
  pieces: string[];
  text?: string;
  bytes: number;
  sequence: number;
};
type InFlight = { bytes: number; sequence: number };

class StreamState implements MarkdownStreamController {
  readonly sessionId: string;
  readonly mode: ParseMode;
  private parseFlags: number | undefined;
  private nativeCreated = false;
  private onError?: (error: MarkdownErrorEvent) => void;
  private viewOnError?: (error: MarkdownErrorEvent) => void;
  private source = '';
  private sourcePages: string[] = [];
  private sourcePieces: string[] = [];
  private sourceBytes = 0;
  private generation = 0;
  private sequence = 0;
  private readySequence = 0;
  private revision = 0;
  private queuedBytes = 0;
  private pending: PendingBatch[] = [];
  private inFlight: InFlight[] = [];
  private frame: number | undefined;
  private finished = false;
  private disposed = false;
  private bound = false;
  private tracing = false;
  private previewFeatures = 0;
  /// The session's HTML tag policy, in the engine's wire form.
  private parseTags: readonly string[] = [];
  private subscribers = new Set<() => void>();
  /// Whether text is still arriving, which is what a stream view sends to native
  /// as `animating: auto`. A view that names `isAnimating` overrides it, so this
  /// is the default rather than a requirement.
  private streaming = false;
  private animationListeners = new Set<(animating: boolean) => void>();
  private controlledSource: CheckedStreamingSource = { source: '', bytes: 0 };
  private controlledInput?: { input: string; streaming: boolean };

  constructor(options: MarkdownStreamOptions, mode: ParseMode) {
    this.sessionId = `downstream-${Date.now()}-${++nextSession}`;
    this.mode = mode;
    this.onError = options.onError;
  }

  /** Called by the bound view; equivalent policies keep the existing session. */
  configure(options: MarkdownSessionOptions): void {
    this.assertActive();
    const flags = resolveParseFlags(
      {
        math: options.math,
        inlineHtml: options.inlineHtml,
        syntax: options.syntax,
        cjk: options.cjk,
        links: options.links,
        autolinkProtocols: options.disableAutolinkProtocols,
        literalTagContent: options.literalTagContent,
        syntaxHighlighting: options.syntaxHighlighting,
      },
      mathAvailable()
    ).flags;
    const tags = resolveAllowedTags(options.allowedTags).tags;
    const features = previewFeatures(options.preview ?? false);
    if (
      this.nativeCreated &&
      this.parseFlags === flags &&
      tags.length === this.parseTags.length &&
      tags.every((tag, i) => tag === this.parseTags[i])
    ) {
      this.setPreview(options.preview ?? false);
      return;
    }
    const source = this.readSource();
    const rebuilding = this.nativeCreated;
    if (rebuilding) {
      NativeDownstreamSession.dispose(this.sessionId);
      this.generation++;
    }
    if (this.frame !== undefined) cancelAnimationFrame(this.frame);
    this.frame = undefined;
    cancelSnapshotPublication(this.prepareSnapshotPublication);
    this.pending = [];
    this.inFlight = [];
    this.queuedBytes = 0;
    this.sequence = 0;
    this.readySequence = 0;
    this.parseFlags = flags;
    this.parseTags = tags;
    this.previewFeatures = features;
    NativeDownstreamSession.create(this.sessionId, this.mode, flags);
    this.nativeCreated = true;
    controllers.set(this.sessionId, this);
    listenForNativeEvents();
    if (tags.length) NativeDownstreamSession.setParseTags(this.sessionId, tags);
    if (features) NativeDownstreamSession.setPreview(this.sessionId, features);
    if (this.tracing) NativeDownstreamSession.setTracing(this.sessionId, true);
    // reset accepts a source larger than a transport chunk and starts a new
    // generation, so old publications cannot target the rebuilt document.
    if (source || this.generation > 0) {
      if (!rebuilding) this.generation++;
      NativeDownstreamSession.reset(this.sessionId, source, this.generation);
    }
    if (this.finished)
      NativeDownstreamSession.finish(
        this.sessionId,
        this.generation,
        this.sequence
      );
    if (rebuilding || source || this.generation > 0) this.publish();
  }

  append(chunk: string): void {
    this.assertActive();
    if (this.finished) {
      throw operationError(
        'invalid_state',
        'Reset the stream before appending after finish.'
      );
    }
    if (typeof chunk !== 'string') {
      throw operationError('invalid_input', 'Markdown chunk must be a string.');
    }
    if (chunk.length === 0) return;
    const bytes = checkedUtf8Bytes(chunk);
    if (typeof bytes !== 'number') {
      throw operationError(bytes.code, bytes.message);
    }
    if (bytes > MAX_CHUNK_BYTES) {
      throw operationError(
        'input_limit',
        `Markdown chunk exceeds ${MAX_CHUNK_BYTES} bytes of UTF-8.`
      );
    }
    if (this.sourceBytes + bytes > MAX_SOURCE_BYTES) {
      throw operationError(
        'input_limit',
        `Markdown source exceeds ${MAX_SOURCE_BYTES} bytes of UTF-8.`
      );
    }
    if (this.queuedBytes + bytes > MAX_QUEUED_BYTES) {
      throw operationError(
        'input_limit',
        `Queued Markdown exceeds ${MAX_QUEUED_BYTES} bytes of UTF-8.`
      );
    }
    this.appendChecked(chunk, bytes);
  }

  private appendChecked(chunk: string, bytes: number): void {
    // The first accepted chunk of a generation is where text starts arriving,
    // which is the transition `onAnimationStart` reports. A chunk that only
    // refreshes a preview policy does not, so this is not on `setPreview`.
    this.noteAnimating(true);
    this.trace('receipt', this.sequence + 1);
    // Keep accepted bytes without repeatedly joining the response. The public
    // source is materialized only when requested; pages bound piece overhead.
    this.sourcePieces.push(chunk);
    if (this.sourcePieces.length === 64) {
      this.sourcePages.push(this.sourcePieces.join(''));
      this.sourcePieces = [];
    }
    this.sourceBytes += bytes;
    this.sequence++;
    // Before the first view binds, text has no parsing policy yet. Keep only
    // the canonical source; configure() seeds native with it on first bind.
    if (!this.nativeCreated) return;
    this.queuedBytes += bytes;
    const last = this.pending[this.pending.length - 1];
    if (last && last.bytes + bytes <= MAX_CHUNK_BYTES) {
      last.pieces.push(chunk);
      last.text = undefined;
      last.bytes += bytes;
      last.sequence = this.sequence;
    } else {
      this.pending.push({ pieces: [chunk], bytes, sequence: this.sequence });
    }
    if (this.frame === undefined) {
      this.frame = requestAnimationFrame(() => {
        this.frame = undefined;
        this.flush();
      });
    }
  }

  /** Internal string-view path. The whole source is bounded to 1 MiB. */
  updateControlledSource(input: string, streaming: boolean): string {
    this.assertActive();
    if (
      this.controlledInput?.input === input &&
      this.controlledInput.streaming === streaming
    )
      return this.controlledSource.source;
    this.controlledInput = { input, streaming };
    const next = checkStreamingSource(this.controlledSource, input, streaming);
    if ('code' in next) {
      this.emitError(next.code, next.message, this.sequence);
      // A rejected final update must still settle the last valid document.
      if (!streaming) this.finish();
      return this.controlledSource.source;
    }
    const previous = this.controlledSource.source;
    if (
      (this.finished && (streaming || next.source !== previous)) ||
      !next.source.startsWith(previous)
    ) {
      this.reset(next.source);
    } else if (next.source !== previous) {
      const addedBytes = next.bytes - this.controlledSource.bytes;
      if (addedBytes <= MAX_CHUNK_BYTES) {
        this.appendChecked(next.source.slice(previous.length), addedBytes);
      } else {
        // At most 16K UTF-16 units fit in one chunk, even for three-byte BMP
        // characters. Keep surrogate pairs together at every boundary.
        for (let start = previous.length; start < next.source.length;) {
          let end = Math.min(start + 16 * 1024, next.source.length);
          const last = next.source.charCodeAt(end - 1);
          if (last >= 0xd800 && last <= 0xdbff) end--;
          const chunk = next.source.slice(start, end);
          const bytes = checkedUtf8Bytes(chunk) as number;
          this.appendChecked(chunk, bytes);
          start = end;
        }
      }
    }
    this.controlledSource = next;
    if (!streaming) this.finish();
    return next.source;
  }

  /** Send accepted chunks immediately. Useful before finish and in deterministic tests. */
  flush(): void {
    if (this.disposed) return;
    if (this.frame !== undefined) {
      cancelAnimationFrame(this.frame);
      this.frame = undefined;
    }
    while (this.pending.length) {
      const batch = this.pending[0]!;
      this.trace('batch_flush_start', batch.sequence);
      try {
        batch.text ??= batch.pieces.join('');
        NativeDownstreamSession.append(
          this.sessionId,
          batch.text,
          this.generation,
          batch.sequence
        );
        this.inFlight.push({ bytes: batch.bytes, sequence: batch.sequence });
        this.pending.shift();
        this.trace('batch_flush_end', batch.sequence);
      } catch (error) {
        this.emitError(
          'render_failed',
          `Native append failed: ${String(error)}`,
          batch.sequence
        );
        break;
      }
    }
  }

  reset(source = ''): void {
    this.assertActive();
    if (typeof source !== 'string') {
      throw operationError(
        'invalid_input',
        'Markdown source must be a string.'
      );
    }
    const bytes = checkedUtf8Bytes(source);
    if (typeof bytes !== 'number') {
      throw operationError(bytes.code, bytes.message);
    }
    const generation = this.generation + 1;
    if (this.nativeCreated)
      NativeDownstreamSession.reset(this.sessionId, source, generation);
    if (this.frame !== undefined) cancelAnimationFrame(this.frame);
    this.frame = undefined;
    this.source = source;
    this.sourcePages = [];
    this.sourcePieces = [];
    this.sourceBytes = bytes;
    this.generation = generation;
    this.sequence = 0;
    this.readySequence = 0;
    this.queuedBytes = 0;
    this.pending = [];
    this.inFlight = [];
    this.finished = false;
    // A new generation has shown nothing yet, so the next append is a fresh
    // start rather than a continuation.
    this.noteAnimating(false);
    this.publish();
  }

  finish(): void {
    this.assertActive();
    if (this.finished) return;
    this.flush();
    if (this.pending.length) {
      throw operationError(
        'render_failed',
        'Native append failed; finish was not sent. Retry finish or reset the stream.'
      );
    }
    if (this.nativeCreated)
      NativeDownstreamSession.finish(
        this.sessionId,
        this.generation,
        this.sequence
      );
    this.finished = true;
    // The last snapshot is still scheduled, and every fade it started runs to its
    // end; this is the transition an app's `onAnimationEnd` reports. It fires
    // here rather than when the last fade finishes, which is Streamdown's rule and
    // which is up to about half a second earlier.
    this.noteAnimating(false);
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.tracing = false;
    if (this.frame !== undefined) cancelAnimationFrame(this.frame);
    this.frame = undefined;
    cancelSnapshotPublication(this.prepareSnapshotPublication);
    this.pending = [];
    this.inFlight = [];
    this.subscribers.clear();
    this.source = '';
    this.sourcePages = [];
    this.sourcePieces = [];
    this.animationListeners.clear();
    this.viewOnError = undefined;
    this.controlledSource = { source: '', bytes: 0 };
    this.controlledInput = undefined;
    controllers.delete(this.sessionId);
    if (this.nativeCreated) NativeDownstreamSession.dispose(this.sessionId);
    stopListeningIfUnused();
  }

  async getSource(): Promise<string> {
    this.assertActive();
    return this.readSource();
  }

  private readSource(): string {
    if (this.sourcePages.length || this.sourcePieces.length) {
      this.source += this.sourcePages.join('') + this.sourcePieces.join('');
      this.sourcePages = [];
      this.sourcePieces = [];
    }
    return this.source;
  }

  binding(): StreamBinding {
    this.assertActive();
    return {
      sessionId: this.sessionId,
      mode: this.mode,
      generation: this.generation,
      sequence: this.sequence,
      readySequence: this.readySequence,
      revision: this.revision,
    };
  }

  setTracing(enabled: boolean): void {
    this.assertActive();
    if (this.nativeCreated)
      NativeDownstreamSession.setTracing(this.sessionId, enabled);
    this.tracing = enabled;
  }

  setPreview(options: boolean | MarkdownPreviewOptions): void {
    this.assertActive();
    const features = previewFeatures(options);
    if (features === this.previewFeatures) return;
    this.previewFeatures = features;
    if (this.nativeCreated)
      NativeDownstreamSession.setPreview(this.sessionId, features);
  }

  private trace(event: string, sequence: number): void {
    if (this.tracing) {
      emitJsStreamTrace(this.sessionId, this.generation, sequence, event);
    }
  }

  /**
   * Reports a change in whether text is arriving, once per transition.
   *
   * The stream's own answer rather than a prop, because a view can tell that it
   * is mid-answer without JavaScript watching for it: an app that wants a caret
   * or a fade asks for the animation, not for the flag.
   */
  subscribeAnimation(listener: (animating: boolean) => void): () => void {
    this.animationListeners.add(listener);
    return () => {
      this.animationListeners.delete(listener);
    };
  }

  /** Whether text is still arriving, which native follows as `animating: auto`. */
  isStreaming(): boolean {
    return this.streaming;
  }

  private noteAnimating(next: boolean): void {
    if (this.streaming === next) return;
    this.streaming = next;
    this.animationListeners.forEach((listener) => listener(next));
  }

  bind(onError?: (error: MarkdownErrorEvent) => void): () => void {
    this.assertActive();
    if (this.bound)
      throw operationError(
        'session_bound',
        'A Markdown stream can bind only one view.'
      );
    this.bound = true;
    this.viewOnError = onError;
    return () => {
      this.bound = false;
      this.viewOnError = undefined;
    };
  }

  subscribe(callback: () => void): () => void {
    this.subscribers.add(callback);
    return () => {
      this.subscribers.delete(callback);
    };
  }

  snapshotReady(event: SnapshotReadyEvent): void {
    if (
      this.disposed ||
      event.generation !== this.generation ||
      event.sequence > this.sequence ||
      event.sequence < this.readySequence
    )
      return;
    this.readySequence = event.sequence;
    while (
      this.inFlight.length &&
      this.inFlight[0]!.sequence <= event.sequence
    ) {
      this.queuedBytes -= this.inFlight.shift()!.bytes;
    }
    this.publishOnNextFrame();
  }

  sessionError(event: SessionErrorEvent): void {
    if (
      this.disposed ||
      event.generation !== this.generation ||
      event.sequence > this.sequence
    )
      return;
    const code = isErrorCode(event.code) ? event.code : 'render_failed';
    (this.viewOnError ?? this.onError)?.({ ...event, code });
  }

  private publish(): void {
    cancelSnapshotPublication(this.prepareSnapshotPublication);
    this.notifySnapshotRevision(++this.revision);
  }

  /**
   * Notifies at most once per frame, sharing an explicit React Native batch
   * with every other dirty stream. React still controls when each surface
   * commits; publication reads the newest native snapshot.
   */
  private publishOnNextFrame(): void {
    scheduleSnapshotPublication(this.prepareSnapshotPublication);
  }

  private readonly prepareSnapshotPublication = (): (() => void) => {
    const revision = ++this.revision;
    return () => this.notifySnapshotRevision(revision);
  };

  private notifySnapshotRevision(revision: number): void {
    this.subscribers.forEach((callback) => {
      // Any subscriber can reset or dispose this stream during notification.
      // Check each callback, since reset already publishes the new revision.
      if (!this.disposed && this.revision === revision) {
        callback();
      }
    });
  }

  private emitError(
    code: MarkdownErrorCode,
    message: string,
    sequence: number
  ): void {
    (this.viewOnError ?? this.onError)?.({
      code,
      message,
      generation: this.generation,
      sequence,
      recoverable: true,
    });
  }

  private assertActive(): void {
    if (this.disposed)
      throw operationError(
        'invalid_state',
        'Markdown stream has been disposed.'
      );
  }
}

function operationError(
  code: MarkdownErrorCode,
  message: string
): MarkdownOperationError {
  return Object.assign(new Error(message), {
    name: 'MarkdownOperationError',
    code,
  });
}

function isErrorCode(code: string): code is MarkdownErrorCode {
  return [
    'invalid_input',
    'input_limit',
    'invalid_state',
    'session_bound',
    'parse_failed',
    'render_failed',
  ].includes(code);
}

function listenForNativeEvents(): void {
  if (removeSnapshotListener) return;
  const snapshot = NativeDownstreamSession.onSnapshotReady((event) => {
    controllers.get(event.sessionId)?.snapshotReady(event);
  });
  const error = NativeDownstreamSession.onSessionError((event) => {
    controllers.get(event.sessionId)?.sessionError(event);
  });
  removeSnapshotListener = () => snapshot.remove();
  removeErrorListener = () => error.remove();
}

function stopListeningIfUnused(): void {
  if (controllers.size > 0) return;
  removeSnapshotListener?.();
  removeErrorListener?.();
  removeSnapshotListener = undefined;
  removeErrorListener = undefined;
}

/** Create a stream whose canonical source stays in JavaScript and native. */
export function createMarkdownStream(
  options: MarkdownStreamOptions = {}
): MarkdownStreamController {
  const unsupported = Object.keys(options).find((key) => key !== 'onError');
  if (unsupported)
    throw operationError(
      'invalid_input',
      `${unsupported} belongs on Downstream or DownstreamProvider, not createMarkdownStream.`
    );
  return new StreamState(options, 'incremental');
}

/** Internal switch for comparing both native parsers in one build. */
export function createMarkdownStreamWithMode(
  options: MarkdownSessionOptions & MarkdownStreamOptions,
  mode: ParseMode
): StreamState {
  const state = new StreamState(options, mode);
  state.configure(options);
  return state;
}

/**
 * The controller's own state, which a view reads for the transitions a prop
 * cannot carry: whether text is arriving, and when it started and stopped.
 */
export type { StreamState };

export function getStreamState(
  controller: MarkdownStreamController
): StreamState {
  if (!(controller instanceof StreamState)) {
    throw operationError(
      'invalid_input',
      'stream must come from createMarkdownStream.'
    );
  }
  return controller;
}
