import manifest from '../../../benchmarks/fixtures/manifest.json';

export type Fixture = (typeof manifest.fixtures)[number];

export const fixtures: readonly Fixture[] = manifest.fixtures;

export function fixtureSource(fixture: Fixture): string {
  return fixture.chunks.join('');
}

export function fixtureById(id: string): Fixture {
  const fixture = fixtures.find((item) => item.id === id);
  if (!fixture) {
    throw new Error(`Unknown fixture: ${id}`);
  }
  return fixture;
}
