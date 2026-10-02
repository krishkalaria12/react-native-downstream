#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@class RCTImageLoader;

/// What one image load found. A failure is a state, not an error: an image that
/// 404s draws its alt text, which is what a reader wants, and the view says so
/// rather than throwing.
typedef NS_ENUM(NSInteger, DSImageLoadStatus) {
  DSImageLoadStatusLoaded = 0,
  DSImageLoadStatusFailed = 1,
};

/// One image's bytes and size, as a loader reports them.
@interface DSImageLoadResult : NSObject
@property (nonatomic, readonly) DSImageLoadStatus status;
@property (nonatomic, readonly, nullable) NSURLResponse *response;
/// Present for `Loaded`, and held for as long as the view needs it: a download
/// writes these bytes to a file, so releasing them here would make a save fail
/// for a reason the reader cannot see.
@property (nonatomic, readonly, nullable) NSData *data;
/// The decoded picture, which the view draws. Present for `Loaded`.
@property (nonatomic, readonly, nullable) UIImage *image;
/// The image's own pixels, which is what `imageBox` takes a size from, the way
/// CSS takes an image's natural size. A downscaled request reports the size it
/// decoded at, which is never smaller than the column it was bounded by.
@property (nonatomic, readonly) CGSize pixelSize;
/// The MIME type, for the download's file name. Empty when the server sent none.
@property (nonatomic, readonly) NSString *mimeType;
@end

/// One image's load, which a view holds and cancels.
@interface DSImageLoad : NSObject
@property (nonatomic, readonly) NSURL *url;
@property (nonatomic, readonly) CGSize requestedSize;
@end

/**
 * Loads images through React Native's own pipeline, with the contract's limits.
 *
 * The platform's loader rather than a new one: the app's HTTP stack, its cookies,
 * its disk cache, and any custom URL handlers are all already there, and a
 * second loader would mean a second cache and a second set of credentials. On
 * iOS that is `RCTImageLoader` from the context container, which is the same
 * object React Native's own `<Image>` reads, and bridgeless mode puts it there.
 *
 * The three rules that are ours rather than the platform's:
 *
 *   - At most four requests in flight. A model can write a paragraph of
 *     `![](…)` and a chat list must not open thirty sockets to a host it does
 *     not know, both for the reader's battery and for the host's patience.
 *   - Requests for one URL are deduplicated, so two messages showing the same
 *     chart cost one request.
 *   - Every request is reference-counted by the views that want it, and is
 *     cancelled when the last one lets go. A recycled chat list therefore
 *     releases what it started, and scrolling away is a cancel.
 */
@interface DSImageLoader : NSObject

/// The process-wide loader. `reactLoader` is the platform's own, and is held
/// weakly because it lives in the bridge's context container.
+ (instancetype)sharedLoaderWithReactLoader:(nullable RCTImageLoader *)reactLoader;

/// The requests in flight, for a test and for the benchmark's memory check.
@property (nonatomic, readonly) NSUInteger inFlightCount;

/// Loads `url` at the size it will be drawn at, and calls `completion` on the
/// main queue.
///
/// `completion` is not called for a request that was cancelled, because a
/// cancelled request is one the caller has already stopped wanting. It is called
/// exactly once for every other outcome, including a failure.
///
/// The size is in points multiplied by the screen scale, which is what makes a
/// 4,000-pixel photo cost a phone-sized bitmap. A GIF shows its first frame and
/// an SVG fails, because neither platform's image pipeline decodes one and a
/// formula of failed frames is not a picture.
- (nullable DSImageLoad *)loadURL:(NSURL *)url
                            size:(CGSize)size
                           scale:(CGFloat)scale
                       callback:(void (^)(DSImageLoad *load, DSImageLoadResult *_Nullable result))completion;

/// Cancels one view's interest in a load. The request itself stops only when no
/// view wants it, which is what lets two messages share one request and one of
/// them go away.
- (void)cancelLoad:(DSImageLoad *)load;

@end

/// The bytes behind a loaded image, fetched once when a reader asks to save one.
///
/// The platform's loader keeps its own cache and hands back a decoded `UIImage`,
/// and the bytes are not part of that contract. So a save costs one request
/// through the app's own stack — which is the request the web version cannot
/// make, where a CORS failure leaves a reader with no way to save a picture at
/// all.
void DSDownloadImageBytes(NSURL *url, void (^)(NSData *_Nullable, NSString *mimeType));

NS_ASSUME_NONNULL_END
