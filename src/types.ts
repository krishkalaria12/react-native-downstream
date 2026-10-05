/** Public API types. The stream controller types describe the planned session API. */
import type { ComponentType } from 'react';
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
  | 'control_failed'
  | 'renderer_failed';

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
  /** The fence language a `renderer_failed` names; present only for that code. */
  language?: string;
}>;

export type CustomRendererProps = Readonly<{
  /** The fence's text, exactly what the code block's copy button copies. */
  code: string;
  /** The info string's first word as written, cut at ':'. Compare case-insensitively. */
  language: string;
  /** The info string after the language, trimmed. Undefined when there is none. */
  meta?: string;
  /** True until the fence is settled; see the Streaming section for the exact rule. */
  isIncomplete: boolean;
}>;

export type CustomRenderer = Readonly<{
  /** Matched case-insensitively against the normalized fence language. */
  language: string | readonly string[];
  component: ComponentType<CustomRendererProps>;
}>;

export type MarkdownPlugins = Readonly<{
  renderers?: readonly CustomRenderer[];
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
 * A formula the engine could not typeset.
 *
 * Reported once per distinct source per view, and only after the block holding
 * it settles, so a formula that is still arriving does not report a failure for
 * every token it grows through.
 */
export type MarkdownMathErrorEvent = Readonly<{
  /** The TeX between the delimiters, as written. */
  source: string;
  /** The engine's message. Empty when the math package is not installed. */
  message: string;
  /** True for a block or a promoted `$$…$$` paragraph, false for inline math. */
  display: boolean;
  /** Source generation, so an error from a replaced document can be dropped. */
  generation: number;
}>;

/** A press on an image, or a link around one. */
export type MarkdownImageEvent = Readonly<{
  url: string;
  alt: string;
  title?: string;
  /** The `![…](…)` occurrence, so a press can be mapped back to the source. */
  range: MarkdownSourceRange;
}>;

/**
 * How the engine recognizes math.
 *
 * Every form is off unless the view opts into math at all, so a document with no
 * formula parses exactly as it did before math existed.
 */
export type MarkdownMathOptions = Readonly<{
  /**
   * `$…$` as inline math. Off by default, as in Streamdown, so `$5 and $10`
   * stays text.
   */
  singleDollarTextMath?: boolean;
  /**
   * `\(…\)` and `\[…\]`. On by default; Streamdown does not parse them, and they
   * are what OpenAI-family models emit. Pass `false` where `\(…\)` in prose
   * means literal parentheses.
   */
  brackets?: boolean;
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
  /**
   * Close an unfinished inline `$$…$$` span so the text after it shows as math
   * once the prefix lays out. Off by default, and proven by reparsing like every
   * other repair. Streamdown closes it unconditionally; this does not.
   */
  math?: boolean;
  /**
   * Hide a half-arrived allowlisted tag (`<br`, `</su`) at the stream tail.
   * Off by default, as every preview feature.
   */
  htmlTags?: boolean;
}>;

/**
 * Which inline tag syntaxes the parser recognizes beyond the three raw tags.
 *
 * Both forms are off by default, because enabling either makes text that was
 * prose into a styled span.
 */
export type MarkdownSyntaxOptions = Readonly<{
  /**
   * `^x^` is a superscript. Off: `2^10 and 3^5` would pair the carets.
   */
  superscript?: boolean;
  /**
   * `~x~` is a subscript instead of strikethrough. Off, as in Streamdown.
   */
  subscript?: boolean;
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
  /**
   * Math parsing for this stream, read once at creation and kept by `reset()`.
   *
   * Parse options are part of a source's identity, so a stream takes them when
   * it is created rather than per snapshot. The view's `math` prop still decides
   * whether parsed formulas are typeset.
   */
  math?: boolean | MarkdownMathOptions;
  /**
   * Render `<br>`, `<sub>` and `<sup>`. Every other tag stays text. On by
   * default; `false` restores the release that showed every tag as source.
   */
  inlineHtml?: boolean;
  /** md4c's `^x^` and `~x~` syntaxes, both off by default. */
  syntax?: MarkdownSyntaxOptions;
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
export type MarkdownControlKind = 'code' | 'table' | 'image';

/** What a control button did. */
export type MarkdownControlAction = 'copy' | 'download' | 'wrap' | 'expand';

/** What a native control button did, reported to the matching callback. */
export type MarkdownControlEvent = Readonly<{
  block: MarkdownControlKind;
  action: MarkdownControlAction;
  /** Copy and download only. */
  format?: 'text' | 'markdown' | 'csv' | 'tsv';
  /** Wrap only: the block's layout after the press. */
  wrapped?: boolean;
  /** Expand only: whether the block's height cap is lifted after the press. */
  expanded?: boolean;
  /**
   * Fence language as written, in the case the writer used: `ts`, `TS`, and
   * `Ts` all come back as written. It is not a normalized key, so compare
   * case-insensitively.
   */
  language: string;
  /** Saved file name, for downloads. */
  filename?: string;
  /** The image's destination, for an `image` block. */
  url?: string;
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
            /**
             * Per-block expand button in the code header, which lifts
             * `codeBlockMaxHeight` for one block. It appears only on a block
             * taller than the cap, so it is the way out of scrolling inside a
             * scrolling chat.
             */
            expand?:
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
      /**
       * The download button on a loaded image, as Streamdown's
       * `controls.image.download`. There is no copy button: selecting and
       * copying an image's alt text is what a reader means by copying it, and
       * Streamdown has none.
       */
      image?:
        | boolean
        | Readonly<{
            download?:
              | boolean
              | Readonly<{
                  /** Base name without an extension. The extension is guessed. */
                  filename?: string;
                  onDownload?: (event: MarkdownControlEvent) => void;
                  onError?: (error: MarkdownErrorEvent) => void;
                }>;
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
  /** Show all of a code block taller than `codeBlockMaxHeight`. */
  expandCode: string;
  /** Put the code block's height cap back. */
  collapseCode: string;
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
  /** Shown in place of an image whose load failed. */
  imageNotAvailable: string;
  /** The label of a loaded image's download button. */
  downloadImage: string;
  /** The accessible name of an image with no alt text. */
  image: string;
  /** The accessible name of an image the prefix list does not allow. */
  imageBlocked: string;
}>;

/**
 * The tallest a code block's body grows, in points. 400 by default, as in
 * Streamdown.
 *
 * Streamdown's `resolveMaxHeight`, narrowed to numbers. It accepts a CSS
 * length, which has no meaning in a native layout, so every value that is not
 * a non-negative finite number becomes the default instead.
 */
export const defaultCodeBlockMaxHeight = 400;

/** The values that remove the cap rather than setting one. */
const UNCAPPED: ReadonlySet<string> = new Set(['0', 'none', 'Infinity']);

export function resolveMaxHeight(
  value: number | '0' | 'none' | 'Infinity' | undefined
): number {
  if (value === undefined) return defaultCodeBlockMaxHeight;
  if (typeof value === 'number') {
    // `Infinity` is Streamdown's own "no cap", and native reads 0 as no cap.
    if (value === Infinity) return 0;
    // A negative, `NaN`, or fractional value is not a height, and silently
    // clamping it would draw a card the caller never asked for.
    return Number.isFinite(value) && value >= 0
      ? value
      : defaultCodeBlockMaxHeight;
  }
  return UNCAPPED.has(value) ? 0 : defaultCodeBlockMaxHeight;
}

export const defaultTranslations: MarkdownTranslations = {
  copyCode: 'Copy Code',
  downloadFile: 'Download file',
  copied: 'Copied',
  wrapCode: 'Wrap lines',
  unwrapCode: "Don't wrap lines",
  expandCode: 'Show all lines',
  collapseCode: 'Show fewer lines',
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
  imageNotAvailable: 'Image not available',
  downloadImage: 'Download image',
  image: 'Image',
  imageBlocked: 'Image blocked',
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
  superscript?: { fontScale?: number; baselineOffsetScale?: number };
  subscript?: { fontScale?: number; baselineOffsetScale?: number };
  /** A display formula's own box. enriched-markdown's `latex` keys. */
  math?: {
    fontSize?: number;
    color?: MarkdownColor;
    backgroundColor?: MarkdownColor;
    padding?: number;
    marginTop?: number;
    marginBottom?: number;
    textAlign?: 'left' | 'center' | 'right';
    /** A formula that could not be typeset, drawn as its source. Downstream only. */
    errorColor?: MarkdownColor;
  };
  /** A formula in the middle of a sentence. enriched-markdown's `inlineLatex`. */
  inlineMath?: { color?: MarkdownColor };
  /** A loaded image's own box. */
  image?: {
    maxHeight?: number;
    borderRadius?: number;
    marginTop?: number;
    marginBottom?: number;
    /** The pulsing box drawn before a size is known. Downstream only. */
    placeholderColor?: MarkdownColor;
  };
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

/**
 * Streamdown's `animated` options, so a value written for its web component is
 * reused as it is.
 *
 * Every field has a default, and a value the platforms cannot draw falls back to
 * it with one warning rather than being rejected: an app that asks for a blur
 * should still see the fade rather than nothing.
 */
export type MarkdownAnimateOptions = Readonly<{
  /** What a newly revealed unit does as it appears. `fadeIn` by default. */
  animation?: 'fadeIn' | 'slideUp' | 'blurIn' | (string & {});
  /** How long one unit takes to appear, in ms. 150 by default. */
  duration?: number;
  /**
   * A CSS timing keyword or `cubic-bezier()`. `ease` by default.
   *
   * `steps()` and `linear()` with stops draw as `ease`: a native draw pass can
   * only interpolate continuously, and neither has a meaning to it.
   */
  easing?: string;
  /** Whether a new word or a new character is one unit. `word` by default. */
  sep?: 'word' | 'char';
  /** The delay between units, in ms. 40 by default; 0 turns the cascade off. */
  stagger?: number;
  /**
   * How far ahead of now a burst of new text may be scheduled, in ms. 320 by
   * default.
   *
   * A fast stream would otherwise queue seconds of invisible words; this
   * compresses the cascade into the budget instead. A larger value favours a
   * longer, stricter cascade, a smaller one keeps a fast stream caught up.
   */
  maxBacklogMs?: number;
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
   * Line numbers beside fenced and indented code. On by default, as in
   * Streamdown.
   *
   * A fence's `noLineNumbers` hides them for one block; `false` hides them for
   * all of them. Numbers are drawn beside the code and are never copied,
   * selected, or read aloud.
   */
  lineNumbers?: boolean;
  /**
   * The tallest a code block's body grows, in points, before it scrolls inside
   * its card. 400 by default, as in Streamdown.
   *
   * `0`, `'0'`, `'none'`, `Infinity`, and `'Infinity'` remove the cap. A
   * capped block that is still streaming follows its newest line until the
   * reader scrolls away. An app that prefers full-height code passes `0`.
   */
  codeBlockMaxHeight?: number | '0' | 'none' | 'Infinity';
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
  /**
   * Fence-language renderers. A fence whose normalized language matches one
   * mounts its component where the code card would be; any other fence, and
   * every case where the component is not up, keeps the native code card.
   */
  plugins?: MarkdownPlugins;
  /**
   * Typeset math. On when `react-native-downstream-math` is installed and off
   * otherwise; `false` keeps every formula literal, which is exactly today's
   * parse and display.
   *
   * A `stream` takes its parse options from `createMarkdownStream`, and this
   * prop then only decides whether parsed formulas are typeset. Unsupported
   * syntax, and every formula when the math package is absent, shows its source
   * with its delimiters, as KaTeX's `throwOnError: false` does.
   */
  math?: boolean | MarkdownMathOptions;
  /** A formula that could not be typeset, once per distinct source per view. */
  onMathError?: (event: MarkdownMathErrorEvent) => void;
  /**
   * Render `<br>`, `<sub>` and `<sup>`. Every other tag stays text. On by
   * default; `false` restores the release that showed every tag as source.
   */
  inlineHtml?: boolean;
  /** md4c's `^x^` and `~x~` syntaxes, both off by default. */
  syntax?: MarkdownSyntaxOptions;
  /**
   * Load images. On by default, as in Streamdown.
   *
   * `false` shows alt text and makes no request. Images are HTTPS only, at most
   * four requests at once, and cancelled when no view needs them.
   */
  images?: boolean;
  /**
   * rehype-harden's option: the URL prefixes an image may load from. `['*']` by
   * default, which allows any HTTPS host.
   *
   * Every image URL comes from model output, and a model that reads untrusted
   * text can be made to fetch any URL, so an app whose models read web pages,
   * tool output or email should pass its own CDN prefixes here.
   */
  allowedImagePrefixes?: readonly string[];
  /** A press on a loaded image that is not inside a link. */
  onImagePress?: (event: MarkdownImageEvent) => void;
  /**
   * Fade newly arriving text in, word by word, while the view is animating. Off
   * by default, as in Streamdown.
   *
   * It changes drawing only. The text is in the layout from the first frame, so
   * nothing reflows, and a copy, a selection, an export, and a screen reader all
   * see the whole message. Text that has not appeared yet stays in the
   * accessibility tree, at most one backlog — about half a second — ahead of what
   * is drawn.
   *
   * Under Reduce Motion, and where Android's animator scale is 0, the text
   * appears at once instead.
   */
  animated?: boolean | MarkdownAnimateOptions;
  /**
   * Whether text is still arriving.
   *
   * A controlled view has to say so, as in Streamdown. A stream view follows its
   * own controller instead: on from the first append of a generation until
   * `finish()`, with an explicit value overriding that.
   *
   * This decides the fade and the caret and nothing else. A complete document
   * rendered with `animated` on is what `isAnimating: false` already describes,
   * so it appears at once rather than animating text the reader was handed whole.
   */
  isAnimating?: boolean;
  /**
   * A caret after the newest text, drawn while animating. None by default.
   *
   * It takes the color of the last character, sits after the last block of prose
   * or inside a fence that is still open, and takes a line of its own after a
   * table, a closed fence, or a rule, which is where the next block will start.
   * It is never copied, selected, exported, or read aloud.
   */
  caret?: 'block' | 'circle';
  /** `isAnimating` turned on, including on mount. */
  onAnimationStart?: () => void;
  /**
   * `isAnimating` turned off.
   *
   * As in Streamdown this fires when the flag turns off rather than when the last
   * fade finishes, which can be up to about half a second later.
   */
  onAnimationEnd?: () => void;
  onLinkPress?: (event: MarkdownLinkPressEvent) => void;
  onError?: (error: MarkdownErrorEvent) => void;
}>;

export type MarkdownViewProps = MarkdownViewBaseProps &
  (
    | Readonly<{ markdown: string; stream?: never }>
    | Readonly<{ stream: MarkdownStreamController; markdown?: never }>
  );
