#pragma once

// The only ABI shared with react-native-downstream-code. Buffers are borrowed
// until the next call on that instance; each package owns its own allocations.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DownstreamCodeHighlighterFunctions {
  uint32_t version;
  size_t size;
  void *(*create)(void);
  void (*destroy)(void *instance);
  bool (*start)(void *instance, uint8_t language, const uint8_t *image, size_t length, bool reset);
  bool (*lex)(void *instance, const char *bytes, size_t length, const uint32_t **records, size_t *count);
  bool (*capture)(void *instance, const uint8_t **image, size_t *length);
  bool (*reusable)(void *instance);
  uint64_t (*created)(void);
} DownstreamCodeHighlighterFunctions;

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
void DownstreamInstallCodeHighlighter(const void *functions);

#ifdef __cplusplus
}
#endif
