/* eslint-disable no-bitwise -- the preview feature mask is a bitmask */
import NativeDownstreamSession, {
  type SessionErrorEvent,
  type SnapshotReadyEvent,
} from './NativeDownstreamSession';
import { checkedUtf8Bytes, MAX_SOURCE_BYTES } from './controlledSource';
import { emitJsStreamTrace } from './streamTrace';
import type {
  MarkdownErrorCode,
  MarkdownErrorEvent,
  MarkdownOperationError,
  MarkdownPreviewOptions,
  MarkdownStreamController,
  MarkdownStreamOptions,
} from './types';

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
const PREVIEW_ALL =
  PREVIEW_EMPHASIS | PREVIEW_INLINE_CODE | PREVIEW_LINKS | PREVIEW_IMAGES;

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
  return features;
}
const controllers = new Map<string, StreamState>();
let nextSession = 0;
let removeSnapshotListener: (() => void) | undefined;
let removeErrorListener: (() => void) | undefined;

export type ParseMode = 'full' | 'incremental';

export type StreamBinding = Readonly<{
  sessionId: string;
  mode: ParseMode;
  generation: number;
  sequence: number;
  readySequence: number;
  revision: number;
}>;

type PendingBatch = { text: string; bytes: number; sequence: number };
type InFlight = { bytes: number; sequence: number };

class StreamState implements MarkdownStreamController {
  readonly sessionId: string;
  readonly mode: ParseMode;
  private onError?: (error: MarkdownErrorEvent) => void;
  private source = '';
  private sourceBytes = 0;
  private generation = 0;
  private sequence = 0;
  private readySequence = 0;
  private revision = 0;
  private queuedBytes = 0;
  private pending: PendingBatch[] = [];
  private inFlight: InFlight[] = [];
  private frame: number | undefined;
  private snapshotFrame: number | undefined;
  private finished = false;
  private disposed = false;
  private bound = false;
  private tracing = false;
  private previewFeatures = 0;
  private subscribers = new Set<() => void>();

  constructor(options: MarkdownStreamOptions, mode: ParseMode) {
    this.sessionId = `downstream-${Date.now()}-${++nextSession}`;
    this.mode = mode;
    this.onError = options.onError;
    NativeDownstreamSession.create(this.sessionId, mode);
    // Set after create, so a session always exists before it is configured.
    if (options.preview !== undefined) {
      this.setPreview(options.preview);
    }
    controllers.set(this.sessionId, this);
    listenForNativeEvents();
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
    this.trace('receipt', this.sequence + 1);
    this.source += chunk;
    this.sourceBytes += bytes;
    this.queuedBytes += bytes;
    this.sequence++;
    const last = this.pending[this.pending.length - 1];
    if (last && last.bytes + bytes <= MAX_CHUNK_BYTES) {
      last.text += chunk;
      last.bytes += bytes;
      last.sequence = this.sequence;
    } else {
      this.pending.push({ text: chunk, bytes, sequence: this.sequence });
    }
    if (this.frame === undefined) {
      this.frame = requestAnimationFrame(() => {
        this.frame = undefined;
        this.flush();
      });
    }
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
    NativeDownstreamSession.reset(this.sessionId, source, generation);
    if (this.frame !== undefined) cancelAnimationFrame(this.frame);
    this.frame = undefined;
    this.source = source;
    this.sourceBytes = bytes;
    this.generation = generation;
    this.sequence = 0;
    this.readySequence = 0;
    this.queuedBytes = 0;
    this.pending = [];
    this.inFlight = [];
    this.finished = false;
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
    NativeDownstreamSession.finish(
      this.sessionId,
      this.generation,
      this.sequence
    );
    this.finished = true;
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.tracing = false;
    if (this.frame !== undefined) cancelAnimationFrame(this.frame);
    this.frame = undefined;
    this.cancelSnapshotFrame();
    this.pending = [];
    this.inFlight = [];
    this.subscribers.clear();
    controllers.delete(this.sessionId);
    NativeDownstreamSession.dispose(this.sessionId);
    stopListeningIfUnused();
  }

  async getSource(): Promise<string> {
    this.assertActive();
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
    NativeDownstreamSession.setTracing(this.sessionId, enabled);
    this.tracing = enabled;
  }

  setPreview(options: boolean | MarkdownPreviewOptions): void {
    this.assertActive();
    const features = previewFeatures(options);
    if (features === this.previewFeatures) return;
    this.previewFeatures = features;
    NativeDownstreamSession.setPreview(this.sessionId, features);
  }

  private trace(event: string, sequence: number): void {
    if (this.tracing) {
      emitJsStreamTrace(this.sessionId, this.generation, sequence, event);
    }
  }

  bind(): () => void {
    this.assertActive();
    if (this.bound)
      throw operationError(
        'session_bound',
        'A Markdown stream can bind only one view.'
      );
    this.bound = true;
    return () => {
      this.bound = false;
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
    this.onError?.({ ...event, code });
  }

  private publish(): void {
    this.cancelSnapshotFrame();
    this.revision++;
    this.subscribers.forEach((callback) => callback());
  }

  /**
   * Renders at most once per frame. A stream can publish snapshots faster
   * than the screen refreshes, and every render is a Fabric commit on the
   * JavaScript thread; streams that publish in the same frame also share one
   * React batch. The render reads the newest snapshot.
   */
  private publishOnNextFrame(): void {
    if (this.snapshotFrame !== undefined) return;
    this.snapshotFrame = requestAnimationFrame(() => {
      this.snapshotFrame = undefined;
      if (!this.disposed) this.publish();
    });
  }

  private cancelSnapshotFrame(): void {
    if (this.snapshotFrame !== undefined) {
      cancelAnimationFrame(this.snapshotFrame);
      this.snapshotFrame = undefined;
    }
  }

  private emitError(
    code: MarkdownErrorCode,
    message: string,
    sequence: number
  ): void {
    this.onError?.({
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
  return createMarkdownStreamWithMode(options, 'incremental');
}

/** Internal switch for comparing both native parsers in one build. */
export function createMarkdownStreamWithMode(
  options: MarkdownStreamOptions,
  mode: ParseMode
): MarkdownStreamController {
  return new StreamState(options, mode);
}

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
