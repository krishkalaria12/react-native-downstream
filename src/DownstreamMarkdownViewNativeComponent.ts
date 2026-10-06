import {
  codegenNativeComponent,
  type CodegenTypes,
  type HostComponent,
  type ViewProps,
} from 'react-native';

type NativeLinkPressEvent = Readonly<{
  url: string;
  start: CodegenTypes.Int32;
  end: CodegenTypes.Int32;
}>;

type NativeLinkLongPressEvent = Readonly<{
  url: string;
  start: CodegenTypes.Int32;
  end: CodegenTypes.Int32;
}>;

type NativeMarkdownErrorEvent = Readonly<{
  code: string;
  message: string;
  generation: CodegenTypes.Int32;
  sequence: CodegenTypes.Int32;
  recoverable: boolean;
  /**
   * The control a `control_failed` names, `code` or `table`. Empty for every
   * other code. Carried as fields rather than parsed out of `message`, so
   * routing a failure does not depend on how it is worded and an app learns
   * which control failed.
   */
  block: string;
  /** `copy`, `download`, or `wrap` for a `control_failed`; empty otherwise. */
  action: string;
}>;

type NativeImagePressEvent = Readonly<{
  url: string;
  alt: string;
  title: string;
  start: CodegenTypes.Int32;
  end: CodegenTypes.Int32;
}>;

type NativeMathErrorEvent = Readonly<{
  /** The TeX between the delimiters, as written. */
  source: string;
  message: string;
  display: boolean;
  generation: CodegenTypes.Int32;
}>;

type NativeControlActionEvent = Readonly<{
  block: string;
  action: string;
  format: string;
  language: string;
  filename: string;
  /** The image's destination, for an `image` block. Empty otherwise. */
  url: string;
  wrapped: boolean;
  /** Expand only: whether the block's height cap is lifted after the press. */
  expanded: boolean;
  /**
   * Full screen only: the table's own Markdown, as `Export.cpp` writes it, so a
   * modal holding a second view renders the table rather than a rebuild of it.
   * Empty for every other action.
   */
  markdown: string;
  /**
   * The block's ordinal among the document's blocks of its kind, for a press an
   * app keys on. Code, table and image ordinals are three different counts, so
   * this is the segment's own ordinal rather than a block index.
   */
  ordinal: CodegenTypes.Int32;
  /**
   * The source generation the press belongs to, so an event from a document
   * that has since been replaced is dropped rather than delivered to the
   * handlers of the one that replaced it.
   */
  generation: CodegenTypes.Int32;
}>;

/**
 * A press on a task list item's checkbox.
 *
 * `containerId` is the item's own container id, and `checked` is the state the
 * press leaves it in. The document does not change: the app owns the source,
 * which is the only thing that can flip the marker.
 */
export type NativeTaskPress = Readonly<{
  containerId: CodegenTypes.Int32;
  checked: boolean;
  generation: CodegenTypes.Int32;
}>;

export interface NativeProps extends ViewProps {
  /** Complete canonical source, already validated by DownstreamMarkdown. */
  markdown: string;
  /** Source generation reported back with errors, so stale ones are dropped. */
  generation?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /** A session ID replaces markdown as the view's input. */
  sessionId?: string;
  sessionSequence?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /** Changes even when finish produces a second snapshot at the same sequence. */
  sessionRevision?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  selectable?: CodegenTypes.WithDefault<boolean, true>;
  /**
   * Colors fenced code blocks by language. Tokens are always computed by the
   * engine, so this only chooses whether the platform applies them.
   */
  syntaxHighlighting?: CodegenTypes.WithDefault<boolean, true>;
  /**
   * Lay long code lines out at the card's width instead of at their own
   * length, so they wrap rather than scroll sideways. Layout only, so copied
   * text is the same either way.
   */
  codeBlockWrap?: CodegenTypes.WithDefault<boolean, false>;
  /**
   * Which control buttons to build, as a bitmask of code copy, code download,
   * table copy, table download, code wrap, code expand, image download, table
   * expand, and table fullscreen.
   */
  controls?: CodegenTypes.WithDefault<CodegenTypes.Int32, 1023>;
  /**
   * Draw line numbers beside code. Layout only, and they are never copied,
   * selected, or spoken.
   */
  lineNumbers?: CodegenTypes.WithDefault<boolean, true>;
  /**
   * The tallest a code block's body grows, in points, before it scrolls inside
   * its card. 0 means no cap.
   */
  codeBlockMaxHeight?: CodegenTypes.WithDefault<CodegenTypes.Double, 400>;
  /**
   * The tallest a table grows, in points, before it scrolls inside its own
   * frame. 0 means no cap.
   *
   * The same rule as a code block's cap, and for the same reason: a table is
   * the one block kind with no way to leave the document without a scroll of its
   * own.
   */
  tableMaxHeight?: CodegenTypes.WithDefault<CodegenTypes.Double, 300>;
  /**
   * `ltr`, `rtl` or `auto`, as the HTML attribute. `auto` is the default and is
   * the engine's own decision per block; the other two override it, because an app
   * that knows its whole document is one direction should not have to be right
   * about any individual line in it.
   */
  dir?: CodegenTypes.WithDefault<'ltr' | 'rtl' | 'auto', 'auto'>;
  /**
   * Whether a hold on a link may show the system's preview of its destination.
   *
   * iOS only: a text view shows a peek-and-pop preview on a hold, and a chat
   * message that previews every link it holds is a message that navigates by
   * accident. `false` — the default — turns it off, so a hold is the app's
   * `onLinkLongPress` and nothing else.
   */
  linkPreview?: CodegenTypes.WithDefault<boolean, false>;
  /**
   * The origin a relative link resolves against, or empty for the default.
   *
   * One string rather than a resolved URL: the engine keeps the source's own
   * `href` on the press event, so resolution happens where the press lands and
   * a changed origin changes what a press opens without a reparse.
   */
  linkOrigin?: string;
  /**
   * Which HTML tags the source may use beyond the built-in table, as
   * `-1,name,attribute,-1,name,…` in name order; see `src/allowedTags.ts`.
   *
   * A flat list of strings because a codegen object prop would need a struct
   * field per attribute, and this compares as a vector for free. The `-1`
   * sentinel opens each tag because the reader is a loop that has to know where
   * one tag's attributes stop.
   */
  allowedTags?: ReadonlyArray<string>;
  /** Base name for a code download, without an extension. */
  codeFilename?: string;
  /** Base name for a table download, without an extension. */
  tableFilename?: string;
  /** Base name for an image download, without an extension. */
  imageFilename?: string;
  /**
   * Math parse options as a bitmask: enabled, single-dollar text math, and
   * bracket delimiters. Sent rather than resolved here because the parser is
   * the first thing that reads them, and a formula that is never recognized can
   * never be laid out.
   */
  parseFlags?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  /**
   * Load images. Off shows alt text and makes no request, which is also what a
   * document with no image does whatever this says.
   */
  images?: CodegenTypes.WithDefault<boolean, true>;
  /**
   * The URL prefixes an image may load from; `['*']` allows any. Part of the
   * parse's inputs but not of the document, so the same string parses the same
   * under two prefix lists and only the box rule differs.
   */
  allowedImagePrefixes?: ReadonlyArray<string>;
  /** `,`, `;`, a tab, or `auto`, which the platform resolves from its locale. */
  csvSeparator?: string;
  /** Control labels in a fixed key order; see src/controls.ts. */
  controlLabels?: ReadonlyArray<string>;
  /**
   * The control glyphs, as `src/icons.ts` resolves them: a name per button in a
   * fixed order, which the platform maps to its own symbol or asset.
   */
  // A nullable element is not something codegen can express inside an array, so
  // a removed glyph crosses as an empty string and the button reads it as "draw no
  // icon". `src/icons.ts` is where that mapping happens.
  controlIcons?: ReadonlyArray<string>;
  /**
   * Slot ids from src/themeSlots.ts, sorted ascending, one entry per override.
   *
   * Three arrays rather than a codegen object prop: a struct of every field
   * would force the full normalization that loses platform default colors, grow
   * a field per new slot, and give the shadow node nothing cheaper to compare
   * than the struct itself. Three arrays compare as vectors and cost nothing
   * when empty, which is what makes "no theme" structural rather than a path.
   */
  themeSlots?: ReadonlyArray<CodegenTypes.Int32>;
  /**
   * One value per slot: a number, an ARGB color, or an index into themeFonts.
   * A color that differs between appearances takes two slots, light then dark.
   */
  themeValues?: ReadonlyArray<CodegenTypes.Double>;
  /**
   * Font family names referenced by font slots. `Double` rather than `Float` for
   * the values, because a Float holds 24 bits of mantissa and cannot carry a
   * 32-bit ARGB value exactly.
   */
  themeFonts?: ReadonlyArray<string>;
  /**
   * What a newly revealed unit does as it appears. `none` is the default, which
   * is a draw pass that costs one nil check per glyph range.
   */
  animation?: CodegenTypes.WithDefault<'none' | 'fadeIn' | 'slideUp', 'none'>;
  /** Whether a new word or a new character is one reveal unit. */
  animationSplit?: CodegenTypes.WithDefault<'word' | 'char', 'word'>;
  /**
   * `[duration, stagger, maxBacklogMs, x1, y1, x2, y2]`, all in the same units
   * `src/animation.ts` resolves.
   *
   * One array rather than a struct of fields: it compares as a vector, so an
   * unchanged configuration sends nothing, which is the same reason the theme
   * travels as three parallel arrays. It is empty when there is no animation.
   */
  animationTiming?: ReadonlyArray<CodegenTypes.Double>;
  /**
   * Whether text is still arriving. `auto` follows the stream's own flag, which
   * is what a stream view sends; a controlled view sends `on` or `off`.
   */
  animating?: CodegenTypes.WithDefault<'auto' | 'on' | 'off', 'off'>;
  /** The shape drawn after the newest text while animating. */
  caret?: CodegenTypes.WithDefault<'none' | 'block' | 'circle', 'none'>;
  onLinkPress?: CodegenTypes.DirectEventHandler<NativeLinkPressEvent>;
  onLinkLongPress?: CodegenTypes.DirectEventHandler<NativeLinkLongPressEvent>;
  onImagePress?: CodegenTypes.DirectEventHandler<NativeImagePressEvent>;
  onMathError?: CodegenTypes.DirectEventHandler<NativeMathErrorEvent>;
  onMarkdownError?: CodegenTypes.DirectEventHandler<NativeMarkdownErrorEvent>;
  onControlAction?: CodegenTypes.DirectEventHandler<NativeControlActionEvent>;
  onTaskPress?: CodegenTypes.DirectEventHandler<NativeTaskPress>;
}

// The shadow node is hand-written C++ (src/cpp/fabric), so Codegen emits only the
// props and event emitter.
export default codegenNativeComponent<NativeProps>('DownstreamMarkdownView', {
  interfaceOnly: true,
}) as HostComponent<NativeProps>;
