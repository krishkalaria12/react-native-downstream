# iOS memory and layout follow-up

This pass investigates the iOS renderer without a physical-device performance baseline. It preserves the earlier Android work. UIKit simulator checks establish removed work, object lifetime and rendering correctness; they do not establish device RSS, CPU, energy or frame-deadline improvements.

## Avoid repeated offscreen traversal

`DSDocumentView.layoutSubviews` previously reconciled every native segment and traversed every view slot during ordinary layout, including deferred offscreen text. The existing viewport optimization bounded scrolling work but did not bound these repeated UIKit layout passes.

The view now tracks mounted segment indexes. A steady layout visits those indexes; reconciliation runs when the rendered document or exact cached frame array changes. The frame-cache key already includes width, chrome, reader choices, images, hosts and table layout. Unsupported or interactive content remains mounted and receives geometry updates, including outside the viewport. Selection and accessibility still restore the complete document.

The regression uses 3,200 paragraphs grouped into 200 native text segments. Restoring only the previous `layoutSubviews` reproduces 20,200 segment reads and 40,000 view-slot reads over 100 steady layouts. The current source performs 100 of each. Scroll changes require at most eight segment reads in this fixture. Visible pixel identity, width changes, scrolling, replacement, selection/accessibility restoration and offscreen non-text geometry checks pass.

## Retire superseded rendering variants

The iOS document cache retained old mounted revisions until capacity or memory pressure evicted them. Its 8 MiB estimated budget permits many small historical revisions and excludes shared committed payloads. A baseline experiment retained 80 renderings from 40 small documents with two highlighting variants each.

`DSDocumentView` now retires the previous document ID after reconciling a different incoming document or clearing the view. All cached variants of that ID, including the oversized slot, lose cache ownership. Same-document style changes retain their variants. Other views or callers that hold an old rendering keep valid text and geometry. A later request for a retired ID rebuilds it; retirement trades that cache reuse for less historical retention.

The cache tracks keys by document ID under its existing lock. The index holds no renderings. Evicted-key cleanup is amortized, and a hard 256-key cap handles Foundation's advisory cache limits. The cap preserves the incoming preparation and latest oversized slot. The original text budgets and shared committed-segment accounting remain unchanged.

The regression checks 40 mounted document transitions with two variants: none of the 80 historical renderings remains after clearing the view. Held-view validity, unrelated/new rendering reuse and oversized release checks pass. An adversarial `NSCache` subclass ignores count limits during 1,024 unmounted preparations; the index remains at 256 keys and clears on invalidation. Retirement is not a global lifetime fence: an already-running preparation can later cache an old ID again, subject to the normal limits.

## Prepare the requested code-color policy

Both iOS worker preparation and `measurePlatform` previously forced highlighting on. A plain-code mount therefore requested a second rendering with a different cache key. They now use the requested flag from `LayoutHint` and measurement arguments.

The focused UIKit fixture executes the production preparation/measurement methods against the real renderer. Plain prepare/measure/mount lookup changes from two rendering builds and four TextKit layout calls to one build and two calls. Those two cold calls measure intrinsic and wrapped code widths; subsequent measurement and mount lookup add no layout work in this fixture. Highlighted code, color toggles, geometry, retained snapshots, toggle-back reuse and code-free normalization pass.

The fixture substitutes React geometry declarations. The actual pod build separately compiles the full bridge. The shared public measurement cache can reuse geometry across a highlighting-only change without calling `measurePlatform`; this patch does not promise that every color toggle is prepared off the main thread. It also leaves parser token generation enabled.

## Reproduction and device follow-up

The full UIKit layout/retirement suite, focused preparation-policy fixture,
reveal checks and math attachment checks passed. Independent ownership and
viewport reviews found no remaining correctness issues. `git diff --check` passed.
Reproduce the simulator checks with:

```sh
pnpm test:ios:layout
bash tests/ios/run-preparation-policy-harness.sh
pnpm test:ios:reveal
pnpm test:ios:math
```

The complete Downstream pod builds for arm64 and x86_64 simulators:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -project example/ios/Pods/Pods.xcodeproj -target Downstream \
  -configuration Debug -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
```

On hardware, compare matching release baseline/candidate builds with identical source, chunk delivery, viewport, typography and feature settings. Alternate build order and record Instruments allocations, retained memory, main-thread time and animation hitches. Include rich streaming, plain/highlighted code, long conversations, repeated replacement, scrolling/remounting, selection, VoiceOver, tables and active reveal animations. In particular, check whether earlier retirement causes meaningful rebuild or deallocation costs in multi-view and recycled-list workloads.
