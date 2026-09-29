import { useEffect, useState } from 'react';
import {
  Linking,
  Platform,
  Pressable,
  SafeAreaView,
  Settings,
  StatusBar,
  StyleSheet,
  Text,
  useColorScheme,
  View,
} from 'react-native';

import { BenchScreen } from './bench/BenchScreen';
import { StressScreen } from './bench/StressScreen';
import { ChatScreen } from './example/ChatScreen';
import { CompareScreen } from './testing/CompareScreen';

const TABS = [
  { id: 'example', label: 'Example' },
  { id: 'testing', label: 'Testing' },
  { id: 'bench', label: 'Bench' },
  { id: 'stress', label: 'Stress' },
] as const;

type Tab = (typeof TABS)[number]['id'];

export default function App() {
  const [tab, setTab] = useState<Tab>('example');
  const [autoStress, setAutoStress] = useState(false);
  // Launch with `-downstreamStress 1` (iOS) or a downstream://stress VIEW
  // intent (Android) to run the stress screen without interaction.
  useEffect(() => {
    const start = () => {
      setTab('stress');
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
        {TABS.map((item) => (
          <Pressable
            key={item.id}
            accessibilityRole="tab"
            accessibilityState={{ selected: tab === item.id }}
            onPress={() => setTab(item.id)}
            style={[styles.tab, tab === item.id && styles.tabSelected]}
          >
            <Text
              style={[
                styles.tabText,
                tab === item.id && styles.tabTextSelected,
              ]}
            >
              {item.label}
            </Text>
          </Pressable>
        ))}
      </View>
      {tab === 'example' ? (
        <ChatScreen />
      ) : tab === 'testing' ? (
        <CompareScreen />
      ) : tab === 'stress' ? (
        <StressScreen autoStart={autoStress} />
      ) : (
        <BenchScreen />
      )}
    </SafeAreaView>
  );
}

const colors = {
  ink: '#172D2A',
  teal: '#14695E',
  line: '#CBD4CB',
  white: '#FFFFFF',
};

const styles = StyleSheet.create({
  // SafeAreaView pads only on iOS; Android draws under the status bar.
  root: {
    flex: 1,
    paddingTop: Platform.OS === 'android' ? StatusBar.currentHeight : 0,
  },
  light: { backgroundColor: colors.white },
  dark: { backgroundColor: '#000' },
  navigation: {
    flexDirection: 'row',
    gap: 6,
    paddingHorizontal: 12,
    paddingVertical: 6,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: colors.line,
  },
  tab: {
    flex: 1,
    borderRadius: 8,
    paddingVertical: 8,
    alignItems: 'center',
  },
  tabSelected: { backgroundColor: colors.teal },
  tabText: { color: colors.ink, fontSize: 14, fontWeight: '600' },
  tabTextSelected: { color: colors.white },
});
