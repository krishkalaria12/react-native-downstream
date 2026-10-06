// JNI access to parsed documents for com.downstream.DownstreamNative.

#include "DocumentRegistry.h"
#include "ImageSizeCache.h"
#include "Export.h"
#include "Reveal.h"
#include "SegmentPlan.h"
#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "Unicode.h"
#include "WorkerPool.h"
#include "theme/Theme.h"
#include "theme/ThemeLayout.h"
#include "math/MathEngine.h"
#include "math/MathSpeech.h"
#include "theme/ThemeSlots.h"

#include <dlfcn.h>
#include <jni.h>

#include <algorithm>
#include <memory>
#include <mutex>
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

namespace {

// The one live tracker, in a slot the JNI layer owns.
//
// Android holds documents and themes through the engine's own registries, and a
// tracker is per view rather than per document or per session, so there is
// nothing to intern it into. One slot with a compare-and-swap is what keeps two
// views of the same controlled source from sharing each other's "already shown",
// which is the one piece of state a reveal tracker must never share.
std::mutex gRevealMutex;
std::vector<std::unique_ptr<RevealTracker>> gReveals;

RevealTracker *trackerFor(jlong handle) {
  if (handle < 0 || static_cast<size_t>(handle) >= gReveals.size()) {
    return nullptr;
  }
  return gReveals[static_cast<size_t>(handle)].get();
}

/// The options the platform's JavaScript layer resolved, read from the flat array
/// the shadow node already sends. The array is `[effect, split, duration, stagger,
/// maxBacklog, x1, y1, x2, y2, scale]`, so one field on the props carries the
/// whole timeline and the animator scale travels beside it rather than needing
/// its own prop.
RevealOptions optionsFromArray(JNIEnv *env, jdoubleArray timing) {
  RevealOptions options;
  if (timing == nullptr) {
    return options;
  }
  const jsize count = env->GetArrayLength(timing);
  if (count < 9) {
    return options;
  }
  std::vector<double> values(static_cast<size_t>(count));
  env->GetDoubleArrayRegion(timing, 0, count, values.data());
  options.effect = static_cast<RevealEffect>(static_cast<uint32_t>(values[0]));
  options.split = static_cast<RevealSplit>(static_cast<uint32_t>(values[1]));
  // A scale of 0 is the platform saying animations are off, which the plan turns
  // into no fade at all rather than a fade with no duration.
  const double scale = values[9];
  if (scale <= 0) {
    options.effect = RevealEffect::None;
    return options;
  }
  const auto scaled = [scale](double value) { return static_cast<float>(value * scale); };
  options.durationMs = scaled(values[2]);
  options.staggerMs = scaled(values[3]);
  options.maxBacklogMs = scaled(values[4]);
  const auto clamp = [](double value) { return static_cast<float>(std::min(1.0, std::max(0.0, value))); };
  options.easing = {clamp(values[5]), static_cast<float>(values[6]), clamp(values[7]),
                    static_cast<float>(values[8])};
  return options;
}

} // namespace

/// A tracker for one view, as a handle into this layer's table.
extern "C" JNIEXPORT jlong JNICALL
Java_com_downstream_DownstreamNative_nativeRevealCreate(JNIEnv * /*env*/, jclass /*clazz*/) {
  std::lock_guard<std::mutex> lock(gRevealMutex);
  for (size_t index = 0; index < gReveals.size(); index++) {
    if (!gReveals[index]) {
      gReveals[index] = std::make_unique<RevealTracker>();
      return static_cast<jlong>(index);
    }
  }
  gReveals.push_back(std::make_unique<RevealTracker>());
  return static_cast<jlong>(gReveals.size() - 1);
}

/// Releases one view's tracker. A recycled view calls this rather than clearing
/// it, so the slot can be handed to the next view without growing the table.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRevealDispose(JNIEnv * /*env*/, jclass /*clazz*/, jlong handle) {
  std::lock_guard<std::mutex> lock(gRevealMutex);
  RevealTracker *tracker = trackerFor(handle);
  if (tracker) {
    tracker->clear();
    gReveals[static_cast<size_t>(handle)].reset();
  }
}

/// Starts a view's "already shown" from the session's own record, so a message
/// scrolled back into view does not fade all of itself again. An empty `sessionId`
/// seeds nothing, which is the controlled case: a fresh view fades its first
/// non-empty document in full.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRevealSeed(JNIEnv *env,
                                                       jclass /*clazz*/,
                                                       jlong handle,
                                                       jstring sessionId) {
  if (sessionId == nullptr) {
    return;
  }
  const char *id = env->GetStringUTFChars(sessionId, nullptr);
  if (id == nullptr) {
    return;
  }
  const std::string session(id);
  env->ReleaseStringUTFChars(sessionId, id);
  std::lock_guard<std::mutex> lock(gRevealMutex);
  if (RevealTracker *tracker = trackerFor(handle)) {
    tracker->seed(SessionRegistry::shown(session));
  }
}

/// Aligns a newly applied document against the one this view last showed, and
/// returns the units still fading as two parallel arrays: `[block, start, end]`
/// per unit in `ranges`, and its start time in `starts`, both in the order the
/// engine scheduled them. A `null` result means nothing is fading and the
/// platform's ticker should not be running.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeRevealApply(JNIEnv *env,
                                                       jclass /*clazz*/,
                                                       jlong handle,
                                                       jlong documentId,
                                                       jdouble now,
                                                       jdoubleArray timing) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(documentId));
  if (!document) {
    return nullptr;
  }
  const RevealOptions options = optionsFromArray(env, timing);
  std::lock_guard<std::mutex> lock(gRevealMutex);
  RevealTracker *tracker = trackerFor(handle);
  if (tracker == nullptr) {
    return nullptr;
  }
  const std::vector<RevealUnit> &units = tracker->apply(document, now, options);
  if (units.empty()) {
    return nullptr;
  }
  // The record of what is on screen is the session's, not the tracker's alone, so
  // another view of the same stream picks the fade up where this one left it.
  std::vector<jlong> ranges;
  std::vector<jdouble> starts;
  ranges.reserve(units.size() * 3);
  starts.reserve(units.size());
  for (const RevealUnit &unit : units) {
    ranges.insert(ranges.end(), {static_cast<jlong>(unit.block), static_cast<jlong>(unit.start),
                                 static_cast<jlong>(unit.end)});
    starts.push_back(unit.startAt);
  }
  jobjectArray result = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
  jlongArray rangeArray = env->NewLongArray(static_cast<jsize>(ranges.size()));
  env->SetLongArrayRegion(rangeArray, 0, static_cast<jsize>(ranges.size()), ranges.data());
  jdoubleArray startArray = env->NewDoubleArray(static_cast<jsize>(starts.size()));
  env->SetDoubleArrayRegion(startArray, 0, static_cast<jsize>(starts.size()), starts.data());
  env->SetObjectArrayElement(result, 0, rangeArray);
  env->SetObjectArrayElement(result, 1, startArray);
  return result;
}

/// Records the document as shown without scheduling anything, and tells the
/// session, so a view that is not animating still seeds the next one correctly.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRevealNoteShown(JNIEnv *env,
                                                           jclass /*clazz*/,
                                                           jlong handle,
                                                           jlong documentId,
                                                           jstring sessionId) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(documentId));
  if (!document) {
    return;
  }
  std::string session;
  if (sessionId != nullptr) {
    const char *id = env->GetStringUTFChars(sessionId, nullptr);
    if (id != nullptr) {
      session.assign(id);
      env->ReleaseStringUTFChars(sessionId, id);
    }
  }
  {
    std::lock_guard<std::mutex> lock(gRevealMutex);
    if (RevealTracker *tracker = trackerFor(handle)) {
      tracker->seed(document);
    }
  }
  // The session's own lock is taken after this layer's rather than inside it, so
  // the two can never be held in the other order.
  if (!session.empty()) {
    SessionRegistry::noteShown(session, document);
  }
}

/// Drops the units that have finished, and answers whether any are left, which is
/// what the platform's ticker asks each frame.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_downstream_DownstreamNative_nativeRevealPrune(JNIEnv *env,
                                                       jclass /*clazz*/,
                                                       jlong handle,
                                                       jdouble now,
                                                       jdoubleArray timing) {
  std::lock_guard<std::mutex> lock(gRevealMutex);
  RevealTracker *tracker = trackerFor(handle);
  if (tracker == nullptr) {
    return JNI_FALSE;
  }
  return tracker->prune(now, optionsFromArray(env, timing)) ? JNI_TRUE : JNI_FALSE;
}

/// Forgets what was shown, for a new generation, a rebind or a recycled view.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRevealClear(JNIEnv * /*env*/, jclass /*clazz*/, jlong handle) {
  std::lock_guard<std::mutex> lock(gRevealMutex);
  if (RevealTracker *tracker = trackerFor(handle)) {
    tracker->clear();
  }
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

/**
 * The theme's image metrics as [maxHeight, borderRadius, marginTop, marginBottom,
 * fallbackTextHeight].
 *
 * Resolved in C++ by `theme_layout::imageMetrics` rather than by reading five slots
 * on this side, because that function is what the shadow node measures with and a
 * second reader is how a measured image and a drawn image end up disagreeing about
 * a margin. One call per rendered document, not one per measurement: the theme does
 * not change under a snapshot.
 *
 * A missing theme is the default one rather than zeros, so a view that mounted
 * before its theme resolved gets the platform defaults instead of a picture with no
 * height.
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_downstream_DownstreamNative_nativeImageMetrics(JNIEnv *env, jclass /*clazz*/, jlong id) {
  // The default theme rather than zeros, the same way `nativeReadDocument` answers
  // a stale id: a view that mounted before its theme resolved gets the platform
  // defaults instead of a picture with no height. Held in a handle because
  // `defaultTheme` returns one, and dereferencing the temporary would dangle.
  Theme::Handle theme = ThemeRegistry::find(static_cast<uint64_t>(id));
  if (theme == nullptr) {
    theme = Theme::defaultTheme();
  }
  const ImageMetrics metrics = theme_layout::imageMetrics(*theme);
  const jdouble values[] = {metrics.maxHeight, metrics.borderRadius, metrics.marginTop, metrics.marginBottom,
                            metrics.fallbackTextHeight};
  jdoubleArray array = env->NewDoubleArray(5);
  if (array != nullptr) {
    env->SetDoubleArrayRegion(array, 0, 5, values);
  }
  return array;
}

/**
 * The theme's math metrics as [padding, textAlign].
 *
 * `textAlign` is 0 for left, 1 for center, and 2 for right, which is what the slot
 * holds; the platform reads the number rather than mapping a name, because the
 * view that centres a formula is the same one that measures it.
 */
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_downstream_DownstreamNative_nativeMathMetrics(JNIEnv *env, jclass /*clazz*/, jlong id) {
  Theme::Handle theme = ThemeRegistry::find(static_cast<uint64_t>(id));
  if (theme == nullptr) {
    theme = Theme::defaultTheme();
  }
  const jdouble values[] = {theme_layout::mathPadding(*theme), theme_layout::mathTextAlign(*theme),
                            theme_layout::mathMarginTop(*theme), theme_layout::mathMarginBottom(*theme)};
  jdoubleArray array = env->NewDoubleArray(4);
  if (array != nullptr) {
    env->SetDoubleArrayRegion(array, 0, 4, values);
  }
  return array;
}

namespace {

/// One display list, flattened for `MathDisplayList.kt`.
///
/// Doubles rather than floats because a color is a 32-bit word and a code point
/// can be 21 bits, and both have to come back exact. The order is the format;
/// `MathDisplayList.decode` reads it field for field:
///
///   width, height, depth,
///   glyphs, then per glyph: x, y, scale, font, code point, color
///   rules,  then per rule:  x, y, width, height, color, dashed
///   paths,  then per path:  x, y, fill, stroke width, color, commands,
///                           then per command: op, x, y, x1, y1, x2, y2
void appendDisplayList(std::vector<double> &out, const MathLayout &layout) {
  out.insert(out.end(), {layout.width, layout.height, layout.depth, static_cast<double>(layout.glyphs.size())});
  for (const MathGlyph &glyph : layout.glyphs) {
    out.insert(out.end(), {glyph.x, glyph.y, glyph.scale, static_cast<double>(glyph.font),
                           static_cast<double>(glyph.codePoint), static_cast<double>(glyph.color)});
  }
  out.push_back(static_cast<double>(layout.rules.size()));
  for (const MathRule &rule : layout.rules) {
    out.insert(out.end(), {rule.x, rule.y, rule.width, rule.height, static_cast<double>(rule.color),
                           rule.dashed ? 1.0 : 0.0});
  }
  out.push_back(static_cast<double>(layout.paths.size()));
  for (const MathPath &path : layout.paths) {
    out.insert(out.end(), {path.x, path.y, path.fill ? 1.0 : 0.0, path.strokeWidth, static_cast<double>(path.color),
                           static_cast<double>(path.commands.size())});
    for (const PathCommand &command : path.commands) {
      out.insert(out.end(), {static_cast<double>(command.op), command.x, command.y, command.x1, command.y1,
                             command.x2, command.y2});
    }
  }
}

} // namespace

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
  // Each block's math layout, in a fixed order, for the blocks that have one.
  // A separate table rather than more fields in `blocks` because these are
  // fractional ems and `blocks` is all integers: the stride is the only place that
  // order is written down, and it has to match NativeDocument.kt.
  std::vector<double> maths;
  // Every formula in the range, block and inline, as records of
  // [block, start, end, length, then `length` doubles of display list]. A formula
  // with no layout has length zero. Block indexes are relative to `from`, and
  // start and end are UTF-16 offsets into that block's own text.
  std::vector<double> formulas;
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
    for (const MathSpan &span : block.math) {
      formulas.insert(formulas.end(), {static_cast<double>(index - static_cast<size_t>(from)),
                                       static_cast<double>(span.start), static_cast<double>(span.end), 0.0});
      const size_t lengthAt = formulas.size() - 1;
      if (span.layout) {
        appendDisplayList(formulas, *span.layout);
        formulas[lengthAt] = static_cast<double>(formulas.size() - lengthAt - 1);
      }
    }
    if (!block.math.empty()) {
      // A block's formula covers its whole text, so only the first span is ever
      // read here; the parser promotes a lone display formula to its own block
      // rather than leaving two in one.
      const MathSpan &span = block.math.front();
      const jint tex = intern(span.tex);
      const jint error = span.error.empty() ? -1 : intern(span.error);
      // Width, height and depth are the layout's own ems with the baseline at
      // y = height, which is what `mathBlockHeight` on both platforms expects. A
      // formula with no layout is all zeros with `resolved` false, so "not laid
      // out yet" and "laid out and failed" stay distinguishable.
      maths.insert(maths.end(), {
                                  span.layout ? static_cast<double>(span.layout->width) : 0.0,
                                  span.layout ? static_cast<double>(span.layout->height) : 0.0,
                                  span.layout ? static_cast<double>(span.layout->depth) : 0.0,
                                  span.layout ? 1.0 : 0.0,
                                  span.display ? 1.0 : 0.0,
                                  static_cast<double>(tex),
                                  static_cast<double>(error),
                              });
    } else {
      // Every block has an entry, so the maths table is indexed by block the same
      // way `blocks` is. A block with no formula reads as unresolved, which is what
      // a caller that did not check the kind would then have to handle anyway.
      maths.insert(maths.end(), {0.0, 0.0, 0.0, 0.0, 0.0, -1.0, -1.0});
    }
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

  jobjectArray result = env->NewObjectArray(10, objectClass, nullptr);
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
  jdoubleArray mathArray = env->NewDoubleArray(static_cast<jsize>(maths.size()));
  if (!maths.empty()) {
    env->SetDoubleArrayRegion(mathArray, 0, static_cast<jsize>(maths.size()), maths.data());
  }
  env->SetObjectArrayElement(result, 8, mathArray);
  jdoubleArray formulaArray = env->NewDoubleArray(static_cast<jsize>(formulas.size()));
  if (!formulas.empty()) {
    env->SetDoubleArrayRegion(formulaArray, 0, static_cast<jsize>(formulas.size()), formulas.data());
  }
  env->SetObjectArrayElement(result, 9, formulaArray);
  return result;
}

/**
 * The document's images as [urls, alts, titles, records], where records is
 * [start, end, loadable, link] per image.
 *
 * Three string arrays and one number array rather than one array of objects,
 * because this is read once per segment that holds a picture and the objects
 * would be allocated on every read. Three parallel string arrays keep every
 * value the same JNI type, which is what the document read above does for its own
 * strings.
 *
 * What is not here is each image's state: that belongs to a view, not to the
 * document, and it travels in the layout map instead. Reading a document's images
 * is therefore the same for a view that has loaded them and one that has not,
 * which is why it is a document read rather than a state read.
 */
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_DownstreamNative_nativeReadImages(JNIEnv *env, jclass /*clazz*/, jlong id) {
  const DocumentHandle document = DocumentRegistry::find(static_cast<uint64_t>(id));
  if (!document) {
    return nullptr;
  }
  jclass objectClass = env->FindClass("java/lang/Object");
  jclass stringClass = env->FindClass("java/lang/String");
  const auto count = static_cast<jsize>(document->images.size());
  jobjectArray urls = env->NewObjectArray(count, stringClass, nullptr);
  jobjectArray alts = env->NewObjectArray(count, stringClass, nullptr);
  jobjectArray titles = env->NewObjectArray(count, stringClass, nullptr);
  std::vector<jint> records;
  records.reserve(document->images.size() * 4);
  for (size_t index = 0; index < document->images.size(); index++) {
    const Image &image = document->images[index];
    jstring url = toJavaString(env, utf8ToUtf16(image.url));
    jstring alt = toJavaString(env, image.alt);
    jstring title = toJavaString(env, utf8ToUtf16(image.title));
    env->SetObjectArrayElement(urls, static_cast<jsize>(index), url);
    env->SetObjectArrayElement(alts, static_cast<jsize>(index), alt);
    env->SetObjectArrayElement(titles, static_cast<jsize>(index), title);
    env->DeleteLocalRef(url);
    env->DeleteLocalRef(alt);
    env->DeleteLocalRef(title);
    records.push_back(static_cast<jint>(image.range.start));
    records.push_back(static_cast<jint>(image.range.end));
    // `loadable` is the engine's own policy answer, and the link is the enclosing
    // one, which is -1 for a picture that is not inside a link. Both travel here
    // rather than being decided again on this side: the alt text of a blocked
    // destination is a reader's decision, not the view's.
    records.push_back(image.loadable ? 1 : 0);
    records.push_back(image.link);
  }
  jobjectArray result = env->NewObjectArray(4, objectClass, nullptr);
  env->SetObjectArrayElement(result, 0, urls);
  env->SetObjectArrayElement(result, 1, alts);
  env->SetObjectArrayElement(result, 2, titles);
  env->SetObjectArrayElement(result, 3, toJavaArray(env, records));
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
                                    // The kind rather than a table and a code flag: the
                                    // plan has five kinds and two more were added, and a
                                    // boolean per kind is how the fifth one gets forgotten.
                                    static_cast<jlong>(segment.kind),
                                    // The image ordinal, counted by the plan so a view
                                    // matches a picture to its state without walking the
                                    // document. -1 for every other segment.
                                    static_cast<jlong>(segment.imageOrdinal),
                                    // The table ordinal, for the same reason and because a
                                    // reader's expand choice is keyed by it: a block index
                                    // counts rows and moves as a table streams.
                                    static_cast<jlong>(segment.tableOrdinal),
                                });
  }
  jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
  env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
  return array;
}

/// A formula's spoken form, as the engine's own verbalizer produces it.
///
/// Exposed rather than reimplemented in Kotlin, because `MathSpeech.cpp` is the one
/// definition of what `x^2 + y^2` reads as and a second copy would drift from it
/// in exactly the case that matters: a formula neither reader can make sense of.
extern "C" JNIEXPORT jstring JNICALL
Java_com_downstream_DownstreamNative_nativeMathSpeech(JNIEnv *env, jclass /*clazz*/, jstring tex,
                                                      jboolean display) {
  if (tex == nullptr) {
    return nullptr;
  }
  const char *utf8 = env->GetStringUTFChars(tex, nullptr);
  if (utf8 == nullptr) {
    return nullptr;
  }
  const std::string spoken = downstream::math::speak(utf8, display == JNI_TRUE);
  env->ReleaseStringUTFChars(tex, utf8);
  return toJavaString(env, utf8ToUtf16(spoken));
}

namespace {

/// The library `nativeRegisterMathEngine` named, opened on first use.
std::string &mathLibraryName() {
  static auto *name = new std::string();
  return *name;
}

/// Opens RaTeX and finds its three functions. Runs once, under the engine's lock,
/// the first time any document needs a formula laid out.
bool resolveRatex(downstream::MathEngineFunctions *out) {
  void *library = dlopen(mathLibraryName().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    return false;
  }
  // Never closed: the engine's function pointers point into it for the life of
  // the process, the way a linked library's would.
  return downstream::mathEngineFunctionsFromSymbols(dlsym(library, "ratex_parse_and_layout"),
                                                    dlsym(library, "ratex_free_display_list"),
                                                    dlsym(library, "ratex_get_last_error"), out);
}

} // namespace

/// Registers the math package's engine without loading it. `fontDirectory` is the
/// asset path the KaTeX fonts are under, which the Kotlin renderer reads.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRegisterMathEngine(JNIEnv *env, jclass /*clazz*/, jstring library,
                                                              jstring fontDirectory) {
  if (library == nullptr || fontDirectory == nullptr) {
    return;
  }
  const char *name = env->GetStringUTFChars(library, nullptr);
  const char *fonts = env->GetStringUTFChars(fontDirectory, nullptr);
  if (name != nullptr && fonts != nullptr) {
    mathLibraryName() = name;
    downstream::MathEngine::installResolver(resolveRatex, fonts);
  }
  if (name != nullptr) {
    env->ReleaseStringUTFChars(library, name);
  }
  if (fonts != nullptr) {
    env->ReleaseStringUTFChars(fontDirectory, fonts);
  }
}

/// A finished image load, recorded where the shadow node reads it.
///
/// The size is the decoded picture's, in pixels, which `imageBox` treats the way
/// CSS treats an image's natural size; zero means the load failed. Keyed by URL, so
/// every message showing the picture measures at its loaded height.
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_DownstreamNative_nativeRecordImage(JNIEnv *env, jclass /*clazz*/, jstring url, jint width,
                                                       jint height) {
  if (url == nullptr) {
    return;
  }
  const jsize length = env->GetStringLength(url);
  const jchar *chars = env->GetStringChars(url, nullptr);
  if (chars == nullptr) {
    return;
  }
  const std::string key = utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(chars), length));
  env->ReleaseStringChars(url, chars);
  const bool loaded = width > 0 && height > 0;
  downstream::ImageSizeCache::record(
      key, {loaded ? downstream::ImageState::Loaded : downstream::ImageState::Failed,
            loaded ? static_cast<float>(width) : 0.0f, loaded ? static_cast<float>(height) : 0.0f});
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

extern "C" JNIEXPORT jstring JNICALL
Java_com_downstream_DownstreamNative_nativeImageName(JNIEnv *env, jclass /*clazz*/, jstring url, jstring alt,
                                                     jstring mimeType, jstring configured) {
  const auto utf8 = [env](jstring value) -> std::string {
    if (value == nullptr) return {};
    const jchar *chars = env->GetStringChars(value, nullptr);
    if (chars == nullptr) return {};
    const std::string text = utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(chars),
                                                          env->GetStringLength(value)));
    env->ReleaseStringChars(value, chars);
    return text;
  };
  const std::string name = imageDownloadName(utf8(url), utf8(alt), utf8(mimeType), utf8(configured));
  return toJavaString(env, utf8ToUtf16(name));
}
