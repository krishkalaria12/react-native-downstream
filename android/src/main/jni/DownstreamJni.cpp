// JNI access to parsed documents for com.downstream.DownstreamNative.

#include "DocumentRegistry.h"
#include "SegmentPlan.h"
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

/// Flattens blocks [from, to) of a document into one text and integer
/// tables, with block indices and text offsets relative to `from`. Layouts
/// must match NativeDocument.kt. Empty blocks get a zero-width space so they
/// occupy a line. Links are read one at a time by nativeReadLink.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeReadDocument(JNIEnv *env, jclass /*clazz*/, jlong id, jint from, jint to) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || from < 0 || to < from || static_cast<size_t>(to) > document->blocks.size()) {
    return nullptr;
  }

  std::u16string text;
  std::vector<jint> blocks;
  std::vector<jint> containers;
  std::vector<jint> runs;
  std::vector<jint> cells;
  std::vector<std::u16string> strings;

  for (auto index = static_cast<size_t>(from); index < static_cast<size_t>(to); index++) {
    const Block &block = document->blocks[index];
    if (index > static_cast<size_t>(from)) {
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
                                    static_cast<jint>(cells.size() / 3),
                                    static_cast<jint>(block.cells.size()),
                                    static_cast<jint>(block.table),
                                    isAdmonitionTitle(*document, index) ? 1 : 0,
                                });
    for (const TableCell &cell : block.cells) {
      cells.insert(cells.end(), {start + static_cast<jint>(cell.start), start + static_cast<jint>(cell.end),
                                 static_cast<jint>(cell.align)});
    }
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
  env->SetObjectArrayElement(result, 4, stringArray);
  env->SetObjectArrayElement(result, 5, toJavaArray(env, cells));
  return result;
}

/// The document's segments as [from, to, flags, identity] per segment, where
/// flags bit 0 is a table, bit 1 is committed, and bit 2 means the previous
/// block is a code block. Layout must match NativeDocument.kt.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_downstream_DownstreamNative_nativeSegmentPlan(JNIEnv *env, jclass /*clazz*/, jlong id) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document) {
    return nullptr;
  }
  std::vector<jlong> values;
  for (const SegmentPlan &segment : planSegments(*document)) {
    values.insert(values.end(), {
                                    static_cast<jlong>(segment.from),
                                    static_cast<jlong>(segment.to),
                                    (segment.table ? 1 : 0) | (segment.committed ? 2 : 0) |
                                        (segment.from > 0 && document->blocks[segment.from - 1].kind == BlockKind::CodeBlock
                                             ? 4
                                             : 0),
                                    static_cast<jlong>(document->blocks[segment.from].identity),
                                });
  }
  jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
  env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
  return array;
}

/// One link as [url, [rangeStart, rangeEnd]], or null.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeReadLink(JNIEnv *env, jclass /*clazz*/, jlong id, jint index) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || index < 0 || static_cast<size_t>(index) >= document->links.size()) {
    return nullptr;
  }
  const Link &link = document->links[static_cast<size_t>(index)];
  jclass objectClass = env->FindClass("java/lang/Object");
  jobjectArray result = env->NewObjectArray(2, objectClass, nullptr);
  jstring url = toJavaString(env, utf8ToUtf16(link.url));
  env->SetObjectArrayElement(result, 0, url);
  env->DeleteLocalRef(url);
  jintArray range = toJavaArray(env, {static_cast<jint>(link.range.start), static_cast<jint>(link.range.end)});
  env->SetObjectArrayElement(result, 1, range);
  env->DeleteLocalRef(range);
  return result;
}
