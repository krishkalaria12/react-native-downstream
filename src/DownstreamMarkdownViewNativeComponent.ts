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
}>;

type NativeControlActionEvent = Readonly<{
  block: string;
  action: string;
  format: string;
  language: string;
  filename: string;
  wrapped: boolean;
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
   * download, table copy, table download, and code wrap.
   */
  controls?: CodegenTypes.WithDefault<CodegenTypes.Int32, 31>;
  /** Base name for a code download, without an extension. */
  codeFilename?: string;
  /** Base name for a table download, without an extension. */
  tableFilename?: string;
  /** `,`, `;`, a tab, or `auto`, which the platform resolves from its locale. */
  csvSeparator?: string;
  /** Control labels in a fixed key order; see src/controls.ts. */
  controlLabels?: ReadonlyArray<string>;
  onLinkPress?: CodegenTypes.DirectEventHandler<NativeLinkPressEvent>;
  onMarkdownError?: CodegenTypes.DirectEventHandler<NativeMarkdownErrorEvent>;
  onControlAction?: CodegenTypes.DirectEventHandler<NativeControlActionEvent>;
}

// The shadow node is hand-written C++ (src/cpp/fabric), so Codegen emits only the
// props and event emitter.
export default codegenNativeComponent<NativeProps>('DownstreamMarkdownView', {
  interfaceOnly: true,
}) as HostComponent<NativeProps>;
