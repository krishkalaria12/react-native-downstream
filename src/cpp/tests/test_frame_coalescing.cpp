#include "FrameScheduler.h"
#include "SessionRegistry.h"
#include "SessionTrace.h"
#include "check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace downstream;
using namespace std::chrono_literals;

namespace {
std::mutex mutex;
std::condition_variable changed;
std::atomic<unsigned> requests{0};
std::atomic<unsigned> prepares{0};
std::atomic<unsigned> stages{0};
std::atomic<unsigned> drops{0};
std::atomic<unsigned> retirements{0};
bool blocked = false;
std::vector<SessionEvent> events;
std::thread::id frameThread;
std::thread::id inactiveFrameThread;
bool preparationOnFrame = false;
bool callbackOffFrame = false;
std::atomic<bool> retirementOnFrame{false};

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << "FAIL " << message << '\n';
  std::exit(1);
}

template <typename Predicate> void await(Predicate predicate, const char *message) {
  std::unique_lock<std::mutex> lock(mutex);
  require(changed.wait_for(lock, 3s, predicate), message);
}

void prepare(const DocumentHandle &, const std::vector<LayoutHint> &) {
  std::unique_lock<std::mutex> lock(mutex);
  preparationOnFrame |= std::this_thread::get_id() == frameThread ||
                        std::this_thread::get_id() == inactiveFrameThread;
  ++prepares;
  changed.notify_all();
  changed.wait(lock, [] { return !blocked; });
}

void release() {
  std::lock_guard<std::mutex> lock(mutex);
  blocked = false;
  changed.notify_all();
}

void block() {
  std::lock_guard<std::mutex> lock(mutex);
  blocked = true;
}

void tick(double interval = 1000000, bool late = false) {
  await([] { return requests != 0; }, "native frame requested");
  {
    std::lock_guard<std::mutex> lock(mutex);
    require(requests == 1, "at most one frame request is outstanding");
    --requests;
  }
  FrameScheduler::onFrame(interval, late);
}

void create(const std::string &id) {
  require(SessionRegistry::create(id, ParseMode::IncrementalTail, [](const SessionEvent &event) {
    std::lock_guard<std::mutex> lock(mutex);
    callbackOffFrame |= std::this_thread::get_id() != frameThread;
    events.push_back(event);
    changed.notify_all();
  }), "create native frame session");
  SessionRegistry::noteLayout(id, LayoutHint{1, 320});
  SessionTrace::enable(id, [](const TraceEvent &event) {
    std::lock_guard<std::mutex> lock(mutex);
    if (event.event == "snapshot_staged") ++stages;
    if (event.event == "dropped") ++drops;
    if (event.event == "retire_end") {
      if (std::this_thread::get_id() == frameThread || std::this_thread::get_id() == inactiveFrameThread) {
        retirementOnFrame = true;
      }
      ++retirements;
    }
    changed.notify_all();
  });
}

unsigned stagedCount() {
  std::lock_guard<std::mutex> lock(mutex);
  return stages;
}

void awaitStage(unsigned previous) {
  await([previous] { return stages > previous; }, "worker result staged");
}

void dispatchAwait(unsigned stageBefore) {
  const unsigned prepareBefore = prepares.load();
  while (prepares == prepareBefore) {
    tick();
    await([prepareBefore] { return prepares > prepareBefore || requests > 0; },
          "worker dispatch or cooldown continuation");
  }
  awaitStage(stageBefore);
}

void drain() {
  // All sessions are disposed before draining, so callbacks cannot rearm.
  unsigned pending;
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending = requests;
  }
  if (pending) tick();
}

// A platform clock keeps its requester installed while inactive. It redirects
// pending and new callbacks to an asynchronous host queue until frames resume.
class LifecycleClock {
public:
  LifecycleClock() : host_(1) {
    FrameScheduler::install([this] { request(); });
  }

  void inactive(bool hold = false) {
    bool pending;
    {
      std::lock_guard<std::mutex> lock(mutex);
      active_ = false;
      hold_ = hold;
      pending = requests != 0;
    }
    if (pending) scheduleHost();
  }

  void active() {
    std::lock_guard<std::mutex> lock(mutex);
    active_ = true;
    // Rearm the native callback for any request handed back by the host queue.
    // It remains one outstanding scheduler request across the transition.
    if (requests) ++nativeRequests_;
  }

  void releaseHost() {
    std::lock_guard<std::mutex> lock(mutex);
    hold_ = false;
    changed.notify_all();
  }

  unsigned completions() const {
    std::lock_guard<std::mutex> lock(mutex);
    return completions_;
  }

  void awaitCompletion(unsigned previous) {
    await([this, previous] { return completions_ > previous; }, "asynchronous lifecycle callback drained");
  }

private:
  void request() {
    bool useHost;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++requests;
      require(requests == 1, "lifecycle clock keeps one outstanding request");
      useHost = !active_;
      if (active_) ++nativeRequests_;
      changed.notify_all();
    }
    if (useHost) scheduleHost();
  }

  void scheduleHost() {
    host_.post([this] {
      bool run;
      {
        std::unique_lock<std::mutex> lock(mutex);
        inactiveFrameThread = std::this_thread::get_id();
        changed.wait(lock, [this] { return !hold_; });
        run = !active_;
        if (run) {
          require(requests == 1, "inactive callback consumes one pending request");
          --requests;
        }
      }
      // A stale host callback after resume leaves the rearmed native request
      // intact. Native and inactive callbacks cannot publish the same result.
      if (run) FrameScheduler::onFrame(1000000, false);
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completions_;
        changed.notify_all();
      }
    });
  }

  WorkerPool host_;
  bool active_{true};
  bool hold_{false};
  unsigned completions_{0};
  unsigned nativeRequests_{0};
};

void lifecycleRegression() {
  std::vector<SessionEvent> lifecycleEvents;
  LifecycleClock clock;
  const auto createInactive = [&](const std::string &id) {
    require(SessionRegistry::create(id, ParseMode::IncrementalTail, [&](const SessionEvent &event) {
      std::lock_guard<std::mutex> lock(mutex);
      lifecycleEvents.push_back(event);
      changed.notify_all();
    }), "create session without a bound view");
    SessionTrace::enable(id, [](const TraceEvent &event) {
      std::lock_guard<std::mutex> lock(mutex);
      if (event.event == "snapshot_staged") ++stages;
      changed.notify_all();
    });
  };
  // A remembered layout is enough to prepare rendering without binding a view.
  RenderPreparer::noteLayout(LayoutHint{1, 320});
  const std::string noView = "inactive-no-view";
  createInactive(noView);
  require(SessionRegistry::append(noView, "background", 0, 1), "append before native clock becomes inactive");
  CHECK(requests == 1, "native dispatch tick pending before background transition");
  clock.inactive();
  await([&] { return lifecycleEvents.size() == 1; }, "inactive clock publishes without native vsync");
  require(SessionRegistry::finish(noView, 0, 1), "explicitly finish inactive stream without bound view");
  await([&] { return lifecycleEvents.size() == 2; }, "inactive explicit finish settles without view or vsync");
  CHECK(!SessionRegistry::publishedSnapshot(noView, 0, 1).live,
        "asynchronous inactive driver publishes final stream state");
  SessionRegistry::dispose(noView);
  clock.active();

  const std::string staged = "inactive-staged-dispose";
  createInactive(staged);
  const unsigned before = stagedCount();
  require(SessionRegistry::append(staged, "staged", 0, 1), "append staged lifecycle stream");
  tick(); awaitStage(before);
  await([] { return requests == 1; }, "native publication request pending before inactivity");
  clock.inactive(true);
  SessionRegistry::dispose(staged);
  unsigned completed = clock.completions();
  clock.releaseHost(); clock.awaitCompletion(completed);
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(lifecycleEvents.size() == 2 && requests == 0,
          "disposing staged background publication suppresses event and further frames");
  }
  clock.active();

  const std::string resumed = "inactive-foreground-resume";
  createInactive(resumed);
  const unsigned resumeStage = stagedCount();
  require(SessionRegistry::append(resumed, "foreground", 0, 1), "append before pause and resume");
  clock.inactive(true);
  completed = clock.completions();
  clock.active();
  clock.releaseHost(); clock.awaitCompletion(completed);
  CHECK(requests == 1, "resume preserves request when stale inactive callback returns");
  tick(); awaitStage(resumeStage); tick();
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(lifecycleEvents.size() == 3 && lifecycleEvents.back().sessionId == resumed && requests == 0,
          "foreground restores native ticks without duplicate publication or lost wakeup");
  }
  const unsigned finalStage = stagedCount();
  require(SessionRegistry::finish(resumed, 0, 1), "finish resumed foreground stream");
  tick(); awaitStage(finalStage); tick();
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(lifecycleEvents.size() == 4 && requests == 0,
          "resumed foreground finalization retains one-event-per-native-tick behavior");
  }
  SessionRegistry::dispose(resumed);
  CHECK(!preparationOnFrame, "inactive and native frame callbacks only dispatch worker preparation");
  // Replace the capturing requester before the fake platform clock is destroyed.
  FrameScheduler::install([] {
    std::lock_guard<std::mutex> lock(mutex);
    ++requests;
    changed.notify_all();
  });
}

}

int main() {
  frameThread = std::this_thread::get_id();
  FrameScheduler::install([] {
    std::lock_guard<std::mutex> lock(mutex);
    ++requests;
    changed.notify_all();
  });
  RenderPreparer::install(prepare);

  unsigned calls = 0;
  FrameScheduler::post([&](const FrameInfo &) {
    ++calls;
    FrameScheduler::post([&](const FrameInfo &) { ++calls; });
  });
  FrameScheduler::post([&](const FrameInfo &) { ++calls; });
  tick();
  CHECK(calls == 2, "jobs posted during a tick wait for the next tick");
  tick();
  CHECK(calls == 3 && requests == 0, "scheduler does not request frames while idle");

  for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    double observed = 0;
    FrameScheduler::post([&](const FrameInfo &info) { observed = info.intervalMs; });
    tick(invalid);
    CHECK(std::isfinite(observed) && observed > 0, "invalid native frame intervals use a finite default");
  }

  const unsigned concurrentStage = stagedCount();
  const auto concurrentEvents = events.size();
  for (unsigned index = 0; index < 5; ++index) {
    const auto concurrent = "frame-concurrent-" + std::to_string(index);
    create(concurrent);
    require(SessionRegistry::append(concurrent, "first", 0, 1), "append concurrent first delta");
    require(SessionRegistry::append(concurrent, " second", 0, 2), "append concurrent second delta");
  }
  CHECK(requests == 1, "five simultaneous sessions share one native frame request");
  tick();
  await([concurrentStage] { return stages >= concurrentStage + 5; }, "all concurrent workers staged");
  await([] { return requests > 0; }, "concurrent publication frame requested");
  CHECK(events.size() == concurrentEvents && requests == 1,
        "five prepared workers reserve one publication frame and publish nothing early");
  // A worker may be between staging and posting while a tick starts. Its job
  // correctly waits for the following tick rather than joining an active one.
  while (events.size() < concurrentEvents + 5) tick();
  CHECK(events.size() == concurrentEvents + 5 && requests == 0,
        "shared native ticks publish exactly one event per concurrent session");
  for (unsigned index = 0; index < 5; ++index) {
    const auto concurrent = "frame-concurrent-" + std::to_string(index);
    const auto document = SessionRegistry::snapshot(concurrent, 0, 2);
    CHECK(document && document->blocks[0].text == u"first second",
          "concurrent coalescing retains each stream's complete accepted source");
    SessionRegistry::dispose(concurrent);
  }

  const std::string id = "frame-burst";
  create(id);
  require(SessionRegistry::append(id, "a", 0, 1), "append first burst delta");
  require(SessionRegistry::append(id, "b", 0, 2), "append second burst delta");
  require(SessionRegistry::append(id, "c", 0, 3), "append third burst delta");
  CHECK(prepares == 5 && events.size() == concurrentEvents + 5, "burst cannot parse or publish before native frame");
  block();
  const unsigned stageBefore = stagedCount();
  const unsigned burstPrepare = prepares.load();
  const auto burstEvents = events.size();
  tick();
  await([burstPrepare] { return prepares == burstPrepare + 1; }, "first worker reaches preparation gate");
  require(SessionRegistry::append(id, "d", 0, 4), "append while worker is preparing");
  require(SessionRegistry::append(id, "e", 0, 5), "merge another delta while worker is preparing");
  CHECK(requests == 0 && events.size() == burstEvents, "active worker neither polls frames nor publishes early");
  release();
  awaitStage(stageBefore);
  CHECK(!SessionRegistry::snapshot(id, 0, 3), "staged result is not visible before publication tick");
  const unsigned secondStage = stagedCount();
  tick();
  CHECK(events.size() == burstEvents + 1 && events.back().sequence == 3, "one prepared prefix publishes in native tick");
  auto prefix = SessionRegistry::snapshot(id, 0, 3);
  CHECK(prefix && prefix->blocks[0].text == u"abc", "published prefix preserves every accepted byte");
  awaitStage(secondStage);
  CHECK(SessionRegistry::snapshot(id, 0, 3) == prefix && !SessionRegistry::snapshot(id, 0, 5),
        "preparing newer snapshot leaves exact published version available");
  tick();
  CHECK(events.size() == burstEvents + 2 && events.back().sequence == 5, "newer deltas coalesce under latest sequence");
  auto latest = SessionRegistry::snapshot(id, 0, 5);
  CHECK(latest && latest->blocks[0].text == u"abcde", "coalesced snapshot has cumulative source");
  await([] { return retirements > 0; }, "superseded session snapshot owners retire asynchronously");
  CHECK(!retirementOnFrame, "native frame publication never releases old snapshot owners inline");
  CHECK(requests == 0, "settled live session requests no extra frames");

  const unsigned finalStage = stagedCount();
  require(SessionRegistry::finish(id, 0, 5), "finish unchanged source version");
  tick();
  awaitStage(finalStage);
  CHECK(SessionRegistry::publishedSnapshot(id, 0, 5).live, "finished worker does not change publication state early");
  tick();
  CHECK(events.size() == burstEvents + 3 && events.back().sequence == 5 && !SessionRegistry::publishedSnapshot(id, 0, 5).live,
        "finish publishes distinct final snapshot at the same sequence");
  CHECK(requests == 0, "finished stream has no tail polling");
  SessionRegistry::dispose(id);

  const std::string policy = "frame-policy";
  create(policy);
  unsigned before = stagedCount();
  require(SessionRegistry::append(policy, "*bold ==mark==", 0, 1), "append preview source");
  tick(); awaitStage(before); tick();
  const auto original = SessionRegistry::snapshot(policy, 0, 1);
  before = stagedCount();
  require(SessionRegistry::setPreview(policy, {}), "change preview with no new input");
  tick(); awaitStage(before); tick();
  const auto literal = SessionRegistry::snapshot(policy, 0, 1);
  CHECK(original && literal && original->id != literal->id && literal->blocks[0].text == u"*bold ==mark==",
        "same-sequence policy changes are never deduplicated");
  before = stagedCount();
  require(SessionRegistry::setParse(policy, ParseOptions(ParseFlags::Default | ParseFlags::HighlightSyntax)),
          "change parse flags without source append");
  tick(); awaitStage(before); tick();
  const auto highlighted = SessionRegistry::snapshot(policy, 0, 1);
  CHECK(highlighted && highlighted->id != literal->id && highlighted->blocks[0].text == u"*bold mark",
        "same-sequence parse policy changes reach the next frame publication");
  SessionRegistry::dispose(policy);

  const std::string reset = "frame-reset";
  create(reset);
  block();
  before = stagedCount();
  const unsigned resetPrepare = prepares.load();
  require(SessionRegistry::append(reset, "old", 0, 1), "append old generation");
  tick();
  await([resetPrepare] { return prepares > resetPrepare; }, "old generation preparing");
  require(SessionRegistry::reset(reset, "new", 1), "reset during preparation");
  const unsigned dropBefore = drops.load();
  release();
  await([dropBefore] { return drops > dropBefore; }, "obsolete prepared generation dropped");
  tick(); awaitStage(before); tick();
  latest = SessionRegistry::snapshot(reset, 1, 0);
  CHECK(latest && latest->blocks[0].text == u"new" && !SessionRegistry::snapshot(reset, 0, 1),
        "reset suppresses obsolete event and retains replacement bytes");
  SessionRegistry::dispose(reset);

  const std::string disposed = "frame-dispose";
  create(disposed);
  before = stagedCount();
  require(SessionRegistry::append(disposed, "never published", 0, 1), "append disposable source");
  tick(); awaitStage(before);
  const auto eventCount = events.size();
  SessionRegistry::dispose(disposed);
  tick();
  CHECK(events.size() == eventCount && requests == 0, "dispose suppresses staged event and stops frames");

  const std::string finishDuringWork = "frame-finish-active";
  create(finishDuringWork);
  block();
  before = stagedCount();
  const unsigned activePrepare = prepares.load();
  require(SessionRegistry::append(finishDuringWork, "growing", 0, 1), "append active finalization source");
  tick();
  await([activePrepare] { return prepares > activePrepare; }, "active finalization preparation reached");
  require(SessionRegistry::append(finishDuringWork, " done", 0, 2), "accept last delta during prepare");
  require(SessionRegistry::finish(finishDuringWork, 0, 2), "finish while prior snapshot prepares");
  release(); awaitStage(before);
  before = stagedCount();
  const auto beforeFinalEvents = events.size();
  tick(1, true);
  CHECK(events.size() == beforeFinalEvents + 1 && events.back().sequence == 1,
        "active finish publishes partial progress only once on its first tick");
  awaitStage(before); tick();
  latest = SessionRegistry::snapshot(finishDuringWork, 0, 2);
  CHECK(latest && latest->blocks[0].text == u"growing done" &&
        !SessionRegistry::publishedSnapshot(finishDuringWork, 0, 2).live && requests == 0,
        "active finish bypasses load delay and publishes every final byte");
  SessionRegistry::dispose(finishDuringWork);

  // Controlled preparation time alone must raise stride, without a late tick.
  const std::string expensive = "frame-expensive-work";
  create(expensive);
  block();
  before = stagedCount();
  const unsigned expensivePrepare = prepares.load();
  require(SessionRegistry::append(expensive, "costly", 0, 1), "append expensive source");
  tick();
  await([expensivePrepare] { return prepares > expensivePrepare; }, "expensive worker at gate");
  require(SessionRegistry::append(expensive, " merged", 0, 2), "append during expensive preparation");
  // The gate contributes to the measured parse/prepare pass, and the one-ms
  // fake budget makes ten-ms work deterministically exceed all four strides.
  std::this_thread::sleep_for(10ms);
  release(); awaitStage(before);
  before = stagedCount();
  tick(1, false);
  CHECK(prepares == expensivePrepare + 1, "worker cost delays next dispatch without late frame signal");
  tick(); tick();
  CHECK(prepares == expensivePrepare + 1, "cost-adapted cooldown batches two more frame intervals");
  tick(); awaitStage(before); tick();
  latest = SessionRegistry::snapshot(expensive, 0, 2);
  CHECK(latest && latest->blocks[0].text == u"costly merged", "expensive worker still makes cumulative progress");
  SessionRegistry::dispose(expensive);

  // Use a deliberately tiny frame budget to increase stride without sleeps.
  // Late-frame pressure raises it independently of worker timing.
  const std::string load = "frame-load";
  create(load);
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    before = stagedCount();
    require(SessionRegistry::append(load, "x", 0, sequence), "append load sample");
    dispatchAwait(before);
    tick(1, true);
  }
  const unsigned loadedPrepares = prepares.load();
  before = stagedCount();
  require(SessionRegistry::append(load, "y", 0, 4), "append under reduced cadence");
  tick(); tick();
  CHECK(prepares == loadedPrepares, "loaded session accumulates deltas during adaptive cooldown");
  tick();
  awaitStage(before);
  tick();
  CHECK(SessionRegistry::snapshot(load, 0, 4), "adaptive stride still publishes partial progress");

  // Eight healthy publications recover one stride, and repeated healthy work
  // returns to one frame. Publication frames also count toward the cadence.
  for (uint64_t sequence = 5; sequence <= 30; ++sequence) {
    before = stagedCount();
    require(SessionRegistry::append(load, "z", 0, sequence), "append recovery sample");
    dispatchAwait(before);
    tick();
  }
  before = stagedCount();
  const unsigned healthyPrepares = prepares.load();
  require(SessionRegistry::append(load, "q", 0, 31), "append after recovery");
  tick(); awaitStage(before);
  CHECK(prepares == healthyPrepares + 1, "healthy publications recover one-frame dispatch cadence");
  tick();

  // Inflate stride again; finish and reset must bypass it on their next frame.
  for (uint64_t sequence = 32; sequence <= 34; ++sequence) {
    before = stagedCount();
    require(SessionRegistry::append(load, "v", 0, sequence), "append pressure sample");
    dispatchAwait(before);
    tick(1, true);
  }
  before = stagedCount();
  require(SessionRegistry::finish(load, 0, 34), "finish under load");
  tick(); awaitStage(before); tick();
  CHECK(!SessionRegistry::publishedSnapshot(load, 0, 34).live, "finish bypasses adaptive cooldown");
  before = stagedCount();
  require(SessionRegistry::reset(load, "fresh", 1), "reset under load");
  tick(); awaitStage(before); tick();
  CHECK(SessionRegistry::snapshot(load, 1, 0), "reset bypasses adaptive cooldown");
  SessionRegistry::dispose(load);
  drain();
  CHECK(!callbackOffFrame && requests == 0, "events only publish on native frames and idle scheduler stops");
  lifecycleRegression();
  RenderPreparer::install(nullptr);
  FrameScheduler::install({});
  return ds_check_result();
}
