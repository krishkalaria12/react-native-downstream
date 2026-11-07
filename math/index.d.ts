import type {
  DownstreamPlugin,
  MarkdownMathOptions,
} from '@react-native-downstream/core';

/** Enable the installed native math engine for a view or provider. */
export function math(options?: MarkdownMathOptions): DownstreamPlugin;
