import { useEffect, useState } from 'react';
import { Button, ScrollView, StyleSheet, Text, View } from 'react-native';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  type MarkdownStreamController,
} from 'react-native-downstream';

import { featureSample } from './gallery/sample';

/** Device smoke screen for the session path, including unmount and replay. */
export function StreamingChat() {
  const [run, setRun] = useState(0);
  const [stream, setStream] = useState<MarkdownStreamController | null>(null);
  const [status, setStatus] = useState('Starting stream');

  useEffect(() => {
    const controller = createMarkdownStream({
      onError: (error) => setStatus(`${error.code}: ${error.message}`),
    });
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
  }, [run]);

  return (
    <View style={styles.screen}>
      <View style={styles.toolbar}>
        <Text style={styles.status}>{status}</Text>
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
  status: { color: '#555' },
  content: { padding: 16 },
});
