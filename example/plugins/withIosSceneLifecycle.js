const { withAppDelegate, withInfoPlist } = require('expo/config-plugins');

// Expo SDK 55 predates scene support. Xcode 27 requires a scene when launching
// an iOS 27 app, so keep this change in prebuild instead of the generated ios/.
const legacyStartup = `#if os(iOS) || os(tvOS)
    window = UIWindow(frame: UIScreen.main.bounds)
    factory.startReactNative(
      withModuleName: "main",
      in: window,
      launchOptions: launchOptions)
#endif`;

const sceneDelegate = `

class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?

  func scene(
    _ scene: UIScene,
    willConnectTo session: UISceneSession,
    options connectionOptions: UIScene.ConnectionOptions
  ) {
    guard let windowScene = scene as? UIWindowScene,
      let appDelegate = UIApplication.shared.delegate as? AppDelegate,
      let factory = appDelegate.reactNativeFactory else { return }

    let window = UIWindow(windowScene: windowScene)
    self.window = window
    appDelegate.window = window
    factory.startReactNative(
      withModuleName: "main",
      in: window,
      launchOptions: nil)

    for context in connectionOptions.urlContexts {
      open(context)
    }
    for userActivity in connectionOptions.userActivities {
      continueUserActivity(userActivity)
    }
  }

  func sceneDidBecomeActive(_ scene: UIScene) {
    (UIApplication.shared.delegate as? AppDelegate)?.applicationDidBecomeActive(UIApplication.shared)
  }

  func sceneWillResignActive(_ scene: UIScene) {
    (UIApplication.shared.delegate as? AppDelegate)?.applicationWillResignActive(UIApplication.shared)
  }

  func sceneDidEnterBackground(_ scene: UIScene) {
    (UIApplication.shared.delegate as? AppDelegate)?.applicationDidEnterBackground(UIApplication.shared)
  }

  func sceneWillEnterForeground(_ scene: UIScene) {
    (UIApplication.shared.delegate as? AppDelegate)?.applicationWillEnterForeground(UIApplication.shared)
  }

  func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
    for context in URLContexts {
      open(context)
    }
  }

  private func open(_ context: UIOpenURLContext) {
    var options: [UIApplication.OpenURLOptionsKey: Any] = [:]
    if let sourceApplication = context.options.sourceApplication {
      options[.sourceApplication] = sourceApplication
    }
    _ = (UIApplication.shared.delegate as? AppDelegate)?.application(
      UIApplication.shared,
      open: context.url,
      options: options)
  }

  func scene(_ scene: UIScene, continue userActivity: NSUserActivity) {
    continueUserActivity(userActivity)
  }

  private func continueUserActivity(_ userActivity: NSUserActivity) {
    _ = (UIApplication.shared.delegate as? AppDelegate)?.application(
      UIApplication.shared,
      continue: userActivity,
      restorationHandler: { _ in })
  }
}
`;

module.exports = function withIosSceneLifecycle(config) {
  config = withAppDelegate(config, (pluginConfig) => {
    const appDelegate = pluginConfig.modResults;
    if (appDelegate.language !== 'swift') {
      throw new Error(
        'iOS scene lifecycle plugin requires the Expo Swift AppDelegate'
      );
    }
    if (
      appDelegate.contents.includes(
        'class SceneDelegate: UIResponder, UIWindowSceneDelegate'
      ) &&
      !appDelegate.contents.includes(legacyStartup)
    ) {
      return pluginConfig;
    }
    if (!appDelegate.contents.includes(legacyStartup)) {
      throw new Error(
        'Expo AppDelegate startup template changed; inspect the scene lifecycle plugin'
      );
    }
    appDelegate.contents = appDelegate.contents.replace(legacyStartup, '');
    appDelegate.contents += sceneDelegate;
    return pluginConfig;
  });

  return withInfoPlist(config, (pluginConfig) => {
    pluginConfig.modResults.UIApplicationSceneManifest = {
      UIApplicationSupportsMultipleScenes: false,
      UISceneConfigurations: {
        UIWindowSceneSessionRoleApplication: [
          {
            UISceneConfigurationName: 'Default Configuration',
            UISceneDelegateClassName: '$(PRODUCT_MODULE_NAME).SceneDelegate',
          },
        ],
      },
    };
    return pluginConfig;
  });
};
