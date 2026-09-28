import { useState } from 'react';
import { Pressable, SafeAreaView, StyleSheet, Text, View } from 'react-native';

import { PlaybackScreen } from './bench/PlaybackScreen';
import { GalleryScreen } from './gallery/GalleryScreen';

type Screen = 'gallery' | 'playback';

export default function App() {
  const [screen, setScreen] = useState<Screen>('gallery');

  return (
    <SafeAreaView style={styles.root}>
      <View style={styles.tabs}>
        {(['gallery', 'playback'] as const).map((id) => (
          <Pressable
            key={id}
            accessibilityRole="tab"
            accessibilityState={{ selected: screen === id }}
            onPress={() => setScreen(id)}
            style={[styles.tab, screen === id && styles.tabSelected]}
          >
            <Text style={styles.tabText}>
              {id === 'gallery' ? 'Downstream gallery' : 'Baseline playback'}
            </Text>
          </Pressable>
        ))}
      </View>
      {screen === 'gallery' ? <GalleryScreen /> : <PlaybackScreen />}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#fff' },
  tabs: { flexDirection: 'row', gap: 8, padding: 8 },
  tab: {
    flex: 1,
    paddingVertical: 8,
    borderRadius: 6,
    alignItems: 'center',
    backgroundColor: '#eee',
  },
  tabSelected: { backgroundColor: '#cde' },
  tabText: { fontSize: 13 },
});
