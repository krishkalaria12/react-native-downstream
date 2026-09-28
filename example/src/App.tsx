import { useState } from 'react';
import {
  Button,
  Platform,
  SafeAreaView,
  StatusBar,
  StyleSheet,
  View,
} from 'react-native';

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
  // SafeAreaView pads only on iOS; Android draws under the status bar.
  root: {
    flex: 1,
    backgroundColor: '#fff',
    paddingTop: Platform.OS === 'android' ? StatusBar.currentHeight : 0,
  },
  navigation: { flexDirection: 'row', justifyContent: 'space-around' },
});
