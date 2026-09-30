// JNI access to parsed documents for com.downstream.DownstreamNative.

#include "DocumentRegistry.h"
#include "Export.h"
#include "SegmentPlan.h"
#include "SessionTrace.h"
#include "Unicode.h"
#include "WorkerPool.h"

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

constexpr size_t kInlineExportBytes = 64 * 1024;

// The pool is shared with preparation, which may already have attached this
// thread. Attach new workers as daemons and detach if a pool thread ever exits.
JNIEnv *exportWorkerEnv(JavaVM *vm) {
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char *>("ds-export"), nullptr};
  if (vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) return nullptr;
  thread_local struct Detach {
    JavaVM *vm;
    ~Detach() { vm->DetachCurrentThread(); }
  } detach{vm};
  (void)detach;
  return env;
}

void deliverControlExport(JNIEnv *env, jobject callback, jmethodID method, const DocumentHandle &document,
                          size_t from, size_t to, ExportFormat format, char16_t separator, bool includeHtml,
                          const char *failure = nullptr) {
  if (env->PushLocalFrame(4) < 0) {
    env->ExceptionClear();
    return;
  }
  jstring text = nullptr;
  jstring html = nullptr;
  try {
    if (!failure) {
      text = toJavaString(env, exportBlocks(*document, from, to, format, separator));
      if (includeHtml && !env->ExceptionCheck()) {
        html = toJavaString(env, exportBlocks(*document, from, to, ExportFormat::Html, separator));
      }
    }
  } catch (...) {
    failure = "The block could not be exported.";
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    failure = "There was not enough memory to export the block.";
  }
  jstring error = failure ? env->NewStringUTF(failure) : nullptr;
  if (!env->ExceptionCheck()) env->CallVoidMethod(callback, method, text, html, error);
  // A worker has no Java caller to receive a callback exception. Always clear
  // it before returning this thread to the shared pool.
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->PopLocalFrame(nullptr);
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeTraceDocument(JNIEnv *env, jclass /*clazz*/, jlong id, jstring event) {
  if (!event) return;
  const char *name = env->GetStringUTFChars(event, nullptr);
  if (name) {
    downstream::SessionTrace::recordDocument(static_cast<uint64_t>(id), name);
    env->ReleaseStringUTFChars(event, name);
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_downstream_DownstreamNative_nativeIsTracingDocument(JNIEnv * /*env*/, jclass /*clazz*/, jlong id) {
  return downstream::SessionTrace::enabledDocument(static_cast<uint64_t>(id)) ? JNI_TRUE : JNI_FALSE;
}

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
  // Syntax tokens as [start, end, class] per run, with offsets shifted into
  // the flattened text like the inline runs above. Layout must match
  // NativeDocument.kt.
  std::vector<jint> tokens;
  std::vector<std::u16string> strings;

  // Block languages are the only UTF-8 fields a platform reads per block, so
  // they ride in the string table and the blocks point into it.
  auto intern = [&strings](const std::string &value) -> jint {
    if (value.empty()) {
      return -1;
    }
    strings.push_back(utf8ToUtf16(value));
    return static_cast<jint>(strings.size() - 1);
  };

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
                                    intern(block.language),
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
    for (const TokenRun &run : block.tokens) {
      tokens.insert(tokens.end(), {
                                    start + static_cast<jint>(run.start),
                                    start + static_cast<jint>(run.end),
                                    static_cast<jint>(run.token),
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

  jobjectArray result = env->NewObjectArray(7, objectClass, nullptr);
  env->SetObjectArrayElement(result, 0, toJavaString(env, text));
  env->SetObjectArrayElement(result, 1, toJavaArray(env, blocks));
  env->SetObjectArrayElement(result, 2, toJavaArray(env, containers));
  env->SetObjectArrayElement(result, 3, toJavaArray(env, runs));
  env->SetObjectArrayElement(result, 4, stringArray);
  env->SetObjectArrayElement(result, 5, toJavaArray(env, cells));
  env->SetObjectArrayElement(result, 6, toJavaArray(env, tokens));
  return result;
}

/// The document's segments as [from, to, flags, identity, settled, codeOrdinal]
/// per segment, where flags bit 0 is a table, bit 1 is committed, and bit 2 is
/// code. The settled count is the document's, repeated on every segment because
/// there is one per document, and the ordinal is the code block's own, or -1.
/// Layout must match NativeDocument.kt.
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
                                    (segment.isTable() ? 1 : 0) | (segment.committed ? 2 : 0) |
                                        (segment.isCode() ? 4 : 0),
                                    static_cast<jlong>(document->blocks[segment.from].identity),
                                    static_cast<jlong>(document->settledBlocks),
                                    static_cast<jlong>(segment.codeOrdinal),
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

/// Serializes blocks [from, to) of a document, as a String, or null when the
/// document is gone. The same call serves a clipboard write and a saved file,
/// so both platforms can only ever produce identical bytes.
extern "C" JNIEXPORT jstring JNICALL Java_com_downstream_DownstreamNative_nativeExport(
    JNIEnv *env, jclass /*clazz*/, jlong id, jint from, jint to, jint format, jint separator) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || from < 0 || to < from || static_cast<size_t>(to) > document->blocks.size()) {
    return nullptr;
  }
  if (format < 0 || format > static_cast<jint>(ExportFormat::Html)) {
    return nullptr;
  }
  const char16_t unit = separator < 0 ? u',' : static_cast<char16_t>(separator);
  const std::u16string text =
      exportBlocks(*document, static_cast<size_t>(from), static_cast<size_t>(to), static_cast<ExportFormat>(format), unit);
  return toJavaString(env, text);
}

/// Captures a strong snapshot on the caller before any asynchronous work. The
/// registry itself is weak, so looking it up later on a worker is unsafe.
extern "C" JNIEXPORT void JNICALL Java_com_downstream_DownstreamNative_nativeExportControl(
    JNIEnv *env, jclass /*clazz*/, jlong id, jint from, jint to, jint kind, jint format, jint separator,
    jboolean includeHtml, jobject callback) {
  jclass callbackClass = env->GetObjectClass(callback);
  const jmethodID method = env->GetMethodID(callbackClass, "onResult",
                                          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  env->DeleteLocalRef(callbackClass);
  if (!method) return;
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || from < 0 || to <= from || static_cast<size_t>(to) > document->blocks.size()) {
    deliverControlExport(env, callback, method, document, 0, 0, ExportFormat::Text, u',', false,
                         "The block is gone.");
    return;
  }
  if (format < 0 || format > static_cast<jint>(ExportFormat::Html) ||
      static_cast<jint>(document->blocks[from].kind) != kind) {
    deliverControlExport(env, callback, method, document, 0, 0, ExportFormat::Text, u',', false,
                         "The control and the block it names disagree.");
    return;
  }
  size_t estimatedBytes = 0;
  for (size_t index = static_cast<size_t>(from); index < static_cast<size_t>(to) && estimatedBytes <= kInlineExportBytes;
       ++index) {
    const Block &block = document->blocks[index];
    if (block.text.size() > kInlineExportBytes / sizeof(char16_t) || block.cells.size() > kInlineExportBytes / 32) {
      estimatedBytes = kInlineExportBytes + 1;
      break;
    }
    estimatedBytes += block.text.size() * sizeof(char16_t) + block.cells.size() * 32 + 64;
  }
  const auto exportFormat = static_cast<ExportFormat>(format);
  const char16_t unit = separator < 0 ? u',' : static_cast<char16_t>(separator);
  if (estimatedBytes <= kInlineExportBytes) {
    deliverControlExport(env, callback, method, document, from, to, exportFormat, unit, includeHtml);
    return;
  }
  JavaVM *vm = nullptr;
  if (env->GetJavaVM(&vm) != JNI_OK) return;
  jobject retainedCallback = env->NewGlobalRef(callback);
  if (!retainedCallback) return;
  try {
    WorkerPool::shared().post([document, vm, retainedCallback, method, from, to, exportFormat, unit, includeHtml] {
      JNIEnv *worker = exportWorkerEnv(vm);
      // VM attachment failure leaves no legal JNIEnv for notification or
      // DeleteGlobalRef. Releasing this reference then requires an already
      // attached dispatcher; the normal completion path below releases it once.
      if (!worker) return;
      deliverControlExport(worker, retainedCallback, method, document, from, to, exportFormat, unit, includeHtml);
      worker->DeleteGlobalRef(retainedCallback);
    });
  } catch (...) {
    env->DeleteGlobalRef(retainedCallback);
    deliverControlExport(env, callback, method, document, 0, 0, exportFormat, unit, false,
                         "The export could not be queued.");
  }
}

/// The file name a code download saves as, for one block. `configured` is the
/// caller's base name and `fallback` the one the contract names.
extern "C" JNIEXPORT jstring JNICALL
Java_com_downstream_DownstreamNative_nativeDownloadName(JNIEnv *env, jclass /*clazz*/, jlong id, jint index,
                                                         jstring configured, jstring fallback) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || index < 0 || static_cast<size_t>(index) >= document->blocks.size()) {
    return nullptr;
  }
  const Block &block = document->blocks[static_cast<size_t>(index)];
  const char *configuredText = configured != nullptr ? env->GetStringUTFChars(configured, nullptr) : nullptr;
  const char *fallbackText = fallback != nullptr ? env->GetStringUTFChars(fallback, nullptr) : nullptr;
  const std::string name = downloadName(block.info, block.language,
                                        configuredText != nullptr ? configuredText : "",
                                        fallbackText != nullptr ? fallbackText : "file");
  if (configuredText != nullptr) {
    env->ReleaseStringUTFChars(configured, configuredText);
  }
  if (fallbackText != nullptr) {
    env->ReleaseStringUTFChars(fallback, fallbackText);
  }
  return toJavaString(env, utf8ToUtf16(name));
}

/// The file name a table download saves as.
extern "C" JNIEXPORT jstring JNICALL
Java_com_downstream_DownstreamNative_nativeTableName(JNIEnv *env, jclass /*clazz*/, jstring configured, jstring extension) {
  const char *configuredText = configured != nullptr ? env->GetStringUTFChars(configured, nullptr) : nullptr;
  const char *extensionText = extension != nullptr ? env->GetStringUTFChars(extension, nullptr) : nullptr;
  const std::string name = tableDownloadName(
      configuredText != nullptr ? configuredText : "", extensionText != nullptr ? extensionText : "txt");
  if (configuredText != nullptr) {
    env->ReleaseStringUTFChars(configured, configuredText);
  }
  if (extensionText != nullptr) {
    env->ReleaseStringUTFChars(extension, extensionText);
  }
  return toJavaString(env, utf8ToUtf16(name));
}
