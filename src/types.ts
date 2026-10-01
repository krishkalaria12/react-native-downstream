/** Public API types. The stream controller types describe the planned session API. */
import type { ColorValue, StyleProp, TextStyle, ViewStyle } from 'react-native';

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
  /**
   * The control a `control_failed` names. Present only for that code, so an
   * app can route a failure to the control that reported it and can tell an app
   * *which* control failed without parsing the message.
   */
  block?: MarkdownControlKind;
  /** The action that failed; present only alongside `block`. */
  action?: MarkdownControlAction;
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

/** Which kind of block a control acted on. */
export type MarkdownControlKind = 'code' | 'table';

/** What a control button did. */
export type MarkdownControlAction = 'copy' | 'download' | 'wrap';

/** What a native control button did, reported to the matching callback. */
export type MarkdownControlEvent = Readonly<{
  block: MarkdownControlKind;
  action: MarkdownControlAction;
  /** Copy and download only. */
  format?: 'text' | 'markdown' | 'csv' | 'tsv';
  /** Wrap only: the block's layout after the press. */
  wrapped?: boolean;
  /**
   * Fence language as written, in the case the writer used: `ts`, `TS`, and
   * `Ts` all come back as written. It is not a normalized key, so compare
   * case-insensitively.
   */
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

/**
 * A color in both appearances, or one color used for both.
 *
 * Every color string `<Text>` accepts works. A `{ light, dark }` pair is drawn
 * as a dynamic color, so switching appearance needs no new theme and no React
 * render. A processed `DynamicColorIOS` is a pair already, so it is accepted.
 * `PlatformColor` is not: Android resolves it to a fixed integer when props are
 * parsed, so it would not follow a configuration change the way the defaults do.
 * One is warned about in development and ignored.
 */
export type MarkdownColor =
  | ColorValue
  // Both halves optional on purpose: a theme that only wants to change its light
  // appearance writes `{ light }` and leaves the dark half to the default, which
  // is how a color can be themed for one appearance without restating the other.
  // An empty pair is a theme that clears the slot, which draws nothing.
  | Readonly<{ light?: ColorValue; dark?: ColorValue }>;

/**
 * The engine's token classes, in camelCase, for `theme.syntax` and
 * `markdownStyle.codeBlock.syntaxColors`. The class names are the wire names, so
 * no C++ field name reaches JavaScript and `operator` needs no rename.
 */
export type MarkdownTokenClass =
  | 'plain'
  | 'comment'
  | 'keyword'
  | 'type'
  | 'string'
  | 'number'
  | 'constant'
  | 'regex'
  | 'function'
  | 'property'
  | 'operator'
  | 'preprocessor'
  | 'tag'
  | 'attribute'
  | 'inserted'
  | 'deleted'
  | 'heading';

/**
 * The coarse layer: design tokens an app sets once for a whole chat, using the
 * names of Streamdown's CSS variables so a value it already keeps carries over.
 *
 * Each token fans out to a fixed list of slots. `foreground` sets paragraph,
 * heading, table, and list text; `mutedForeground` sets quote text, an h6, list
 * markers, image alt text, and the control chrome; `primary` sets links and
 * footnote references; `muted` sets the code card, inline code, and the table
 * header row; `border` sets the table grid, rules, and quote bars. An element
 * style wins over a token, and a token wins over the default.
 */
export type MarkdownTheme = Readonly<{
  colors?: Readonly<{
    /** Body and heading text. A native view has no inherited text color. */
    foreground?: MarkdownColor;
    /** De-emphasized text: quotes, an h6, markers, alt text, control chrome. */
    mutedForeground?: MarkdownColor;
    /** Links and footnote references. */
    primary?: MarkdownColor;
    /** The code card, inline code, and the table header row. */
    muted?: MarkdownColor;
    /** The table grid, rules, and quote bars. */
    border?: MarkdownColor;
    /** The text selection highlight. */
    selection?: MarkdownColor;
  }>;
  /** Corner radius for code cards and tables. */
  radius?: number;
  fonts?: Readonly<{ sans?: string; heading?: string; mono?: string }>;
  /** Token colors by engine class; see `syntaxColorsFromTheme`. */
  syntax?: Partial<Record<MarkdownTokenClass, MarkdownColor>>;
  /**
   * Caps the platform font scale for this view, as `<Text>`'s
   * `maxFontSizeMultiplier` does. It can cap Dynamic Type and Android's font
   * scale; it cannot turn scaling off. Below 1 is the unset value.
   */
  maxFontSizeMultiplier?: number;
}>;

/**
 * A weight, as `<Text>` spells it or as CSS numbers it.
 *
 * React Native's own type admits the hundred steps and nothing between them,
 * which is narrower than CSS and narrower than what the engine stores: a weight
 * here is a number on the wire, so 250 is a weight the theme can hold and the
 * platform resolves to the nearest face it has.
 */
export type MarkdownFontWeight = TextStyle['fontWeight'] | number;

type MarkdownTextStyle = Readonly<{
  fontSize?: number;
  fontFamily?: string;
  fontWeight?: MarkdownFontWeight;
  color?: MarkdownColor;
  lineHeight?: number;
  marginTop?: number;
  marginBottom?: number;
  /** Adds the italic trait, as a quote does in `presets.streamdown`. */
  fontStyle?: 'normal' | 'italic';
}>;

/**
 * The precise layer: one element at a time.
 *
 * These are `react-native-enriched-markdown`'s key and property names for every
 * key this renderer draws, so a value written for the closest native peer works
 * as it is. A key or property it does not draw is reported once in development
 * rather than silently ignored, so nobody believes it took effect.
 */
export type MarkdownStyle = Readonly<{
  paragraph?: MarkdownTextStyle;
  h1?: MarkdownTextStyle;
  h2?: MarkdownTextStyle;
  h3?: MarkdownTextStyle;
  h4?: MarkdownTextStyle;
  h5?: MarkdownTextStyle;
  h6?: MarkdownTextStyle;
  blockquote?: MarkdownTextStyle & {
    borderColor?: MarkdownColor;
    borderWidth?: number;
    gapWidth?: number;
  };
  list?: MarkdownTextStyle & {
    markerColor?: MarkdownColor;
    bulletColor?: MarkdownColor;
    marginLeft?: number;
    gapWidth?: number;
    itemSpacing?: number;
    markerFontSize?: number;
    markerFontFamily?: string;
    markerFontWeight?: MarkdownFontWeight;
  };
  taskList?: { checkedColor?: MarkdownColor; borderColor?: MarkdownColor };
  code?: {
    fontFamily?: string;
    fontSize?: number;
    color?: MarkdownColor;
    backgroundColor?: MarkdownColor;
  };
  codeBlock?: MarkdownTextStyle & {
    backgroundColor?: MarkdownColor;
    borderColor?: MarkdownColor;
    borderWidth?: number;
    borderRadius?: number;
    padding?: number;
    header?: {
      color?: MarkdownColor;
      buttonColor?: MarkdownColor;
      backgroundColor?: MarkdownColor;
      borderColor?: MarkdownColor;
    };
    /**
     * Token colors by engine class, plus enriched-markdown's own keys:
     * `punctuation` maps to `operator`, and `variable` and `embedded` map to
     * `plain`, which is the rule enriched applies to them.
     */
    syntaxColors?: Partial<
      Record<MarkdownTokenClass | EnrichedSyntaxKey, MarkdownColor>
    >;
  };
  link?: {
    color?: MarkdownColor;
    underline?: boolean;
    fontFamily?: string;
    fontWeight?: TextStyle['fontWeight'];
  };
  strong?: {
    color?: MarkdownColor;
    fontFamily?: string;
    fontWeight?: MarkdownFontWeight;
  };
  em?: {
    color?: MarkdownColor;
    fontFamily?: string;
    fontStyle?: 'italic' | 'normal';
  };
  strikethrough?: { color?: MarkdownColor };
  thematicBreak?: {
    color?: MarkdownColor;
    height?: number;
    marginTop?: number;
    marginBottom?: number;
  };
  table?: MarkdownTextStyle & {
    headerFontFamily?: string;
    headerTextColor?: MarkdownColor;
    headerBackgroundColor?: MarkdownColor;
    rowEvenBackgroundColor?: MarkdownColor;
    rowOddBackgroundColor?: MarkdownColor;
    borderColor?: MarkdownColor;
    borderWidth?: number;
    borderRadius?: number;
    cellPaddingHorizontal?: number;
    cellPaddingVertical?: number;
    /** Column width bounds. Downstream only. */
    minColumnWidth?: number;
    maxColumnWidth?: number;
    /** Toolbar button color. Downstream only. */
    toolbar?: { buttonColor?: MarkdownColor };
  };
  /** GitHub alert colors, keyed by alert type. Downstream only. */
  admonition?: Partial<
    Record<'note' | 'tip' | 'important' | 'warning' | 'caution', MarkdownColor>
  >;
  footnote?: { color?: MarkdownColor };
  imageAlt?: { color?: MarkdownColor };
}>;

/**
 * `react-native-enriched-markdown`'s own `syntaxColors` keys that this engine
 * has no class of its own for. They map onto the engine's classes rather than
 * becoming slots, so a value written for enriched keeps working: `punctuation`
 * is `operator`, and `variable` and `embedded` are `plain`, which is enriched's
 * own rule that they inherit the base color.
 */
export type EnrichedSyntaxKey = 'punctuation' | 'variable' | 'embedded';

/** One value, or values merged left to right the way a style array is. */
type Composable<T> = T | ReadonlyArray<T | null | false | undefined>;

/**
 * A `{ theme, markdownStyle }` pair, as `presets` exports them.
 *
 * A preset is a plain frozen value with no behavior of its own: it goes through
 * the same resolver as any app value, so it costs nothing at run time until it
 * is used. A change to one is a visual change and is listed in the changelog.
 */
export type MarkdownPreset = Readonly<{
  theme: MarkdownTheme;
  markdownStyle: MarkdownStyle;
}>;

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
   * code themes, and `theme.syntax` overrides it per token class.
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
  /**
   * Design tokens for the whole document: colors, fonts, radius, and syntax
   * colors. One value, or an array merged left to right.
   *
   * Colors may be `{ light, dark }` pairs, which are drawn as dynamic colors,
   * so switching appearance needs no new theme and no React render. Every color
   * left unset stays the platform's own semantic color, so a view that passes
   * nothing renders exactly as it did before theming existed.
   */
  theme?: Composable<MarkdownTheme>;
  /**
   * Per-element styles, using `react-native-enriched-markdown`'s key and
   * property names. One value, or an array merged left to right.
   *
   * An element style wins over a `theme` token, and a token wins over the
   * default. Keys this renderer does not draw are reported once in development.
   */
  markdownStyle?: Composable<MarkdownStyle>;
  onLinkPress?: (event: MarkdownLinkPressEvent) => void;
  onError?: (error: MarkdownErrorEvent) => void;
}>;

export type MarkdownViewProps = MarkdownViewBaseProps &
  (
    | Readonly<{ markdown: string; stream?: never }>
    | Readonly<{ stream: MarkdownStreamController; markdown?: never }>
  );
