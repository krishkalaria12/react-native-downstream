#pragma once

#include "Document.h"

#include <cstddef>
#include <cstdint>

namespace downstream {

/// The lexer module's token ids, in its own order, onto the engine's token
/// classes. The module names about seventy tokens and the palette has
/// seventeen classes, so the choices follow GitHub's light and dark themes,
/// which the palette is: whatever those themes color alike shares a class here.
///
/// Punctuation is Plain rather than Operator. The two share the text color, and
/// a Plain run is one the platforms never see.
inline constexpr TokenClass kTokenClasses[] = {
    TokenClass::Plain,        // none
    TokenClass::Attribute,    // attribute
    TokenClass::Attribute,    // attribute.jsx
    TokenClass::Constant,     // boolean
    TokenClass::Comment,      // comment
    TokenClass::Comment,      // comment.doc
    TokenClass::Constant,     // constant
    TokenClass::Constant,     // constant.builtin
    TokenClass::Type,         // constructor: a Haskell or OCaml data constructor names a type
    TokenClass::Deleted,      // diff.minus
    TokenClass::Inserted,     // diff.plus
    TokenClass::Plain,        // embedded: the host language's text inside a template
    TokenClass::Plain,        // emphasis: markup weight has no color of its own
    TokenClass::Plain,        // emphasis.strong
    TokenClass::Function,     // function
    TokenClass::Function,     // function.definition
    TokenClass::Function,     // function.method
    TokenClass::Keyword,      // keyword
    TokenClass::Keyword,      // keyword.control
    TokenClass::Keyword,      // keyword.declaration
    TokenClass::Keyword,      // keyword.import
    TokenClass::Keyword,      // keyword.operator: typeof, instanceof, and, not; GitHub colors them as keywords
    TokenClass::Constant,     // label: goto labels and Rust lifetimes
    TokenClass::Type,         // namespace
    TokenClass::Number,       // number
    TokenClass::Operator,     // operator
    TokenClass::Plain,        // predictive
    TokenClass::Preprocessor, // preproc
    TokenClass::Plain,        // primary
    TokenClass::Property,     // property
    TokenClass::Property,     // property.json_key
    TokenClass::Plain,        // punctuation
    TokenClass::Plain,        // punctuation.bracket
    TokenClass::Plain,        // punctuation.bracket.html
    TokenClass::Plain,        // punctuation.bracket.jsx
    TokenClass::Plain,        // punctuation.delimiter
    TokenClass::Plain,        // punctuation.delimiter.html
    TokenClass::Plain,        // punctuation.delimiter.jsx
    TokenClass::Plain,        // punctuation.list_marker
    TokenClass::Plain,        // punctuation.markup
    TokenClass::Plain,        // punctuation.special
    TokenClass::Tag,          // selector: a CSS type selector names an element
    TokenClass::Attribute,    // selector.class
    TokenClass::Attribute,    // selector.id
    TokenClass::String,       // string
    TokenClass::String,       // string.escape
    TokenClass::Regex,        // string.regex
    TokenClass::String,       // string.special
    TokenClass::Constant,     // string.special.symbol: Ruby and Elixir symbols
    TokenClass::Tag,          // tag
    TokenClass::Tag,          // tag.doctype
    TokenClass::Tag,          // tag.jsx
    TokenClass::Plain,        // text.jsx
    TokenClass::String,       // text.literal: inline code and verbatim text in markup
    TokenClass::Heading,      // title
    TokenClass::Type,         // type
    TokenClass::Type,         // type.builtin
    TokenClass::Type,         // type.class
    TokenClass::Plain,        // variable
    TokenClass::Plain,        // variable.parameter: GitHub leaves parameters in the text color
    TokenClass::Constant,     // variable.special: this, self, super; GitHub colors them as constants
    TokenClass::Constant,     // enum
    TokenClass::Plain,        // hint
    TokenClass::Comment,      // keyword.jsdoc: these three sit inside a doc comment
    TokenClass::String,       // link_text
    TokenClass::String,       // link_uri
    TokenClass::Attribute,    // selector.pseudo
    TokenClass::Tag,          // tag.component.jsx
    TokenClass::Comment,      // type.jsdoc
    TokenClass::Comment,      // variable.jsdoc
    TokenClass::Constant,     // variant: OCaml polymorphic variants
    TokenClass::Plain,        // background: theme-only, never emitted
    TokenClass::Plain,        // foreground: theme-only, never emitted
};

inline constexpr size_t kTokenIdCount = sizeof(kTokenClasses) / sizeof(kTokenClasses[0]);
static_assert(kTokenIdCount == 73, "one class per token the module names");

/// The class for one record's token id. An id this table does not know, which
/// only a module newer than the table could emit, stays plain.
inline TokenClass tokenClassFor(uint32_t id) {
  return id < kTokenIdCount ? kTokenClasses[id] : TokenClass::Plain;
}

} // namespace downstream
