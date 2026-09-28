/** Phase 0 API draft. Runtime exports are introduced with the native view/session. */
import type { StyleProp, ViewStyle } from 'react-native';

/** Half-open offsets in the canonical JavaScript UTF-16 source string. */
export type MarkdownSourceRange = Readonly<{
  start: number;
  end: number;
}>;

export type MarkdownErrorCode =
  | 'invalid_input'
  | 'input_limit'
  | 'invalid_state'
  | 'session_bound'
  | 'parse_failed'
  | 'render_failed';

export type MarkdownErrorEvent = Readonly<{
  code: MarkdownErrorCode;
  message: string;
  generation: number;
  sequence: number;
  recoverable: boolean;
}>;

/** Thrown by rejected synchronous stream operations. */
export interface MarkdownOperationError extends Error {
  readonly code: MarkdownErrorCode;
}

export type MarkdownLinkPressEvent = Readonly<{
  url: string;
  range: MarkdownSourceRange;
}>;

export type MarkdownStreamOptions = Readonly<{
  onError?: (error: MarkdownErrorEvent) => void;
}>;

/** Created by createMarkdownStream(options), planned for the native session phase. */
export interface MarkdownStreamController {
  append(chunk: string): void;
  reset(source?: string): void;
  finish(): void;
  dispose(): void;
  getSource(): Promise<string>;
}

type MarkdownViewBaseProps = Readonly<{
  style?: StyleProp<ViewStyle>;
  selectable?: boolean;
  onLinkPress?: (event: MarkdownLinkPressEvent) => void;
  onError?: (error: MarkdownErrorEvent) => void;
}>;

export type MarkdownViewProps = MarkdownViewBaseProps &
  (
    | Readonly<{ markdown: string; stream?: never }>
    | Readonly<{ stream: MarkdownStreamController; markdown?: never }>
  );
