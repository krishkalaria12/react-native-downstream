# Vendored md4c

These files come from [md4c](https://github.com/mity/md4c) at commit
`902b1bc9172ab45a7ea2539c9db4496ead77daa8` (0.6.0), the same revision as the
fixture oracle in `src/fixtures/manifest.json`. They are unmodified.
md4c is MIT licensed; see `LICENSE.md`.

| File | Used by |
| --- | --- |
| `md4c.c`, `md4c.h` | The native parser on iOS and Android |
| `entity.c`, `entity.h` | HTML entity decoding in text and link targets |
| `md4c-html.c`, `md4c-html.h` | Host tests only (`src/cpp/tests`), to check the vendored parser against the oracle HTML |

Builds rename the public symbols at compile time: `md_parse` becomes `downstream_md_parse` and `entity_lookup` becomes `downstream_entity_lookup`; the tests also rename `md_html`. Apps can link another md4c. Enriched Markdown vendors an older one, and without the prefix the linker can bind our calls to that copy. `MarkdownParser.cpp` fails to compile if the prefix is missing. The definitions live in `Downstream.podspec`, `src/android/src/main/jni/CMakeLists.txt`, and `src/cpp/tests/engine.test.mjs`.

Known issue: the check that a list item cannot begin with two blank lines reads
the last line record in md4c's block buffer as a block record, so whether a
list ends after two blank lines depends on a line's byte offset modulo 256.
`src/cpp/session/IncrementalParser.cpp` pads partial parses to full-parse offsets
so incremental output still matches a full parse.

To update, copy the same files from the new revision, update the oracle
revision in the fixture manifest, and run `pnpm test:native`.
