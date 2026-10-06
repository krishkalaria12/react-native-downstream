import { useCallback } from 'react';
import { Modal, Pressable, StyleSheet, Text, View } from 'react-native';

import { DownstreamMarkdown } from './DownstreamMarkdown';
import type { MarkdownTranslations, MarkdownViewProps } from './types';

/**
 * The table's full-screen view.
 *
 * A React Native `Modal` holding a second `DownstreamMarkdown` over the table's
 * own Markdown, rather than a native screen. Three reasons, and all three are
 * about who owns the decision:
 *
 * - No new native drawing. A table already knows how to lay itself out and how
 *   to scroll; a second view of the same source is the whole feature.
 * - The app can replace it. `renderTableFullscreen` hands the press to the app,
 *   which can push a route instead and keep its own navigation.
 * - The theme, the styles, the translations and the images follow, because it is
 *   the same component with the same props: `tableMaxHeight={0}` and no controls
 *   are the only differences, and both are removals rather than additions.
 *
 * The modal is mounted only while it is open, so a reader who never presses the
 * button pays nothing for it.
 */

/**
 * The props a full-screen table inherits.
 *
 * Everything that changes what a table *looks like*, and nothing that changes
 * what a control *does*: the modal owns its own chrome, and inheriting the
 * surrounding `controls` would draw a second copy of the button that opened it.
 * The omitted set is written out rather than picked with a conditional spread so
 * a prop added to the view later has to be considered here rather than silently
 * inherited.
 */
export type InheritedTableProps = Omit<
  MarkdownViewProps,
  | 'markdown'
  | 'stream'
  | 'tableMaxHeight'
  | 'controls'
  | 'plugins'
  | 'animated'
  | 'isAnimating'
  | 'caret'
  | 'linkSafety'
  | 'renderTableFullscreen'
  | 'onTableFullscreen'
  | 'onAnimationStart'
  | 'onAnimationEnd'
  | 'style'
>;

export type TableFullscreenProps = Readonly<{
  /** The table's own Markdown, as the engine's export wrote it. */
  markdown: string;
  /** The label of the button that closes it, from `translations`. */
  closeLabel: string;
  /** Every prop the surrounding view was given, so the modal matches it. */
  inherited: InheritedTableProps;
  /** Called when the reader closes it, by the button or by a system gesture. */
  onClose: () => void;
  /** `translations`, for the parts the modal itself reads. */
  translations?: Partial<MarkdownTranslations>;
}>;

const styles = StyleSheet.create({
  root: { flex: 1 },
  bar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'flex-end',
    paddingHorizontal: 12,
    paddingVertical: 8,
  },
  close: { paddingHorizontal: 12, paddingVertical: 6 },
  label: { fontSize: 16 },
});

/**
 * The modal, and the one button that leaves it.
 *
 * `onRequestClose` is what Android's back gesture and the hardware back button
 * call, and it closes the modal rather than doing nothing: a reader who swipes
 * away from a table should end up where they were, not in a modal they cannot
 * leave. iOS's own swipe-down does the same through the same callback.
 */
export function TableFullscreen({
  markdown,
  closeLabel,
  inherited,
  onClose,
}: TableFullscreenProps) {
  const handleRequestClose = useCallback(() => onClose(), [onClose]);

  return (
    <Modal
      visible
      animationType="slide"
      presentationStyle="fullScreen"
      onRequestClose={handleRequestClose}
    >
      <View style={styles.root}>
        <View style={styles.bar}>
          <Pressable
            accessibilityRole="button"
            accessibilityLabel={closeLabel}
            onPress={onClose}
            style={styles.close}
          >
            <Text style={styles.label}>{closeLabel}</Text>
          </Pressable>
        </View>
        <DownstreamMarkdown
          {...inherited}
          markdown={markdown}
          // The cap is the only thing removed: a table in a full-screen modal is
          // the one place in a chat where scrolling the whole page is right.
          tableMaxHeight={0}
          // The button that opened this is not in the modal, and a second copy
          // would be a way out of a modal that is already out of the way.
          controls={false}
        />
      </View>
    </Modal>
  );
}
