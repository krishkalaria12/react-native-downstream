/** Public API types. The stream controller types describe the planned session API. */
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

/**
 * Which provisional repairs a stream may make to its unstable tail.
 *
 * An open span renders literally unless a feature here is enabled, and even
 * then only when reparsing proves the repair changed nothing but the block it
 * aimed at. See [streaming semantics](docs/streaming-semantics.md).
 */
export type MarkdownPreviewOptions = Readonly<{
  /**
   * Close a `*`, `_` or `~~` run the writer left open, so its content shows
   * styled. A bare marker with no content after it is never closed, and
   * neither is one in a block that has already ended.
   */
  emphasis?: boolean;
  /**
   * Close an unclosed backtick run, so the text after it shows as code.
   */
  inlineCode?: boolean;
  /**
   * Show an unfinished link, `[text` or `[text](https://exa`, as its text. The
   * partial target is dropped rather than followed, so nothing is tappable
   * until the link's own `)` arrives.
   */
  links?: boolean;
  /**
   * Show an unfinished image, `![alt` or `![alt](https://exa`, as its alt text,
   * which is how a finished image renders too.
   */
  images?: boolean;
}>;

export type MarkdownStreamOptions = Readonly<{
  onError?: (error: MarkdownErrorEvent) => void;
  /**
   * Provisional styling for the unstable tail.
   *
   * Off by default, so an open span renders as the literal text the writer
   * sent. `true` enables every repair; an object enables the ones named.
   * Changing it later re-renders the current tail without disturbing the
   * canonical source.
   */
  preview?: boolean | MarkdownPreviewOptions;
}>;

/** Created by createMarkdownStream(options), planned for the native session phase. */
export interface MarkdownStreamController {
  append(chunk: string): void;
  reset(source?: string): void;
  finish(): void;
  dispose(): void;
  getSource(): Promise<string>;
  /**
   * Turns provisional styling on or off for the rest of this stream. A settled
   * stream keeps the literal source either way.
   */
  setPreview(options: boolean | MarkdownPreviewOptions): void;
}

type MarkdownViewBaseProps = Readonly<{
  style?: StyleProp<ViewStyle>;
  selectable?: boolean;
  /**
   * Colors fenced code blocks by their info string. On by default.
   *
   * A block whose language the engine has no lexer for, and one over 64 KiB,
   * stay plain either way. The default palette follows GitHub's light and dark
   * code themes; overriding per-token colors is not part of this release.
   */
  syntaxHighlighting?: boolean;
  onLinkPress?: (event: MarkdownLinkPressEvent) => void;
  onError?: (error: MarkdownErrorEvent) => void;
}>;

export type MarkdownViewProps = MarkdownViewBaseProps &
  (
    | Readonly<{ markdown: string; stream?: never }>
    | Readonly<{ stream: MarkdownStreamController; markdown?: never }>
  );
