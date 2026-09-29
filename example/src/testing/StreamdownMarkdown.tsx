import { StreamdownText } from 'react-native-streamdown';

/** The candidate under test: whole-document reparse on every update. */
export function StreamdownMarkdown({ markdown }: { markdown: string }) {
  return <StreamdownText markdown={markdown} flavor="github" />;
}
