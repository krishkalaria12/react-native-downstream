import { SafeAreaView, StyleSheet } from 'react-native';

import { GalleryScreen } from './gallery/GalleryScreen';

export default function App() {
  return (
    <SafeAreaView style={styles.root}>
      <GalleryScreen />
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#fff' },
});
