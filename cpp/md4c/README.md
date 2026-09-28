# Vendored md4c

These files come from [md4c](https://github.com/mity/md4c) at commit
`902b1bc9172ab45a7ea2539c9db4496ead77daa8` (0.6.0), the same revision as the
fixture oracle in `benchmarks/fixtures/manifest.json`. They are unmodified.
md4c is MIT licensed; see `LICENSE.md`.

| File | Used by |
| --- | --- |
| `md4c.c`, `md4c.h` | The native parser on iOS and Android |
| `entity.c`, `entity.h` | HTML entity decoding in text and link targets |
| `md4c-html.c`, `md4c-html.h` | Host tests only (`cpp/tests`), to check the vendored parser against the oracle HTML |

To update, copy the same files from the new revision, update the oracle
revision in the fixture manifest, and run `python3 cpp/tests/test_engine.py`.
