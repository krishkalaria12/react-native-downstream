/**
 * The Markdown documents the app plays back. The same list is used by the
 * repository's benchmarks, so a workload measured in the app and a workload
 * measured in `pnpm bench` are the same documents.
 */
export {
  sampleById as fixtureById,
  samples as fixtures,
  type Sample as Fixture,
} from '../../../__benchmarks__/samples';
