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

export interface Spec extends TurboModule {
  create(sessionId: string, mode: string): void;
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
  readonly onSnapshotReady: CodegenTypes.EventEmitter<SnapshotReadyEvent>;
  readonly onSessionError: CodegenTypes.EventEmitter<SessionErrorEvent>;
  readonly onTrace: CodegenTypes.EventEmitter<NativeTraceEvent>;
}

export default TurboModuleRegistry.getEnforcing<Spec>('DownstreamSession');
