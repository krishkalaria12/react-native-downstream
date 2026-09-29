#pragma once

#include "MarkdownParser.h"
#include "WorkerPool.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace downstream {

/// A controlled `markdown` value, parsed and prepared for layout on the
/// worker pool. Fabric requests it when a shadow node receives a new source
/// and collects it at layout, so the parse and the platform text work overlap
/// with React's commit and with every other view in the same commit.
class PreparedDocument {
public:
  /// Returns the preparation of `source`, starting it on the pool when it is
  /// not already cached. Equal sources share one result, so a message that is
  /// remounted or rendered twice keeps its document and platform layout.
  static std::shared_ptr<PreparedDocument> request(std::string_view source);

  /// Drops cached results. For tests.
  static void clearCache();

  explicit PreparedDocument(std::string source);

  const std::string &source() const { return source_; }
  bool done() const { return task_->done(); }
  /// Runs the preparation on the calling thread if no worker has started it,
  /// otherwise waits for the worker to finish.
  void runOrWait() { task_->runOrWait(); }
  /// Calls `callback` once the result is available, on the thread that
  /// finished it, or immediately when it is already available.
  void onDone(std::function<void()> callback) { task_->onDone(std::move(callback)); }
  /// Valid once done() is true.
  const ParseResult &result() const { return result_; }

private:
  void prepare();

  const std::string source_;
  ParseResult result_;
  std::shared_ptr<Task> task_;
};

} // namespace downstream
