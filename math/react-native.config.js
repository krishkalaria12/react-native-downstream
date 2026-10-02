/**
 * Autolinking: the iOS pod is at the package root and the Android library is in
 * android/, which are the CLI's defaults, so this only names the package class.
 *
 * @type {import('@react-native-community/cli-types').UserDependencyConfig}
 */
module.exports = {
  dependency: {
    platforms: {
      android: {
        packageImportPath: 'import com.downstream.math.DownstreamMathPackage;',
        packageInstance: 'new DownstreamMathPackage()',
      },
    },
  },
};
