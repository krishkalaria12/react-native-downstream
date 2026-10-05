/**
 * The native sources live under src/, so the CLI has to be told where to look
 * instead of using the default ios/ and android/ folders at the package root.
 *
 * @type {import('@react-native-community/cli-types').UserDependencyConfig}
 */
module.exports = {
  dependency: {
    platforms: {
      android: {
        sourceDir: 'src/android',
        // Builds the shared C++ engine and the custom shadow node, which
        // Codegen does not generate for interfaceOnly components.
        cmakeListsPath: 'src/main/jni/CMakeLists.txt',
        componentDescriptors: [
          'DownstreamMarkdownViewComponentDescriptor',
          'DownstreamFenceSlotComponentDescriptor',
        ],
        // The session TurboModule is shared C++ (src/cpp/session), compiled by
        // the CMakeLists above.
        cxxModuleHeaderName: 'DownstreamSessionModule',
      },
    },
  },
};
