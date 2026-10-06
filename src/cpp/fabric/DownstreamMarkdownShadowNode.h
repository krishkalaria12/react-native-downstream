#pragma once

#include "DownstreamMarkdownState.h"
#include "DownstreamTextLayoutManager.h"
#include "ImageSizeCache.h"
#include "PreparedDocument.h"
#include "theme/Theme.h"

#include <react/renderer/components/DownstreamSpec/EventEmitters.h>
#include <react/renderer/components/DownstreamSpec/Props.h>
#include <react/renderer/components/view/ConcreteViewShadowNode.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace facebook::react {

JSI_EXPORT extern const char DownstreamMarkdownViewComponentName[];

/// Measures a document with the platform text system. The parsed document
/// reaches the view through state, so the view never parses.
///
/// A controlled `markdown` value is parsed and prepared on the worker pool as
/// soon as the node receives it, so every Markdown view in a commit is
/// prepared in parallel while React finishes the commit. Layout collects the
/// result. A large source is not waited for: the view keeps its previous
/// document until the preparation finishes and a state update relayouts it.
class DownstreamMarkdownShadowNode final
    : public ConcreteViewShadowNode<DownstreamMarkdownViewComponentName, DownstreamMarkdownViewProps,
                                    DownstreamMarkdownViewEventEmitter, DownstreamMarkdownState> {
public:
  DownstreamMarkdownShadowNode(const ShadowNodeFragment &fragment, const ShadowNodeFamily::Shared &family,
                               ShadowNodeTraits traits);
  DownstreamMarkdownShadowNode(const ShadowNode &sourceShadowNode, const ShadowNodeFragment &fragment);

  static ShadowNodeTraits BaseTraits() {
    auto traits = ConcreteViewShadowNode::BaseTraits();
    traits.set(ShadowNodeTraits::Trait::LeafYogaNode);
    traits.set(ShadowNodeTraits::Trait::MeasurableYogaNode);
    return traits;
  }

  void setLayoutManager(std::shared_ptr<const DownstreamTextLayoutManager> layoutManager);

  /**
   * Asks for a relayout after the view's image loads have reported.
   *
   * The platform has already recorded each result in `ImageSizeCache`; this
   * reads the document's answers back out of it and dispatches the state update.
   * Going through the cache is what makes a load that finished after its view
   * was recycled still count, and what makes six loads in one frame produce one
   * relayout rather than six.
   *
   * Static because a view holds a `std::shared_ptr<const ShadowNode>` and not
   * the concrete node, and because the node that has to be re-measured is
   * whichever revision is mounted by the time the update lands.
   */
  static void imagesChanged(const std::shared_ptr<const ShadowNode> &node);

  Size measureContent(const LayoutContext &layoutContext, const LayoutConstraints &layoutConstraints) const override;

  void layout(LayoutContext layoutContext) override;

private:
  struct PendingError {
    std::string code;
    std::string message;
  };

  /// A mounted `DownstreamFenceSlot` child: the code block it hosts and the
  /// child itself.
  struct SlotChild {
    uint32_t ordinal{0};
    std::shared_ptr<const ShadowNode> node;
    size_t childIndex{0};
  };

  /// The slot children a measurement or a layout has to consider, in child
  /// order. A child whose `display` is `none` — which is what React gives a
  /// suspended subtree — is not in the list, so it hosts nothing and its fence
  /// draws as a code card.
  ///
  /// Native decides this from the children it actually has rather than from a
  /// language list JavaScript sent, so the two cannot disagree about which
  /// fences are hosted.
  std::vector<SlotChild> slotChildren() const;

  /// What each slot child measures at, as the input both platforms lay the
  /// document out with. Empty when there are no slot children, so a view with
  /// no renderers measures and mounts exactly as it did before this existed.
  downstream::HostLayout measureSlots(const LayoutContext &layoutContext,
                                      const LayoutConstraints &layoutConstraints) const;

  /// Gives each mounted slot child the rectangle the measurement reserved for
  /// it, which is `ParagraphShadowNode::layout`'s loop over inline views: this
  /// node is a Yoga leaf, so Yoga ignores its children and the node lays them
  /// out itself.
  void layoutSlots(LayoutContext layoutContext);

  /// One controlled source's preparation, shared by every clone of the node
  /// that shows that source.
  struct ControlledSource {
    std::shared_ptr<downstream::PreparedDocument> prepared;
    /// Set once a relayout has been requested for when the preparation ends.
    std::atomic<bool> relayoutRequested{false};
  };

  void requestControlledSource();
  /// Returns the document to measure and mount. A failed parse keeps the last
  /// valid document.
  const downstream::DocumentHandle &getDocument() const;
  /// Asks for a relayout once a preparation that layout did not wait for ends.
  void relayoutWhenPrepared() const;

  /// Sends `onMathError` for the formulas that newly failed in `document`.
  void reportMathErrors(const downstream::DocumentHandle &document,
                        const downstream::DocumentHandle &previous) const;

  /// The node's layout hint at a scale and a width. Carries the theme, so the
  /// worker builds the rendering the view will mount and Fabric measures the
  /// sizes the view draws.
  /// The hint a preparation is made for. `document` is the one whose image answers
  /// it carries: the node's resolved document when measuring, and the fallback
  /// it already holds when requesting a preparation, because resolving the
  /// document there would read a preparation that does not exist yet.
  downstream::LayoutHint layoutHint(react::Float fontScale, react::Float width,
                                    const downstream::DocumentHandle &document) const;

  /// The chrome a document lays out with, from the props and the reader's
  /// choices. It is a height, not a rendering, so it is recomputed here rather
  /// than baked into a segment's text.
  uint32_t chromeFor(const DownstreamMarkdownState &state) const;

  /// The table's cap. Its own function beside `codeProps` for the same reason
  /// one takes props and the reader's choices elsewhere: a prop change lands in
  /// props a revision before the state that records it does, so measurement
  /// reads props rather than the state's copy of them.
  downstream::TableLayoutProps tableProps(const DownstreamMarkdownState &state) const;

  /// The theme these theme props resolve to. Interned only when the props
  /// change, so an unchanged theme costs nothing per revision, and the handle is
  /// held beside the rest of the node's state so clones share it.
  downstream::Theme::Handle resolveTheme() const;
  /// True when the previous props would resolve to the same theme. The arrays
  /// are compared as vectors rather than through the registry, so an unchanged
  /// inline literal does not intern.
  bool themeUnchanged(const DownstreamMarkdownViewProps &previous) const;
  /// The theme's id, or 0 for the default one, which is its id too.
  uint64_t themeId() const;
  /// The code block's props. Read here rather than from the state, for the same
  /// reason `tableProps` reads the table's.
  downstream::CodeLayoutProps codeProps(const DownstreamMarkdownState &state) const;
  /// What each image in the document is, from the process-wide size cache and
  /// the props. Rebuilt on every measurement, which is cheap: the cache is a
  /// lookup per image and a document has as many images as it has pictures.
  downstream::ImageLayout imageLayout(const downstream::DocumentHandle &document) const;
  /// The same answers without the policy, for the state. A view draws from this
  /// and never needs to know which prefixes the app allows.
  downstream::ImageStateLayout imageState(const downstream::ImageLayout &layout) const {
    return layout.state;
  }
  /// Asks for a relayout once images that have not answered do. The platform
  /// records a finished load in the cache and calls in, exactly as
  /// `relayoutWhenPrepared` is called when a parse finishes.
  void relayoutWhenImagesChange(downstream::ImageStateLayout answers) const;

  /// The caret this document draws, from the `caret` and `animating` props and
  /// the document's own `streaming` flag.
  ///
  /// Resolved here rather than in each view because it is a height: the
  /// placeholder is in the measured text of the last segment, so the node that
  /// measures and the view that draws have to answer the same question, and an
  /// own-line caret is a whole extra segment.
  downstream::PlacedCaret caretFor(const downstream::DocumentHandle &document) const;

  std::shared_ptr<const DownstreamTextLayoutManager> layoutManager_;
  std::shared_ptr<ControlledSource> controlled_;
  /// Null for the default theme, which is what an app that passes neither prop
  /// gets. The registry hands out one shared object per distinct value.
  mutable downstream::Theme::Handle theme_;
  mutable downstream::DocumentHandle document_;
  /// The revision has not chosen its document yet. Once chosen, measurement
  /// and mounting use the same one.
  mutable bool needsDocument_{true};
  /// The revision chose a fallback because its source was still being
  /// prepared; the next revision collects the result.
  mutable bool awaitingPrepared_{false};
  mutable std::optional<PendingError> pendingError_;
  /// What the slot children measured at during the measurement this revision
  /// took, so `layout()` mounts the view with the same heights rather than
  /// measuring a second time at whatever width layout ended up with.
  mutable downstream::HostLayout measuredHosts_;
};

} // namespace facebook::react
