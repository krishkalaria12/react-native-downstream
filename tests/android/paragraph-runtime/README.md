This fixture builds an isolated instrumentation APK from production paragraph layout, `TextSegmentView`, accessibility, reveal and caret sources. It extracts the current production `TextSegment`, `PrecomputedTexts` and link hit-test declarations into the fixture compilation. It uses real `StaticLayout`, `Canvas`, bitmap pixels, `PrecomputedText`, accessibility nodes and main-thread `TextView` methods. Test-only collaborators replace the React/JNI host, theme records and unsupported interactive features; Android framework text and drawing APIs are never stubbed.

Run against an already connected device:

```sh
ANDROID_SERIAL=emulator-5554 node tests/android/paragraph-runtime/run.mjs
```

Or launch the existing test AVD:

```sh
DOWNSTREAM_PARAGRAPH_AVD=downstream-test node tests/android/paragraph-runtime/run.mjs
```

The runner uses the installed SDK's `aapt2`, `d8`, `apksigner`, a Kotlin compiler and a cached Kotlin 2.2-compatible standard library. `ANDROID_HOME` and `DOWNSTREAM_KOTLIN_STDLIB` can override their locations. It installs only `com.downstream.paragraphruntime` and removes that test package when finished. An emulator launched by the runner is stopped afterward; an existing device or emulator is left running.

Set `DOWNSTREAM_PARAGRAPH_RESULTS` to save the successful instrumentation JSON to an existing output directory. The fixture checks live greedy geometry and pixels, selection paths and hit-test geometry, minimum line heights, retained snapshots, concurrent preparation, independent interleaved append streams, Unicode fallback, actual mounted view assignment/drawing/accessibility/reveal, selection and settlement, oversized suffix fallback, zero-width remount and return to a warm width after cold-width fallback.

The benchmark performs one excluded warm-up round and three measured ABBA rounds at each size. Full `SIMPLE` and incremental layouts receive identical prefixes, paint and width. A full `HIGH_QUALITY` comparator measures the canonical wrapping policy separately. CPU timings include snapshot construction and layout creation; they exclude drawing, JNI/native parsing, React and frame presentation. Shaped-character counters describe the characters requested from `StaticLayout`, rather than measured internal font-engine operations.

Emulator CPU timings describe this fixture on that emulator. They are not physical-device frame-rate measurements.
