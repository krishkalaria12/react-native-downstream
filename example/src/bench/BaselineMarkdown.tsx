import { StreamdownText } from 'react-native-streamdown';

export function BaselineMarkdown({ markdown }: { markdown: string }) {
  return <StreamdownText markdown={markdown} flavor="github" />;
}
