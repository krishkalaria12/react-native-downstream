/** Public API types. The stream controller types describe the planned session API. */
import type { ComponentType, ReactNode } from 'react';

import type {
  ColorValue,
  ImageSourcePropType,
  StyleProp,
  TextStyle,
  ViewStyle,
} from 'react-native';

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

/**
 * Props for a component drawn in place of an allowed block tag.
 *
 * The tag name is a prop so one component can serve several tags.
 */
export type TagRendererProps = Readonly<{
  name: string;
  attributes: ReadonlyArray<readonly [string, string]>;
  markdown: string;
  isIncomplete: boolean;
}>;

export type TagRenderer = Readonly<{
  name: string | readonly string[];
  component: ComponentType<TagRendererProps>;
}>;

export type FallbackRendererProps = CustomRendererProps | TagRendererProps;

export type MarkdownPlugins = Readonly<{
  renderers?: readonly CustomRenderer[];
  tags?: readonly TagRenderer[];
  fallbackComponent?: ComponentType<FallbackRendererProps>;
}>;

/** Thrown by rejected synchronous stream operations. */
export interface MarkdownOperationError extends Error {
  readonly code: MarkdownErrorCode;
}

export type MarkdownLinkPressEvent = Readonly<{
  /**
   * The destination to open, resolved against `links.defaultOrigin` when the
   * document held a relative one.
   *
   * An app with its own router wants this, because it is the only one of the
   * two that is a real URL.
   */
  url: string;
  /**
   * The destination as written, never rewritten. This is what maps a press back
   * to the message's own text, and for a relative link it is the only form that
   * says what the writer wrote.
   */
  href: string;
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
 * aimed at.
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
  /**
   * `==x==` is highlighted text. Off: a version number in `==` is not a
   * highlighter, and Streamdown has no such syntax.
   */
  highlight?: boolean;
  /**
   * `__x__` is underlined text. Off — and it takes `_` away from emphasis, so
   * turning it on changes what `_x_` means everywhere.
   */
  underline?: boolean;
  /**
   * `||x||` is hidden until tapped. Off: hiding text behind a tap is an app's
   * decision about its own messages.
   */
  spoiler?: boolean;
}>;

/**
 * A protocol a bare address may not autolink to, as Streamdown's
 * `disableAutolinkProtocols` names it.
 *
 * Only the three the engine can autolink: `http`, `https` and `mailto`. A
 * `www.` autolink is judged by `http`, because that is the scheme it becomes.
 *
 * Names are compared trimmed and case-insensitively, and the colon is optional,
 * so `'mailto'`, `'MailTo'` and `'mailto:'` are one entry. It affects a bare
 * address only: `[text](mailto:…)` is a link a writer typed the brackets for and
 * stays one.
 */
export type MarkdownAutolinkProtocol =
  | 'http'
  | 'https'
  | 'mailto'
  | (string & {});

/** The protocols autolinking is off for. An empty or absent list is all of them. */
export type MarkdownAutolinkProtocols = readonly MarkdownAutolinkProtocol[];

/**
 * Which link destinations a document may contain.
 *
 * Off by default, because a relative reference names no target until something
 * resolves it, and Streamdown shows such a link as plain text until it is given
 * a `defaultOrigin` to resolve against.
 */
export type MarkdownLinksOptions = Readonly<{
  /**
   * `/…`, `./…`, `../…`, `?…` and `#…` are links. A bare `docs/a` is not one of
   * them even when this is on, because without a base there is nothing to say
   * which document it means; Streamdown shows that one as text too.
   */
  relative?: boolean;
  /**
   * The origin a relative link resolves against, as rehype-harden's
   * `defaultOrigin`. Supplying one turns relative links on, because an origin
   * with nothing to resolve is not a configuration.
   *
   * It does not rewrite the source: `onLinkPress` reports the `href` exactly as
   * written and a resolved `url` beside it, so an app that maps a press back to
   * its own state sees the author's text.
   */
  defaultOrigin?: string;
}>;

/**
 * A tag the source may use, beyond the built-in table, with the attributes it
 * may carry. Streamdown's shape, so a value written for its `allowedTags` works
 * as it is.
 *
 * `true` allows the tag with no attributes, and an array allows those
 * attributes. A tag named here is a real HTML element name only by accident:
 * `Array<string>` is a generic, and a name that is not an HTML element stays
 * literal whichever way it is written.
 */
export type MarkdownAllowedTags = Readonly<
  Record<string, boolean | readonly string[]>
>;

/**
 * A press on a tag the app styled or handles, as `onTagPress` reports it.
 *
 * The name, the attributes the policy allowed, the tag's own visible text, and
 * where it was written — the four things a press needs and none of the parse.
 */
export type MarkdownTagEvent = Readonly<{
  tag: string;
  attributes: Readonly<Record<string, string>>;
  text: string;
  range: MarkdownSourceRange;
}>;

/** A table's full-screen button. */
export type MarkdownTableFullscreenEvent = Readonly<{
  /**
   * The table's own Markdown, as the engine's export wrote it. A modal or a
   * route renders this and gets the same table, rather than one rebuilt from the
   * model.
   */
  markdown: string;
  /** The table's ordinal among the document's tables, for an app that keys on it. */
  ordinal: number;
}>;

/**
 * What a reader is asked before an external link opens.
 *
 * On by default, as in Streamdown. A view with an `onLinkPress` is unaffected by
 * all of it: the app decides what a press means and this never runs.
 */
export type MarkdownLinkSafetyOptions = Readonly<{
  /**
   * Intercept a press and confirm it before anything opens. On by default. With
   * it off the view still opens the link itself when the app passed no
   * `onLinkPress`, just without asking.
   */
  enabled?: boolean;
  /**
   * Decides whether a URL opens without asking. `true` opens it, `false` shows
   * the confirmation, and a promise is awaited first, so an app can check a list
   * against the network.
   */
  onLinkCheck?: (url: string) => boolean | Promise<boolean>;
  /**
   * Replaces the confirmation. It receives the URL, the strings from
   * `translations`, and an `onConfirm`/`onCancel` pair; it is responsible for
   * closing itself, which is what lets an app use a bottom sheet and a native
   * alert the same way.
   */
  renderModal?: (props: MarkdownLinkSafetyModalProps) => ReactNode;
}>;

/** What `linkSafety.renderModal` is given. */
export type MarkdownLinkSafetyModalProps = Readonly<{
  url: string;
  /** `externalLinkWarning`, as a translation key's value. */
  title: string;
  /** `openLink`. */
  openLabel: string;
  /** `copyLink`. */
  copyLabel: string;
  /** `close`. */
  cancelLabel: string;
  onConfirm: () => void;
  onCancel: () => void;
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
  /**
   * CJK-friendly emphasis, strikethrough and autolink boundaries. On by default;
   * `false` restores CommonMark's intraword rules, where `**太字**` next to a
   * punctuation mark and `~~` next to any letter both stay literal.
   */
  cjk?: boolean;
  /** Which link destinations the source may contain; see `MarkdownLinksOptions`. */
  links?: MarkdownLinksOptions;
  /** Protocols a bare address may not autolink to. */
  disableAutolinkProtocols?: MarkdownAutolinkProtocols;
  /**
   * Which HTML tags the source may use, beyond the built-in table; see
   * `MarkdownAllowedTags`.
   */
  allowedTags?: MarkdownAllowedTags;
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
export type MarkdownControlKind = 'code' | 'table' | 'image' | 'details';

/** What a control button did. */
export type MarkdownControlAction =
  | 'copy'
  | 'download'
  | 'wrap'
  | 'expand'
  | 'fullscreen';

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
   * The table's own Markdown, for `fullscreen` only.
   *
   * It is the source the table was written as, taken from the engine's export
   * rather than rebuilt, so a full-screen table is the same table. Empty for
   * every other action.
   */
  markdown?: string;
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
            /**
             * Per-table expand button, which lifts `tableMaxHeight` for one
             * table. It appears only on a table taller than the cap.
             */
            expand?:
              | boolean
              | Readonly<{
                  onToggle?: (event: MarkdownControlEvent) => void;
                }>;
            /**
             * Per-table full-screen button, which opens the table's own Markdown
             * in a modal holding a second view. Streamdown's key.
             */
            fullscreen?:
              | boolean
              | Readonly<{
                  onToggle?: (event: MarkdownControlEvent) => void;
                }>;
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
      /**
       * The toggle on a `<details>` summary, which shows and hides the body.
       *
       * Its own section because a collapsible block is neither a code block nor
       * a table: an app that turned both of those off for a quiet document keeps
       * the ability to open a body its writers used.
       */
      details?:
        | boolean
        | Readonly<{
            expand?:
              | boolean
              | Readonly<{
                  /**
                   * Called after the body is shown or hidden. `expanded` on the
                   * event says which of the two happened.
                   */
                  onToggle?: (event: MarkdownControlEvent) => void;
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
  /** Title of the confirmation shown before an external link opens. */
  externalLinkWarning: string;
  /** The confirmation's primary action: open the link. */
  openLink: string;
  /** The confirmation's copy action. */
  copyLink: string;
  /** Dismiss, and the modal's close button's accessible name. */
  close: string;
  /** Open a table in its own full-screen view. */
  viewFullscreen: string;
  /** Leave the full-screen table view. */
  exitFullscreen: string;
  /** Show all of a table taller than `tableMaxHeight`. */
  expandTable: string;
  /** Put the table's height cap back. */
  collapseTable: string;
  /** Open a collapsed `<details>` body. */
  expandDetails: string;
  /** Collapse an open `<details>` body. */
  collapseDetails: string;
}>;

/**
 * The tallest a code block's body grows, in points. 400 by default, as in
 * Streamdown.
 */
export const defaultCodeBlockMaxHeight = 400;

/**
 * The tallest a table grows before it scrolls inside its own frame. 300 by
 * default, as in Streamdown.
 *
 * The same rule as a code block's cap and for the same reason: a table is the
 * one block kind with no way to leave the document, so without a cap a
 * thousand-row table inside a scrolling chat has no way back. `0` removes the
 * cap, which is the choice an app that renders its own tables full-height
 * makes.
 */
export const defaultTableMaxHeight = 300;

/** The values that remove a cap rather than setting one. */
const UNCAPPED: ReadonlySet<string> = new Set(['0', 'none', 'Infinity']);

/**
 * Streamdown's `resolveMaxHeight`, narrowed to numbers, with the default as a
 * parameter.
 *
 * It accepts a CSS length, which has no meaning in a native layout, so every
 * value that is not a non-negative finite number becomes the default instead.
 * One function for both caps rather than two: the rules are the same four
 * branches, and a copy is a rule that can drift from the one it copied.
 */
export function resolveMaxHeight(
  value: number | '0' | 'none' | 'Infinity' | undefined,
  fallback: number = defaultCodeBlockMaxHeight
): number {
  if (value === undefined) return fallback;
  if (typeof value === 'number') {
    // `Infinity` is Streamdown's own "no cap", and native reads 0 as no cap.
    if (value === Infinity) return 0;
    // A negative, `NaN`, or fractional value is not a height, and silently
    // clamping it would draw a card the caller never asked for.
    return Number.isFinite(value) && value >= 0 ? value : fallback;
  }
  return UNCAPPED.has(value) ? 0 : fallback;
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
  externalLinkWarning: 'Open this link?',
  openLink: 'Open',
  copyLink: 'Copy link',
  close: 'Close',
  viewFullscreen: 'View fullscreen',
  exitFullscreen: 'Exit fullscreen',
  expandTable: 'Show all rows',
  collapseTable: 'Show fewer rows',
  expandDetails: 'Show details',
  collapseDetails: 'Hide details',
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

/**
 * A control button's glyph. A platform name draws the platform's own glyph — an
 * SF Symbol on iOS, a named drawable on Android — and a source is fetched or
 * bundled as an image.
 *
 * A URL is deliberately not accepted: `DSImageLoader` only loads HTTPS, and a
 * control glyph comes from the app's own assets rather than from a message.
 */
export type MarkdownIconSource =
  | Readonly<{ name: string }>
  | Readonly<{ image: ImageSourcePropType }>;

/**
 * Every button a control can draw, by the key Streamdown's `icons` uses where
 * one exists.
 *
 * A key here is the same word as the button: `copy` is the copy button on a code
 * block, a table and an image. The `tableFullscreen` and `detailsChevron` keys
 * are Downstream's, because Streamdown has no full-screen table and its details
 * chevron is part of the browser's own widget.
 */
export type MarkdownControlIcon =
  | 'copy'
  | 'check'
  | 'download'
  | 'maximize'
  | 'close'
  | 'wrap'
  | 'expand'
  | 'collapse'
  | 'tableFullscreen'
  | 'detailsChevron';

/**
 * Every icon key, in the order the native views read them.
 *
 * Appended at the end and never reordered: the array travels positionally beside
 * `controlLabels`, so a key added in the middle would give every later button a
 * different glyph on a release built against the older list.
 */
export const CONTROL_ICON_KEYS = [
  'copy',
  'check',
  'download',
  'maximize',
  'close',
  'wrap',
  'expand',
  'collapse',
  'tableFullscreen',
  'detailsChevron',
] as const;

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
   * CJK-friendly emphasis, strikethrough and autolink boundaries. On by
   * default; `false` restores CommonMark's intraword rules, which leave
   * `「**強調**」` and `~~削除~~` literal.
   *
   * A document with no CJK in it parses identically either way, which
   * `tests/parse-options.test.ts` asserts against the real engine rather than
   * taking on trust.
   */
  cjk?: boolean;
  /** Which link destinations the source may contain; see `MarkdownLinksOptions`. */
  links?: MarkdownLinksOptions;
  /**
   * Protocols a bare address may not autolink to, as Streamdown's
   * `disableAutolinkProtocols`. Absent means all of them are.
   */
  disableAutolinkProtocols?: MarkdownAutolinkProtocols;
  /**
   * Which HTML tags the source may use beyond the built-in table. A named tag
   * that spans lines is a container, drawn by `plugins.tags` or
   * `fallbackComponent`; see `MarkdownAllowedTags`.
   */
  allowedTags?: MarkdownAllowedTags;
  /**
   * Show a tag's source between its open and close instead of parsing what is
   * inside it. Off by default, and what an app whose custom tags are not
   * Markdown wants: `<user>Bob</user>` is a name, not a paragraph.
   */
  literalTagContent?: boolean;
  /**
   * Rewrite the indentation of a document that starts with an HTML block,
   * moving it after the tag instead of dropping it.
   *
   * The rewrite keeps the source the same length, so every source offset is
   * unchanged, and it only runs when the document starts with a tag, which is
   * Streamdown's own rule.
   */
  normalizeHtmlIndentation?: boolean;
  /** The tallest a table grows before it scrolls, in points. 300 by default. */
  tableMaxHeight?: number | '0' | 'none' | 'Infinity';
  /**
   * Which way the document lays out, as the HTML `dir` attribute.
   *
   * `auto` is the default and is the engine's own decision, block by block: a
   * block reads the way the majority of its strong characters read, and one with
   * no strong character of its own inherits the block before it. The other two
   * override that for every block, which is what an app whose whole document is
   * one direction should reach for rather than being right about each line.
   *
   * The decision is made in the engine rather than on either platform, because
   * "the first strong letter" is not the same rule anywhere and a paragraph that
   * opened with inline code and then Arabic came out two different ways.
   */
  dir?: 'ltr' | 'rtl' | 'auto';

  /**
   * What a reader is asked before a link opens, when the app passed no
   * `onLinkPress`. On by default.
   *
   * Without an `onLinkPress` a press does nothing at all, which no reader
   * expects from something drawn as a link; this is what makes it open instead,
   * after asking.
   */
  linkSafety?: boolean | MarkdownLinkSafetyOptions;
  /**
   * A press on a tag `allowedTags` allows and `markdownStyle.tags` or
   * `onTagPress` cares about.
   */
  onTagPress?: (event: MarkdownTagEvent) => void;
  /**
   * Replaces the full-screen table modal.
   *
   * It receives the table's own Markdown and nothing else about the view: an app
   * that pushes a route needs the text, not this library's modal.
   */
  renderTableFullscreen?: (event: MarkdownTableFullscreenEvent) => ReactNode;
  /** A table's full-screen button, for an app with its own handler. */
  onTableFullscreen?: (event: MarkdownTableFullscreenEvent) => void;
  /**
   * Glyphs for the control buttons, by key, as Streamdown's `icons`. A key here
   * replaces the platform's own glyph for that button; a key with no entry keeps
   * it.
   */
  icons?: Partial<Record<MarkdownControlIcon, MarkdownIconSource>>;

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
