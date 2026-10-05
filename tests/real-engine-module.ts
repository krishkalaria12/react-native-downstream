/**
 * A TurboModule backed by the real C++ engine.
 *
 * A `vi.fn()` TurboModule proves only that the JavaScript called a function
 * with some arguments; it cannot show that a document rendered correctly. This
 * one keeps a real source per session, parses it with the parser that ships in
 * the app, and emits the snapshot and trace events the controller expects from
 * what actually happened. A test can then assert on the text a reader sees.
 *
 * Only the transport is simulated. The parse is real.
 */
import { vi } from 'vitest';

import {
  fenceSlots,
  replaySession,
  type FenceSlot,
  type RenderModel,
  type SessionMode,
  type SessionOp,
} from './engine';

/** The largest source a synchronous query parses, as the module's bound is. */
export const SYNC_SLOT_BYTES = 16 * 1024;

type SnapshotListener = (event: object) => void;
type ErrorListener = (event: object) => void;
type TraceListener = (event: object) => void;

interface Session {
  mode: SessionMode;
  source: string;
  generation: number;
  sequence: number;
  finished: boolean;
  model: RenderModel | undefined;
  /** Provisional-styling features, as the native bitmask. */
  preview: number;
  /** Math parse options, as the native bitmask, fixed at creation. */
  mathFlags: number;
  /**
   * Everything the worker has applied to this session's parser, in order.
   * Each pass replays it through the real `IncrementalParser`, so what the
   * parser does with a finished stream or a preview policy is the engine's
   * answer, not this file's.
   */
  steps: SessionOp[];
}

const sessions = new Map<string, Session>();
const snapshotListeners = new Set<SnapshotListener>();
const errorListeners = new Set<ErrorListener>();
const traceListeners = new Set<TraceListener>();
const nativeTraceEnabled = new Set<string>();

/** The render model a session last produced, for assertions. */
export function snapshotOf(sessionId: string): RenderModel | undefined {
  return sessions.get(sessionId)?.model;
}

/** The canonical source a session has accumulated, as the worker sees it. */
export function sourceForSession(sessionId: string): string {
  return sessions.get(sessionId)?.source ?? '';
}

/** How many times native was handed work, for asserting a batch count. */
export const nativeWork = { appends: 0, parses: 0 };

/** The provisional-styling features a session is configured with. */
export function previewFeaturesOf(sessionId: string): number {
  return sessions.get(sessionId)?.preview ?? 0;
}

/** The math parse options a session was created with. */
export function mathFlagsOf(sessionId: string): number {
  return sessions.get(sessionId)?.mathFlags ?? 0;
}

function nowNs(): string {
  return String(Math.round(performance.now() * 1e6));
}

function emitTrace(
  sessionId: string,
  generation: number,
  sequence: number,
  event: string
): void {
  if (!nativeTraceEnabled.has(sessionId)) return;
  for (const listener of traceListeners) {
    listener({
      sessionId,
      generation,
      sequence,
      event,
      clockDomain: 'native-monotonic',
      timeNs: nowNs(),
    });
  }
}

/** Runs one worker pass and reports a snapshot, like the worker does. */
function publish(session: Session, sessionId: string): void {
  emitTrace(sessionId, session.generation, session.sequence, 'append_received');
  nativeWork.parses += 1;
  session.steps.push({ op: 'parse', finished: session.finished });
  const model = replaySession(session.mode, session.steps);
  session.model = model;
  emitTrace(sessionId, session.generation, session.sequence, 'parse_start');
  emitTrace(sessionId, session.generation, session.sequence, 'parse_end');
  if (model.error) {
    for (const listener of errorListeners) {
      listener({
        sessionId,
        generation: session.generation,
        sequence: session.sequence,
        code: model.error,
        message: model.error,
        recoverable: true,
      });
    }
    return;
  }
  for (const listener of snapshotListeners) {
    listener({
      sessionId,
      generation: session.generation,
      sequence: session.sequence,
    });
  }
}

/**
 * How many times a fence-slot query reached the engine, so a test can assert
 * that a view with no renderers asks nothing and that an unchanged list asks
 * once.
 */
export const slotWork = { queries: 0, async: 0 };

/** The slots a controlled source holds, as the TurboModule answers them. */
function controlledSlots(
  source: string,
  mathFlags: number,
  languages: readonly string[]
): { pending: boolean; slots: readonly FenceSlot[] } {
  slotWork.queries += 1;
  // The real bound: a source past it answers `pending` and the caller asks the
  // worker instead, which is what keeps a big message off the JavaScript thread.
  const bytes = Buffer.byteLength(source, 'utf8');
  if (bytes > SYNC_SLOT_BYTES) {
    return { pending: true, slots: [] };
  }
  return {
    pending: false,
    slots: fenceSlots(source, languages, mathFlags, true),
  };
}

export const realEngineModule = {
  __esModule: true,
  default: {
    mathAvailable: () => true,

    create(sessionId: string, mode: string, mathFlags = 0) {
      sessions.set(sessionId, {
        // The TurboModule treats anything but `incremental` as full mode.
        mode: mode === 'incremental' ? 'incremental' : 'full',
        source: '',
        generation: 0,
        sequence: 0,
        finished: false,
        model: undefined,
        preview: 0,
        mathFlags,
        steps: [],
      });
    },

    append(
      sessionId: string,
      chunk: string,
      generation: number,
      sequence: number
    ) {
      nativeWork.appends += 1;
      const session = sessions.get(sessionId);
      if (!session || session.generation !== generation) return;
      session.source += chunk;
      session.steps.push({ op: 'append', chunk });
      session.sequence = sequence;
      publish(session, sessionId);
    },

    reset(sessionId: string, source: string, generation: number) {
      const session = sessions.get(sessionId);
      if (!session) return;
      session.source = source;
      session.steps.push({ op: 'replace', source });
      session.generation = generation;
      session.sequence = 0;
      session.finished = false;
      publish(session, sessionId);
    },

    finish(sessionId: string, generation: number, sequence: number) {
      const session = sessions.get(sessionId);
      if (!session || session.generation !== generation) return;
      session.finished = true;
      session.sequence = sequence;
      publish(session, sessionId);
    },

    dispose(sessionId: string) {
      nativeTraceEnabled.delete(sessionId);
      sessions.delete(sessionId);
    },

    setTracing(sessionId: string, enabled: boolean) {
      if (enabled) nativeTraceEnabled.add(sessionId);
      else nativeTraceEnabled.delete(sessionId);
    },

    fenceSlots(
      source: string,
      mathFlags: number,
      languages: readonly string[]
    ): { pending: boolean; slots: readonly FenceSlot[] } {
      return controlledSlots(source, mathFlags, languages);
    },

    async fenceSlotsAsync(
      source: string,
      mathFlags: number,
      languages: readonly string[]
    ): Promise<readonly FenceSlot[]> {
      slotWork.async += 1;
      // The worker parses the same source with the same rule; only the thread
      // differs, and a test cannot see which thread ran it.
      return fenceSlots(source, languages, mathFlags, true);
    },

    sessionFenceSlots(
      sessionId: string,
      generation: number,
      sequence: number,
      languages: readonly string[]
    ): readonly FenceSlot[] {
      const session = sessions.get(sessionId);
      if (
        !session ||
        session.generation !== generation ||
        session.sequence !== sequence
      ) {
        return [];
      }
      slotWork.queries += 1;
      // The snapshot the view is mounting, parsed from the session's own source
      // with the session's own flags, which is what the native query reads: no
      // parse of its own, one walk of the blocks it already holds.
      return fenceSlots(
        session.source,
        languages,
        session.mathFlags,
        !session.finished
      );
    },

    setPreview(sessionId: string, features: number) {
      const session = sessions.get(sessionId);
      if (!session) return;
      session.preview = features;
      session.steps.push({ op: 'preview', features });
      // The native side queues a pass so a policy change reaches the reader
      // without new input, unless the stream has settled and there is nothing
      // left to repaint. The policy still reaches the parser either way.
      if (!session.finished) publish(session, sessionId);
    },

    onSnapshotReady(callback: SnapshotListener) {
      snapshotListeners.add(callback);
      return { remove: () => snapshotListeners.delete(callback) };
    },

    onSessionError(callback: ErrorListener) {
      errorListeners.add(callback);
      return { remove: () => errorListeners.delete(callback) };
    },

    onTrace(callback: TraceListener) {
      traceListeners.add(callback);
      return { remove: () => traceListeners.delete(callback) };
    },
  },
};

/** Clears every session and counter between tests. */
export function resetEngine(): void {
  sessions.clear();
  snapshotListeners.clear();
  errorListeners.clear();
  traceListeners.clear();
  nativeTraceEnabled.clear();
  nativeWork.appends = 0;
  nativeWork.parses = 0;
  slotWork.queries = 0;
  slotWork.async = 0;
}

export const mockTurboModule = vi.fn(() => realEngineModule.default);
