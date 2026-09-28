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
  readonly onSnapshotReady: CodegenTypes.EventEmitter<SnapshotReadyEvent>;
  readonly onSessionError: CodegenTypes.EventEmitter<SessionErrorEvent>;
}

export default TurboModuleRegistry.getEnforcing<Spec>('DownstreamSession');
