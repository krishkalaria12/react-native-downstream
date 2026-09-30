#import "DSFileExporter.h"

#import <React/RCTUtils.h>

@implementation DSFileExporter {
  NSURL *_directory;
  void (^_completion)(NSURL *_Nullable, NSError *_Nullable);
}

- (instancetype)init {
  if (self = [super init]) {
    // A directory per export, so a second export never overwrites the file a
    // reader already chose while the first dialog was still open.
    _directory = [NSURL fileURLWithPath:[NSTemporaryDirectory()
                                          stringByAppendingPathComponent:
                                              [NSString stringWithFormat:@"downstream-export-%@",
                                                                         NSUUID.UUID.UUIDString]]];
  }
  return self;
}

- (void)dealloc {
  [self removeDirectory];
}

- (void)removeDirectory {
  if (_directory == nil) {
    return;
  }
  [[NSFileManager defaultManager] removeItemAtURL:_directory error:nil];
  _directory = nil;
}

- (void)saveData:(NSData *)data
       fileName:(NSString *)fileName
    completion:(void (^)(NSURL *_Nullable, NSError *_Nullable))completion {
  NSError *error = nil;
  if (![[NSFileManager defaultManager] createDirectoryAtURL:_directory
                               withIntermediateDirectories:YES
                                                attributes:nil
                                                     error:&error]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, error);
    });
    return;
  }
  NSURL *file = [_directory URLByAppendingPathComponent:fileName];
  if (![data writeToURL:file options:NSDataWritingAtomic error:&error]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(nil, error);
    });
    return;
  }

  UIDocumentPickerViewController *picker =
      [[UIDocumentPickerViewController alloc] initForExportingURLs:@[file] asCopy:YES];
  picker.delegate = self;
  _completion = completion;
  // The reader's dialog is presented from whatever the app is presenting, which
  // is the React helper's whole job.
  UIViewController *presenter = RCTPresentedViewController();
  if (presenter == nil) {
    _completion = nil;
    [self removeDirectory];
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
  void (^completion)(NSURL *_Nullable, NSError *_Nullable) = _completion;
  _completion = nil;
  [self removeDirectory];
  if (completion != nil) {
    completion(urls.firstObject, nil);
  }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
  // A cancel is neither a success nor a failure, so nothing is reported: an app
  // that counts copies must not count the reader changing their mind.
  _completion = nil;
  [self removeDirectory];
}

@end
