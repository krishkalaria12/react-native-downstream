#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace downstream {

/// Immutable token publications share complete pages and replace only their
/// dirty suffix. Contiguous compatibility is materialized once, on demand.
template<class T> class SnapshotVector {
  static constexpr size_t kPageEntries = 64;
  static constexpr size_t kBranches = 32;
  struct Node {
    std::array<std::shared_ptr<const Node>, kBranches> children{};
    std::vector<T> page;
  };
  struct Storage {
    std::shared_ptr<const Node> root;
    size_t levels{0};
    size_t size{0};
    mutable std::mutex mutex;
    mutable std::shared_ptr<const std::vector<T>> flat;
    mutable uint64_t flattenedBytes{0};
  };
  std::shared_ptr<Storage> storage_;
  static size_t capacity(size_t levels) {
    size_t result = 1;
    while (levels-- > 0) result *= kBranches;
    return result;
  }
  static const std::vector<T> &pageAt(const Storage &storage, size_t index) {
    const Node *node = storage.root.get();
    for (size_t level = storage.levels; level > 0; level--) {
      node = node->children[(index / capacity(level - 1)) % kBranches].get();
    }
    return node->page;
  }

public:
  struct CopyWork {
    uint64_t payloadBytes{0};
    uint64_t indexBytes{0};
  };

private:
  static std::shared_ptr<const Node> put(std::shared_ptr<const Node> node, size_t levels,
                                       size_t index, std::vector<T> page, CopyWork &work) {
    auto next = node && levels > 0 ? std::make_shared<Node>(*node) : std::make_shared<Node>();
    work.indexBytes += sizeof(Node);
    if (levels == 0) next->page = std::move(page);
    else {
      const size_t child = (index / capacity(levels - 1)) % kBranches;
      next->children[child] = put(next->children[child], levels - 1, index, std::move(page), work);
    }
    return next;
  }
  /// Remove unreachable suffix pages, sharing every wholly retained subtree.
  static std::shared_ptr<const Node> prefix(std::shared_ptr<const Node> node, size_t levels,
                                          size_t pages, CopyWork &work) {
    if (pages == 0 || !node) return nullptr;
    if (pages >= capacity(levels)) return node;
    auto next = std::make_shared<Node>(*node);
    work.indexBytes += sizeof(Node);
    const size_t childCapacity = capacity(levels - 1);
    for (size_t child = 0; child < kBranches; child++) {
      const size_t from = child * childCapacity;
      if (from >= pages) next->children[child].reset();
      else if (pages - from < childCapacity) {
        next->children[child] = prefix(next->children[child], levels - 1, pages - from, work);
      }
    }
    return next;
  }

public:
  SnapshotVector() = default;
  SnapshotVector &operator=(std::vector<T> values) {
    auto next = std::make_shared<Storage>();
    next->size = values.size();
    next->flat = std::make_shared<const std::vector<T>>(std::move(values));
    storage_ = std::move(next);
    return *this;
  }
  size_t size() const { return storage_ ? storage_->size : 0; }
  bool empty() const { return size() == 0; }
  const T &operator[](size_t index) const {
    return storage_->root ? pageAt(*storage_, index / kPageEntries)[index % kPageEntries] : (*storage_->flat)[index];
  }
  const T &back() const { return (*this)[size() - 1]; }
  const std::vector<T> &flat() const {
    static const std::vector<T> emptyValues;
    if (!storage_) return emptyValues;
    std::lock_guard<std::mutex> lock(storage_->mutex);
    if (!storage_->flat) {
      auto values = std::make_shared<std::vector<T>>();
      values->reserve(size());
      for (size_t i = 0; i * kPageEntries < size(); i++) {
        const auto &page = pageAt(*storage_, i);
        values->insert(values->end(), page.begin(), page.end());
      }
      storage_->flattenedBytes = size() * sizeof(T);
      storage_->flat = std::move(values);
    }
    return *storage_->flat;
  }
  uint64_t flattenedBytes() const {
    if (!storage_) return 0;
    std::lock_guard<std::mutex> lock(storage_->mutex);
    return storage_->flattenedBytes;
  }
  bool sharesStorage(const SnapshotVector &other) const { return storage_ == other.storage_; }
  operator const std::vector<T> &() const { return flat(); }
  const T *data() const { return flat().data(); }

  class const_iterator {
    const SnapshotVector *owner_{nullptr};
    size_t index_{0};
  public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T *;
    using reference = const T &;
    const_iterator() = default;
    const_iterator(const SnapshotVector *owner, size_t index) : owner_(owner), index_(index) {}
    reference operator*() const { return (*owner_)[index_]; }
    pointer operator->() const { return &**this; }
    reference operator[](difference_type offset) const { return *(*this + offset); }
    const_iterator &operator++() { ++index_; return *this; }
    const_iterator operator++(int) { auto old = *this; ++*this; return old; }
    const_iterator &operator--() { --index_; return *this; }
    const_iterator operator--(int) { auto old = *this; --*this; return old; }
    const_iterator &operator+=(difference_type offset) { index_ += offset; return *this; }
    const_iterator &operator-=(difference_type offset) { index_ -= offset; return *this; }
    const_iterator operator+(difference_type offset) const { auto next = *this; return next += offset; }
    const_iterator operator-(difference_type offset) const { auto next = *this; return next -= offset; }
    friend const_iterator operator+(difference_type offset, const_iterator value) { return value += offset; }
    difference_type operator-(const_iterator other) const {
      return static_cast<difference_type>(index_) - static_cast<difference_type>(other.index_);
    }
    bool operator==(const_iterator other) const { return owner_ == other.owner_ && index_ == other.index_; }
    bool operator!=(const_iterator other) const { return !(*this == other); }
    bool operator<(const_iterator other) const { return index_ < other.index_; }
    bool operator>(const_iterator other) const { return other < *this; }
    bool operator<=(const_iterator other) const { return !(other < *this); }
    bool operator>=(const_iterator other) const { return !(*this < other); }
  };
  const_iterator begin() const { return {this, 0}; }
  const_iterator end() const { return {this, size()}; }
  void clear() { storage_.reset(); }

  /// Caller proves the retained entries are unchanged. No prefix comparison or
  /// flattening occurs; work counts copied entries separately from index nodes.
  CopyWork replaceSuffix(size_t retained, const T *suffix, size_t count) {
    if (retained > size()) throw std::out_of_range("snapshot vector prefix");
    if (retained == size() && count == 0) return {};
    if (retained == 0 && count == 0) {
      clear();
      return {};
    }
    CopyWork work;
    auto next = std::make_shared<Storage>();
    work.indexBytes += sizeof(Storage);
    if (retained == 0) {
      // An unfinished line can replace every token. There is no immutable
      // prefix to share, so preserve the contiguous publication fast path.
      next->size = count;
      next->flat = std::make_shared<const std::vector<T>>(suffix, suffix + count);
      work.payloadBytes = count * sizeof(T);
      storage_ = std::move(next);
      return work;
    }
    if (storage_ && storage_->root) {
      next->levels = storage_->levels;
      next->root = prefix(storage_->root, next->levels, retained / kPageEntries, work);
      next->size = retained - retained % kPageEntries;
    }
    // A flat initial publication is paged once. Subsequent publications reuse
    // every completed page, including across index depth changes.
    while (next->size < retained || count > 0) {
      const size_t index = next->size / kPageEntries;
      while (index >= capacity(next->levels)) {
        auto root = std::make_shared<Node>();
        root->children[0] = next->root;
        next->root = std::move(root);
        next->levels++;
        work.indexBytes += sizeof(Node);
      }
      std::vector<T> page;
      page.reserve(kPageEntries);
      while (next->size < retained && page.size() < kPageEntries) {
        page.push_back((*this)[next->size++]);
      }
      const size_t take = std::min(kPageEntries - page.size(), count);
      if (take > 0) page.insert(page.end(), suffix, suffix + take);
      next->size += take;
      count -= take;
      if (take > 0) suffix += take;
      work.payloadBytes += page.size() * sizeof(T);
      next->root = put(next->root, next->levels, index, std::move(page), work);
    }
    storage_ = std::move(next);
    return work;
  }
  void push_back(T value) { replaceSuffix(size(), &value, 1); }
};

} // namespace downstream
