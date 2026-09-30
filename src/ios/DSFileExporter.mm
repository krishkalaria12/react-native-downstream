#import "DSFileExporter.h"

#import <React/RCTUtils.h>

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
  for (NSURL *directory in _directories.allValues) {
    [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
  }
}

/// The export's own scratch directory, removed as soon as its result is known.
- (void)discard:(NSString *)token {
  NSURL *directory = _directories[token];
  if (directory == nil) {
    return;
  }
  [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
  [_directories removeObjectForKey:token];
  [_completions removeObjectForKey:token];
}

- (void)saveData:(NSData *)data
       fileName:(NSString *)fileName
     completion:(void (^)(NSURL *_Nullable, NSError *_Nullable))completion {
  NSError *error = nil;
  NSString *token = NSUUID.UUID.UUIDString;
  NSURL *directory =
      [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
                                                      [NSString stringWithFormat:@"downstream-export-%@", token]]];
  if (![[NSFileManager defaultManager] createDirectoryAtURL:directory
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:&error]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, error);
    });
    return;
  }
  NSURL *file = [directory URLByAppendingPathComponent:fileName];
  if (![data writeToURL:file options:NSDataWritingAtomic error:&error]) {
    [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, error);
    });
    return;
  }

  UIDocumentPickerViewController *picker =
      [[UIDocumentPickerViewController alloc] initForExportingURLs:@[file] asCopy:YES];
  picker.delegate = self;
  // The dialog carries the token, so a delegate callback knows which export it
  // belongs to without a second picker or a single shared completion.
  picker.restorationIdentifier = token;
  _directories[token] = directory;
  _completions[token] = [completion copy];
  // The reader's dialog is presented from whatever the app is presenting, which
  // is the React helper's whole job.
  UIViewController *presenter = RCTPresentedViewController();
  if (presenter == nil) {
    [self discard:token];
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, [NSError errorWithDomain:@"Downstream"
                                           code:0
                                       userInfo:@{NSLocalizedDescriptionKey : @"No view controller to present from."}]);
    });
    return;
  }
  [presenter presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  NSString *token = controller.restorationIdentifier;
  void (^completion)(NSURL *_Nullable, NSError *_Nullable) = token != nil ? _completions[token] : nil;
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
