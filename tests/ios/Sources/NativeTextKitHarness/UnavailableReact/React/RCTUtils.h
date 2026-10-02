#pragma once

#import <UIKit/UIKit.h>

// DSFileExporter uses this single React entry point. The harness links a
// fail-fast implementation because it exercises layout without a React app.
extern "C" UIViewController *RCTPresentedViewController(void);
