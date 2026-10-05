// A parse-only stand-in for folly::dynamic.
//
// React Native's own headers include <folly/dynamic.h>, and the ReactCommon
// sources are the only ones that do. Nothing here is ever compiled into a
// shipped binary: `pnpm test:fabric` runs the compiler in `-fsyntax-only` mode
// over the C++ this package adds to React Native's own build, and folly is not
// on the machine in a form this check could use.
//
// So this is a declaration of the members those headers name, and nothing more.
// A member that does not exist here fails the check for the wrong reason, which
// is the trade: a stub that grows is cheaper than vendoring folly to check three
// files.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace folly {

class dynamic {
 public:
  dynamic() = default;
  dynamic(std::nullptr_t) {}
  dynamic(bool) {}
  dynamic(int) {}
  dynamic(int64_t) {}
  dynamic(double) {}
  dynamic(const char *) {}
  dynamic(std::string) {}
  dynamic(const dynamic &) = default;
  dynamic(dynamic &&) = default;
  dynamic &operator=(const dynamic &) = default;
  dynamic &operator=(dynamic &&) = default;

  bool isNull() const;
  bool isBool() const;
  bool isInt() const;
  bool isNumber() const;
  bool isDouble() const;
  bool isString() const;
  bool isObject() const;
  bool isArray() const;
  bool empty() const;

  bool asBool() const;
  bool getBool() const;
  int64_t asInt() const;
  int64_t getInt() const;
  double asDouble() const;
  double getDouble() const;
  std::string asString() const;
  std::string getString() const;
  std::vector<dynamic> asVector() const;
  std::vector<dynamic> getVector() const;
  std::vector<std::pair<dynamic, dynamic>> items() const;

  const dynamic *get(const std::string &key) const;
  const dynamic &operator[](const std::string &key) const;
  template <typename K>
  const dynamic &at(const K &key) const;

  size_t size() const;
  void reserve(size_t count);
  void push_back(const dynamic &value);
  void clear();

  using iterator = std::vector<dynamic>::iterator;
  using const_iterator = std::vector<dynamic>::const_iterator;
  iterator begin();
  const_iterator begin() const;
  iterator end();
  const_iterator end() const;
  iterator erase(const_iterator first, const_iterator last);

 private:
  std::vector<dynamic> storage_;
};

} // namespace folly