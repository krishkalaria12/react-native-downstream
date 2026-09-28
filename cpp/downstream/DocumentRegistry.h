#pragma once

#include "Document.h"

namespace downstream {

/// Lets platform code that cannot hold a C++ pointer (Android views and view
/// managers) look up a live document by id. Holds weak references only; the
/// shadow node state owns the document.
class DocumentRegistry {
public:
  static void add(const DocumentHandle &document);
  static DocumentHandle find(uint64_t id);
};

} // namespace downstream
