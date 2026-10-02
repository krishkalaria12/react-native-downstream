#import <UIKit/UIKit.h>
#include "DSFonts.h"
UIFont *DSMakeFont(const std::string &, CGFloat size, double weight, bool, bool monospaced) {
  const CGFloat mapped = weight >= 700 ? UIFontWeightBold : (weight >= 500 ? UIFontWeightMedium : UIFontWeightRegular);
  return monospaced ? [UIFont monospacedSystemFontOfSize:size weight:mapped]
                    : [UIFont systemFontOfSize:size weight:mapped];
}
BOOL DSFontIsFallback(const std::string &, UIFont *) { return NO; }
