#pragma once

// The vendored lexer module in code/cpp/wasm: wasm2c output compiled as plain C,
// with no WebAssembly runtime involved. Every file that touches the module
// includes this header rather than highlights.h, so the runtime configuration
// and the private symbol names below are the same everywhere.

// Memory comes from malloc and every access is bounds-checked. The default on
// 64-bit hosts reserves gigabytes of address space behind guard pages, which
// iOS may refuse and which would need a process-wide signal handler.
#define WASM_RT_USE_MMAP 0
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 1
// Deep recursion traps before it can exhaust a worker thread's stack. The
// lexers nest only for embedded languages, far below this.
#ifndef WASM_RT_MAX_CALL_STACK_DEPTH
#define WASM_RT_MAX_CALL_STACK_DEPTH 256
#endif
// A trap reaches HighlightsModule.c's handler rather than the runtime's own
// jump buffer. The runtime saves the signal mask with every call it guards,
// two system calls each, which only a signal handler needs, and there is none.
#define WASM_RT_TRAP_HANDLER downstream_highlights_trap

// An app may link its own wasm2c module, so the runtime's external names are
// renamed the way md4c's are, and cannot collide with another copy.
#define wasm_rt_allocate_externref_table downstream_wasm_rt_allocate_externref_table
#define wasm_rt_allocate_funcref_table downstream_wasm_rt_allocate_funcref_table
#define wasm_rt_allocate_memory downstream_wasm_rt_allocate_memory
#define wasm_rt_allocate_memory_shared downstream_wasm_rt_allocate_memory_shared
#define wasm_rt_call_stack_depth downstream_wasm_rt_call_stack_depth
#define wasm_rt_free downstream_wasm_rt_free
#define wasm_rt_free_externref_table downstream_wasm_rt_free_externref_table
#define wasm_rt_free_funcref_table downstream_wasm_rt_free_funcref_table
#define wasm_rt_free_memory downstream_wasm_rt_free_memory
#define wasm_rt_free_memory_shared downstream_wasm_rt_free_memory_shared
#define wasm_rt_free_thread downstream_wasm_rt_free_thread
#define wasm_rt_grow_externref_table downstream_wasm_rt_grow_externref_table
#define wasm_rt_grow_funcref_table downstream_wasm_rt_grow_funcref_table
#define wasm_rt_grow_memory downstream_wasm_rt_grow_memory
#define wasm_rt_grow_memory_shared downstream_wasm_rt_grow_memory_shared
#define wasm_rt_init downstream_wasm_rt_init
#define wasm_rt_init_thread downstream_wasm_rt_init_thread
#define wasm_rt_is_initialized downstream_wasm_rt_is_initialized
#define wasm_rt_last_segment_val downstream_wasm_rt_last_segment_val
#define wasm_rt_memory_is_default32 downstream_wasm_rt_memory_is_default32
#define wasm_rt_memory_is_default32_shared downstream_wasm_rt_memory_is_default32_shared
#define wasm_rt_saved_call_stack_depth downstream_wasm_rt_saved_call_stack_depth
#define wasm_rt_strerror downstream_wasm_rt_strerror
#define wasm_rt_trap downstream_wasm_rt_trap
#define w2c_env_is_id_continue downstream_w2c_env_is_id_continue
#define w2c_env_is_id_start downstream_w2c_env_is_id_start
#define wasm_tailcall_w2c_env_is_id_continue downstream_wasm_tailcall_w2c_env_is_id_continue
#define wasm_tailcall_w2c_env_is_id_start downstream_wasm_tailcall_w2c_env_is_id_start

#include "highlights.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The module's one import object: the host functions it calls back into. It
/// holds the instance's memory rather than its data pointer, because the module
/// can grow, and so move, its memory in the middle of a call.
struct w2c_env {
  const wasm_rt_memory_t *memory;
};

typedef w2c_downstream__highlights HighlightsModule;

/// Calls into the module. Each returns WASM_RT_TRAP_NONE, or the trap that
/// stopped it, after which the instance must not be used again. A trap unwinds
/// with longjmp, so the frame that catches it is C, here, and no C++ frame
/// with a destructor is ever skipped.
wasm_rt_trap_t downstream_highlights_instantiate(HighlightsModule *module, struct w2c_env *env);
wasm_rt_trap_t downstream_highlights_stream(HighlightsModule *module, uint32_t reset);
wasm_rt_trap_t downstream_highlights_clear(HighlightsModule *module);
wasm_rt_trap_t downstream_highlights_capture(HighlightsModule *module, uint32_t at, uint32_t *length);
wasm_rt_trap_t downstream_highlights_restore(HighlightsModule *module, uint32_t at, uint32_t length);

#ifdef __cplusplus
}
#endif
