#pragma once

// Structural comparison over the render model.
//
// The incremental parser proves a committed prefix is still correct by
// comparing md4c's fresh output against what it committed, and the preview
// pass proves a repair changed nothing but the block it aimed at by the same
// comparison. Both answers have to agree exactly, so there is one
// implementation.

#include "Document.h"

namespace downstream::scan {

/// True when two blocks would lay out and paint identically. Identity and
/// sourceRange are excluded: they describe where a block came from, not what
/// it is. A formula's layout is excluded too, for the same reason and by
/// `MathSpan`'s own operator==.
bool sameBlock(const Block &a, const Block &b);

/// True when two tags have the same content, attributes and source ownership.
bool sameTag(const Tag &a, const Tag &b);

/// True when two images would draw identically. The load state is not an image
/// property: it belongs to a view and lives in `ImageStateLayout`, so two
/// records that name the same picture are the same record however differently
/// they have loaded.
bool sameImage(const Image &a, const Image &b);

/// True when two documents would lay out and paint identically. Document ids
/// differ between parses by construction, so they are not compared.
bool sameDocument(const Document &a, const Document &b);

} // namespace downstream::scan
