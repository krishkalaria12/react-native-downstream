import { useState } from 'react';
import {
  ScrollView,
  StyleSheet,
  Text,
  useColorScheme,
  View,
} from 'react-native';
import {
  DownstreamMarkdown,
  type MarkdownErrorEvent,
  type MarkdownLinkPressEvent,
} from 'react-native-downstream';

import { fixtures, fixtureSource } from './fixtures';
import { featureCases } from './featureCases';
import { featureSample } from './sample';

export function GalleryScreen() {
  const [lastEvent, setLastEvent] = useState('Tap a link');
  const dark = useColorScheme() === 'dark';

  const handleLink = ({ url, range }: MarkdownLinkPressEvent) =>
    setLastEvent(`link ${url} [${range.start}, ${range.end})`);
  const handleError = ({ code, message }: MarkdownErrorEvent) =>
    setLastEvent(`error ${code}: ${message}`);

  return (
    <View style={styles.screen}>
      <Text
        style={[styles.event, dark ? styles.textDark : styles.textLight]}
        testID="gallery-event"
      >
        {lastEvent}
      </Text>
      <ScrollView contentContainerStyle={styles.content}>
        <DownstreamMarkdown
          selectable
          markdown={featureSample}
          onLinkPress={handleLink}
          onError={handleError}
        />
        {featureCases.map((feature) => (
          <View
            key={feature.id}
            style={[styles.card, dark ? styles.cardDark : styles.cardLight]}
            testID={feature.id}
          >
            <Text
              style={[styles.label, dark ? styles.textDark : styles.textLight]}
            >
              {feature.title}
            </Text>
            <DownstreamMarkdown
              selectable
              markdown={feature.markdown}
              onLinkPress={handleLink}
              onError={handleError}
            />
          </View>
        ))}
        {fixtures.map((fixture) => (
          <View
            key={fixture.id}
            style={[styles.card, dark ? styles.cardDark : styles.cardLight]}
          >
            <Text
              style={[styles.label, dark ? styles.textDark : styles.textLight]}
            >
              {fixture.id}
            </Text>
            <DownstreamMarkdown
              markdown={fixtureSource(fixture)}
              onLinkPress={handleLink}
              onError={handleError}
            />
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1 },
  content: { padding: 16, gap: 16 },
  event: {
    paddingHorizontal: 16,
    paddingVertical: 8,
    fontSize: 12,
  },
  card: {
    borderRadius: 8,
    borderWidth: StyleSheet.hairlineWidth,
    padding: 12,
    gap: 6,
  },
  cardLight: { borderColor: '#bbb' },
  cardDark: { borderColor: '#555' },
  textLight: { color: '#555' },
  textDark: { color: '#aaa' },
  label: { fontSize: 12 },
});
