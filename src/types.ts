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
  | 'render_failed'
  | 'control_failed';

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

/** What a native control button did, reported to the matching callback. */
export type MarkdownControlEvent = Readonly<{
  block: 'code' | 'table';
  action: 'copy' | 'download' | 'wrap';
  /** Copy and download only. */
  format?: 'text' | 'markdown' | 'csv' | 'tsv';
  /** Wrap only: the block's layout after the press. */
  wrapped?: boolean;
  /** Fence language as written, lower-cased; '' for indented code and tables. */
  language: string;
  /** Saved file name, for downloads. */
  filename?: string;
}>;

export type MarkdownCopyControl =
  | boolean
  | Readonly<{
      onCopy?: (event: MarkdownControlEvent) => void;
      onError?: (error: MarkdownErrorEvent) => void;
    }>;

export type MarkdownDownloadControl =
  | boolean
  | Readonly<{
      /** Base name without an extension. The extension comes from the block. */
      filename?: string;
      onDownload?: (event: MarkdownControlEvent) => void;
      onError?: (error: MarkdownErrorEvent) => void;
    }>;

/**
 * Streamdown's `controls` shape, so a value written for it works as it is.
 *
 * Each key is a boolean or an object, and a missing key means on. `true` is
 * the default, and `false` removes the code header and the table toolbar
 * without changing either block's layout.
 */
export type MarkdownControls =
  | boolean
  | Readonly<{
      code?:
        | boolean
        | Readonly<{
            copy?: MarkdownCopyControl;
            download?: MarkdownDownloadControl;
            /** Per-block wrap button in the code header. */
            wrap?:
              | boolean
              | Readonly<{
                  onToggle?: (event: MarkdownControlEvent) => void;
                }>;
          }>;
      table?:
        | boolean
        | Readonly<{
            copy?: MarkdownCopyControl;
            download?: MarkdownDownloadControl;
            csvSeparator?: ',' | ';' | '\t' | 'auto';
          }>;
    }>;

/**
 * Labels for control buttons and announcements. Streamdown's key names and
 * English defaults, limited to the keys these controls use.
 */
export type MarkdownTranslations = Readonly<{
  /** Copy the code block. */
  copyCode: string;
  /** Save the code block to a file. */
  downloadFile: string;
  /** Spoken after a copy succeeds. */
  copied: string;
  /** Turn on the code header's wrap button. */
  wrapCode: string;
  /** Turn off the code header's wrap button. */
  unwrapCode: string;
  /** Open the table's copy menu. */
  copyTable: string;
  copyTableAsMarkdown: string;
  copyTableAsCsv: string;
  copyTableAsTsv: string;
  /** Open the table's download menu. */
  downloadTable: string;
  downloadTableAsCsv: string;
  downloadTableAsMarkdown: string;
  /** Menu item labels in the chosen format. */
  tableFormatMarkdown: string;
  tableFormatCsv: string;
  tableFormatTsv: string;
}>;

export const defaultTranslations: MarkdownTranslations = {
  copyCode: 'Copy Code',
  downloadFile: 'Download file',
  copied: 'Copied',
  wrapCode: 'Wrap lines',
  unwrapCode: "Don't wrap lines",
  copyTable: 'Copy table',
  copyTableAsMarkdown: 'Copy table as Markdown',
  copyTableAsCsv: 'Copy table as CSV',
  copyTableAsTsv: 'Copy table as TSV',
  downloadTable: 'Download table',
  downloadTableAsCsv: 'Download table as CSV',
  downloadTableAsMarkdown: 'Download table as Markdown',
  tableFormatMarkdown: 'Markdown',
  tableFormatCsv: 'CSV',
  tableFormatTsv: 'TSV',
};

type MarkdownViewBaseProps = Readonly<{
  style?: StyleProp<ViewStyle>;
  /**
   * Native text selection. On by default, applied per settled segment, so
   * text that is still streaming is never selectable.
   *
   * An app that opens its own message menu on a long press passes `false`.
   */
  selectable?: boolean;
  /**
   * Colors fenced code blocks by their info string. On by default.
   *
   * A block whose language the engine has no lexer for, and one over 64 KiB,
   * stay plain either way. The default palette follows GitHub's light and dark
   * code themes; overriding per-token colors is not part of this release.
   */
  syntaxHighlighting?: boolean;
  /**
   * Wrap long code lines instead of scrolling them sideways. Off by default,
   * as in Streamdown and GitHub. Changes layout only, never copied text.
   */
  codeBlockWrap?: boolean;
  /**
   * Copy and download buttons on code blocks and tables. On by default.
   *
   * A control turns on as soon as its own block is settled, so a finished code
   * block can be copied while the rest of the message is still streaming.
   */
  controls?: MarkdownControls;
  /** Overrides for control labels and spoken announcements. */
  translations?: Partial<MarkdownTranslations>;
  onLinkPress?: (event: MarkdownLinkPressEvent) => void;
  onError?: (error: MarkdownErrorEvent) => void;
}>;

export type MarkdownViewProps = MarkdownViewBaseProps &
  (
    | Readonly<{ markdown: string; stream?: never }>
    | Readonly<{ stream: MarkdownStreamController; markdown?: never }>
  );
