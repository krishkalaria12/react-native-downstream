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

export interface NativeProps extends ViewProps {
  /** Complete canonical source, already validated by DownstreamMarkdown. */
  markdown: string;
  /** Source generation reported back with errors, so stale ones are dropped. */
  generation?: CodegenTypes.WithDefault<CodegenTypes.Int32, 0>;
  onLinkPress?: CodegenTypes.DirectEventHandler<NativeLinkPressEvent>;
  onMarkdownError?: CodegenTypes.DirectEventHandler<NativeMarkdownErrorEvent>;
}

// The shadow node is hand-written C++ (cpp/fabric), so Codegen emits only the
// props and event emitter.
export default codegenNativeComponent<NativeProps>('DownstreamMarkdownView', {
  interfaceOnly: true,
}) as HostComponent<NativeProps>;
