import { StreamdownText } from 'react-native-streamdown';

/**
 * The candidate under test: whole-document reparse on every update.
 *
 * The installed native `react-native-streamdown` is 0.2.0, which is a thin
 * pass-through to `react-native-enriched-markdown` and has no `animated`,
 * `caret`, or `isAnimating` props at all. Its only animation control is
 * `streamingAnimation`, and that is a different feature: enriched-markdown fades
 * the whole tail as one unit, edits the text storage on every frame, and has no
 * caret. So this wrapper passes the one prop the peer really has, and the Testing
 * screen labels the comparison for what it is.
 */
export function StreamdownMarkdown({
  markdown,
  streamingAnimation = false,
}: {
  markdown: string;
  /** The peer's whole-tail fade, which is not a per-word cascade. */
  streamingAnimation?: boolean;
}) {
  return (
    <StreamdownText
      markdown={markdown}
      flavor="github"
      streamingAnimation={streamingAnimation}
    />
  );
}
