#import <Foundation/Foundation.h>

#ifdef __cplusplus
#include "SnapshotText.h"

/// An immutable UTF-16 view of native snapshot pages. Foundation and TextKit
/// may copy their own buffers, but reading this string never flattens the source.
@interface DSSnapshotString : NSString
- (instancetype)initWithSnapshot:(const downstream::SnapshotText &)snapshot;
@end
#endif
