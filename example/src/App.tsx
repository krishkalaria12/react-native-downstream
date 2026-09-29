import { useEffect, useState } from 'react';
import {
  Button,
  Linking,
  Platform,
  Settings,
  SafeAreaView,
  StatusBar,
  StyleSheet,
  useColorScheme,
  View,
} from 'react-native';

import { GalleryScreen } from './gallery/GalleryScreen';
import { PlaybackScreen } from './bench/PlaybackScreen';
import { StressScreen } from './bench/StressScreen';
import { StreamingChat } from './StreamingChat';

export default function App() {
  const [screen, setScreen] = useState<
    'gallery' | 'stream' | 'bench' | 'stress'
  >('gallery');
  const [autoStress, setAutoStress] = useState(false);
  // Launch with `-downstreamStress 1` (iOS) or a downstream://stress VIEW
  // intent (Android) to run the stress screen without interaction.
  useEffect(() => {
    const start = () => {
      setScreen('stress');
      setAutoStress(true);
    };
    if (Platform.OS === 'ios' && Settings.get('downstreamStress')) start();
    Linking.getInitialURL().then((url) => {
      if (url?.includes('stress')) start();
    });
  }, []);
  const dark = useColorScheme() === 'dark';
  return (
    <SafeAreaView style={[styles.root, dark ? styles.dark : styles.light]}>
      <View style={styles.navigation}>
        <Button title="Gallery" onPress={() => setScreen('gallery')} />
        <Button title="Stream" onPress={() => setScreen('stream')} />
        <Button title="Benchmark" onPress={() => setScreen('bench')} />
        <Button title="Stress" onPress={() => setScreen('stress')} />
      </View>
      {screen === 'gallery' ? (
        <GalleryScreen />
      ) : screen === 'stream' ? (
        <StreamingChat />
      ) : screen === 'stress' ? (
        <StressScreen autoStart={autoStress} />
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
