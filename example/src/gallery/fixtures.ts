import manifest from '../../../fixtures/manifest.json';

export type Fixture = (typeof manifest.fixtures)[number];

export const fixtures: readonly Fixture[] = manifest.fixtures;

export function fixtureSource(fixture: Fixture): string {
  return fixture.chunks.join('');
}
