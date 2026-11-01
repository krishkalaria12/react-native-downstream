// The vendored lexer module and its runtime, as one translation unit, so the
// configuration in HighlightsModule.h reaches every file without build flags.
// The vendored sources are generated, so their warnings are not ours to fix.

#include "HighlightsModule.h"

#include <setjmp.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include "wasm-rt-impl.c"
#include "wasm-rt-mem-impl.c"
#include "highlights.c"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Where a trap on this thread unwinds to: the innermost call below. Calls do
// not nest, because the module's imports never call back into it.
static WASM_RT_THREAD_LOCAL sigjmp_buf trapTarget;

void downstream_highlights_trap(wasm_rt_trap_t code) { siglongjmp(trapTarget, (int)code); }

// Arms trapTarget for one call. The runtime restores its call depth from the
// saved one before it reports a trap, so the depth is saved here too.
#define GUARD()                                                        \
  WASM_RT_SAVE_STACK_DEPTH();                                          \
  const wasm_rt_trap_t trap = (wasm_rt_trap_t)sigsetjmp(trapTarget, 0); \
  if (trap != WASM_RT_TRAP_NONE) {                                     \
    return trap;                                                       \
  }

wasm_rt_trap_t downstream_highlights_instantiate(HighlightsModule *module, struct w2c_env *env) {
  GUARD();
  wasm2c_downstream__highlights_instantiate(module, env);
  return WASM_RT_TRAP_NONE;
}

wasm_rt_trap_t downstream_highlights_stream(HighlightsModule *module, uint32_t reset) {
  GUARD();
  w2c_downstream__highlights_highlightStream(module, reset);
  return WASM_RT_TRAP_NONE;
}

wasm_rt_trap_t downstream_highlights_clear(HighlightsModule *module) {
  GUARD();
  w2c_downstream__highlights_streamClear(module);
  return WASM_RT_TRAP_NONE;
}

wasm_rt_trap_t downstream_highlights_capture(HighlightsModule *module, uint32_t at, uint32_t *length) {
  GUARD();
  *length = w2c_downstream__highlights_streamCapture(module, at);
  return WASM_RT_TRAP_NONE;
}

wasm_rt_trap_t downstream_highlights_restore(HighlightsModule *module, uint32_t at, uint32_t length) {
  GUARD();
  w2c_downstream__highlights_streamRestore(module, at, length);
  return WASM_RT_TRAP_NONE;
}
