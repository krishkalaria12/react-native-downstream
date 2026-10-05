import {
  codegenNativeComponent,
  type CodegenTypes,
  type HostComponent,
  type ViewProps,
} from 'react-native';

/**
 * One fenced block's React child, hosted by the Markdown view's shadow node.
 *
 * Not exported from the package: an app reaches a renderer by passing a
 * component to `plugins.renderers`, and this view exists so that component has
 * somewhere to live — a real Fabric child the shadow node can measure and place
 * — and so that nothing an app renders itself can be mistaken for one.
 *
 * `ordinal` is the code block's ordinal among the document's code blocks, the
 * same number the code card's wrap and expand choices are keyed by, so the two
 * always name the same block however the fences around them change.
 */
interface NativeProps extends ViewProps {
  ordinal: CodegenTypes.Int32;
}

export default codegenNativeComponent<NativeProps>(
  'DownstreamFenceSlot'
) as HostComponent<NativeProps>;
