// JNI access to parsed documents for com.downstream.DownstreamNative.

#include "DocumentRegistry.h"
#include "Export.h"
#include "SegmentPlan.h"
#include "SessionTrace.h"
#include "Unicode.h"
#include "WorkerPool.h"
#include "theme/Theme.h"
#include "theme/ThemeLayout.h"
#include "theme/ThemeSlots.h"

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

// Interns a theme from the arrays the shadow node built, and returns its id.
//
// The id is what travels: the node's state carries it, and the platforms look
// the theme up again through the same registry the node interned it into. That
// is why nothing here returns a slot value, and why a theme costs one id rather
// than 232 numbers every time a frame's state crosses JNI.
extern "C" JNIEXPORT jlong JNICALL
Java_com_downstream_DownstreamNative_nativeInternTheme(JNIEnv *env,
                                                       jclass /*clazz*/,
                                                       jintArray slotIds,
                                                       jdoubleArray values,
                                                       jobjectArray families) {
  if (slotIds == nullptr || values == nullptr) {
    return 0;
  }
  const jsize slots = env->GetArrayLength(slotIds);
  const jsize count = env->GetArrayLength(values);
  std::vector<uint32_t> slots32(static_cast<size_t>(slots));
  std::vector<double> numbers(static_cast<size_t>(count));
  env->GetIntArrayRegion(slotIds, 0, slots, reinterpret_cast<jint *>(slots32.data()));
  env->GetDoubleArrayRegion(values, 0, count, numbers.data());
  std::vector<std::string> fonts;
  if (families != nullptr) {
    const jsize fontCount = env->GetArrayLength(families);
    fonts.reserve(static_cast<size_t>(fontCount));
    for (jsize index = 0; index < fontCount; index++) {
      auto name = static_cast<jstring>(env->GetObjectArrayElement(families, index));
      if (name == nullptr) {
        fonts.emplace_back();
        continue;
      }
      const char *utf = env->GetStringUTFChars(name, nullptr);
      fonts.emplace_back(utf != nullptr ? utf : "");
      if (utf != nullptr) {
        env->ReleaseStringUTFChars(name, utf);
      }
      env->DeleteLocalRef(name);
    }
  }
  // Interning is total: a malformed list comes back as the default theme rather
  // than as a theme with half its values, so this returns 0 for that case too.
  return static_cast<jlong>(ThemeRegistry::intern(slots32, numbers, fonts)->id());
}

// The theme's resolved values, in one crossing.
//
//   [0] DoubleArray, one entry per slot id: every metric, weight and flag
//   [1] IntArray,    the light half of every color, as ARGB
//   [2] IntArray,    the dark half of every color
//   [3] BooleanArray, which colors the theme actually set
//   [4] String[],    the families a font slot names
//
// One call rather than one per slot because the alternative is 232 crossings per
// rendering, and a rendering is built per segment per snapshot. The unset flag
// is separate from the color because transparent black is a color a theme can
// ask for, so zero cannot mean "not set".
//
// The metrics array carries a color slot's number too, which is meaningless: it
// is one entry per id so a caller can index by the id it already holds without
// a translation table of its own.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeThemeSnapshot(JNIEnv *env, jclass /*clazz*/, jlong id) {
  // An id the pool does not hold answers with the default theme, so a caller
  // with a stale id draws the platform's own look rather than nothing.
  Theme::Handle theme = ThemeRegistry::find(static_cast<uint64_t>(id));
  if (theme == nullptr) {
    theme = Theme::defaultTheme();
  }
  const size_t count = theme::kSlotIdCount;

  jdoubleArray numbers = env->NewDoubleArray(static_cast<jsize>(count));
  std::vector<jdouble> numberValues(count, 0);
  std::vector<jint> lightValues(count, 0);
  std::vector<jint> darkValues(count, 0);
  std::vector<jboolean> setValues(count, JNI_FALSE);
  // Which halves of a color the theme named, light in bit 0 and dark in bit 1,
  // so an appearance whose half is unset draws its own default.
  std::vector<jbyte> halfValues(count, 0);
  for (size_t slot = 1; slot < count; slot++) {
    // One flag per slot, whatever kind it is: a color that was set, a font that
    // names a family, a metric that was named. The renderer needs to tell
    // "this theme asked for 700" from "this theme said nothing", and the two are
    // otherwise the same number.
    setValues[slot - 1] = theme->slotSet(static_cast<uint32_t>(slot)) ? JNI_TRUE : JNI_FALSE;
    if (theme::kSlotKinds[slot - 1] == static_cast<uint8_t>(theme::SlotKind::Color)) {
      const Theme::Color &color = theme->color(static_cast<uint32_t>(slot));
      lightValues[slot - 1] = static_cast<jint>(color.light);
      darkValues[slot - 1] = static_cast<jint>(color.dark);
      halfValues[slot - 1] = static_cast<jbyte>((color.lightSet ? 1 : 0) | (color.darkSet ? 2 : 0));
      continue;
    }
    // A font slot's number is its index into the family list, -1 for the
    // system font. The family lives outside `number`, so reading that would
    // send 0 and point every unset font slot at the theme's first family.
    numberValues[slot - 1] = theme::kSlotKinds[slot - 1] == static_cast<uint8_t>(theme::SlotKind::Font)
        ? static_cast<jdouble>(theme->fontIndex(static_cast<uint32_t>(slot)))
        : theme->number(static_cast<uint32_t>(slot));
  }
  env->SetDoubleArrayRegion(numbers, 0, static_cast<jsize>(count), numberValues.data());

  jintArray light = toJavaArray(env, lightValues);
  jintArray dark = toJavaArray(env, darkValues);
  jbooleanArray set = env->NewBooleanArray(static_cast<jsize>(count));
  env->SetBooleanArrayRegion(set, 0, static_cast<jsize>(count), setValues.data());
  jbyteArray halves = env->NewByteArray(static_cast<jsize>(count));
  env->SetByteArrayRegion(halves, 0, static_cast<jsize>(count), halfValues.data());

  const std::vector<std::string> &families = theme->fontNames();
  jobjectArray familyArray = env->NewObjectArray(static_cast<jsize>(families.size()),
                                                  env->FindClass("java/lang/String"),
                                                  nullptr);
  for (size_t index = 0; index < families.size(); index++) {
    jstring name = env->NewStringUTF(families[index].c_str());
    env->SetObjectArrayElement(familyArray, static_cast<jsize>(index), name);
    // The array holds its own reference now, so the local one goes.
    env->DeleteLocalRef(name);
  }

  jobjectArray result = env->NewObjectArray(6, env->FindClass("java/lang/Object"), nullptr);
  env->SetObjectArrayElement(result, 0, numbers);
  env->SetObjectArrayElement(result, 1, light);
  env->SetObjectArrayElement(result, 2, dark);
  env->SetObjectArrayElement(result, 3, set);
  env->SetObjectArrayElement(result, 4, familyArray);
  env->SetObjectArrayElement(result, 5, halves);
  return result;
}

// The theme the platform font scale is capped at, which is the one theme value
// that changes measurement without changing a single drawing call.
//
// An id the pool does not hold means the theme is gone, which cannot happen
// while the node's state carries the id, so the answer falls back to the
// platform's own scale rather than to zero.
extern "C" JNIEXPORT jdouble JNICALL
Java_com_downstream_DownstreamNative_nativeFontScaleCap(JNIEnv * /*env*/,
                                                        jclass /*clazz*/,
                                                        jlong id,
                                                        jdouble platformScale) {
  const Theme::Handle theme = ThemeRegistry::find(static_cast<uint64_t>(id));
  if (theme == nullptr) {
    return platformScale;
  }
  return theme_layout::fontScale(*theme, platformScale);
}

/// Flattens blocks [from, to) of a document into one text and integer
/// tables, with block indices and text offsets relative to `from`. Layouts
/// must match NativeDocument.kt. Empty blocks get a zero-width space so they
/// occupy a line. Links are read one at a time by nativeReadLink.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeReadDocument(JNIEnv *env,
                                                        jclass /*clazz*/,
                                                        jlong id,
                                                        jint from,
                                                        jint to,
                                                        jlong themeId) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document || from < 0 || to < from || static_cast<size_t>(to) > document->blocks.size()) {
    return nullptr;
  }
  // A document the theme pool does not hold answers with the default theme, so a
  // stale id lays the blocks out with the platform's own metrics rather than
  // with nothing at all.
  Theme::Handle theme = ThemeRegistry::find(static_cast<uint64_t>(themeId));
  if (theme == nullptr) {
    theme = Theme::defaultTheme();
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
  // Each block's resolved typography and margins, in the fixed order
  // `theme_layout::kBlockLayoutStride` writes. Resolved here rather than in
  // Kotlin so the rules in ThemeLayout are the only copy of them.
  std::vector<double> layouts;
  const size_t layoutFields = static_cast<size_t>(to - from) * theme_layout::kBlockLayoutStride;
  // The trailing scalar carries the margin needed at the segment boundary,
  // avoiding a second document read just to flatten the preceding block.
  layouts.resize(layoutFields + 1);
  if (from > 0) {
    const auto previous = theme_layout::blockLayout(*theme, document->blocks[static_cast<size_t>(from - 1)]);
    layouts[layoutFields] = theme->number(previous.marginBottom);
  }

  // Block languages and fence titles are the only UTF-8 fields a platform reads
  // per block, so they ride in the string table and the blocks point into it.
  // The meta itself is parsed by the engine, so Kotlin reads the values rather
  // than scanning the info string a second way.
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
                                    intern(block.meta.title),
                                    static_cast<jint>(block.meta.startLine),
                                    block.meta.noLineNumbers ? 1 : 0,
                                });
    theme_layout::writeBlockLayout(
      *theme, theme_layout::blockLayout(*theme, block), layouts,
      static_cast<size_t>(index - static_cast<size_t>(from)) * theme_layout::kBlockLayoutStride);
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

  jobjectArray result = env->NewObjectArray(8, objectClass, nullptr);
  env->SetObjectArrayElement(result, 0, toJavaString(env, text));
  env->SetObjectArrayElement(result, 1, toJavaArray(env, blocks));
  env->SetObjectArrayElement(result, 2, toJavaArray(env, containers));
  env->SetObjectArrayElement(result, 3, toJavaArray(env, runs));
  env->SetObjectArrayElement(result, 4, stringArray);
  env->SetObjectArrayElement(result, 5, toJavaArray(env, cells));
  env->SetObjectArrayElement(result, 6, toJavaArray(env, tokens));
  jdoubleArray layoutArray = env->NewDoubleArray(static_cast<jsize>(layouts.size()));
  if (!layouts.empty()) {
    env->SetDoubleArrayRegion(layoutArray, 0, static_cast<jsize>(layouts.size()), layouts.data());
  }
  env->SetObjectArrayElement(result, 7, layoutArray);
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
  const std::string name = downloadName(block.meta, block.language,
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
