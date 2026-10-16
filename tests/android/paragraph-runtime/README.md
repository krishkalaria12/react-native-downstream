This fixture builds an isolated instrumentation APK from production paragraph layout, `TextSegmentView`, accessibility, reveal and caret sources. It extracts the current production `TextSegment`, `CodeSegment`, `PrecomputedTexts` and link hit-test declarations into the fixture compilation. It uses real `StaticLayout`, `Canvas`, bitmap pixels, `PrecomputedText`, accessibility nodes and main-thread `TextView` methods. Test-only collaborators replace the React host, theme records and unsupported interactive features; Android framework text and drawing APIs are never stubbed. The page-export checks compile production `SnapshotText`, `PagedTextBridge` and `PagedText` into the fixture; a small test-only JNI wrapper creates and retains snapshots. The production bridge performs UTF-16 transfer and weak-cache lookup on ART.

Run against an already connected device:

```sh
ANDROID_SERIAL=emulator-5554 node tests/android/paragraph-runtime/run.mjs
```

Or launch the existing test AVD:

```sh
DOWNSTREAM_PARAGRAPH_AVD=downstream-test node tests/android/paragraph-runtime/run.mjs
```

The runner uses the installed SDK's `aapt2`, `d8`, `apksigner`, an NDK, a Kotlin compiler and a cached Kotlin 2.2-compatible standard library. `ANDROID_HOME` and `DOWNSTREAM_KOTLIN_STDLIB` can override their locations. It installs only `com.downstream.paragraphruntime` and removes that test package when finished. An emulator launched by the runner is stopped afterward; an existing device or emulator is left running.

Set `DOWNSTREAM_PARAGRAPH_RESULTS` to save the successful instrumentation JSON to an existing output directory. The fixture checks live greedy geometry and pixels, selection paths and hit-test geometry, minimum line heights, retained snapshots, concurrent preparation, independent interleaved append streams, Unicode fallback, actual mounted view assignment/drawing/accessibility/reveal, selection and settlement, oversized suffix fallback, zero-width remount and return to a warm width after cold-width fallback.

The benchmark performs one excluded warm-up round and three measured ABBA rounds at each size. Full `SIMPLE` and incremental layouts receive identical prefixes, paint and width. A full `HIGH_QUALITY` comparator measures the canonical wrapping policy separately. CPU timings include snapshot construction and layout creation; they exclude drawing, JNI/native parsing, React and frame presentation. Shaped-character counters describe the characters requested from `StaticLayout`, rather than measured internal font-engine operations.

Emulator CPU timings describe this fixture on that emulator. They are not physical-device frame-rate measurements.

The paged-text checks compare actual transferred UTF-16 units with full-prefix transfer, verify shared Java page identity, live code-model ownership across GC and collection after model release, surrogate pairs across page boundaries, retained snapshots, replacement/reset, concurrent reads, weak-cache collection and bounded eviction. Canonical `TextSegmentView` (the code card's body view) geometry, pixels, accessibility text and selection are compared with an ordinary String. The framework span builder still copies its Java character buffer; the page bridge removes the native contiguous copy and repeated JNI transfer of unchanged pages.

The paged fixture exercises the production bridge helper and `PagedText`, plus production `CodeSegment`/`CodeLayout` and canonical `TextSegmentView`, with a minimal `RenderedDocument` collaborator. It does not execute the whole production `nativeReadDocument` metadata exporter or code-card chrome; those are covered here by Android source/JNI compilation and the existing metadata/segment contracts, rather than this raster test.

Set `DOWNSTREAM_PAGED_ONLY=1` to run only the JNI/paged-text checks. This excludes the unchanged paragraph layout regression checks and timing benchmark; the results explicitly record `paragraphChecksSkipped`.

Set `DOWNSTREAM_REVEAL_ONLY=1` to run only reveal drawing checks. These compare production `TextSegmentView` pixels with the previous bucket drawing algorithm on both hardware and software canvases. They cover prepared, ordinary and chunked text, nested part recordings, multiple alpha buckets, hidden text, padding, inset and scrolled viewports, changed content and completed frames. Chunked output is compared against the canonical full layout. A counting span verifies one source draw per animated hardware frame and the unchanged repeated draws on software. The JSON also reports median main-thread CPU time for 80 animated display-list recordings for production and legacy paths. Both receive 120 warmup frames, followed by five ABBA rounds with 80 frames per sample. There is no timing threshold.

Set `DOWNSTREAM_DECORATION_ONLY=1` to run only custom decoration drawing checks. Clipped hardware and software viewports must match retained whole-document custom decoration passes at the same clip for prepared, precomputed and chunked text. The source includes hundreds of links, strikes and inline-code backgrounds, long spans crossing viewports, mixed font sizes, backgrounds extending beyond their source lines and RTL text. A tracked source and layout verify that decoration span queries and selection paths are bounded by visible content and report the legacy selection-path count.

A code model retains its source pages because a weak JNI cache alone cannot keep them reusable after GC. `CodeSegment.sourcePages` uses the existing React Native `DoNotStrip` member annotation; React Native's consumer rules keep annotated members without enabling optimization, so the owner survives release shrinking too. Render-cache costs include the retained UTF-16 page characters.
