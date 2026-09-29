import { useState } from 'react';
import {
  Button,
  Platform,
  SafeAreaView,
  StatusBar,
  StyleSheet,
  useColorScheme,
  View,
} from 'react-native';

import { GalleryScreen } from './gallery/GalleryScreen';
import { PlaybackScreen } from './bench/PlaybackScreen';
import { StreamingChat } from './StreamingChat';

export default function App() {
  const [screen, setScreen] = useState<'gallery' | 'stream' | 'bench'>(
    'gallery'
  );
  const dark = useColorScheme() === 'dark';
  return (
    <SafeAreaView style={[styles.root, dark ? styles.dark : styles.light]}>
      <View style={styles.navigation}>
        <Button title="Gallery" onPress={() => setScreen('gallery')} />
        <Button title="Stream" onPress={() => setScreen('stream')} />
        <Button title="Benchmark" onPress={() => setScreen('bench')} />
      </View>
      {screen === 'gallery' ? (
        <GalleryScreen />
      ) : screen === 'stream' ? (
        <StreamingChat />
      ) : (
        <PlaybackScreen />
      )}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  // SafeAreaView pads only on iOS; Android draws under the status bar.
  root: {
    flex: 1,
    paddingTop: Platform.OS === 'android' ? StatusBar.currentHeight : 0,
  },
  light: { backgroundColor: '#fff' },
  dark: { backgroundColor: '#000' },
  navigation: { flexDirection: 'row', justifyContent: 'space-around' },
});
