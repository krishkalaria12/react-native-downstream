#pragma once

#include "IncrementalParser.h"

#include <cstdint>
#include <functional>
#include <string>

namespace downstream {

using SnapshotCallback = std::function<void(const std::string &, uint64_t, uint64_t)>;

// Native sessions survive view unmounts. A worker publishes immutable documents
// only for the current generation and sequence.
class SessionRegistry {
public:
  static bool create(const std::string &id, ParseMode mode, SnapshotCallback callback);
  static bool append(const std::string &id, const std::string &chunk, uint64_t generation, uint64_t sequence);
  static bool reset(const std::string &id, const std::string &source, uint64_t generation);
  static bool finish(const std::string &id, uint64_t generation, uint64_t sequence);
  static void dispose(const std::string &id);
  static DocumentHandle snapshot(const std::string &id, uint64_t generation, uint64_t sequence);
};

} // namespace downstream
