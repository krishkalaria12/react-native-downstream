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

const defaultResolver = config.resolver.resolveRequest;
config = getBundleModeMetroConfig(config);
const bundleModeResolver = config.resolver.resolveRequest;

const librarySource = path.resolve(__dirname, '..', 'src', 'index.tsx');

config.resolver.resolveRequest = (context, moduleName, platform) => {
  // Use the library source, not a possibly stale lib/ build.
  if (moduleName === 'react-native-downstream') {
    return { type: 'sourceFile', filePath: librarySource };
  }
  if (moduleName.startsWith('react-native-worklets/.worklets/')) {
    return bundleModeResolver(context, moduleName, platform);
  }
  if (defaultResolver) {
    return defaultResolver(context, moduleName, platform);
  }
  return context.resolveRequest(context, moduleName, platform);
};

module.exports = config;
