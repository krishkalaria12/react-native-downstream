# Optional code package: native archive sizes

Measured on 2026-11-01 against `cc65ca1`, before the highlighter extraction.
These are matched, unstripped static engine archives compiled for each target.
They exclude React Native platform glue and are not APK/IPA download sizes or
loaded-memory measurements. The existing streaming benchmark results are unchanged.

| Target            | Before, bytes | Core now, bytes | Reduction, bytes | Reduction | Optional code archive, bytes |
| ----------------- | ------------: | --------------: | ---------------: | --------: | ---------------------------: |
| iOS arm64         |     2,765,976 |       2,205,504 |          560,472 |    20.26% |                      582,488 |
| Android arm64-v8a |     4,990,206 |       3,873,886 |        1,116,320 |    22.37% |                    1,159,012 |

The optional archive is additional when the app installs the code package. The
split adds a small adapter, so the combined archive sizes need not equal the
previous monolithic archive. App linking and stripping can change the final delta.

Both sides use `-Oz`, `-DNDEBUG`, hidden visibility, function/data sections, and
the same md4c symbol prefixes. iOS targets arm64 with deployment target 15.1 and
the iPhoneOS 27.2 SDK. Android targets arm64-v8a, API 24, with NDK 27.1.12297006.
The generated WASM lexer is byte-identical after its move, so its object was
compiled once per target and reused in the optional archive.

Reproduce with the installed Xcode and Android NDK:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  node scripts/measure-core-size.mjs cc65ca1 /tmp/downstream-core-size.json
```

The script selects the SDK from `DEVELOPER_DIR`, the Android SDK from
`ANDROID_HOME`, and NDK 27.1.12297006 unless `DOWNSTREAM_SIZE_NDK` is supplied.
The [recorded measurements](core-package-size.json) omit machine-local temporary
archive paths. The script's raw output includes those paths for inspection.
