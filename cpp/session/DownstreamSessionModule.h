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

  void create(jsi::Runtime &rt, std::string sessionId, std::string mode);
  void append(jsi::Runtime &rt, std::string sessionId, std::string chunk, double generation, double sequence);
  void reset(jsi::Runtime &rt, std::string sessionId, std::string source, double generation);
  void finish(jsi::Runtime &rt, std::string sessionId, double generation, double sequence);
  void dispose(jsi::Runtime &rt, std::string sessionId);

private:
  /// Lets session workers reach the module only while it is alive.
  struct Events;

  void deliver(const downstream::SessionEvent &event);
  void reportError(const std::string &sessionId, const std::string &code, const std::string &message,
                   double generation, double sequence);

  std::shared_ptr<Events> events_;
  std::unordered_set<std::string> sessions_;
};

#pragma clang diagnostic pop

} // namespace facebook::react
