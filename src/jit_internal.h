#ifndef TOI_JIT_INTERNAL_H
#define TOI_JIT_INTERNAL_H

#include "jit_backend.h"
#include "jit_ir.h"
#include "jit_native_trace.h"
#include "jit_pipeline.h"
#include "object.h"
#include "vm.h"

#define JIT_TARGET_EXIT 0xFFFFu
#define TYPED_EXIT_NONE 0xFFFFu

typedef enum {
    TYPED_EXIT_REASON_NONE = 0,
    TYPED_EXIT_REASON_TYPE = 1,
    TYPED_EXIT_REASON_STACK = 2,
    TYPED_EXIT_REASON_CALL = 3,
    TYPED_EXIT_REASON_INVOKE = 4,
    TYPED_EXIT_REASON_TABLE = 5,
    TYPED_EXIT_REASON_CONTROL = 6,
    TYPED_EXIT_REASON_UNSUPPORTED = 7
} TypedExitReason;

typedef struct {
    ObjFunction* function;
    JitProgram* program;
    uint8_t queued;
    uint8_t compiled;
    uint8_t failed;
    uint16_t compile_attempts;
    uint8_t runtime_disabled;
    uint32_t exec_attempts;
    uint32_t exec_hits;
    uint16_t runtime_probe_limit;
} JitTrackedFunction;

typedef struct {
    ObjFunction* function;
    uint32_t opcode_offset;
    uint32_t loop_start_offset;
    uint32_t after_offset;
    uint16_t max_slot;
    uint16_t max_stack;
    uint8_t opcode;
    uint8_t compiled;
    uint8_t failed;
    uint8_t has_program;
    uint16_t entry_pc;
    uint32_t hot_count;
    uint32_t compile_attempts;
    uint8_t fail_reason;
    uint8_t fail_opcode;
    uint32_t fail_offset;
    int op_count;
    JitOp* ops;
    ObjClosure** call_ic_closures;
    JitProgram** call_ic_programs;
    ObjClosure** call_ic2_closures;
    JitProgram** call_ic2_programs;
    ObjClosure** call_ic3_closures;
    JitProgram** call_ic3_programs;
    ObjClosure** call_ic4_closures;
    JitProgram** call_ic4_programs;
    ObjClosure** call_ic5_closures;
    JitProgram** call_ic5_programs;
    uint32_t native_call_entry_cap;
    ObjClosure** native_call_entry_targets_by_offset;
    void** native_call_entry_fns_by_offset;
    uint8_t numeric_fast_eligible;
    uint8_t typed_fast_eligible;
    uint8_t typed_fast_disabled;
    uint32_t typed_fast_attempts;
    uint32_t typed_fast_hits;
    uint8_t typed_ir_ready;
    uint16_t typed_exit_count;
    uint32_t typed_exit_total;
    uint8_t typed_last_exit_reason;
    uint32_t typed_hot_exit_threshold;
    uint16_t typed_hot_exit_count;
    uint16_t* typed_pc_exit_ids;
    uint8_t* typed_exit_reasons;
    uint32_t* typed_exit_hits;
    uint8_t* typed_exit_hot;
    uint8_t value_fast_eligible;
    uint8_t value_fast_disabled;
    uint8_t mixed_call_heavy;
    // Runs that had to bail at an in-trace call without finishing an iteration.
    // A trace that keeps doing that costs more than the plain VM (entry + commit
    // per iteration), so it is retired once the count gets high enough.
    uint32_t call_stall_runs;
    uint32_t value_fast_attempts;
    uint32_t value_fast_hits;
    uint8_t runtime_disabled;
    uint32_t exec_attempts;
    uint32_t exec_hits;
    uint16_t runtime_probe_limit;
    ToiJitNumericLoopFn native_numeric_loop_fn;
    ToiJitSlotLoopFn native_slot_loop_fn;
    ToiJitValueLoopFn native_value_loop_fn;
    ToiJitNativeTraceFn native_trace_fn;
    uint16_t native_value_instr_count;
    uint8_t native_value_var_slot;
    uint8_t native_value_end_slot;
    ToiJitValueInstr native_value_instrs[256];
    uint8_t native_trace_ready;
    uint8_t native_trace_exec_safe;
    uint8_t native_trace_direct_core;
    uint8_t native_trace_fast_calls;
    uint16_t native_trace_instr_count;
    uint16_t native_trace_guard_count;
    uint8_t native_trace_has_call;
    uint8_t native_trace_has_invoke;
    uint8_t native_trace_has_build_string;
    uint8_t native_trace_has_slice;
    uint8_t native_trace_has_mutating_table;
    uint8_t native_trace_has_get_table_generic;
    uint16_t native_trace_prefix_work;
    ToiJitNativeTraceInstr native_trace_instrs[256];
    ToiJitNativeGuard native_trace_guards[256];
    void* native_trace_code_mem;
    size_t native_trace_code_size;
    uint64_t native_trace_exec_attempts;
    uint64_t native_trace_exec_hits;
    uint64_t native_trace_exit_hits;
    uint64_t native_trace_continue_hits;
    uint64_t native_trace_soft_bails;
    uint64_t native_trace_guard_failures;
    uint64_t native_trace_deopts;
    uint32_t native_trace_sandbox_runs;
    uint8_t native_trace_sandbox_disabled;
    uint8_t native_trace_sandbox_ok;
    uint8_t native_loop_acc_slot;
    uint8_t native_loop_var_slot;
    uint8_t native_loop_end_slot;
    uint8_t native_loop_const_count;
    uint8_t native_slot_loop_slot_count;
    uint8_t native_slot_loop_used_count;
    uint8_t native_slot_loop_written_count;
    uint8_t native_slot_loop_used_slots[64];
    uint8_t native_slot_loop_written_slots[64];
    uint8_t native_slot_loop_upvalue_const_count;
    uint8_t native_slot_loop_upvalue_const_idx[16];
    uint8_t native_slot_loop_upvalue_idx[16];
    double native_loop_consts[32];
    ToiJitQforEmptyFn native_qfor_empty_fn;
    ToiJitMathReduceFn native_math_reduce_fn;
    uint8_t math_acc_slot;
    uint8_t math_var_slot;
    uint8_t math_end_slot;
    double math_c1;
    double math_c2;
    double math_c3;
    void* native_code_mem;
    size_t native_code_size;
} JitLoopTrace;

#define TOI_JIT_LOOP_CACHE_SIZE 1024

typedef struct {
    ObjFunction* function;
    uint32_t opcode_offset;
    uint8_t opcode;
    uint32_t loop_index_plus1;
} JitLoopCacheEntry;

#define TOI_JIT_FUNC_CACHE_SIZE 2048

typedef struct {
    ObjFunction* function;
    uint32_t tracked_index_plus1;
} JitFuncCacheEntry;

typedef struct {
    ObjFunction* function;
    uint32_t loop_opcode_offset;
    uint32_t exit_byte_offset;
    uint16_t exit_id;
    uint8_t reason;
    uint8_t stitched;
    uint32_t hits;
} JitSideExitCandidate;

typedef struct ToiJitState {
    JitTrackedFunction* tracked;
    int count;
    int capacity;
    JitLoopTrace* loops;
    int loop_count;
    int loop_capacity;
    JitSideExitCandidate* side_exits;
    int side_exit_count;
    int side_exit_capacity;
    JitFuncCacheEntry func_cache[TOI_JIT_FUNC_CACHE_SIZE];
    JitLoopCacheEntry loop_cache[TOI_JIT_LOOP_CACHE_SIZE];
    uint8_t loop_exec_enabled;
    uint8_t loop_numeric_fast_enabled;
    uint8_t loop_value_fast_enabled;
    uint32_t hot_loop_threshold;
    uint64_t loop_backedge_events;
    uint64_t loop_compile_attempts;
    uint64_t loop_compiled_count;
    uint32_t loop_live_count; /* compiled traces still worth probing */
    uint64_t loop_compile_fail_count;
    uint64_t loop_fail_reason_unsupported;
    uint64_t loop_fail_reason_decode;
    uint64_t loop_fail_reason_jump;
    uint64_t loop_fail_reason_stack;
    uint8_t loop_last_fail_opcode;
    uint32_t loop_last_fail_offset;
    uint8_t loop_last_fail_reason;
    uint64_t loop_fail_opcode_counts[256];
    uint64_t loop_exec_attempts;
    uint64_t loop_exec_hits;
    uint8_t backend_host_supported;
    uint8_t backend_enabled;
    ToiJitBackendKind backend_kind;
    uint64_t backend_loop_compile_attempts;
    uint64_t backend_loop_compile_success;
    uint64_t backend_loop_compile_fail;
    uint64_t backend_value_loop_compile_attempts;
    uint64_t backend_value_loop_compile_success;
    uint64_t backend_value_loop_compile_fail;
    uint64_t backend_value_loop_exec_attempts;
    uint64_t backend_value_loop_exec_hits;
    uint64_t backend_value_loop_runtime_disabled;
    uint64_t backend_loop_exec_attempts;
    uint64_t backend_loop_exec_hits;
    uint64_t backend_slot_lower_attempts;
    uint64_t backend_slot_lower_success;
    uint64_t backend_slot_lower_fail;
    uint64_t backend_slot_lower_fail_opcode_counts[256];
    uint64_t native_trace_build_attempts;
    uint64_t native_trace_build_success;
    uint64_t native_trace_build_fail;
    uint64_t native_trace_compile_attempts;
    uint64_t native_trace_compile_success;
    uint64_t native_trace_compile_fail;
    uint64_t native_trace_compile_direct_core;
    uint64_t native_trace_exec_safe_traces;
    uint64_t native_trace_exec_unsafe_traces;
    uint64_t native_trace_unsafe_opcode_counts[256];
    uint64_t native_trace_runtime_disabled;
    uint8_t native_trace_exec_globally_disabled;
    uint8_t native_trace_exec_disable_reason;
    uint64_t native_trace_exec_attempts;
    uint64_t native_trace_exec_hits;
    uint64_t native_trace_exec_attempts_direct_core;
    uint64_t native_trace_exec_hits_direct_core;
    uint64_t native_trace_guard_failures;
    uint64_t native_trace_deopts;
    uint64_t native_trace_deopt_opcode_counts[256];
    uint64_t native_trace_deopt_reason_counts[32];
    uint64_t native_trace_sandbox_attempts;
    uint64_t native_trace_sandbox_success;
    uint64_t native_trace_sandbox_fail;
    uint8_t native_trace_exec_enabled;
    uint8_t native_trace_sandbox_enabled;
    uint8_t native_trace_compiled_exec_enabled;
    uint32_t native_trace_active_depth;
    uint64_t func_runtime_disabled;
    uint64_t loop_runtime_disabled;
    uint64_t loop_numeric_fast_attempts;
    uint64_t loop_numeric_fast_hits;
    uint64_t loop_typed_fast_attempts;
    uint64_t loop_typed_fast_hits;
    uint64_t loop_typed_exit_promotions;
    uint64_t loop_typed_side_exit_stitches;
    uint64_t loop_side_exit_candidates;
    uint64_t loop_side_exit_hits;
    uint8_t next_pipeline_enabled;
    JitPipelineState* next_pipeline;
} ToiJitState;

JitLoopTrace* toi_jit_native_trace_compile_context(void);

#endif
