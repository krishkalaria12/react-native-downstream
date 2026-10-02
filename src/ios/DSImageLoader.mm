#import "DSImageLoader.h"

#import <React/RCTImageLoader.h>
#import <React/RCTUtils.h>

#import <algorithm>
#import <cstdio>
#import <functional>
#import <list>
#import <map>
#import <memory>
#import <mutex>
#import <string>
#import <vector>

namespace {

/// The contract's cap. Four, because a phone's radios and a reader's patience
/// both stop caring well before a paragraph of images has finished loading.
constexpr size_t kMaxConcurrentLoads = 4;

/// The image's pixel size, from the decoded image.
///
/// The platform's size-aware load already decoded at the size that was asked for,
/// so the picture's own `size` is the size it will be drawn at — which is what
/// makes a message laid out at a cached size and the picture that fills it agree.
CGSize pixelSizeOf(UIImage *image) {
  return image != nil ? image.size : CGSizeZero;
}

NSString *mimeTypeOf(NSURLResponse *response) {
  NSString *type = response.MIMEType;
  return type.length > 0 ? type : @"";
}

/// The key one request is filed under: its URL and the size it decodes at. A URL
/// at two sizes is two requests, because a thumbnail and a full-bleed picture are
/// two different decodes.
std::string keyFor(NSString *url, CGSize size) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "@%gx%g", size.width, size.height);
  return std::string(url.UTF8String ?: "") + buffer;
}

/// One request, shared by every view that wants this URL at this size.
struct Request {
  std::string url;
  CGSize size{0, 0};
  CGFloat scale{1};
  /// One callback per interested view. A view that cancels has its callback
  /// removed, which is what makes the reference count a count of interested
  /// views rather than a count of requests.
  std::vector<std::function<void(DSImageLoad *, DSImageLoadResult *)>> callbacks;
  RCTImageLoader *loader = nullptr;
  /// Bumped when every view lets go, so a completion already queued for this
  /// request is not delivered to a view that has since stopped wanting it.
  size_t generation = 0;
};

/// Everything the loader owns, under one lock. A load completes on the platform's
/// queue and a view releases on the main one, so the two must not race on the
/// same map; one lock for all of it is simpler than one lock per structure and
/// there is no path that takes two.
struct Store {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Request>> live;
  /// Requests waiting for a slot, oldest first, so the first view to ask is the
  /// first served.
  std::list<std::string> queued;
  size_t generation = 1;
};

/// Deliberately leaked, so a load that completes during teardown cannot write to
/// a destroyed map.
Store &store() {
  static auto *instance = new Store();
  return *instance;
}

/// The platform's own loader, or null before the bridge is up. Weak because it
/// lives in the bridge's context container and the bridge can go away first.
__weak RCTImageLoader *weakReactLoader = nil;
DSImageLoader *sharedLoader = nil;

DSImageLoadResult *resultFor(NSURLResponse *response, UIImage *image) {
  return [[DSImageLoadResult alloc] initWithStatus:DSImageLoadStatusLoaded
                                           response:response
                                               data:nil
                                          pixelSize:pixelSizeOf(image)
                                           mimeType:mimeTypeOf(response)];
}

/// The bytes behind a loaded image, for a download.
///
/// The platform's loader hands back a decoded `UIImage` and keeps its own cache,
/// and the bytes are not part of that contract — so a save fetches them once,
/// through the app's own stack, at the moment the reader asks for one. That is
/// the request the web version cannot make: a CORS failure there leaves the
/// reader with no way to save a picture at all, and here it costs one request
/// that only happens because someone tapped a button.
NSData *DSDownloadBytes(NSURL *url, void (^completion)(NSData * _Nullable, NSString *mimeType)) {
  NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
      completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
          completion(error != nil ? nil : data, mimeTypeOf(response));
        });
      }];
  [task resume];
  return nil;
}

DSImageLoadResult *failureResult(void) {
  return [[DSImageLoadResult alloc] initWithStatus:DSImageLoadStatusFailed
                                           response:nil
                                               data:nil
                                          pixelSize:CGSizeZero
                                           mimeType:@""];
}

/// Starts as many queued requests as the cap allows. Called with the lock NOT
/// held, because starting a request can call back synchronously on a test double.
void drainQueue(void);

void deliver(const std::shared_ptr<Request> &request, DSImageLoadResult *result) {
  std::vector<std::function<void(DSImageLoad *, DSImageLoadResult *)>> callbacks;
  {
    std::lock_guard<std::mutex> lock(store().mutex);
    const auto found = store().live.find(request->url);
    if (found == store().live.end() || found->second != request ||
        found->second->generation != request->generation) {
      // Every view let go while the result was in flight, which is a
      // cancellation. Nothing is delivered, because nobody is left to draw it.
      return;
    }
    callbacks = request->callbacks;
    store().live.erase(found);
    const std::string key = request->url;
    store().queued.remove(key);
  }
  // The platform calls back on its own queue and a view draws on the main one,
  // so this hop is the one that has to be explicit.
  dispatch_async(dispatch_get_main_queue(), ^{
    for (const auto &callback : callbacks) {
      callback(nil, result);
    }
  });
  drainQueue();
}

void drainQueue(void) {
  while (true) {
    std::shared_ptr<Request> request;
    RCTImageLoader *loader = nil;
    {
      std::lock_guard<std::mutex> lock(store().mutex);
      if (store().live.size() >= kMaxConcurrentLoads || store().queued.empty()) {
        return;
      }
      const std::string key = store().queued.front();
      store().queued.pop_front();
      const auto found = store().live.find(key);
      if (found == store().live.end()) {
        // Every view that wanted it let go while it was queued, which costs no
        // request and is the same as a cancellation.
        continue;
      }
      request = found->second;
      loader = request->loader;
    }
    if (loader == nil || request->callbacks.empty()) {
      continue;
    }
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:request->url.c_str()]];
    if (url == nil) {
      deliver(request, failureResult());
      continue;
    }
    // The platform's own size-aware load, so a 4,000-pixel photo costs a
    // phone-sized bitmap. A GIF shows its first frame and an SVG fails, because
    // neither pipeline decodes one and a formula of failed frames is not a
    // picture.
    RCTImageURISource *source =
        [[RCTImageURISource alloc] initWithURL:url size:request->size scale:request->scale];
    const std::shared_ptr<Request> held = request;
    // The full overload, because there is no shorter one on the loader's protocol:
    // `size:scale:resizeMode:` alone is a `UIImageView` category method, not an
    // `RCTImageLoaderProtocol` one, and calling it here would compile only if the
    // concrete class happened to declare it. The two blocks it also takes are the
    // progressive-decode ones, and nil for both is what "draw the image when it is
    // whole" means.
    //
    // `RCTResizeModeCenter` rather than `Cover`: the box was already fitted to the
    // image's own proportions by `imageBox`, so letting the loader cover this box
    // would crop a picture to a rectangle it was never measured against.
    [loader loadImageWithURLRequest:source
                               size:request->size
                              scale:request->scale
                            clipped:NO
                          resizeMode:RCTResizeModeCenter
                       progressBlock:nil
                    partialLoadBlock:nil
                     completionBlock:^(NSError *error, UIImage *image) {
                       deliver(held, error != nil || image == nil
                                            ? failureResult()
                                            : resultFor(nil, image));
                     }];
  }
}

} // namespace

void DSDownloadImageBytes(NSURL *url, void (^completion)(NSData *, NSString *)) {
  DSDownloadBytes(url, completion);
}

@implementation DSImageLoadResult

- (instancetype)initWithStatus:(DSImageLoadStatus)status
                      response:(NSURLResponse *)response
                          data:(NSData *)data
                     pixelSize:(CGSize)pixelSize
                      mimeType:(NSString *)mimeType {
  if (!(self = [super init])) {
    return nil;
  }
  _status = status;
  _response = response;
  _data = data;
  _pixelSize = pixelSize;
  _mimeType = mimeType;
  return self;
}

@end

@implementation DSImageLoad

- (instancetype)initWithURL:(NSURL *)url size:(CGSize)size {
  if (!(self = [super init])) {
    return nil;
  }
  _url = url;
  _requestedSize = size;
  return self;
}

@end

@implementation DSImageLoader

+ (instancetype)sharedLoaderWithReactLoader:(RCTImageLoader *)reactLoader {
  @synchronized(self) {
    if (sharedLoader == nil) {
      sharedLoader = [[self alloc] init];
    }
    // A loader that arrives after the first call replaces the weak reference, so
    // a view that mounts after the bridge is up still gets the platform's own
    // pipeline rather than nothing.
    if (reactLoader != nil) {
      weakReactLoader = reactLoader;
    }
    return sharedLoader;
  }
}

- (NSUInteger)inFlightCount {
  std::lock_guard<std::mutex> lock(store().mutex);
  return store().live.size();
}

- (DSImageLoad *)loadURL:(NSURL *)url
                   size:(CGSize)size
                  scale:(CGFloat)scale
              callback:(void (^)(DSImageLoad *, DSImageLoadResult *_Nullable))completion {
  if (url == nil || completion == nil) {
    return nil;
  }
  if (!(size.width > 0) || !(size.height > 0)) {
    // Nothing to decode to. A view with no size yet asks again when it has one,
    // so this is nothing rather than a request for an unknown size.
    return nil;
  }
  RCTImageLoader *loader = weakReactLoader;
  if (loader == nil) {
    // No pipeline means no image, and the reader gets their alt text rather than
    // a box that never fills.
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, failureResult());
    });
    return nil;
  }

  const NSString *absolute = url.absoluteString ?: @"";
  const std::string key = keyFor(absolute, size);
  DSImageLoad *handle = [[DSImageLoad alloc] initWithURL:url size:size];
  {
    std::lock_guard<std::mutex> lock(store().mutex);
    const auto found = store().live.find(key);
    if (found != store().live.end()) {
      // Deduplicated: a second message showing the same chart shares the first
      // request rather than starting a second one.
      found->second->callbacks.push_back([completion copy]);
      return handle;
    }
    auto request = std::make_shared<Request>();
    request->url = key;
    request->size = size;
    request->scale = scale;
    request->loader = loader;
    request->generation = store().generation;
    request->callbacks.push_back([completion copy]);
    store().live[key] = request;
    store().queued.push_back(key);
  }
  drainQueue();
  return handle;
}

- (void)cancelLoad:(DSImageLoad *)load {
  if (load == nil) {
    return;
  }
  const std::string key = keyFor(load.url.absoluteString ?: @"", load.requestedSize);
  {
    std::lock_guard<std::mutex> lock(store().mutex);
    const auto found = store().live.find(key);
    if (found == store().live.end()) {
      return;
    }
    if (!found->second->callbacks.empty()) {
      // One view let go. The request survives while another wants it, which is
      // what lets two messages share one request and one of them go away.
      found->second->callbacks.pop_back();
    }
    if (!found->second->callbacks.empty()) {
      return;
    }
    // The last one let go, so the request is dropped and its generation bumped so
    // a completion already queued for it reaches nobody.
    found->second->generation = ++store().generation;
    store().live.erase(found);
    store().queued.remove(key);
  }
  // A freed slot belongs to whoever has been waiting longest.
  drainQueue();
}

@end
