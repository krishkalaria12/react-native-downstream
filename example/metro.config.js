const path = require('path');
const { getDefaultConfig } = require('expo/metro-config');
const {
  getBundleModeMetroConfig,
} = require('react-native-worklets/bundleMode');

let config = getDefaultConfig(__dirname);
config.watchFolders.push(path.resolve(__dirname, '..'));
config.watchFolders.push(
  path.join(
    path.dirname(require.resolve('react-native-worklets/package.json')),
    '.worklets'
  )
);

const librarySource = path.resolve(__dirname, '..', 'src', 'index.tsx');
config = getBundleModeMetroConfig(config);
const bundleModeResolver = config.resolver.resolveRequest;

config.resolver.resolveRequest = (context, moduleName, platform) => {
  if (moduleName === '@react-native-downstream/core') {
    return { type: 'sourceFile', filePath: librarySource };
  }
  return bundleModeResolver(context, moduleName, platform);
};

module.exports = config;
