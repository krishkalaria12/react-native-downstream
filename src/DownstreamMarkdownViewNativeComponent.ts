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
   * The source generation the press belongs to, so an event from a document
   * that has since been replaced is dropped rather than delivered to the
   * handlers of the one that replaced it.
   */
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
   * Which control buttons to build, as a bitmask of code copy, code
   * download, table copy, table download, code wrap, code expand, and image
   * download.
   */
  controls?: CodegenTypes.WithDefault<CodegenTypes.Int32, 127>;
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
  mathFlags?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
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
  onLinkPress?: CodegenTypes.DirectEventHandler<NativeLinkPressEvent>;
  onImagePress?: CodegenTypes.DirectEventHandler<NativeImagePressEvent>;
  onMathError?: CodegenTypes.DirectEventHandler<NativeMathErrorEvent>;
  onMarkdownError?: CodegenTypes.DirectEventHandler<NativeMarkdownErrorEvent>;
  onControlAction?: CodegenTypes.DirectEventHandler<NativeControlActionEvent>;
}

// The shadow node is hand-written C++ (src/cpp/fabric), so Codegen emits only the
// props and event emitter.
export default codegenNativeComponent<NativeProps>('DownstreamMarkdownView', {
  interfaceOnly: true,
}) as HostComponent<NativeProps>;
