#include "DownstreamTextLayoutManager.h"

#include "Clipboard.h"
#include "DocumentRegistry.h"
#include "ImageLoaderRegistry.h"
#include "theme/Theme.h"

#include <fbjni/fbjni.h>
#include <jni.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/renderer/core/conversions.h>

#include <atomic>
#include <exception>
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
                              const downstream::HostLayout &hosts = {},
                              facebook::react::Float width = 0,
                              const downstream::TableLayout &tables = {}) {
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
  folly::dynamic data = folly::dynamic::object("documentId", static_cast<double>(documentId))(
      "fontScale", static_cast<double>(fontScale))("chrome", static_cast<double>(chrome))(
      // The width the worker is preparing for. Zero for the `measure` path, which
      // already has Yoga's constraint and does not need it here.
      "width", static_cast<double>(width))(
      // The booleans travel as numbers, so the whole map is one type across JNI.
      "codeBlockWrap", code.props.wrap ? 1.0 : 0.0)("lineNumbers", code.props.lineNumbers ? 1.0 : 0.0)(
      "codeBlockMaxHeight", static_cast<double>(code.props.maxHeight))(
      "wrapToggles", ordinalArray(code.choices.wrapToggles))("expanded", ordinalArray(code.choices.expanded))(
      // The table cap and its choices, beside the code block's: a table is the
      // second block kind with a height of its own, and a capped table measures
      // at one height and an expanded one at another.
      "tableMaxHeight", static_cast<double>(tables.props.maxHeight))(
      "tableExpanded", ordinalArray(tables.choices.expanded))(
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
  // An absent key is the empty HostLayout on Kotlin's side. Ordinary markdown
  // needs no host array allocation or JNI map entry.
  if (!hosts.empty()) {
    // One object per hosted fence, with the height its component measured at.
    auto hostEntries = folly::dynamic::array();
    for (const auto &entry : hosts.entries) {
      hostEntries.push_back(folly::dynamic::object("ordinal", static_cast<double>(entry.ordinal))(
          "height", static_cast<double>(entry.height)));
    }
    data["hostEntries"] = std::move(hostEntries);
  }
  return data;
}

// DownstreamNative.prepare, resolved on a Java thread by nativeInstallPreparer.
// Worker threads are attached by the pool, so FindClass would use the system
// class loader there and miss the app's classes.
std::mutex installMutex;
JavaVM *javaVm = nullptr;
jclass nativeClass = nullptr;
jmethodID prepareMethod = nullptr;
jmethodID hostFramesMethod = nullptr;
std::atomic<bool> javaReady{false};

/// `DownstreamNative.writeClipboard`, resolved once on a Java thread.
///
/// Held as a global reference so the writer below can use it from any thread:
/// the TurboModule that answers `copyText` runs on the JavaScript thread, which
/// is a Java thread, but a reader who calls the exported helper from a worker
/// would attach and then need a class loader it does not have.
jmethodID clipboardMethod = nullptr;

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

/// The rectangles of the hosted React children, as `[ordinal, left, top, right,
/// bottom]` groups in points, or null when Java cannot be asked. `width` is in
/// points too, as Yoga measured it.
///
/// Reads the `RenderedDocument` that the measurement just filled rather than
/// laying anything out again: the document is cached by id and width, so this is
/// the same object the view is about to draw, and the rectangles it returns are
/// the ones its frames are stacked from. Nothing here parses or measures.
std::shared_ptr<std::vector<jfloat>> hostFrames(jlong documentId, jobject layoutMap, jfloat width) {
  if (!javaReady.load(std::memory_order_acquire) || hostFramesMethod == nullptr) {
    return nullptr;
  }
  JNIEnv *env = attachedEnv();
  if (env == nullptr || layoutMap == nullptr) {
    return nullptr;
  }
  auto array = static_cast<jfloatArray>(env->CallStaticObjectMethod(
      nativeClass, hostFramesMethod, documentId, layoutMap, width));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return nullptr;
  }
  if (array == nullptr) {
    return nullptr;
  }
  const jsize length = env->GetArrayLength(array);
  auto frames = std::make_shared<std::vector<jfloat>>(length);
  if (length > 0) {
    env->GetFloatArrayRegion(array, 0, length, frames->data());
  }
  env->DeleteLocalRef(array);
  return frames;
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
  // disagree. The map is a `ReadableNativeMap`, so the descriptor names the
  // `ReadableMap` the Kotlin methods declare: a `java.util.Map` descriptor
  // matches no method at all.
  const jmethodID method = env->GetStaticMethodID(clazz, "prepare", "(JLcom/facebook/react/bridge/ReadableMap;)V");
  // A failed lookup leaves a NoSuchMethodError pending, and any further JNI call
  // with one pending aborts the process, so each lookup clears its own.
  env->ExceptionClear();
  // Resolved with the preparer rather than lazily at the first measurement: a
  // measurement runs on Fabric's layout thread, and a `GetStaticMethodID` there
  // would race the class loader. A null here means the app runs a build without
  // this method, and hosted children are not placed rather than crashing.
  const jmethodID frames =
      env->GetStaticMethodID(clazz, "hostFrames", "(JLcom/facebook/react/bridge/ReadableMap;F)[F");
  env->ExceptionClear();
  if (env->GetJavaVM(&javaVm) != JNI_OK) {
    env->ExceptionClear();
    return;
  }
  if (method == nullptr) {
    return;
  }
  // The map a worker hands `prepare` is built in C++ before any Java frame is
  // on that thread, and fbjni finds its class with the calling thread's loader,
  // which on a thread the pool attached is the system's and cannot see React
  // Native's classes. Resolved here, on a Java thread, fbjni keeps the class as
  // a global reference and the workers never look it up themselves.
  facebook::react::ReadableNativeMap::javaClassStatic();
  nativeClass = static_cast<jclass>(env->NewGlobalRef(clazz));
  prepareMethod = method;
  hostFramesMethod = frames;
  javaReady.store(true, std::memory_order_release);
}

namespace {

/// Hands the text to the Android clipboard manager, through Kotlin.
///
/// Returns false rather than throwing when no context has been recorded yet or
/// the manager refuses the clip: a reader whose platform has no clipboard still
/// gets the confirmation, and its Open button.
bool writeClipboard(std::string_view text) {
  JNIEnv *env = attachedEnv();
  if (env == nullptr || nativeClass == nullptr || clipboardMethod == nullptr) {
    return false;
  }
  jstring value = env->NewStringUTF(text.data());
  if (value == nullptr) {
    env->ExceptionClear();
    return false;
  }
  const jboolean written = env->CallStaticBooleanMethod(nativeClass, clipboardMethod, value);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  env->DeleteLocalRef(value);
  return written == JNI_TRUE;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeInstallClipboard(JNIEnv *env, jclass clazz) {
  std::lock_guard<std::mutex> lock(installMutex);
  if (clipboardMethod != nullptr) {
    return;
  }
  clipboardMethod = env->GetStaticMethodID(clazz, "writeClipboard", "(Ljava/lang/String;)Z");
  // A failed lookup leaves a NoSuchMethodError pending, and any further JNI call
  // with one pending aborts the process.
  env->ExceptionClear();
  // Installed even when the lookup failed: the writer is the one that reports
  // the failure, rather than the TurboModule having to know whether Android has
  // a clipboard at all.
  downstream::installClipboardWriter(&writeClipboard);
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
DocumentMeasurement DownstreamTextLayoutManager::measurePlatform(
    const downstream::DocumentHandle &document, Float fontSizeMultiplier, Float maximumWidth, SurfaceId surfaceId,
    uint32_t chrome, const downstream::CodeLayout &code, const downstream::PlacedCaret &caret,
    const downstream::ImageLayout &images, const downstream::HostLayout &hosts,
    const downstream::Theme::Handle &theme, const downstream::TableLayout &tables) const {
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
      codeLayoutData(document->id, fontSizeMultiplier, chrome, code, caret, images, theme, hosts, 0, tables));

  auto propsMap = ReadableNativeMap::newObjectCxxArgs(folly::dynamic::object());
  auto componentName = make_jstring("DownstreamMarkdownView");

  const jlong packed = measure(fabricUIManager, surfaceId, componentName.get(),
                               reinterpret_cast<ReadableMap::javaobject>(localDataMap.get()),
                               reinterpret_cast<ReadableMap::javaobject>(propsMap.get()), nullptr, 0.0f,
                               maximumWidth, 0.0f, std::numeric_limits<Float>::infinity());

  DocumentMeasurement measurement;
  measurement.size = yogaMeassureToSize(packed);
  if (hosts.entries.empty()) {
    // A view with no renderers pays nothing: no second crossing into Java, and
    // the same measurement it has always made.
    return measurement;
  }
  // `FabricUIManager.measure` returns a packed size and nothing else: only
  // `measureText` reports attachment positions, and a hosted child is not text.
  // So the rectangles come from the `RenderedDocument` that measurement just
  // filled, read back through one more static call, as `[ordinal, left, top,
  // right, bottom]` groups.
  const jfloat framesWidth = static_cast<jfloat>(maximumWidth);
  const auto frames = hostFrames(document->id, reinterpret_cast<ReadableMap::javaobject>(localDataMap.get()),
                                 framesWidth);
  if (frames != nullptr) {
    // Five floats per host: the ordinal, then the two edges of its rectangle,
    // which is what Android's `hostFrames` reports. The edges become the
    // origin and the size a shadow node places a child at, so the subtraction
    // happens here rather than on either side guessing.
    for (size_t index = 0; index + 4 < frames->size(); index += 5) {
      const float left = frames->at(index + 1);
      const float top = frames->at(index + 2);
      const float right = frames->at(index + 3);
      const float bottom = frames->at(index + 4);
      measurement.slots.push_back({static_cast<uint32_t>(frames->at(index)),
                                   Rect{Point{left, top}, Size{std::max(0.0f, right - left), std::max(0.0f, bottom - top)}}});
    }
  }
  return measurement;
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
  // A preparation is a head start and never an answer: measurement builds the
  // same rendering if this does not, so anything that fails here is dropped
  // rather than allowed to take a pool thread, and the process, down with it.
  try {
    for (const auto &hint : hints) {
      // The whole code layout, the caret, and the whole image layout come with
      // the hint, because a prepared height is only the height of the layout the
      // reader actually has.
      auto map = ReadableNativeMap::newObjectCxxArgs(codeLayoutData(document->id, hint.fontScale, hint.chrome,
                                                                     hint.code, hint.caret, hint.images, hint.theme,
                                                                     downstream::HostLayout{}, hint.width,
                                                                     hint.tables));
      env->CallStaticVoidMethod(nativeClass, prepareMethod, static_cast<jlong>(document->id),
                                reinterpret_cast<ReadableMap::javaobject>(map.get()));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
      }
    }
  } catch (const std::exception &) {
    env->ExceptionClear();
  }
}

} // namespace facebook::react
