#include "DownstreamTextLayoutManager.h"

#include "DocumentRegistry.h"
#include "ImageLoaderRegistry.h"
#include "theme/Theme.h"

#include <fbjni/fbjni.h>
#include <jni.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/renderer/core/conversions.h>

#include <atomic>
#include <mutex>

using namespace facebook::jni;

namespace {

folly::dynamic ordinalArray(const std::vector<uint32_t> &ordinals) {
  auto array = folly::dynamic::array();
  for (const uint32_t ordinal : ordinals) {
    array.push_back(static_cast<double>(ordinal));
  }
  return array;
}

/// The document id, font scale, chrome, code layout, image layout, and theme id
/// as the map the view manager reads. `measure`, `updateState`, and the state
/// wrapper all resolve this same shape, so a measurement and a mount cannot
/// disagree about a height or about the colors it drew it with.
folly::dynamic codeLayoutData(uint64_t documentId, facebook::react::Float fontScale, uint32_t chrome,
                              const downstream::CodeLayout &code, const downstream::PlacedCaret &caret,
                              const downstream::ImageLayout &images,
                              const downstream::Theme::Handle &theme = nullptr,
                              facebook::react::Float width = 0) {
  // One array of five numbers per image rather than five parallel arrays: a
  // measurement copies this map, and parallel arrays make "which array is one
  // longer" a question the Kotlin side has to answer defensively. State codes
  // are the small integers `ImageState` defines, so nothing needs translating.
  auto imageEntries = folly::dynamic::array();
  for (const auto &entry : images.state.entries) {
    imageEntries.push_back(folly::dynamic::object("ordinal", static_cast<double>(entry.ordinal))(
        "state", static_cast<double>(entry.state))("pixelWidth", static_cast<double>(entry.pixelWidth))(
        "pixelHeight", static_cast<double>(entry.pixelHeight)));
  }
  auto prefixes = folly::dynamic::array();
  for (const auto &prefix : images.allowedPrefixes) {
    prefixes.push_back(prefix);
  }
  return folly::dynamic::object("documentId", static_cast<double>(documentId))(
      "fontScale", static_cast<double>(fontScale))("chrome", static_cast<double>(chrome))(
      // The width the worker is preparing for. Zero for the `measure` path, which
      // already has Yoga's constraint and does not need it here.
      "width", static_cast<double>(width))(
      // The booleans travel as numbers, so the whole map is one type across JNI.
      "codeBlockWrap", code.props.wrap ? 1.0 : 0.0)("lineNumbers", code.props.lineNumbers ? 1.0 : 0.0)(
      "codeBlockMaxHeight", static_cast<double>(code.props.maxHeight))(
      "wrapToggles", ordinalArray(code.choices.wrapToggles))("expanded", ordinalArray(code.choices.expanded))(
      // The caret's two small enums and the images, the same way: the caret's
      // placeholder is part of the prepared text, so a worker has to reserve it
      // too or the view draws at a height the measurement did not measure.
      "caretKind", static_cast<double>(caret.kind))("caretPlace", static_cast<double>(caret.place))(
      // Images: whether they are on, each image's answer, and the prefixes that
      // produced them. The view only ever sees the answers.
      "imagesEnabled", images.state.enabled ? 1.0 : 0.0)("imageEntries", imageEntries)(
      "allowedImagePrefixes", prefixes)(
      // By id, as the document is: no slot value is copied on every measurement.
      "themeId", static_cast<double>(theme ? theme->id() : 0));
}

// DownstreamNative.prepare, resolved on a Java thread by nativeInstallPreparer.
// Worker threads are attached by the pool, so FindClass would use the system
// class loader there and miss the app's classes.
std::mutex installMutex;
JavaVM *javaVm = nullptr;
jclass nativeClass = nullptr;
jmethodID prepareMethod = nullptr;
std::atomic<bool> javaReady{false};

/// The JNIEnv for a pool thread, attaching it for the rest of its life.
JNIEnv *attachedEnv() {
  JNIEnv *env = nullptr;
  if (javaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char *>("ds-worker"), nullptr};
  if (javaVm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) {
    return nullptr;
  }
  // Pool threads live as long as the process; detach one that ever exits.
  thread_local struct Detach {
    ~Detach() { javaVm->DetachCurrentThread(); }
  } detach;
  (void)detach;
  return env;
}

/// The app's loader, as a global ref for the life of the process.
///
/// An atomic rather than a lock: it is read on every image load and written once,
/// and there is no state to protect beyond the reference itself. The previous
/// reference is deleted rather than leaked, because a loader replaced on a
/// reload is otherwise pinned until the process ends.
std::atomic<jobject> appImageLoader{nullptr};

/// What the registry asks for on every image load. A function rather than the
/// handle itself, because Java resolves the loader after this file's first use
/// and a resolver that read a pointer once would hold the answer to a question
/// asked too early.
void *resolveAppImageLoader() {
  return appImageLoader.load(std::memory_order_acquire);
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeInstallImageLoader(JNIEnv *env, jclass, jobject loader) {
  jobject previous = appImageLoader.exchange(env->NewGlobalRef(loader), std::memory_order_acq_rel);
  if (previous != nullptr) {
    env->DeleteGlobalRef(previous);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeReleaseImageLoader(JNIEnv *env, jclass) {
  jobject previous = appImageLoader.exchange(nullptr, std::memory_order_acq_rel);
  if (previous != nullptr) {
    env->DeleteGlobalRef(previous);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeInstallPreparer(JNIEnv *env, jclass clazz) {
  std::lock_guard<std::mutex> lock(installMutex);
  if (javaReady.load(std::memory_order_relaxed)) {
    return;
  }
  // The document id and the layout map. The map is the same shape `measure` and
  // `updateState` carry, so a worker prepares the layout the view is about to draw
  // rather than a near copy of it that a fourth positional argument could make
  // disagree.
  const jmethodID method = env->GetStaticMethodID(clazz, "prepare", "(JLjava/util/Map;)V");
  if (method == nullptr || env->GetJavaVM(&javaVm) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  nativeClass = static_cast<jclass>(env->NewGlobalRef(clazz));
  prepareMethod = method;
  javaReady.store(true, std::memory_order_release);
}

namespace facebook::react {

// Hands the engine the resolver for the app's own image loader.
//
// The mirror of iOS's `RCTImageLoader` read from the context container: it is the
// object React Native's own `<Image>` loads through, so the app's Fresco
// configuration, its network detector, and its disk cache all apply to a picture a
// model wrote. The handle arrives from Java rather than from the container,
// because Fresco's pipeline is reachable from the ReactContext and the container
// holds only the bridge's own services.
void DownstreamTextLayoutManager::installPlatformServices(const facebook::react::ContextContainer &) {
  downstream::ImageLoaderRegistry::install(&resolveAppImageLoader);
}

// Measures through FabricUIManager.measure, which calls
// DownstreamMarkdownViewManager.measure on the calling thread. The view
// manager reads the document from the registry and lays it out with the same
// StaticLayout configuration the view draws with.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId surfaceId, uint32_t chrome,
                                                  const downstream::CodeLayout &code,
                                                  const downstream::PlacedCaret &caret,
                                                  const downstream::ImageLayout &images,
                                                  const downstream::Theme::Handle &theme) const {
  // Java resolves the theme through the registry by id, the way it resolves the
  // document, so no slot value is copied across JNI on a measurement. Zero is
  // the default theme's own id.

  downstream::DocumentRegistry::add(document);
  const auto &fabricUIManager = contextContainer_->at<jni::global_ref<jobject>>("FabricUIManager");
  static const auto measure =
      jni::findClassStatic("com/facebook/react/fabric/FabricUIManager")
          ->getMethod<jlong(jint, jstring, ReadableMap::javaobject, ReadableMap::javaobject, ReadableMap::javaobject,
                            jfloat, jfloat, jfloat, jfloat)>("measure");

  // The chrome, the whole code layout, and the theme's id travel with the
  // document, so the view manager measures at the height the same layout
  // produces, which is what the view then lays out. A code block's height depends
  // on whether it wraps, whether its cap applies, and whether it has a gutter,
  // and one document can hold every combination at once, so all of it is part of
  // the measurement rather than something the view resolves later.
  //
  // The same map shape is what the view manager reads in `updateState` and in
  // `measure`, so all three resolve one layout rather than three.
  auto localDataMap = ReadableNativeMap::newObjectCxxArgs(
      codeLayoutData(document->id, fontSizeMultiplier, chrome, code, caret, images, theme));

  auto propsMap = ReadableNativeMap::newObjectCxxArgs(folly::dynamic::object());
  auto componentName = make_jstring("DownstreamMarkdownView");

  const jlong packed = measure(fabricUIManager, surfaceId, componentName.get(),
                               reinterpret_cast<ReadableMap::javaobject>(localDataMap.get()),
                               reinterpret_cast<ReadableMap::javaobject>(propsMap.get()), nullptr, 0.0f,
                               maximumWidth, 0.0f, std::numeric_limits<Float>::infinity());
  return yogaMeassureToSize(packed);
}

// Builds the spans and precomputed text on the worker through
// DownstreamNative.prepare, which fills the same RenderedDocument cache that
// measure and the view read.
void DownstreamTextLayoutManager::prepare(const downstream::DocumentHandle &document,
                                          const std::vector<downstream::LayoutHint> &hints) {
  if (!javaReady.load(std::memory_order_acquire)) {
    return;
  }
  JNIEnv *env = attachedEnv();
  if (env == nullptr) {
    return;
  }
  downstream::DocumentRegistry::add(document);
  for (const auto &hint : hints) {
    // The whole code layout, the caret, and the whole image layout come with the
    // hint, because a prepared height is only the height of the layout the reader
    // actually has.
    auto map = ReadableNativeMap::newObjectCxxArgs(codeLayoutData(
        document->id, hint.fontScale, hint.chrome, hint.code, hint.caret, hint.images, hint.theme, hint.width));
    env->CallStaticVoidMethod(nativeClass, prepareMethod, static_cast<jlong>(document->id),
                              reinterpret_cast<ReadableMap::javaobject>(map.get()));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      return;
    }
  }
}

} // namespace facebook::react
