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
  /**
   * Creates a session. `mathFlags` is the parse option bitmask, read once here
   * and kept for the life of the session: parse options are part of a source's
   * identity, so they cannot change under a stream the way a preview policy can.
   */
  create(sessionId: string, mode: string, mathFlags: number): void;
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
   * Whether a math engine is linked into this process, so JavaScript can settle
   * the `math` prop's default during render instead of after a first pass.
   *
   * A method rather than a field because codegen only allows a property on a
   * TurboModule when it is a non-null EventEmitter; anything else has to be
   * spelled as a method, sync or not. The answer only changes when the app's
   * binary does, which is a relaunch, so nothing here is cached on the native side.
   */
  mathAvailable(): boolean;
  readonly onSnapshotReady: CodegenTypes.EventEmitter<SnapshotReadyEvent>;
  readonly onSessionError: CodegenTypes.EventEmitter<SessionErrorEvent>;
  readonly onTrace: CodegenTypes.EventEmitter<NativeTraceEvent>;
}

export default TurboModuleRegistry.getEnforcing<Spec>('DownstreamSession');
