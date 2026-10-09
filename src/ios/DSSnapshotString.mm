#import "DSSnapshotString.h"

@interface DSSnapshotString () {
  downstream::SnapshotText _snapshot;
  NSUInteger _offset;
  NSUInteger _length;
}
- (instancetype)initWithSnapshot:(const downstream::SnapshotText &)snapshot range:(NSRange)range;
@end

@implementation DSSnapshotString

- (instancetype)initWithSnapshot:(const downstream::SnapshotText &)snapshot {
  return [self initWithSnapshot:snapshot range:NSMakeRange(0, snapshot.size())];
}

- (instancetype)initWithSnapshot:(const downstream::SnapshotText &)snapshot range:(NSRange)range {
  if (self = [super init]) {
    _snapshot = snapshot;
    _offset = range.location;
    _length = range.length;
  }
  return self;
}

- (NSUInteger)length {
  return _length;
}

- (unichar)characterAtIndex:(NSUInteger)index {
  if (index >= _length) {
    [NSException raise:NSRangeException format:@"Character index %lu exceeds string length %lu",
                 (unsigned long)index, (unsigned long)_length];
  }
  return _snapshot[_offset + index];
}

- (void)getCharacters:(unichar *)buffer range:(NSRange)range {
  if (range.location > _length || range.length > _length - range.location) {
    [NSException raise:NSRangeException format:@"Character range %@ exceeds string length %lu",
                 NSStringFromRange(range), (unsigned long)_length];
  }
  _snapshot.copyTo(reinterpret_cast<char16_t *>(buffer), _offset + range.location, range.length);
}

- (NSString *)substringWithRange:(NSRange)range {
  if (range.location > _length || range.length > _length - range.location) {
    [NSException raise:NSRangeException format:@"Substring range %@ exceeds string length %lu",
                 NSStringFromRange(range), (unsigned long)_length];
  }
  if (range.location == 0 && range.length == _length) return self;
  // Retain the snapshot directly, rather than a chain of parent strings.
  return [[DSSnapshotString alloc] initWithSnapshot:_snapshot
                                            range:NSMakeRange(_offset + range.location, range.length)];
}

- (id)copyWithZone:(NSZone *)zone {
  return self;
}

@end
