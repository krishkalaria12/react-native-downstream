/**
 * The Markdown documents the app plays back. The unit tests render the same
 * list, so a document checked in tests is the one shown in the app.
 */
export {
  sampleById as fixtureById,
  samples as fixtures,
  type Sample as Fixture,
} from '../../../tests/samples';
