import { useState } from 'react';
import { Button, SafeAreaView, StyleSheet, View } from 'react-native';

import { GalleryScreen } from './gallery/GalleryScreen';
import { StreamingChat } from './StreamingChat';

export default function App() {
  const [screen, setScreen] = useState<'gallery' | 'stream'>('gallery');
  return (
    <SafeAreaView style={styles.root}>
      <View style={styles.navigation}>
        <Button title="Gallery" onPress={() => setScreen('gallery')} />
        <Button title="Stream" onPress={() => setScreen('stream')} />
      </View>
      {screen === 'gallery' ? <GalleryScreen /> : <StreamingChat />}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#fff' },
  navigation: { flexDirection: 'row', justifyContent: 'space-around' },
});
