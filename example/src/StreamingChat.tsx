import { useEffect, useState } from 'react';
import {
  Button,
  ScrollView,
  StyleSheet,
  Text,
  useColorScheme,
  View,
} from 'react-native';
import {
  DownstreamMarkdown,
  type MarkdownStreamController,
} from 'react-native-downstream';

// The parse-mode switch is internal to the package, so Phase 5 can compare
// both modes from one build without making it public API.
import {
  createMarkdownStreamWithMode,
  type ParseMode,
} from '../../src/createMarkdownStream';
import { featureCases } from './gallery/featureCases';
import { featureSample } from './gallery/sample';

/** Device smoke screen for the session path, including unmount and replay. */
export function StreamingChat() {
  const dark = useColorScheme() === 'dark';
  const [run, setRun] = useState(0);
  const [mode, setMode] = useState<ParseMode>('full');
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  const [status, setStatus] = useState('Starting stream');

  useEffect(() => {
    const controller = createMarkdownStreamWithMode(
      { onError: (error) => setStatus(`${error.code}: ${error.message}`) },
      mode
    );
    const characters = Array.from(featureSample);
    let offset = 0;
    setStream(controller);
    setStatus('Streaming');
    const timer = setInterval(() => {
      try {
        if (offset >= characters.length) {
          controller.finish();
          setStatus('Complete');
          clearInterval(timer);
          return;
        }
        controller.append(characters.slice(offset, offset + 8).join(''));
        offset += 8;
      } catch (error) {
        setStatus(String(error));
        clearInterval(timer);
      }
    }, 40);
    return () => {
      clearInterval(timer);
      controller.dispose();
    };
  }, [run, mode]);

  return (
    <View style={styles.screen}>
      <View style={styles.toolbar}>
        <Text style={dark ? styles.statusDark : styles.statusLight}>
          {status}
        </Text>
        <Button
          title={mode === 'full' ? 'Full parse' : 'Incremental'}
          onPress={() => {
            setStream(null);
            setMode((previous) =>
              previous === 'full' ? 'incremental' : 'full'
            );
          }}
        />
        <Button
          title="Replay"
          onPress={() => {
            setStream(null);
            setRun((previous) => previous + 1);
          }}
        />
      </View>
      <ScrollView contentContainerStyle={styles.content}>
        {stream && <DownstreamMarkdown stream={stream} selectable />}
        {featureCases.map((feature) => (
          <View key={feature.id} style={styles.completed}>
            <Text style={dark ? styles.statusDark : styles.statusLight}>
              Completed: {feature.title}
            </Text>
            <DownstreamMarkdown markdown={feature.markdown} selectable />
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  toolbar: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    padding: 12,
  },
  statusLight: { color: '#555' },
  statusDark: { color: '#aaa' },
  content: { padding: 16 },
  completed: { paddingTop: 24 },
});
