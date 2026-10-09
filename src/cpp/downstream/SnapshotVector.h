#pragma once

#include <memory>
#include <vector>

namespace downstream {

/// Immutable publication shares a vector rather than copying it for every
/// Block snapshot. Explicit builder operations detach before mutation.
template<class T> class SnapshotVector {
  std::shared_ptr<const std::vector<T>> values_;
  const std::vector<T> &read() const {
    static const std::vector<T> emptyValues;
    return values_ ? *values_ : emptyValues;
  }
public:
  SnapshotVector() = default;
  SnapshotVector &operator=(std::vector<T> values) {
    values_ = std::make_shared<const std::vector<T>>(std::move(values));
    return *this;
  }
  operator const std::vector<T> &() const { return read(); }
  size_t size() const { return read().size(); }
  bool empty() const { return read().empty(); }
  const T &operator[](size_t index) const { return read()[index]; }
  const T &back() const { return read().back(); }
  const T *data() const { return read().data(); }
  auto begin() const { return read().begin(); }
  auto end() const { return read().end(); }
  void clear() { values_.reset(); }
  void push_back(T value) {
    std::vector<T> next = read();
    next.push_back(std::move(value));
    *this = std::move(next);
  }
};

} // namespace downstream
