import NativeDownstreamSession, {
  type NativeTraceEvent,
} from './NativeDownstreamSession';
import { getStreamState } from './createMarkdownStream';
import type { MarkdownStreamController } from './types';

export type JsStreamTraceEvent = Readonly<{
  sessionId: string;
  generation: number;
  sequence: number;
  event: string;
  clockDomain: 'js-performance';
  timeMs: number;
}>;

export type StreamTraceEvent = NativeTraceEvent | JsStreamTraceEvent;
type TraceListener = (event: StreamTraceEvent) => void;

const listeners = new Set<TraceListener>();
let nativeSubscription: { remove(): void } | undefined;

/** Internal benchmark hook. Subscribe before enabling tracing. */
export function subscribeStreamTrace(listener: TraceListener): () => void {
  listeners.add(listener);
  if (!nativeSubscription) {
    nativeSubscription = NativeDownstreamSession.onTrace((event) => {
      publish(event);
      emitJsStreamTrace(
        event.sessionId,
        event.generation,
        event.sequence,
        'native_event_received'
      );
    });
  }
  return () => {
    listeners.delete(listener);
    if (listeners.size === 0) {
      nativeSubscription?.remove();
      nativeSubscription = undefined;
    }
  };
}

/** Enable timestamps only for this stream. Dispose clears its native trace. */
export function setStreamTracing(
  stream: MarkdownStreamController,
  enabled: boolean
): void {
  getStreamState(stream).setTracing(enabled);
}

export function emitJsStreamTrace(
  sessionId: string,
  generation: number,
  sequence: number,
  event: string
): void {
  if (listeners.size === 0) return;
  publish({
    sessionId,
    generation,
    sequence,
    event,
    clockDomain: 'js-performance',
    timeMs: (
      globalThis as unknown as { performance: { now(): number } }
    ).performance.now(),
  });
}

function publish(event: StreamTraceEvent): void {
  listeners.forEach((listener) => listener(event));
}
