#pragma once

#include "MarkdownParser.h"
#include "RenderPreparer.h"
#include "WorkerPool.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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
  ///
  /// `hint` is the requesting node's own layout hint, which carries its theme.
  /// Its width is zero, because a node that has just been created has not been
  /// measured yet; the theme is still prepared, at the widths views use now. The
  /// hint is prepared alongside the recent ones rather than instead of them, so
  /// the first view in a commit to show a source is not prepared for whichever
  /// theme the last measured view used.
  static std::shared_ptr<PreparedDocument> request(std::string_view source,
                                                   const LayoutHint &hint = LayoutHint());

  /// Drops cached results. For tests.
  static void clearCache();

  explicit PreparedDocument(std::string source);

  const std::string &source() const { return source_; }
  /// Notes one more layout this source should be prepared for. Called by
  /// `request`, and by a node whose theme changed under a source already parsed.
  void noteHint(const LayoutHint &hint);
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

  /// The hints this source should be prepared for, newest first and distinct.
  /// Guarded because a view can note one after the worker has started, which is
  /// what happens when a theme changes while a message is already parsed.
  std::vector<LayoutHint> hintsForPrepare() const;

  const std::string source_;
  ParseResult result_;
  std::shared_ptr<Task> task_;
  mutable std::mutex hintMutex_;
  mutable std::vector<LayoutHint> hints_;
};

} // namespace downstream
