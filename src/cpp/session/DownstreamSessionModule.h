#pragma once

#if __has_include(<ReactCodegen/DownstreamSpecJSI.h>)
#include <ReactCodegen/DownstreamSpecJSI.h>
#else
#include <DownstreamSpecJSI.h>
#endif

#include <memory>
#include <string>
#include <unordered_set>

namespace downstream {
struct SessionEvent;
struct TraceEvent;
}

namespace facebook::react {

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
   * `mathFlags` is the `mathFlags` prop from `src/math.ts`, carried unchanged so
   * one number crosses the bridge rather than a set of booleans that JavaScript
   * would have to keep in step with the native bits. Applied by a separate
   * `setMath` below rather than here, so this method has no reason to know what
   * the bits mean.
   */
  void create(jsi::Runtime &rt, std::string sessionId, std::string mode, double mathFlags);
  void append(jsi::Runtime &rt, std::string sessionId, std::string chunk, double generation, double sequence);
  void reset(jsi::Runtime &rt, std::string sessionId, std::string source, double generation);
  void finish(jsi::Runtime &rt, std::string sessionId, double generation, double sequence);
  void dispose(jsi::Runtime &rt, std::string sessionId);
  void setTracing(jsi::Runtime &rt, std::string sessionId, bool enabled);
  void setPreview(jsi::Runtime &rt, std::string sessionId, double features);

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
