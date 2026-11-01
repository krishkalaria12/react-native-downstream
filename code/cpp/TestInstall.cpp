// Host tests explicitly link this installer. Production uses the platform's
// autolinking installer, so the core never references the optional package.
#include "DownstreamCodeHighlighter.h"
extern "C" const void *DownstreamGetCodeHighlighter();
namespace {
struct Install {
  Install() { DownstreamInstallCodeHighlighter(DownstreamGetCodeHighlighter()); }
} install;
}
