#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace downstream {

/// Builder strings use copy-on-write; proven append snapshots use immutable
/// 1 Ki-unit pages behind a persistent 32-way index. A render/export boundary
/// can request contiguous text once, without making parser appends flatten it.
class SnapshotText {
  friend void appendUtf8(SnapshotText &out, std::string_view bytes);
  static constexpr size_t kPageUnits = 1024;
  static constexpr size_t kBranches = 32;
  struct Node {
    std::array<std::shared_ptr<const Node>, kBranches> children{};
    std::u16string page;
  };
  struct Pages {
    std::shared_ptr<const Node> root;
    size_t levels{0};
    size_t size{0};
    mutable std::mutex mutex;
    mutable std::shared_ptr<const std::u16string> flat;
    mutable uint64_t flattenedBytes{0};
  };
  std::shared_ptr<std::u16string> builder_;
  std::shared_ptr<Pages> pages_;

  static size_t capacity(size_t levels) {
    size_t result = 1;
    for (size_t i = 0; i < levels; i++) result *= kBranches;
    return result;
  }
  static const Node &pageAt(const Pages &pages, size_t index) {
    const Node *node = pages.root.get();
    for (size_t level = pages.levels; level > 0; level--) {
      node = node->children[(index / capacity(level - 1)) % kBranches].get();
    }
    return *node;
  }
  static std::shared_ptr<const Node> put(std::shared_ptr<const Node> node, size_t levels,
                                       size_t index, std::u16string page, uint64_t &copied) {
    auto next = node && levels > 0 ? std::make_shared<Node>(*node) : std::make_shared<Node>();
    copied += sizeof(Node) + next->page.size() * sizeof(char16_t);
    if (levels == 0) {
      next->page = std::move(page);
    } else {
      const size_t child = (index / capacity(levels - 1)) % kBranches;
      next->children[child] = put(next->children[child], levels - 1, index, std::move(page), copied);
    }
    return next;
  }
  std::u16string &write() {
    if (pages_) {
      builder_ = std::make_shared<std::u16string>(flat());
      pages_.reset();
    } else if (!builder_) {
      builder_ = std::make_shared<std::u16string>();
    } else if (builder_.use_count() != 1) {
      builder_ = std::make_shared<std::u16string>(*builder_);
    }
    return *builder_;
  }

public:
  SnapshotText() = default;
  explicit SnapshotText(std::u16string text) : builder_(std::make_shared<std::u16string>(std::move(text))) {}
  explicit SnapshotText(const char16_t *text) : SnapshotText(std::u16string(text)) {}
  SnapshotText &operator=(std::u16string text) {
    builder_ = std::make_shared<std::u16string>(std::move(text));
    pages_.reset();
    return *this;
  }
  SnapshotText &operator=(const char16_t *text) { return *this = std::u16string(text); }
  size_t size() const { return pages_ ? pages_->size : builder_ ? builder_->size() : 0; }
  bool empty() const { return size() == 0; }
  char16_t operator[](size_t index) const {
    return pages_ ? pageAt(*pages_, index / kPageUnits).page[index % kPageUnits] : (*builder_)[index];
  }
  char16_t back() const { return (*this)[size() - 1]; }
  const std::u16string &flat() const {
    static const std::u16string emptyText;
    if (!pages_) return builder_ ? *builder_ : emptyText;
    std::lock_guard<std::mutex> lock(pages_->mutex);
    if (!pages_->flat) {
      auto text = std::make_shared<std::u16string>();
      text->reserve(pages_->size);
      for (size_t i = 0; i * kPageUnits < pages_->size; i++) text->append(pageAt(*pages_, i).page);
      pages_->flattenedBytes = text->size() * sizeof(char16_t);
      pages_->flat = std::move(text);
    }
    return *pages_->flat;
  }
  uint64_t flattenedBytes() const {
    if (!pages_) return 0;
    std::lock_guard<std::mutex> lock(pages_->mutex);
    return pages_->flattenedBytes;
  }
  operator const std::u16string &() const { return flat(); }
  operator std::u16string_view() const { return flat(); }
  const char16_t *data() const { return flat().data(); }
  const char16_t *c_str() const { return flat().c_str(); }
  auto begin() const { return flat().begin(); }
  auto end() const { return flat().end(); }
  std::u16string substr(size_t start, size_t count = std::u16string::npos) const { return flat().substr(start, count); }
  int compare(size_t start, size_t count, std::u16string_view other) const { return flat().compare(start, count, other); }
  size_t find(std::u16string_view needle, size_t start = 0) const { return flat().find(needle, start); }
  size_t find(char16_t needle, size_t start = 0) const { return flat().find(needle, start); }
  size_t rfind(std::u16string_view needle, size_t start = std::u16string::npos) const { return flat().rfind(needle, start); }
  void clear() { builder_.reset(); pages_.reset(); }
  void reserve(size_t size) { write().reserve(size); }
  void resize(size_t size) { write().resize(size); }
  void push_back(char16_t unit) { write().push_back(unit); }
  void pop_back() { write().pop_back(); }
  void append(std::u16string_view text) { write().append(text); }
  SnapshotText &operator+=(std::u16string_view text) { append(text); return *this; }
  void append(size_t count, char16_t unit) { write().append(count, unit); }
  template<class Iterator> void append(Iterator first, Iterator last) { write().append(first, last); }
  void assign(size_t count, char16_t unit) { write().assign(count, unit); }
  void assign(std::u16string_view text) { write().assign(text); }
  void assign(const char16_t *text, size_t count) { write().assign(text, count); }

  /// Returns storage copied, including page-index nodes. Old roots and flattened
  /// caches remain immutable, even when callers retain every prior snapshot.
  uint64_t appendSnapshot(std::u16string_view added) {
    if (added.empty()) return 0;
    auto next = std::make_shared<Pages>();
    uint64_t copied = sizeof(Pages);
    if (pages_) {
      next->root = pages_->root;
      next->levels = pages_->levels;
      next->size = pages_->size;
    } else if (builder_ && !builder_->empty()) {
      for (size_t start = 0; start < builder_->size(); start += kPageUnits) {
        const size_t index = start / kPageUnits;
        while (index >= capacity(next->levels)) {
          auto root = std::make_shared<Node>();
          root->children[0] = next->root;
          next->root = std::move(root);
          next->levels++;
          copied += sizeof(Node);
        }
        std::u16string page = builder_->substr(start, kPageUnits);
        copied += page.size() * sizeof(char16_t);
        next->root = put(next->root, next->levels, index, std::move(page), copied);
      }
      next->size = builder_->size();
    }
    while (!added.empty()) {
      const size_t index = next->size / kPageUnits;
      while (index >= capacity(next->levels)) {
        auto root = std::make_shared<Node>();
        root->children[0] = next->root;
        next->root = std::move(root);
        next->levels++;
        copied += sizeof(Node);
      }
      const size_t used = next->size % kPageUnits;
      std::u16string page = used ? pageAt(*next, index).page : std::u16string();
      copied += page.size() * sizeof(char16_t);
      const size_t take = std::min(kPageUnits - used, added.size());
      page.append(added.substr(0, take));
      copied += take * sizeof(char16_t);
      next->root = put(next->root, next->levels, index, std::move(page), copied);
      next->size += take;
      added.remove_prefix(take);
    }
    builder_.reset();
    pages_ = std::move(next);
    return copied;
  }
  friend bool operator==(const SnapshotText &a, const SnapshotText &b) {
    if (a.pages_ && a.pages_ == b.pages_) return true;
    if (!a.pages_ && !b.pages_ && a.builder_ == b.builder_) return true;
    return a.size() == b.size() && a.flat() == b.flat();
  }
  friend bool operator!=(const SnapshotText &a, const SnapshotText &b) { return !(a == b); }
  friend bool operator==(const SnapshotText &a, std::u16string_view b) { return a.size() == b.size() && a.flat() == b; }
  friend bool operator!=(const SnapshotText &a, std::u16string_view b) { return !(a == b); }
  friend bool operator==(std::u16string_view a, const SnapshotText &b) { return b == a; }
  friend bool operator!=(std::u16string_view a, const SnapshotText &b) { return !(b == a); }
};

} // namespace downstream
