import { useEffect, useState } from 'react';
import { Linking, Platform, Settings, StyleSheet, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import { BenchScreen } from './bench/BenchScreen';
import { StressScreen } from './bench/StressScreen';
import { ChatScreen } from './example/ChatScreen';
import { ROUTES, routeById, type RouteId } from './navigation';
import { CompareScreen } from './testing/CompareScreen';
import { NavigationDrawer } from './ui/drawer';
import { ShellProvider, useShell } from './ui/shell';
import { ThemeProvider, useTheme } from './ui/theme';

/** Launch with `-downstreamStress 1` (iOS) or a downstream://stress VIEW intent
 * (Android) to run the stress screen without interaction. */
function useStressDeepLink(): boolean {
  const [autoStress, setAutoStress] = useState(false);
  useEffect(() => {
    const start = () => setAutoStress(true);
    if (Platform.OS === 'ios' && Settings.get('downstreamStress')) start();
    Linking.getInitialURL().then(
      (url) => {
        if (url?.includes('stress')) start();
      },
      () => {}
    );
    const subscription = Linking.addEventListener('url', ({ url }) => {
      if (url.includes('stress')) start();
    });
    return () => subscription.remove();
  }, []);
  return autoStress;
}

function Navigation() {
  const { open, setOpen } = useShell();
  const t = useTheme();
  const autoStress = useStressDeepLink();
  const [id, setId] = useState<RouteId>(() =>
    autoStress ? 'stress' : 'example'
  );

  useEffect(() => {
    if (autoStress) setId('stress');
  }, [autoStress]);

  return (
    <NavigationDrawer
      open={open}
      onOpenChange={setOpen}
      route={routeById(id)}
      routes={ROUTES}
      onSelect={setId}
    >
      <StatusBar style={t.scheme === 'dark' ? 'light' : 'dark'} />
      {/* Every screen stays mounted, so a run configured on one of them
          survives a trip through the drawer. A hidden layer leaves the layout
          entirely, which keeps native views from measuring off-screen. */}
      <View style={styles.host}>
        {ROUTES.map((route) => (
          <View
            key={route.id}
            style={[styles.layer, route.id !== id && styles.hidden]}
            pointerEvents={route.id === id ? 'auto' : 'none'}
          >
            {route.id === 'example' ? (
              <ChatScreen />
            ) : route.id === 'testing' ? (
              <CompareScreen />
            ) : route.id === 'bench' ? (
              <BenchScreen />
            ) : (
              <StressScreen autoStart={autoStress} />
            )}
          </View>
        ))}
      </View>
    </NavigationDrawer>
  );
}

export default function App() {
  return (
    <SafeAreaProvider>
      <ThemeProvider>
        <ShellProvider>
          <Navigation />
        </ShellProvider>
      </ThemeProvider>
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  host: { flex: 1 },
  layer: { flex: 1 },
  hidden: { display: 'none' },
});
