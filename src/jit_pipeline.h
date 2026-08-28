#ifndef TOI_JIT_PIPELINE_H
#define TOI_JIT_PIPELINE_H

#include <stdint.h>

#include "object.h"

typedef struct JitPipelineState JitPipelineState;
typedef struct {
    uint64_t hot_functions;
    uint64_t compile_attempts;
    uint64_t compile_success;
    uint64_t compile_fail;
    uint64_t exec_attempts;
    uint64_t exec_hits;
    uint64_t functions_built;
    uint64_t ir_ops_total;
    uint64_t ir_unsupported_total;
    uint64_t ir_call_sites_total;
    uint64_t ir_invoke_sites_total;
    uint64_t ir_loop_sites_total;
    uint64_t lowered_trace_count;
    uint64_t lowered_trace_ops_total;
    uint64_t lowered_trace_calls_total;
    uint64_t lowered_trace_invokes_total;
    uint64_t lowered_trace_unsupported_total;
    uint64_t lowered_callsite_count;
    uint64_t lowered_callsite_guard_hints;
    uint64_t link_edge_total;
    uint64_t link_edge_resolved;
    uint64_t link_edge_unresolved;
    uint64_t link_exec_attempts;
    uint64_t link_exec_hits;
    uint64_t link_patch_attempts;
    uint64_t link_patch_hits;
    uint8_t link_exec_enabled;
    uint8_t link_patch_enabled;
} JitPipelineStats;

JitPipelineState* jit_pipeline_create(void);
void jit_pipeline_free(JitPipelineState* state);
void jit_pipeline_mark_roots(JitPipelineState* state);
void jit_pipeline_note_hot_function(JitPipelineState* state, ObjFunction* function);
int jit_pipeline_try_compile_stub(JitPipelineState* state, ObjFunction* function);
void jit_pipeline_note_exec_attempt(JitPipelineState* state);
void jit_pipeline_note_exec_hit(JitPipelineState* state);
uint64_t jit_pipeline_hot_functions(const JitPipelineState* state);
uint64_t jit_pipeline_compile_attempts(const JitPipelineState* state);
uint64_t jit_pipeline_compile_success(const JitPipelineState* state);
uint64_t jit_pipeline_compile_fail(const JitPipelineState* state);
uint64_t jit_pipeline_exec_attempts(const JitPipelineState* state);
uint64_t jit_pipeline_exec_hits(const JitPipelineState* state);
uint64_t jit_pipeline_functions_built(const JitPipelineState* state);
uint64_t jit_pipeline_ir_ops_total(const JitPipelineState* state);
uint64_t jit_pipeline_ir_unsupported_total(const JitPipelineState* state);
uint64_t jit_pipeline_ir_call_sites_total(const JitPipelineState* state);
uint64_t jit_pipeline_ir_invoke_sites_total(const JitPipelineState* state);
uint64_t jit_pipeline_ir_loop_sites_total(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_trace_count(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_trace_ops_total(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_trace_calls_total(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_trace_invokes_total(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_trace_unsupported_total(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_callsite_count(const JitPipelineState* state);
uint64_t jit_pipeline_lowered_callsite_guard_hints(const JitPipelineState* state);
uint64_t jit_pipeline_link_edge_count(const JitPipelineState* state);
uint64_t jit_pipeline_link_resolved_count(const JitPipelineState* state);
uint64_t jit_pipeline_link_unresolved_count(const JitPipelineState* state);
int jit_pipeline_has_resolved_callee(const JitPipelineState* state, ObjFunction* function);
int jit_pipeline_has_resolved_callee_at_site(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function);
int jit_pipeline_can_execute_linked(const JitPipelineState* state, ObjFunction* callee_function);
int jit_pipeline_can_execute_linked_at_site(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function);
int jit_pipeline_link_exec_is_enabled(const JitPipelineState* state);
int jit_pipeline_link_patch_is_enabled(const JitPipelineState* state);
void jit_pipeline_note_link_exec_attempt(JitPipelineState* state);
void jit_pipeline_note_link_exec_hit(JitPipelineState* state);
void jit_pipeline_note_link_patch_attempt(JitPipelineState* state);
void jit_pipeline_note_link_patch_hit(JitPipelineState* state);
uint32_t jit_pipeline_link_site_cache_lookup(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function);
void jit_pipeline_link_site_cache_store(
    JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function,
    uint32_t payload_plus1);
void jit_pipeline_link_site_cache_clear(JitPipelineState* state);
int jit_pipeline_pipeline_enabled(void);
int jit_pipeline_link_exec_enabled(void);
int jit_pipeline_link_patch_enabled(void);
int jit_pipeline_native_trace_compiled_exec_enabled(void);
void jit_pipeline_collect_stats(const JitPipelineState* state, JitPipelineStats* out_stats);

#endif
