#pragma once

#if __has_include(<ReactCodegen/DownstreamSpecJSI.h>)
#include <ReactCodegen/DownstreamSpecJSI.h>
#else
#include <DownstreamSpecJSI.h>
#endif

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <jsi/jsi.h>
#include <react/bridging/Promise.h>

namespace downstream {
struct SessionEvent;
struct TraceEvent;
}

namespace facebook::react {

/// The slot structure codegen generates for `FenceSlot`.
using FenceSlotResult = NativeDownstreamSessionFenceSlot<std::int32_t, std::string, std::optional<std::string>,
                                                          std::string, bool, double>;

// The spec requires a method named create, which hides TurboModule::create.
// A using-declaration would make the spec's &T::create ambiguous.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"

/// The DownstreamSession TurboModule on iOS and Android. Sessions live in
/// downstream::SessionRegistry, where Fabric views find them by ID; this
/// module disposes the sessions it created when the JavaScript runtime goes
/// away. Methods run on the JavaScript thread and never wait for a parse.
class DownstreamSessionModule : public NativeDownstreamSessionCxxSpec<DownstreamSessionModule> {
public:
  explicit DownstreamSessionModule(std::shared_ptr<CallInvoker> jsInvoker);
  ~DownstreamSessionModule() override;

  /**
   * Starts a session.
   *
   * `parseFlags` is the `parseFlags` prop from `src/parseFlags.ts`, carried unchanged so
   * one number crosses the bridge rather than a set of booleans that JavaScript
   * would have to keep in step with the native bits. Applied by a separate
   * `setParse` below rather than here, so this method has no reason to know what
   * the bits mean.
   */
  void create(jsi::Runtime &rt, std::string sessionId, std::string mode, double parseFlags);
  void append(jsi::Runtime &rt, std::string sessionId, std::string chunk, double generation, double sequence);
  void reset(jsi::Runtime &rt, std::string sessionId, std::string source, double generation);
  void finish(jsi::Runtime &rt, std::string sessionId, double generation, double sequence);
  void dispose(jsi::Runtime &rt, std::string sessionId);
  void setTracing(jsi::Runtime &rt, std::string sessionId, bool enabled);
  void setPreview(jsi::Runtime &rt, std::string sessionId, double features);

  /**
   * Replaces the session's HTML tag policy, keeping the flags `create` set.
   *
   * `tags` is the flat list `src/allowedTags.ts` writes: `name,attribute,…` in
   * name order. A tag with an odd number of entries after its name, or one whose
   * attributes are not the same length as the last tag's, is a caller that is not
   * this library, and the whole list is refused rather than guessed at: a policy
   * half-read would allow an attribute nobody named.
   */
  void setParseTags(jsi::Runtime &rt, std::string sessionId, std::vector<std::string> tags);

  /**
   * Whether a math engine is linked into this process.
   *
   * So JavaScript can settle the `math` prop's default during its first render
   * rather than after a layout pass, which is the difference between a formula
   * laying out on the first frame and one visibly re-laying a frame later.
   *
   * The runtime is unused and is there because React Native's generated binding
   * requires a synchronous method on a TurboModule to take one; a synchronous
   * method is a property with braces or a method without arguments in the
   * TypeScript spec, and codegen rejects anything else.
   */
  bool mathAvailable(jsi::Runtime &rt);
  bool codeAvailable(jsi::Runtime &rt);

  /**
   * Puts `text` on the system clipboard.
   *
   * React Native core has no clipboard, and the link-safety confirmation offers
   * "Copy link", so a promise that keeps that promise. The platform installed the
   * writer when its module loaded; a binary without one resolves `false` rather
   * than rejecting, because a reader on a platform with no clipboard still gets
   * the confirmation and its Open button.
   *
   * As with the fence slots, the promise is created here and settled here: the
   * generated binding requires a promise-returning method to return one.
   */
  AsyncPromise<bool> copyText(jsi::Runtime &rt, std::string text);

  /**
   * The fenced blocks a controlled source holds that name a registered
   * language, as the renderer props JavaScript passes on.
   *
   * The one structure that crosses into JavaScript, and the only one that ever
   * will for a document: a parse tree, a block list and source ranges all stay
   * native, because a renderer needs the text of one fence and nothing else.
   * It is exactly what Streamdown's `CustomRendererProps` carries, so a
   * renderer written for the web needs only its component rewritten.
   *
   * A source already prepared answers from that preparation. A source up to
   * 16 KiB that no preparation holds is parsed here, on the thread that asked,
   * *without* highlighting, math layout or platform preparation, and the
   * answer is kept in a small LRU. This is the one place a parse runs on the
   * JavaScript thread, and it only moves earlier a wait layout was going to
   * make anyway: layout already waits for the same prepared bytes
   * (`kWaitForPreparedBytes`). A hint a preparation collected its layouts for
   * would never be prepared later, which is why this does not join the shared
   * preparation instead of answering from it.
   *
   * A larger source answers `{ pending: true }` rather than parsing: the view
   * draws code cards until `fenceSlotsAsync` resolves, which is what a large
   * source does today while its preparation runs.
   */
  jsi::Object fenceSlots(jsi::Runtime &rt, std::string source, double mathFlags,
                         std::vector<std::string> languages);

  /// The same answer for a source too large to parse synchronously, parsed on
  /// the worker pool. Resolves with the slots, empty when the source has none
  /// or does not parse.
  ///
  /// The promise is created here, on the JavaScript thread, and settled from the
  /// worker: React Native's generated binding requires a method whose spec
  /// return type is a promise to return one, and passing a promise's resolve
  /// and reject down as arguments is not a shape this binding can express.
  AsyncPromise<std::vector<FenceSlotResult>> fenceSlotsAsync(jsi::Runtime &rt, std::string source, double mathFlags,
                                                             std::vector<std::string> languages);
  /// The fenced blocks one published snapshot holds that name a registered
  /// language. Reads the snapshot the view is mounting for `sessionSequence`
  /// and walks its blocks: no parse, no worker, and safe to call from render
  /// for every snapshot a stream publishes.
  ///
  /// `sequence` matters as much as `generation`: it names the exact snapshot,
  /// so the slots a view renders are the slots of the document it is about to
  /// mount rather than of whichever snapshot arrived while it was rendering.
  jsi::Array sessionFenceSlots(jsi::Runtime &rt, std::string sessionId, double generation, double sequence,
                               std::vector<std::string> languages);
  /// The tag slots a controlled source holds, for the tag names the app
  /// registered. Only containers whose tag is named are reported: a tag with
  /// no renderer draws its children natively, so there is nothing to mount
  /// for it.
  jsi::Array tagSlots(jsi::Runtime &rt, std::string source, double parseFlags, std::vector<std::string> names, std::vector<std::string> tags);
  /// The tag slots of one published snapshot, which is the snapshot the view
  /// is mounting for `sessionSequence`. Walks blocks and never parses, so it
  /// is safe to call on every snapshot of a stream from render.
  jsi::Array sessionTagSlots(jsi::Runtime &rt, std::string sessionId, double generation, double sequence,
                             std::vector<std::string> names);

  jsi::Object sessionFenceSlotChanges(jsi::Runtime &rt, std::string sessionId, double generation,
                                     double sequence, std::vector<std::string> languages, double baseToken);
  jsi::Object sessionTagSlotChanges(jsi::Runtime &rt, std::string sessionId, double generation,
                                   double sequence, std::vector<std::string> names, double baseToken);

private:
  /// Lets session workers reach the module only while it is alive.
  struct Events;

  void deliver(const downstream::SessionEvent &event);
  void deliverTrace(const downstream::TraceEvent &event);
  void reportError(const std::string &sessionId, const std::string &code, const std::string &message,
                   double generation, double sequence);

  std::shared_ptr<Events> events_;
  std::unordered_set<std::string> sessions_;
};

#pragma clang diagnostic pop

} // namespace facebook::react
