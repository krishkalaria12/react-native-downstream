#include "DownstreamTextLayoutManager.h"

#include "DocumentRegistry.h"

#include <fbjni/fbjni.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/renderer/core/conversions.h>

using namespace facebook::jni;

namespace facebook::react {

// Measures through FabricUIManager.measure, which calls
// DownstreamMarkdownViewManager.measure on the calling thread. The view
// manager reads the document from the registry and lays it out with the same
// StaticLayout configuration the view draws with.
Size DownstreamTextLayoutManager::measurePlatform(const downstream::DocumentHandle &document,
                                                  Float fontSizeMultiplier, Float maximumWidth,
                                                  SurfaceId surfaceId) const {
  downstream::DocumentRegistry::add(document);

  const auto &fabricUIManager = contextContainer_->at<jni::global_ref<jobject>>("FabricUIManager");
  static const auto measure =
      jni::findClassStatic("com/facebook/react/fabric/FabricUIManager")
          ->getMethod<jlong(jint, jstring, ReadableMap::javaobject, ReadableMap::javaobject, ReadableMap::javaobject,
                            jfloat, jfloat, jfloat, jfloat)>("measure");

  folly::dynamic localData = folly::dynamic::object("documentId", static_cast<double>(document->id))(
      "fontScale", static_cast<double>(fontSizeMultiplier));
  auto localDataMap = ReadableNativeMap::newObjectCxxArgs(std::move(localData));
  auto propsMap = ReadableNativeMap::newObjectCxxArgs(folly::dynamic::object());
  auto componentName = make_jstring("DownstreamMarkdownView");

  const jlong packed = measure(fabricUIManager, surfaceId, componentName.get(),
                               reinterpret_cast<ReadableMap::javaobject>(localDataMap.get()),
                               reinterpret_cast<ReadableMap::javaobject>(propsMap.get()), nullptr, 0.0f,
                               maximumWidth, 0.0f, std::numeric_limits<Float>::infinity());
  return yogaMeassureToSize(packed);
}

} // namespace facebook::react
