#import "DSFileExporter.h"

#import <React/RCTUtils.h>

#include "WorkerPool.h"

@implementation DSFileExporter {
  /// One scratch directory and one completion per export in flight, keyed by
  /// the dialog's token. A second download started before the first dialog
  /// closes therefore gets its own directory and its own completion: neither
  /// export can overwrite the file the reader already chose, and neither is
  /// handed the other's result.
  NSMutableDictionary<NSString *, NSURL *> *_directories;
  NSMutableDictionary<NSString *, void (^)(NSURL *_Nullable, NSError *_Nullable)> *_completions;
}

- (instancetype)init {
  if (self = [super init]) {
    _directories = [NSMutableDictionary new];
    _completions = [NSMutableDictionary new];
  }
  return self;
}

- (void)dealloc {
  NSArray<NSURL *> *directories = _directories.allValues;
  downstream::WorkerPool::shared().post([directories] {
    @autoreleasepool {
      for (NSURL *directory in directories) {
        [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
      }
    }
  });
}

/// The export's own scratch directory, removed as soon as its result is known.
- (void)discard:(NSString *)token {
  NSURL *directory = _directories[token];
  if (directory == nil) {
    return;
  }
  [_directories removeObjectForKey:token];
  [_completions removeObjectForKey:token];
  downstream::WorkerPool::shared().post([directory] {
    @autoreleasepool {
      [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
    }
  });
}

- (void)saveData:(NSData *)data
       fileName:(NSString *)fileName
  shouldPresent:(BOOL (^)(void))shouldPresent
     completion:(void (^)(NSURL *_Nullable, NSError *_Nullable))completion {
  __weak DSFileExporter *weakSelf = self;
  void (^writeFile)(void) = ^{
    @autoreleasepool {
      NSError *error = nil;
      NSString *token = NSUUID.UUID.UUIDString;
      NSURL *directory =
          [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
                                                          [NSString stringWithFormat:@"downstream-export-%@", token]]];
      NSURL *file = [directory URLByAppendingPathComponent:fileName];
      const BOOL created = [[NSFileManager defaultManager] createDirectoryAtURL:directory
                                                  withIntermediateDirectories:YES
                                                                   attributes:nil
                                                                        error:&error];
      const BOOL written = created && [data writeToURL:file options:NSDataWritingAtomic error:&error];
      if (!written && created) {
        [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        DSFileExporter *exporter = weakSelf;
        if (exporter == nil || !shouldPresent()) {
          if (written) {
            downstream::WorkerPool::shared().post([directory] {
              @autoreleasepool {
                [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
              }
            });
          }
          return;
        }
        if (!written) {
          completion(nil, error);
          return;
        }
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForExportingURLs:@[file] asCopy:YES];
        picker.delegate = exporter;
        // All picker state belongs to the main thread; workers only write files.
        picker.restorationIdentifier = token;
        exporter->_directories[token] = directory;
        exporter->_completions[token] = [completion copy];
        UIViewController *presenter = RCTPresentedViewController();
        if (presenter == nil) {
          [exporter discard:token];
          completion(nil, [NSError errorWithDomain:@"Downstream"
                                             code:0
                                         userInfo:@{NSLocalizedDescriptionKey : @"No view controller to present from."}]);
          return;
        }
        [presenter presentViewController:picker animated:YES completion:nil];
      });
    }
  };
  downstream::WorkerPool::shared().post([writeFile] { writeFile(); });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  NSString *token = controller.restorationIdentifier;
  void (^completion)(NSURL *_Nullable, NSError *_Nullable) = nil;
  if (token != nil) {
    completion = _completions[token];
  }
  if (token != nil) {
    [self discard:token];
  }
  if (completion != nil) {
    completion(urls.firstObject, nil);
  }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
  // A cancel is neither a success nor a failure, so nothing is reported: an app
  // that counts copies must not count the reader changing their mind. The
  // scratch directory still goes, because the file is already in the picker.
  NSString *token = controller.restorationIdentifier;
  if (token != nil) {
    [self discard:token];
  }
}

@end
