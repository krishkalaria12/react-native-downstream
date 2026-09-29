import { useEffect, useState } from 'react';
import { Linking, Platform, Settings, StyleSheet, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import { BenchScreen } from './bench/BenchScreen';
import { StressScreen } from './bench/StressScreen';
import { ChatScreen } from './example/ChatScreen';
import { ROUTES, routeById, type RouteId } from './navigation';
import { CompareScreen } from './testing/CompareScreen';
import { PreviewScreen, type PreviewLaunch } from './testing/PreviewScreen';
import { NavigationDrawer } from './ui/drawer';
import { ShellProvider, useShell } from './ui/shell';
import { ThemeProvider, useTheme } from './ui/theme';

interface Launch {
  route: 'stress' | 'preview';
  preview?: PreviewLaunch;
}

/** `mode=incremental&state=finished&from=4`, as a URL query or a launch argument. */
function parsePreviewLaunch(query: string): PreviewLaunch {
  const launch: PreviewLaunch = {};
  for (const pair of query.split('&')) {
    const [key, value = ''] = pair.split('=');
    if (key === 'preview' && (value === 'on' || value === 'off')) {
      launch.preview = value;
    } else if (
      key === 'mode' &&
      (value === 'full' || value === 'incremental')
    ) {
      launch.mode = value;
    } else if (key === 'state' && (value === 'open' || value === 'finished')) {
      launch.state = value;
    } else if (key === 'from' && /^\d+$/.test(value)) {
      launch.from = Number(value);
    }
  }
  return launch;
}

function launchFromUrl(url: string | null | undefined): Launch | undefined {
  if (url?.includes('stress')) return { route: 'stress' };
  if (url?.includes('preview')) {
    return {
      route: 'preview',
      preview: parsePreviewLaunch(url.split('?')[1] ?? ''),
    };
  }
  return undefined;
}

/**
 * Launch with `-downstreamStress 1` (iOS) or a downstream://stress VIEW intent
 * (Android) to run the stress screen without interaction.
 *
 * `-downstreamPreview 'mode=incremental&from=4'` (iOS) or a
 * downstream://preview?mode=incremental&from=4 VIEW intent (Android) opens the
 * preview gallery configured, so each state can be captured without a tap.
 */
function useLaunch(): Launch | undefined {
  const [launch, setLaunch] = useState<Launch>();
  useEffect(() => {
    if (Platform.OS === 'ios') {
      const preview = Settings.get('downstreamPreview');
      if (Settings.get('downstreamStress')) setLaunch({ route: 'stress' });
      else if (preview !== undefined && preview !== null) {
        setLaunch({
          route: 'preview',
          preview: parsePreviewLaunch(String(preview)),
        });
      }
    }
    const apply = (url: string | null | undefined) => {
      const next = launchFromUrl(url);
      if (next) setLaunch(next);
    };
    Linking.getInitialURL().then(apply, () => {});
    const subscription = Linking.addEventListener('url', ({ url }) =>
      apply(url)
    );
    return () => subscription.remove();
  }, []);
  return launch;
}

function Navigation() {
  const { open, setOpen } = useShell();
  const t = useTheme();
  const launch = useLaunch();
  const autoStress = launch?.route === 'stress';
  const [id, setId] = useState<RouteId>('example');

  useEffect(() => {
    if (launch) setId(launch.route);
  }, [launch]);

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
            ) : route.id === 'preview' ? (
              <PreviewScreen launch={launch?.preview} />
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
