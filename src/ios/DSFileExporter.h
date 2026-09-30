#pragma once

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

/// Writes an export to a file and hands it to the system save dialog, which is
/// the mobile equivalent of the download link Streamdown puts on a file.
///
/// A save dialog needs no permission, no manifest entry, and no FileProvider,
/// and it is the one path a reader expects to mean "put this somewhere I can
/// find it". Each export gets its own temporary directory, so two downloads in
/// flight cannot overwrite one another's file, and each is removed in the
/// delegate callback for that export or when the exporter goes away, whichever
/// happens first.
@interface DSFileExporter : NSObject <UIDocumentPickerDelegate>

- (instancetype)init;

/// Presents the save dialog for `data` under `filename`. The completion runs
/// on the main thread with the URL the reader chose, or with nil and no error
/// when the dialog was cancelled: a cancel is neither a success nor a failure,
/// so it reports nothing.
- (void)saveData:(NSData *)data
       fileName:(NSString *)fileName
    completion:(void (^)(NSURL *_Nullable url, NSError *_Nullable error))completion;

@end

NS_ASSUME_NONNULL_END
