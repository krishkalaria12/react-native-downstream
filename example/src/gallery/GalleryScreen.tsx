import { useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
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

  const handleLink = ({ url, range }: MarkdownLinkPressEvent) =>
    setLastEvent(`link ${url} [${range.start}, ${range.end})`);
  const handleError = ({ code, message }: MarkdownErrorEvent) =>
    setLastEvent(`error ${code}: ${message}`);

  return (
    <View style={styles.screen}>
      <Text style={styles.event} testID="gallery-event">
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
          <View key={feature.id} style={styles.card} testID={feature.id}>
            <Text style={styles.label}>{feature.title}</Text>
            <DownstreamMarkdown
              selectable
              markdown={feature.markdown}
              onLinkPress={handleLink}
              onError={handleError}
            />
          </View>
        ))}
        {fixtures.map((fixture) => (
          <View key={fixture.id} style={styles.card}>
            <Text style={styles.label}>{fixture.id}</Text>
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
    color: '#555',
  },
  card: {
    borderRadius: 8,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: '#bbb',
    padding: 12,
    gap: 6,
  },
  label: { fontSize: 12, color: '#777' },
});
