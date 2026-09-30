#include "DownstreamTextLayoutManager.h"

#include "DocumentRegistry.h"

#include <fbjni/fbjni.h>
#include <jni.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/renderer/core/conversions.h>

#include <atomic>
#include <mutex>

using namespace facebook::jni;

namespace {

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

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeInstallPreparer(JNIEnv *env, jclass clazz) {
  std::lock_guard<std::mutex> lock(installMutex);
  if (javaReady.load(std::memory_order_relaxed)) {
    return;
  }
  const jmethodID method = env->GetStaticMethodID(clazz, "prepare", "(JFFIZI[I)V");
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
                                                  SurfaceId surfaceId, uint32_t chrome, bool codeBlockWrap,
                                                  const std::vector<uint32_t> &wrapToggles) const {
  downstream::DocumentRegistry::add(document);

  const auto &fabricUIManager = contextContainer_->at<jni::global_ref<jobject>>("FabricUIManager");
  static const auto measure =
      jni::findClassStatic("com/facebook/react/fabric/FabricUIManager")
          ->getMethod<jlong(jint, jstring, ReadableMap::javaobject, ReadableMap::javaobject, ReadableMap::javaobject,
                            jfloat, jfloat, jfloat, jfloat)>("measure");

  // The chrome and the reader's wrap choices travel with the document, so the
  // view manager measures at the height the same layout produces, which is what
  // the view then lays out. A code block's height depends on whether it wraps,
  // and a document can hold wrapped and unwrapped code at once, so the choices
  // are part of the measurement rather than something the view resolves later.
  folly::dynamic toggleArray = folly::dynamic::array();
  for (const uint32_t ordinal : wrapToggles) {
    toggleArray.push_back(static_cast<double>(ordinal));
  }
  folly::dynamic localData = folly::dynamic::object("documentId", static_cast<double>(document->id))(
      "fontScale", static_cast<double>(fontSizeMultiplier))("chrome", static_cast<double>(chrome))(
      "codeBlockWrap", codeBlockWrap ? 1.0 : 0.0)("wrapToggles", std::move(toggleArray));
  auto localDataMap = ReadableNativeMap::newObjectCxxArgs(std::move(localData));
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
    // The wrap choices come with the hint, because a prepared height is only
    // the height of the layout the reader actually has.
    jintArray toggles = env->NewIntArray(static_cast<jsize>(hint.wrapToggles.size()));
    if (toggles != nullptr && !hint.wrapToggles.empty()) {
      std::vector<jint> values(hint.wrapToggles.begin(), hint.wrapToggles.end());
      env->SetIntArrayRegion(toggles, 0, static_cast<jsize>(values.size()), values.data());
    }
    env->CallStaticVoidMethod(nativeClass, prepareMethod, static_cast<jlong>(document->id),
                              static_cast<jfloat>(hint.fontScale), static_cast<jfloat>(hint.width),
                              static_cast<jint>(hint.chrome),
                              static_cast<jboolean>(hint.codeBlockWrap ? JNI_TRUE : JNI_FALSE), toggles);
    env->DeleteLocalRef(toggles);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      return;
    }
  }
}

} // namespace facebook::react
