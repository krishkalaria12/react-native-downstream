/**
 * @type {import('@react-native-community/cli-types').UserDependencyConfig}
 */
module.exports = {
  dependency: {
    platforms: {
      android: {
        // Builds the shared C++ engine and the custom shadow node, which
        // Codegen does not generate for interfaceOnly components.
        cmakeListsPath: 'src/main/jni/CMakeLists.txt',
        componentDescriptors: ['DownstreamMarkdownViewComponentDescriptor'],
        // The session TurboModule is shared C++ (cpp/session), compiled by
        // the CMakeLists above.
        cxxModuleHeaderName: 'DownstreamSessionModule',
      },
    },
  },
};
