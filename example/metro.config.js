const path = require('path');
const { getDefaultConfig } = require('expo/metro-config');

const config = getDefaultConfig(__dirname);
config.watchFolders.push(path.resolve(__dirname, '..'));

const librarySource = path.resolve(__dirname, '..', 'src', 'index.tsx');
const defaultResolver = config.resolver.resolveRequest;

config.resolver.resolveRequest = (context, moduleName, platform) => {
  // Use the library source, not a possibly stale lib/ build.
  if (moduleName === 'react-native-downstream') {
    return { type: 'sourceFile', filePath: librarySource };
  }
  if (defaultResolver) {
    return defaultResolver(context, moduleName, platform);
  }
  return context.resolveRequest(context, moduleName, platform);
};

module.exports = config;
