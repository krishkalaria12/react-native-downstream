#include "DownstreamTextLayoutManager.h"

#include "DocumentRegistry.h"
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

/// The document id, font scale, chrome, code layout, and theme id as the map the
/// view manager reads. `measure`, `updateState`, and the state wrapper all
/// resolve this same shape, so a measurement and a mount cannot disagree about a
/// height or about the colors it drew it with.
folly::dynamic codeLayoutData(uint64_t documentId, facebook::react::Float fontScale, uint32_t chrome,
                              const downstream::CodeLayout &code,
                              const downstream::Theme::Handle &theme = nullptr) {
  return folly::dynamic::object("documentId", static_cast<double>(documentId))(
      "fontScale", static_cast<double>(fontScale))("chrome", static_cast<double>(chrome))(
      // The booleans travel as numbers, so the whole map is one type across JNI.
      "codeBlockWrap", code.props.wrap ? 1.0 : 0.0)("lineNumbers", code.props.lineNumbers ? 1.0 : 0.0)(
      "codeBlockMaxHeight", static_cast<double>(code.props.maxHeight))(
      "wrapToggles", ordinalArray(code.choices.wrapToggles))("expanded", ordinalArray(code.choices.expanded))(
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

/// A sorted choice set as a Java array. An empty set becomes an empty array,
/// which is what `binarySearch` on the Kotlin side reads as "nothing chosen".
jintArray ordinalsToJava(JNIEnv *env, const std::vector<uint32_t> &ordinals) {
  jintArray array = env->NewIntArray(static_cast<jsize>(ordinals.size()));
  if (array != nullptr && !ordinals.empty()) {
    std::vector<jint> values(ordinals.begin(), ordinals.end());
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
  }
  return array;
}

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

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeInstallPreparer(JNIEnv *env, jclass clazz) {
  std::lock_guard<std::mutex> lock(installMutex);
  if (javaReady.load(std::memory_order_relaxed)) {
    return;
  }
  // documentId, fontScale, width, chrome, codeBlockWrap, lineNumbers,
  // codeBlockMaxHeight, wrapToggles, expanded, themeId.
  const jmethodID method = env->GetStaticMethodID(clazz, "prepare", "(JFFIZZF[I[IJ)V");
  if (method == nullptr || env->GetJavaVM(&javaVm) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  nativeClass = static_cast<jclass>(env->NewGlobalRef(clazz));
  prepareMethod = method;
  javaReady.store(true, std::memory_order_release);
}

namespace facebook::react {

// Measures through FabricUIManager.measure, which calls
// DownstreamMarkdownViewManager.measure on the calling thread. The view
// manager reads the document from the registry and lays it out with the same
// StaticLayout configuration the view draws with.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId surfaceId, uint32_t chrome,
                                                  const downstream::CodeLayout &code,
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
      codeLayoutData(document->id, fontSizeMultiplier, chrome, code, theme));

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
    // The whole code layout comes with the hint, because a prepared height is
    // only the height of the layout the reader actually has.
    jintArray toggles = ordinalsToJava(env, hint.code.choices.wrapToggles);
    jintArray expanded = ordinalsToJava(env, hint.code.choices.expanded);
    env->CallStaticVoidMethod(nativeClass, prepareMethod, static_cast<jlong>(document->id),
                              static_cast<jfloat>(hint.fontScale), static_cast<jfloat>(hint.width),
                              static_cast<jint>(hint.chrome),
                              static_cast<jboolean>(hint.code.props.wrap ? JNI_TRUE : JNI_FALSE),
                              static_cast<jboolean>(hint.code.props.lineNumbers ? JNI_TRUE : JNI_FALSE),
                              static_cast<jfloat>(hint.code.props.maxHeight), toggles, expanded,
                              static_cast<jlong>(hint.themeId()));
    env->DeleteLocalRef(toggles);
    env->DeleteLocalRef(expanded);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      return;
    }
  }
}

} // namespace facebook::react
