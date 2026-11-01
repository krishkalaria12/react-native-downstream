#import <Foundation/Foundation.h>
#import <Downstream/DownstreamCodeHighlighter.h>
extern "C" const void *DownstreamGetCodeHighlighter();

@interface DSCodeInstaller : NSObject
@end
@implementation DSCodeInstaller
+ (void)load { DownstreamInstallCodeHighlighter(DownstreamGetCodeHighlighter()); }
@end
