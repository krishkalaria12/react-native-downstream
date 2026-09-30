const { withPodfile } = require('expo/config-plugins');

// Some pods still declare an iOS deployment target from long ago: iosMath, which
// react-native-enriched-markdown pulls in, says 6.0, and its font resource
// bundle inherits it. Xcode 27 supports 15.0 and later and fails the build
// rather than warning. Raising every pod target to the app's own minimum keeps
// this in prebuild instead of the generated ios/.
const marker = '# @generated withIosPodDeploymentTarget';

const raiseTargets = `    ${marker}
    minimum = podfile_properties['ios.deploymentTarget'] || '15.1'
    installer.pods_project.targets.each do |target|
      target.build_configurations.each do |build|
        current = build.build_settings['IPHONEOS_DEPLOYMENT_TARGET']
        if current && Gem::Version.new(current) < Gem::Version.new(minimum)
          build.build_settings['IPHONEOS_DEPLOYMENT_TARGET'] = minimum
        end
      end
    end
`;

module.exports = function withIosPodDeploymentTarget(config) {
  return withPodfile(config, (pluginConfig) => {
    const podfile = pluginConfig.modResults.contents;
    if (podfile.includes(marker)) return pluginConfig;
    const hook = 'post_install do |installer|\n';
    if (!podfile.includes(hook)) {
      throw new Error(
        'withIosPodDeploymentTarget: the Podfile has no post_install hook to extend.'
      );
    }
    pluginConfig.modResults.contents = podfile.replace(
      hook,
      hook + raiseTargets
    );
    return pluginConfig;
  });
};
