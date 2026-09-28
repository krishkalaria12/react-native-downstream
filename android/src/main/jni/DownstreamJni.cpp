// JNI access to parsed documents for com.downstream.DownstreamNative.

#include "DocumentRegistry.h"
#include "Unicode.h"

#include <jni.h>

#include <string>
#include <vector>

using namespace downstream;

namespace {

jstring toJavaString(JNIEnv *env, const std::u16string &text) {
  return env->NewString(reinterpret_cast<const jchar *>(text.data()), static_cast<jsize>(text.size()));
}

jintArray toJavaArray(JNIEnv *env, const std::vector<jint> &values) {
  jintArray array = env->NewIntArray(static_cast<jsize>(values.size()));
  env->SetIntArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
  return array;
}

} // namespace

/// Flattens a document into one text and integer tables. Layouts must match
/// NativeDocument.kt. Empty blocks get a zero-width space so they occupy a
/// line.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeReadDocument(JNIEnv *env, jclass /*clazz*/, jlong id) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document) {
    return nullptr;
  }

  std::u16string text;
  std::vector<jint> blocks;
  std::vector<jint> containers;
  std::vector<jint> runs;
  std::vector<jint> links;
  std::vector<std::u16string> strings;

  for (size_t index = 0; index < document->blocks.size(); index++) {
    const Block &block = document->blocks[index];
    if (index > 0) {
      text.push_back(u'\n');
    }
    const auto start = static_cast<jint>(text.size());
    text.append(block.text.empty() ? std::u16string(u"\u200B") : block.text);
    blocks.insert(blocks.end(), {
                                    static_cast<jint>(block.kind),
                                    block.headingLevel,
                                    block.tableHeader ? 1 : 0,
                                    static_cast<jint>(block.spacingBefore),
                                    start,
                                    static_cast<jint>(text.size()),
                                    static_cast<jint>(containers.size() / 5),
                                    static_cast<jint>(block.containers.size()),
                                    static_cast<jint>(block.cells.size()),
                                });
    for (const Container &container : block.containers) {
      jint marker = -1;
      if (!container.marker.empty()) {
        marker = static_cast<jint>(strings.size());
        strings.push_back(container.marker);
      }
      containers.insert(containers.end(), {
                                              static_cast<jint>(container.kind),
                                              static_cast<jint>(container.id),
                                              container.showsMarker ? 1 : 0,
                                              static_cast<jint>(container.task),
                                              marker,
                                          });
    }
    for (const InlineRun &run : block.runs) {
      runs.insert(runs.end(), {
                                  start + static_cast<jint>(run.start),
                                  start + static_cast<jint>(run.end),
                                  static_cast<jint>(run.styles),
                                  run.link,
                              });
    }
  }

  for (const Link &link : document->links) {
    const auto url = static_cast<jint>(strings.size());
    strings.push_back(utf8ToUtf16(link.url));
    links.insert(links.end(), {url, static_cast<jint>(link.range.start), static_cast<jint>(link.range.end)});
  }

  jclass objectClass = env->FindClass("java/lang/Object");
  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray stringArray = env->NewObjectArray(static_cast<jsize>(strings.size()), stringClass, nullptr);
  for (size_t i = 0; i < strings.size(); i++) {
    jstring value = toJavaString(env, strings[i]);
    env->SetObjectArrayElement(stringArray, static_cast<jsize>(i), value);
    env->DeleteLocalRef(value);
  }

  jobjectArray result = env->NewObjectArray(6, objectClass, nullptr);
  env->SetObjectArrayElement(result, 0, toJavaString(env, text));
  env->SetObjectArrayElement(result, 1, toJavaArray(env, blocks));
  env->SetObjectArrayElement(result, 2, toJavaArray(env, containers));
  env->SetObjectArrayElement(result, 3, toJavaArray(env, runs));
  env->SetObjectArrayElement(result, 4, toJavaArray(env, links));
  env->SetObjectArrayElement(result, 5, stringArray);
  return result;
}
