# Renderer ownership: own the Fabric view

Decided 28 September 2026, from source review of React Native Enriched Markdown 0.6.0 and the Phase 0 feature matrix. No device measurement informed this decision. The post-implementation benchmark checks it.

## Decision

Downstream owns its Fabric component, shadow node, and platform renderers. It does not adapt or fork Enriched Markdown's renderer. It keeps md4c as the parser, at the same revision as the fixture oracle.

## What Enriched's source shows

- **Input is a string prop parsed inside each platform view.** On iOS, `MarkdownParserBridge.mm` runs md4c from the view and builds an Objective-C AST. On Android, `Parser.kt` calls into C++ over JNI, which allocates a Kotlin `MarkdownASTNode` object per node. A session-owned snapshot would need a new input path through both platform renderers.
- **iOS measurement blocks the main thread.** `ShadowMeasurementUtils.h` measures by `dispatch_sync` to the main queue and renders a mock off-screen view when the real one is missing or stale. A layout pass therefore waits on UIKit.
- **Streaming is a display policy on the complete string.** Streaming mode reads the mounted view's current bounds instead of measuring exactly, and relies on a height counter in state to request a later update. That is the opposite of the plan's contract, where the view applies a versioned measurement and never shows a size from another revision.
- **The feature surface is larger than the first release.** Math, spoilers, the text input component, custom context menus, and web targets would all have to be carried in a fork.

Accepting a session snapshot without a long-lived divergent fork is not feasible. The parser boundary, both platform renderers, and the measurement path would all change.

## What Downstream does instead

- `cpp/downstream` parses with md4c into an immutable `Document`: leaf blocks with container paths, UTF-16 text, flat style runs, and actionable links with UTF-16 source ranges. Both platforms read the same model; neither sees an AST.
- `cpp/fabric` has a hand-written shadow node. It parses once per `markdown` value, measures on the layout thread, and passes the document to the view through Fabric state. The view renders exactly the document and font scale that were measured.
- iOS measures and draws with the same TextKit 1 configuration. Standalone TextKit objects are used off the main thread, so measurement never waits on UIKit.
- Android measures in `DownstreamMarkdownViewManager.measure` and draws with the same `StaticLayout` configuration. Java reads documents by id from a weak native registry, so the render model is not serialized per update.

The streaming session planned for Phase 2 replaces the shadow node's parse with worker-produced documents. The measurement and mounting paths stay the same.

## Patterns borrowed from Enriched

- `LeafYogaNode` and `MeasurableYogaNode` traits on a custom shadow node, with a component descriptor that injects the measurer.
- Android measurement through `FabricUIManager.measure` and a `cmakeListsPath` plus `componentDescriptors` autolinking entry.
- md4c compiled with `MD4C_USE_UTF8`.

## Costs we accept

- Selection, rich table layout, images, and syntax highlighting must be built here. The plan schedules them for the feature phase.
- Very tall documents draw into a single layer on iOS and a single `StaticLayout` on Android. Per-block layers are a candidate once the incremental tail gives blocks stable identity.

## How this gets revisited

The post-implementation benchmark compares Enriched + Streamdown with both Downstream parse modes on physical devices. If Downstream misses budgets that Enriched meets on the same workload, profile first. The fallback remains to render with Enriched and keep this package's streaming session API.
