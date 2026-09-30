# Vendored Lexilla

Syntax lexers from [Lexilla](https://github.com/ScintillaOrg/lexilla), the
lexer library that powers Scintilla, Notepad++, and SciTE. Only the lexers the
first release needs are here, plus the lexlib support they compile against.

## Versions

| Upstream | Version | Where |
| --- | --- | --- |
| Lexilla | 5.5.4 (`lexilla554.tgz`) | `lexers/`, `lexlib/`, `SciLexer.h`, `Lexilla.h`, `License.txt` |
| Scintilla | 5.6.7 (`scintilla567.tgz`) | `ILexer.h`, `Sci_Position.h`, `Scintilla.h`, `ScintillaTypes.h` |

Both are under the same HPND-style license, reproduced in `License.txt`. The
license requires keeping the copyright and permission notice in copies, so
`License.txt` ships in the npm package and Lexilla is credited in the
repository README.

## Files taken

- `lexers/`: `LexBash`, `LexCPP`, `LexCSS`, `LexDiff`, `LexHTML`, `LexJSON`,
  `LexLua`, `LexMarkdown`, `LexPython`, `LexRuby`, `LexRust`, `LexSQL`,
  `LexTOML`, `LexYAML`. `LexHTML` also supplies the `xml` and `phpscript`
  lexers; only `xml` is registered.
- `lexlib/`: the support these lexers include or link: `Accessor`,
  `CharacterCategory`, `CharacterSet`, `DefaultLexer`, `InList`, `LexAccessor`,
  `LexerBase`, `LexerModule`, `PropSetSimple`, `StyleContext`, `WordList`, and
  the header-only `CatalogueModules`, `OptionSet`, `SparseState`, `StringCopy`,
  and `SubStyles`.

Not vendored: every other lexer, `LexerSimple`, the `access/` JSI wrapper, the
dynamic-library entry points in `src/`, and the test harness.

## Symbol isolation

An app may link its own Scintilla or Lexilla. Both projects rename their
namespaces with a preprocessor define, and this copy does the same, the way the
vendored md4c prefixes `md_parse`:

```
-DLexilla=downstream_lexilla -DScintilla=downstream_scintilla
```

The defines are in `src/android/src/main/jni/CMakeLists.txt`,
`Downstream.podspec`, and the host-test build in `src/cpp/tests/engine.test.mjs`.
They are private to these sources: nothing else in the repository may use them
without the same defines.

## Lexer names

`LanguageRegistry` registers a `Lexilla::CatalogueModules` holding the
`LexerModule` objects these lexers export:

| Name | Word lists | Language |
| --- | --- | --- |
| `cpp` | 0 keywords, 1 types | C family, JS, TS, Java, Go, Swift, Kotlin, C#, Dart, Scala, Objective C |
| `python` | 0 keywords | Python |
| `bash` | 0 keywords | Shell, console, Dockerfile |
| `json` | none | JSON, JSONC, JSON5 |
| `hypertext` | none | HTML |
| `xml` | none | XML, SVG, Vue, PHP |
| `css` | 0 keywords, 1 selectors | CSS, SCSS, Less |
| `sql` | 0 keywords, 1 keywords | SQL dialects |
| `rust` | 0 keywords, 1 types | Rust |
| `yaml` | 0 keywords | YAML |
| `ruby` | 0 keywords | Ruby |
| `diff` | none | Unified diffs |
| `markdown` | none | Markdown |
| `lua` | 0 keywords, 1..8 globals | Lua |
| `toml` | 0 keywords | TOML |

The keyword strings and the style-number maps live in the generated
`src/cpp/highlight/LanguageTables.cpp`, which `generate-tables.mjs` writes from
SciTE's `.properties` files. No build runs the generator.

## Upgrades

Bump the version here, replace the files, and run `pnpm test:native`. The
golden token fixtures in `src/cpp/tests/highlight_fixtures.mjs` are what
catches a lexer upgrade that changes output: every difference is either an
intentional improvement to record or a regression to fix.
