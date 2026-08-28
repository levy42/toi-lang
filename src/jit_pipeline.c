#include "jit_pipeline.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "memory.h"

int jit_pipeline_link_exec_enabled(void);
int jit_pipeline_link_patch_enabled(void);

typedef enum {
    NEXT_IR_OP_NOP = 0,
    NEXT_IR_OP_CONST,
    NEXT_IR_OP_NIL,
    NEXT_IR_OP_TRUE,
    NEXT_IR_OP_FALSE,
    NEXT_IR_OP_POP,
    NEXT_IR_OP_GET_LOCAL,
    NEXT_IR_OP_GET2_LOCAL,
    NEXT_IR_OP_SET_LOCAL,
    NEXT_IR_OP_SET_LOCAL_POP,
    NEXT_IR_OP_GET_UPVALUE,
    NEXT_IR_OP_APPEND,
    NEXT_IR_OP_GET_TABLE,
    NEXT_IR_OP_GET_TABLE_NUM,
    NEXT_IR_OP_SET_TABLE,
    NEXT_IR_OP_SET_TABLE_NUM,
    NEXT_IR_OP_ADD,
    NEXT_IR_OP_SUB,
    NEXT_IR_OP_MUL,
    NEXT_IR_OP_DIV,
    NEXT_IR_OP_MOD,
    NEXT_IR_OP_NEG,
    NEXT_IR_OP_NOT,
    NEXT_IR_OP_CMP_EQ,
    NEXT_IR_OP_CMP_LT,
    NEXT_IR_OP_CMP_GT,
    NEXT_IR_OP_JUMP,
    NEXT_IR_OP_JUMP_IF_FALSE,
    NEXT_IR_OP_JUMP_IF_TRUE,
    NEXT_IR_OP_LOOP,
    NEXT_IR_OP_CALL,
    NEXT_IR_OP_INVOKE,
    NEXT_IR_OP_BUILD_STRING2,
    NEXT_IR_OP_BUILD_STRING3,
    NEXT_IR_OP_BUILD_STRING4,
    NEXT_IR_OP_BUILD_STRING5,
    NEXT_IR_OP_ADJUST_STACK,
    NEXT_IR_OP_RETURN,
    NEXT_IR_OP_UNSUPPORTED
} NextIrOp;

typedef struct {
    NextIrOp op;
    uint8_t raw_opcode;
    uint32_t byte_offset;
    uint8_t a;
    uint8_t b;
    uint16_t jump;
    uint32_t target_offset;
} NextIrInstr;

typedef enum {
    NEXT_ARGC_SHAPE_FIXED0 = 0,
    NEXT_ARGC_SHAPE_FIXED1,
    NEXT_ARGC_SHAPE_FIXED2,
    NEXT_ARGC_SHAPE_FIXED3PLUS,
    NEXT_ARGC_SHAPE_DYNAMIC
} NextArgcShape;

typedef struct {
    uint32_t ir_index;
    uint32_t byte_offset;
    uint8_t is_invoke;
    uint8_t argc_known;
    uint8_t argc;
    uint8_t argc_shape;
    int16_t callee_from_top;
    uint8_t has_guard_hint;
    ObjClosure* guard_hint_closure;
} NextLoweredCallsite;

typedef struct {
    uint32_t start_ir_index;
    uint32_t end_ir_index;
    uint32_t op_count;
    uint32_t call_sites;
    uint32_t invoke_sites;
    uint32_t unsupported_sites;
    NextLoweredCallsite* callsites;
    int callsite_count;
    int callsite_capacity;
} NextLoweredTrace;

typedef struct {
    ObjFunction* caller_function;
    ObjFunction* callee_function;
    uint32_t callsite_offset;
    uint32_t trace_index;
    uint32_t callsite_index;
    uint8_t resolved;
} NextLinkEdge;

typedef struct {
    ObjFunction* caller_function;
    ObjFunction* callee_function;
    uint32_t callsite_offset;
} NextResolvedSiteEntry;

typedef struct {
    ObjFunction* caller_function;
    ObjFunction* callee_function;
    uint32_t callsite_offset;
    uint32_t payload_plus1;
} NextLinkSiteCacheEntry;

#define TOI_JIT_NEXT_LINK_SITE_CACHE_SIZE 2048

typedef struct {
    ObjFunction* function;
    NextIrInstr* ir;
    int ir_count;
    int ir_capacity;
    uint32_t unsupported_count;
    uint32_t call_sites;
    uint32_t invoke_sites;
    uint32_t loop_sites;
    NextLoweredTrace* traces;
    int trace_count;
    int trace_capacity;
    uint32_t lowered_trace_ops_total;
    uint32_t lowered_trace_calls_total;
    uint32_t lowered_trace_invokes_total;
    uint32_t lowered_trace_unsupported_total;
    uint32_t lowered_callsite_count;
    uint32_t lowered_callsite_guard_hints;
    uint8_t build_ok;
    uint8_t malformed;
} JitPipelineFunction;

struct JitPipelineState {
    JitPipelineFunction* functions;
    int function_count;
    int function_capacity;
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
    NextLinkEdge* link_edges;
    int link_edge_count;
    int link_edge_capacity;
    uint64_t link_edge_total;
    uint64_t link_edge_resolved;
    uint64_t link_edge_unresolved;
    uint64_t link_exec_attempts;
    uint64_t link_exec_hits;
    uint64_t link_patch_attempts;
    uint64_t link_patch_hits;
    uint8_t link_exec_enabled;
    uint8_t link_patch_enabled;
    ObjFunction** resolved_callee_table;
    int resolved_callee_capacity;
    NextResolvedSiteEntry* resolved_site_table;
    int resolved_site_capacity;
    NextLinkSiteCacheEntry link_site_cache[TOI_JIT_NEXT_LINK_SITE_CACHE_SIZE];
};

static uint32_t jit_pipeline_mix_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static uint32_t jit_pipeline_link_site_cache_hash(
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function) {
    uintptr_t a = (uintptr_t)caller_function;
    uintptr_t b = (uintptr_t)callee_function;
    uint32_t h = (uint32_t)(a >> 4) ^ (uint32_t)(a >> 17) ^ (uint32_t)(b >> 7) ^ (uint32_t)(b >> 21);
    h ^= callsite_offset * 2654435761u;
    return h & (TOI_JIT_NEXT_LINK_SITE_CACHE_SIZE - 1u);
}

static int jit_pipeline_pow2_ceil(int n) {
    int cap = 8;
    while (cap < n && cap < (1 << 30)) cap <<= 1;
    return cap;
}

static void jit_pipeline_clear_link_indexes(JitPipelineState* state) {
    if (state == NULL) return;
    free(state->resolved_callee_table);
    state->resolved_callee_table = NULL;
    state->resolved_callee_capacity = 0;
    free(state->resolved_site_table);
    state->resolved_site_table = NULL;
    state->resolved_site_capacity = 0;
}

void jit_pipeline_link_site_cache_clear(JitPipelineState* state) {
    if (state == NULL) return;
    memset(state->link_site_cache, 0, sizeof(state->link_site_cache));
}

static int jit_pipeline_rebuild_link_indexes(JitPipelineState* state) {
    if (state == NULL) return 0;
    jit_pipeline_clear_link_indexes(state);
    if (state->link_edge_count <= 0) return 1;

    int callee_cap = jit_pipeline_pow2_ceil(state->link_edge_count * 2);
    ObjFunction** callee_table = (ObjFunction**)calloc((size_t)callee_cap, sizeof(ObjFunction*));
    if (callee_table == NULL) return 0;

    int site_cap = jit_pipeline_pow2_ceil(state->link_edge_count * 2);
    NextResolvedSiteEntry* site_table =
        (NextResolvedSiteEntry*)calloc((size_t)site_cap, sizeof(NextResolvedSiteEntry));
    if (site_table == NULL) {
        free(callee_table);
        return 0;
    }

    for (int i = 0; i < state->link_edge_count; i++) {
        const NextLinkEdge* edge = &state->link_edges[i];
        if (edge->callee_function == NULL || edge->caller_function == NULL) continue;

        uint32_t callee_h = jit_pipeline_mix_u64((uint64_t)(uintptr_t)edge->callee_function);
        int callee_idx = (int)(callee_h & (uint32_t)(callee_cap - 1));
        for (int probe = 0; probe < callee_cap; probe++) {
            ObjFunction* cur = callee_table[callee_idx];
            if (cur == NULL || cur == edge->callee_function) {
                callee_table[callee_idx] = edge->callee_function;
                break;
            }
            callee_idx = (callee_idx + 1) & (callee_cap - 1);
        }

        uint64_t key = (uint64_t)(uintptr_t)edge->caller_function;
        key ^= ((uint64_t)(uintptr_t)edge->callee_function >> 4);
        key ^= ((uint64_t)edge->callsite_offset << 17);
        uint32_t site_h = jit_pipeline_mix_u64(key);
        int site_idx = (int)(site_h & (uint32_t)(site_cap - 1));
        for (int probe = 0; probe < site_cap; probe++) {
            NextResolvedSiteEntry* slot = &site_table[site_idx];
            if (slot->caller_function == NULL ||
                (slot->caller_function == edge->caller_function &&
                 slot->callee_function == edge->callee_function &&
                 slot->callsite_offset == edge->callsite_offset)) {
                slot->caller_function = edge->caller_function;
                slot->callee_function = edge->callee_function;
                slot->callsite_offset = edge->callsite_offset;
                break;
            }
            site_idx = (site_idx + 1) & (site_cap - 1);
        }
    }

    state->resolved_callee_table = callee_table;
    state->resolved_callee_capacity = callee_cap;
    state->resolved_site_table = site_table;
    state->resolved_site_capacity = site_cap;
    return 1;
}

static JitPipelineFunction* jit_pipeline_find_function(JitPipelineState* state, ObjFunction* function) {
    if (state == NULL || function == NULL) return NULL;
    for (int i = 0; i < state->function_count; i++) {
        if (state->functions[i].function == function) return &state->functions[i];
    }
    return NULL;
}

static JitPipelineFunction* jit_pipeline_find_or_add_function(JitPipelineState* state, ObjFunction* function) {
    if (state == NULL || function == NULL) return NULL;
    JitPipelineFunction* existing = jit_pipeline_find_function(state, function);
    if (existing != NULL) return existing;
    if (state->function_count >= state->function_capacity) {
        int new_capacity = (state->function_capacity < 16) ? 16 : state->function_capacity * 2;
        JitPipelineFunction* grown = (JitPipelineFunction*)realloc(state->functions, (size_t)new_capacity * sizeof(JitPipelineFunction));
        if (grown == NULL) return NULL;
        memset(grown + state->function_capacity, 0, (size_t)(new_capacity - state->function_capacity) * sizeof(JitPipelineFunction));
        state->functions = grown;
        state->function_capacity = new_capacity;
    }
    JitPipelineFunction* slot = &state->functions[state->function_count++];
    memset(slot, 0, sizeof(*slot));
    slot->function = function;
    return slot;
}

static int jit_pipeline_ir_push(JitPipelineFunction* fn, const NextIrInstr* in) {
    if (fn == NULL || in == NULL) return 0;
    if (fn->ir_count >= fn->ir_capacity) {
        int new_capacity = (fn->ir_capacity < 64) ? 64 : fn->ir_capacity * 2;
        NextIrInstr* grown = (NextIrInstr*)realloc(fn->ir, (size_t)new_capacity * sizeof(NextIrInstr));
        if (grown == NULL) return 0;
        fn->ir = grown;
        fn->ir_capacity = new_capacity;
    }
    fn->ir[fn->ir_count++] = *in;
    return 1;
}

static int jit_pipeline_trace_push(JitPipelineFunction* fn, const NextLoweredTrace* t) {
    if (fn == NULL || t == NULL) return 0;
    if (fn->trace_count >= fn->trace_capacity) {
        int new_capacity = (fn->trace_capacity < 8) ? 8 : fn->trace_capacity * 2;
        NextLoweredTrace* grown = (NextLoweredTrace*)realloc(fn->traces, (size_t)new_capacity * sizeof(NextLoweredTrace));
        if (grown == NULL) return 0;
        fn->traces = grown;
        fn->trace_capacity = new_capacity;
    }
    fn->traces[fn->trace_count++] = *t;
    return 1;
}

static int jit_pipeline_trace_callsite_push(NextLoweredTrace* trace, const NextLoweredCallsite* site) {
    if (trace == NULL || site == NULL) return 0;
    if (trace->callsite_count >= trace->callsite_capacity) {
        int new_capacity = (trace->callsite_capacity < 4) ? 4 : trace->callsite_capacity * 2;
        NextLoweredCallsite* grown =
            (NextLoweredCallsite*)realloc(trace->callsites, (size_t)new_capacity * sizeof(NextLoweredCallsite));
        if (grown == NULL) return 0;
        trace->callsites = grown;
        trace->callsite_capacity = new_capacity;
    }
    trace->callsites[trace->callsite_count++] = *site;
    return 1;
}

static int jit_pipeline_link_push(JitPipelineState* state, const NextLinkEdge* edge) {
    if (state == NULL || edge == NULL) return 0;
    if (state->link_edge_count >= state->link_edge_capacity) {
        int new_capacity = (state->link_edge_capacity < 16) ? 16 : state->link_edge_capacity * 2;
        NextLinkEdge* grown = (NextLinkEdge*)realloc(state->link_edges, (size_t)new_capacity * sizeof(NextLinkEdge));
        if (grown == NULL) return 0;
        state->link_edges = grown;
        state->link_edge_capacity = new_capacity;
    }
    state->link_edges[state->link_edge_count++] = *edge;
    return 1;
}

static int jit_pipeline_ir_find_index_by_offset(const JitPipelineFunction* fn, uint32_t byte_offset) {
    if (fn == NULL) return -1;
    for (int i = 0; i < fn->ir_count; i++) {
        if (fn->ir[i].byte_offset == byte_offset) return i;
    }
    return -1;
}

static ObjClosure* jit_pipeline_find_call_ic_hint(const Chunk* chunk, uint32_t byte_offset) {
    if (chunk == NULL || byte_offset >= (uint32_t)chunk->count) return NULL;
    ObjClosure* c = NULL;
    if (chunk->call_ic_closures != NULL) c = chunk->call_ic_closures[byte_offset];
    if (c == NULL && chunk->call_ic2_closures != NULL) c = chunk->call_ic2_closures[byte_offset];
    if (c == NULL && chunk->call_ic3_closures != NULL) c = chunk->call_ic3_closures[byte_offset];
    if (c == NULL && chunk->call_ic4_closures != NULL) c = chunk->call_ic4_closures[byte_offset];
    if (c == NULL && chunk->call_ic5_closures != NULL) c = chunk->call_ic5_closures[byte_offset];
    return c;
}

static int jit_pipeline_decode_callsite(const NextIrInstr* in, uint8_t* out_argc_known, uint8_t* out_argc, uint8_t* out_shape) {
    if (in == NULL || out_argc_known == NULL || out_argc == NULL || out_shape == NULL) return 0;
    *out_argc_known = 1;
    *out_argc = 0;
    switch (in->raw_opcode) {
        case OP_CALL0:
        case OP_QCALL0:
        case OP_QINVOKE0_STR_LOWER:
            *out_argc = 0;
            *out_shape = NEXT_ARGC_SHAPE_FIXED0;
            return 1;
        case OP_CALL1:
        case OP_QCALL1:
        case OP_QCALL1_STR_FIND:
        case OP_QINVOKE1_STR_FIND:
            *out_argc = 1;
            *out_shape = NEXT_ARGC_SHAPE_FIXED1;
            return 1;
        case OP_CALL2:
        case OP_QCALL2:
            *out_argc = 2;
            *out_shape = NEXT_ARGC_SHAPE_FIXED2;
            return 1;
        case OP_CALL:
            *out_argc = in->a;
            *out_shape = (in->a >= 3) ? NEXT_ARGC_SHAPE_FIXED3PLUS
                                       : (in->a == 2 ? NEXT_ARGC_SHAPE_FIXED2
                                                     : (in->a == 1 ? NEXT_ARGC_SHAPE_FIXED1 : NEXT_ARGC_SHAPE_FIXED0));
            return 1;
        case OP_INVOKE:
            *out_argc = in->b;
            *out_shape = (in->b >= 3) ? NEXT_ARGC_SHAPE_FIXED3PLUS
                                       : (in->b == 2 ? NEXT_ARGC_SHAPE_FIXED2
                                                     : (in->b == 1 ? NEXT_ARGC_SHAPE_FIXED1 : NEXT_ARGC_SHAPE_FIXED0));
            return 1;
        default:
            *out_argc_known = 0;
            *out_shape = NEXT_ARGC_SHAPE_DYNAMIC;
            return 1;
    }
}

static int jit_pipeline_read_u16(const Chunk* chunk, int offset, uint16_t* out) {
    if (chunk == NULL || out == NULL || offset < 0 || offset + 1 >= chunk->count) return 0;
    *out = (uint16_t)(((uint16_t)chunk->code[offset] << 8) | (uint16_t)chunk->code[offset + 1]);
    return 1;
}

static int jit_pipeline_decode_closure_width(const Chunk* chunk, int offset, int* out_next_offset) {
    if (chunk == NULL || out_next_offset == NULL) return 0;
    if (offset + 1 >= chunk->count) return 0;
    uint8_t constant = chunk->code[offset + 1];
    if (constant >= chunk->constants.count) return 0;
    Value c = chunk->constants.values[constant];
    if (!IS_FUNCTION(c)) return 0;
    ObjFunction* fn = AS_FUNCTION(c);
    if (fn == NULL || fn->upvalue_count < 0) return 0;
    if (fn->upvalue_count > (INT_MAX - 2) / 2) return 0;
    int width = 2 + fn->upvalue_count * 2;
    if (offset + width > chunk->count) return 0;
    *out_next_offset = offset + width;
    return 1;
}

static int jit_pipeline_instr_end(const Chunk* chunk, int offset, int* out_next_offset) {
    if (chunk == NULL || out_next_offset == NULL || offset < 0 || offset >= chunk->count) return 0;
    uint8_t op = chunk->code[offset];
    switch (op) {
        case OP_CONSTANT:
        case OP_GET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_SET_GLOBAL_POP:
        case OP_DELETE_GLOBAL:
        case OP_ADD_CONST:
        case OP_SUB_CONST:
        case OP_MUL_CONST:
        case OP_DIV_CONST:
        case OP_MOD_CONST:
        case OP_GET_FIELD_CONST:
        case OP_SET_FIELD_CONST:
        case OP_SET_FIELD_CONST_KEEP:
        case OP_IMPORT:
            if (offset + 1 >= chunk->count) return 0;
            *out_next_offset = offset + 2;
            return 1;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_LOOP:
        case OP_INVOKE:
        case OP_QINVOKE0_STR_LOWER:
        case OP_UNPACK:
        case OP_GET2_LOCAL:
        case OP_INC_LOCAL:
        case OP_SUB_LOCAL_CONST:
        case OP_MUL_LOCAL_CONST:
        case OP_DIV_LOCAL_CONST:
        case OP_MOD_LOCAL_CONST:
        case OP_FOR_PREP:
        case OP_FOR_LOOP:
        case OP_QFOR_PREP_NUM:
        case OP_QFOR_LOOP_NUM:
            if (offset + 2 >= chunk->count) return 0;
            *out_next_offset = offset + ((op == OP_FOR_PREP || op == OP_FOR_LOOP || op == OP_QFOR_PREP_NUM || op == OP_QFOR_LOOP_NUM) ? 5 : 3);
            return *out_next_offset <= chunk->count;
        case OP_CALL:
        case OP_CALL_NAMED:
        case OP_CALL_EXPAND:
        case OP_RETURN_N:
        case OP_ADJUST_STACK:
        case OP_BUILD_STRING:
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP:
        case OP_ADD_SET_LOCAL:
        case OP_ADD_SET_LOCAL_POP:
        case OP_SUB_SET_LOCAL:
        case OP_SUB_SET_LOCAL_POP:
        case OP_MUL_SET_LOCAL:
        case OP_MUL_SET_LOCAL_POP:
        case OP_DIV_SET_LOCAL:
        case OP_DIV_SET_LOCAL_POP:
        case OP_MOD_SET_LOCAL:
        case OP_MOD_SET_LOCAL_POP:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE:
        case OP_SET_UPVALUE_POP:
        case OP_PRINT:
            if (offset + 1 >= chunk->count) return 0;
            *out_next_offset = offset + 2;
            return 1;
        case OP_CLOSURE:
            return jit_pipeline_decode_closure_width(chunk, offset, out_next_offset);
        default:
            *out_next_offset = offset + 1;
            return 1;
    }
}

static NextIrOp jit_pipeline_map_opcode(uint8_t op) {
    switch (op) {
        case OP_CONSTANT: return NEXT_IR_OP_CONST;
        case OP_NIL: return NEXT_IR_OP_NIL;
        case OP_TRUE: return NEXT_IR_OP_TRUE;
        case OP_FALSE: return NEXT_IR_OP_FALSE;
        case OP_POP: return NEXT_IR_OP_POP;
        case OP_GET_LOCAL: return NEXT_IR_OP_GET_LOCAL;
        case OP_GET2_LOCAL: return NEXT_IR_OP_GET2_LOCAL;
        case OP_SET_LOCAL: return NEXT_IR_OP_SET_LOCAL;
        case OP_SET_LOCAL_POP: return NEXT_IR_OP_SET_LOCAL_POP;
        case OP_GET_UPVALUE: return NEXT_IR_OP_GET_UPVALUE;
        case OP_APPEND: return NEXT_IR_OP_APPEND;
        case OP_GET_TABLE: return NEXT_IR_OP_GET_TABLE;
        case OP_GET_TABLE_NUM: return NEXT_IR_OP_GET_TABLE_NUM;
        case OP_SET_TABLE: return NEXT_IR_OP_SET_TABLE;
        case OP_SET_TABLE_NUM: return NEXT_IR_OP_SET_TABLE_NUM;
        case OP_ADD:
        case OP_ADD_CONST:
        case OP_ADD_INPLACE:
        case OP_IADD:
        case OP_FADD:
        case OP_QFADD:
            return NEXT_IR_OP_ADD;
        case OP_SUBTRACT:
        case OP_SUB_CONST:
        case OP_ISUB:
        case OP_FSUB:
        case OP_QFSUB:
            return NEXT_IR_OP_SUB;
        case OP_MULTIPLY:
        case OP_MUL_CONST:
        case OP_IMUL:
        case OP_FMUL:
        case OP_QFMUL:
            return NEXT_IR_OP_MUL;
        case OP_DIVIDE:
        case OP_DIV_CONST:
        case OP_IDIV:
        case OP_FDIV:
        case OP_QFDIV:
            return NEXT_IR_OP_DIV;
        case OP_MODULO:
        case OP_MOD_CONST:
        case OP_IMOD:
        case OP_FMOD:
        case OP_QFMOD:
            return NEXT_IR_OP_MOD;
        case OP_NEGATE: return NEXT_IR_OP_NEG;
        case OP_NOT: return NEXT_IR_OP_NOT;
        case OP_EQUAL: return NEXT_IR_OP_CMP_EQ;
        case OP_LESS:
        case OP_QLESS_NUM:
            return NEXT_IR_OP_CMP_LT;
        case OP_GREATER:
        case OP_QGREATER_NUM:
            return NEXT_IR_OP_CMP_GT;
        case OP_JUMP: return NEXT_IR_OP_JUMP;
        case OP_JUMP_IF_FALSE: return NEXT_IR_OP_JUMP_IF_FALSE;
        case OP_JUMP_IF_TRUE: return NEXT_IR_OP_JUMP_IF_TRUE;
        case OP_LOOP:
        case OP_FOR_LOOP:
        case OP_QFOR_LOOP_NUM:
            return NEXT_IR_OP_LOOP;
        case OP_CALL:
        case OP_CALL0:
        case OP_CALL1:
        case OP_CALL2:
        case OP_QCALL0:
        case OP_QCALL1:
        case OP_QCALL2:
        case OP_QCALL1_STR_FIND:
            return NEXT_IR_OP_CALL;
        case OP_INVOKE:
        case OP_QINVOKE0_STR_LOWER:
        case OP_QINVOKE1_STR_FIND:
            return NEXT_IR_OP_INVOKE;
        case OP_BUILD_STRING2: return NEXT_IR_OP_BUILD_STRING2;
        case OP_BUILD_STRING3: return NEXT_IR_OP_BUILD_STRING3;
        case OP_BUILD_STRING4: return NEXT_IR_OP_BUILD_STRING4;
        case OP_BUILD_STRING5: return NEXT_IR_OP_BUILD_STRING5;
        case OP_ADJUST_STACK: return NEXT_IR_OP_ADJUST_STACK;
        case OP_RETURN: return NEXT_IR_OP_RETURN;
        default: return NEXT_IR_OP_UNSUPPORTED;
    }
}

static int jit_pipeline_build_ir(JitPipelineFunction* fn) {
    if (fn == NULL || fn->function == NULL) return 0;
    Chunk* chunk = &fn->function->chunk;
    free(fn->ir);
    fn->ir = NULL;
    fn->ir_count = 0;
    fn->ir_capacity = 0;
    fn->unsupported_count = 0;
    fn->call_sites = 0;
    fn->invoke_sites = 0;
    fn->loop_sites = 0;
    fn->malformed = 0;
    fn->build_ok = 0;

    for (int offset = 0; offset < chunk->count;) {
        int next_offset = 0;
        if (!jit_pipeline_instr_end(chunk, offset, &next_offset) || next_offset <= offset) {
            fn->malformed = 1;
            return 0;
        }
        uint8_t op = chunk->code[offset];
        NextIrInstr in;
        memset(&in, 0, sizeof(in));
        in.op = jit_pipeline_map_opcode(op);
        in.raw_opcode = op;
        in.byte_offset = (uint32_t)offset;
        if (next_offset > offset + 1) in.a = chunk->code[offset + 1];
        if (next_offset > offset + 2) in.b = chunk->code[offset + 2];
        if (op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_JUMP_IF_TRUE || op == OP_LOOP) {
            uint16_t jump = 0;
            if (!jit_pipeline_read_u16(chunk, offset + 1, &jump)) {
                fn->malformed = 1;
                return 0;
            }
            in.jump = jump;
            in.target_offset = (uint32_t)((op == OP_LOOP) ? (offset + 3 - (int)jump) : (offset + 3 + (int)jump));
        } else if (op == OP_FOR_PREP || op == OP_FOR_LOOP || op == OP_QFOR_PREP_NUM || op == OP_QFOR_LOOP_NUM) {
            uint16_t jump = 0;
            if (!jit_pipeline_read_u16(chunk, offset + 3, &jump)) {
                fn->malformed = 1;
                return 0;
            }
            in.jump = jump;
            in.target_offset = (uint32_t)((op == OP_FOR_LOOP || op == OP_QFOR_LOOP_NUM)
                                           ? (offset + 5 - (int)jump)
                                           : (offset + 5 + (int)jump));
        }
        if (!jit_pipeline_ir_push(fn, &in)) return 0;
        if (in.op == NEXT_IR_OP_UNSUPPORTED) fn->unsupported_count++;
        else if (in.op == NEXT_IR_OP_CALL) fn->call_sites++;
        else if (in.op == NEXT_IR_OP_INVOKE) fn->invoke_sites++;
        else if (in.op == NEXT_IR_OP_LOOP) fn->loop_sites++;

        offset = next_offset;
    }

    fn->build_ok = 1;
    return 1;
}

static int jit_pipeline_lower_traces(JitPipelineFunction* fn) {
    if (fn == NULL || !fn->build_ok || fn->ir == NULL || fn->ir_count <= 0) return 0;
    free(fn->traces);
    fn->traces = NULL;
    fn->trace_count = 0;
    fn->trace_capacity = 0;
    fn->lowered_trace_ops_total = 0;
    fn->lowered_trace_calls_total = 0;
    fn->lowered_trace_invokes_total = 0;
    fn->lowered_trace_unsupported_total = 0;
    fn->lowered_callsite_count = 0;
    fn->lowered_callsite_guard_hints = 0;

    for (int i = 0; i < fn->ir_count; i++) {
        const NextIrInstr* edge = &fn->ir[i];
        if (edge->op != NEXT_IR_OP_LOOP) continue;
        if (edge->target_offset >= edge->byte_offset) continue;
        int start_i = jit_pipeline_ir_find_index_by_offset(fn, edge->target_offset);
        if (start_i < 0 || start_i > i) continue;

        NextLoweredTrace t;
        memset(&t, 0, sizeof(t));
        t.start_ir_index = (uint32_t)start_i;
        t.end_ir_index = (uint32_t)i;
        t.op_count = (uint32_t)(i - start_i + 1);
        for (int j = start_i; j <= i; j++) {
            NextIrOp op = fn->ir[j].op;
            if (op == NEXT_IR_OP_CALL) t.call_sites++;
            else if (op == NEXT_IR_OP_INVOKE) t.invoke_sites++;
            else if (op == NEXT_IR_OP_UNSUPPORTED) t.unsupported_sites++;
            if (op == NEXT_IR_OP_CALL || op == NEXT_IR_OP_INVOKE) {
                NextLoweredCallsite site;
                memset(&site, 0, sizeof(site));
                site.ir_index = (uint32_t)j;
                site.byte_offset = fn->ir[j].byte_offset;
                site.is_invoke = (op == NEXT_IR_OP_INVOKE) ? 1u : 0u;
                uint8_t argc_known = 0;
                uint8_t argc = 0;
                uint8_t shape = NEXT_ARGC_SHAPE_DYNAMIC;
                if (!jit_pipeline_decode_callsite(&fn->ir[j], &argc_known, &argc, &shape)) return 0;
                site.argc_known = argc_known;
                site.argc = argc;
                site.argc_shape = shape;
                site.callee_from_top = argc_known ? (int16_t)argc : (int16_t)-1;
                ObjClosure* hint = jit_pipeline_find_call_ic_hint(&fn->function->chunk, site.byte_offset);
                site.guard_hint_closure = NULL;
                if (hint != NULL && hint->function != NULL) {
                    site.guard_hint_closure = hint;
                    site.has_guard_hint = 1u;
                } else {
                    site.has_guard_hint = 0u;
                }
                if (!jit_pipeline_trace_callsite_push(&t, &site)) return 0;
                fn->lowered_callsite_count++;
                if (site.has_guard_hint) fn->lowered_callsite_guard_hints++;
            }
        }
        if (!jit_pipeline_trace_push(fn, &t)) return 0;
        fn->lowered_trace_ops_total += t.op_count;
        fn->lowered_trace_calls_total += t.call_sites;
        fn->lowered_trace_invokes_total += t.invoke_sites;
        fn->lowered_trace_unsupported_total += t.unsupported_sites;
    }
    return 1;
}

static int jit_pipeline_rebuild_links(JitPipelineState* state) {
    if (state == NULL) return 0;
    jit_pipeline_link_site_cache_clear(state);
    state->link_edge_count = 0;
    state->link_edge_total = 0;
    state->link_edge_resolved = 0;
    state->link_edge_unresolved = 0;

    for (int fi = 0; fi < state->function_count; fi++) {
        JitPipelineFunction* fn = &state->functions[fi];
        if (!fn->build_ok || fn->trace_count <= 0) continue;
        for (int ti = 0; ti < fn->trace_count; ti++) {
            NextLoweredTrace* tr = &fn->traces[ti];
            for (int ci = 0; ci < tr->callsite_count; ci++) {
                NextLoweredCallsite* cs = &tr->callsites[ci];
                if (!cs->has_guard_hint || cs->guard_hint_closure == NULL || cs->guard_hint_closure->function == NULL) continue;
                NextLinkEdge edge;
                edge.caller_function = fn->function;
                edge.callee_function = cs->guard_hint_closure->function;
                edge.callsite_offset = cs->byte_offset;
                edge.trace_index = (uint32_t)ti;
                edge.callsite_index = (uint32_t)ci;
                edge.resolved = 1u;
                if (!jit_pipeline_link_push(state, &edge)) return 0;
                state->link_edge_total++;
                if (edge.resolved) state->link_edge_resolved++;
                else state->link_edge_unresolved++;
            }
        }
    }
    return jit_pipeline_rebuild_link_indexes(state);
}

JitPipelineState* jit_pipeline_create(void) {
    JitPipelineState* state = (JitPipelineState*)calloc(1, sizeof(JitPipelineState));
    if (state == NULL) return NULL;
    state->link_exec_enabled = (uint8_t)jit_pipeline_link_exec_enabled();
    state->link_patch_enabled = (uint8_t)jit_pipeline_link_patch_enabled();
    return state;
}

void jit_pipeline_free(JitPipelineState* state) {
    if (state == NULL) return;
    for (int i = 0; i < state->function_count; i++) {
        free(state->functions[i].ir);
        state->functions[i].ir = NULL;
        for (int t = 0; t < state->functions[i].trace_count; t++) {
            free(state->functions[i].traces[t].callsites);
            state->functions[i].traces[t].callsites = NULL;
        }
        free(state->functions[i].traces);
        state->functions[i].traces = NULL;
    }
    free(state->functions);
    state->functions = NULL;
    free(state->link_edges);
    state->link_edges = NULL;
    jit_pipeline_clear_link_indexes(state);
    free(state);
}

void jit_pipeline_mark_roots(JitPipelineState* state) {
    if (state == NULL) return;
    for (int i = 0; i < state->function_count; i++) {
        if (state->functions[i].function != NULL) mark_object((struct Obj*)state->functions[i].function);
        for (int t = 0; t < state->functions[i].trace_count; t++) {
            NextLoweredTrace* trace = &state->functions[i].traces[t];
            for (int c = 0; c < trace->callsite_count; c++) {
                if (trace->callsites[c].guard_hint_closure != NULL) {
                    mark_object((struct Obj*)trace->callsites[c].guard_hint_closure);
                }
            }
        }
    }
    for (int i = 0; i < state->link_edge_count; i++) {
        if (state->link_edges[i].caller_function != NULL) mark_object((struct Obj*)state->link_edges[i].caller_function);
        if (state->link_edges[i].callee_function != NULL) mark_object((struct Obj*)state->link_edges[i].callee_function);
    }
    for (int i = 0; i < TOI_JIT_NEXT_LINK_SITE_CACHE_SIZE; i++) {
        NextLinkSiteCacheEntry* e = &state->link_site_cache[i];
        if (e->payload_plus1 == 0) continue;
        if (e->caller_function != NULL) mark_object((struct Obj*)e->caller_function);
        if (e->callee_function != NULL) mark_object((struct Obj*)e->callee_function);
    }
}

void jit_pipeline_note_hot_function(JitPipelineState* state, ObjFunction* function) {
    if (state == NULL || function == NULL) return;
    state->hot_functions++;
}

int jit_pipeline_try_compile_stub(JitPipelineState* state, ObjFunction* function) {
    if (state == NULL || function == NULL) return 0;
    JitPipelineFunction* fn = jit_pipeline_find_or_add_function(state, function);
    if (fn == NULL) {
        state->compile_attempts++;
        state->compile_fail++;
        return 0;
    }
    if (fn->build_ok || fn->malformed) return 0;

    state->compile_attempts++;
    if (!jit_pipeline_build_ir(fn)) {
        state->compile_fail++;
        return 0;
    }
    state->compile_success++;
    state->functions_built++;
    state->ir_ops_total += (uint64_t)fn->ir_count;
    state->ir_unsupported_total += (uint64_t)fn->unsupported_count;
    state->ir_call_sites_total += (uint64_t)fn->call_sites;
    state->ir_invoke_sites_total += (uint64_t)fn->invoke_sites;
    state->ir_loop_sites_total += (uint64_t)fn->loop_sites;
    if (jit_pipeline_lower_traces(fn)) {
        state->lowered_trace_count += (uint64_t)fn->trace_count;
        state->lowered_trace_ops_total += (uint64_t)fn->lowered_trace_ops_total;
        state->lowered_trace_calls_total += (uint64_t)fn->lowered_trace_calls_total;
        state->lowered_trace_invokes_total += (uint64_t)fn->lowered_trace_invokes_total;
        state->lowered_trace_unsupported_total += (uint64_t)fn->lowered_trace_unsupported_total;
        state->lowered_callsite_count += (uint64_t)fn->lowered_callsite_count;
        state->lowered_callsite_guard_hints += (uint64_t)fn->lowered_callsite_guard_hints;
    }
    if (!jit_pipeline_rebuild_links(state)) return 0;
    return 0;
}

void jit_pipeline_note_exec_attempt(JitPipelineState* state) {
    if (state == NULL) return;
    state->exec_attempts++;
}

void jit_pipeline_note_exec_hit(JitPipelineState* state) {
    if (state == NULL) return;
    state->exec_hits++;
}

uint64_t jit_pipeline_hot_functions(const JitPipelineState* state) {
    return (state != NULL) ? state->hot_functions : 0u;
}

uint64_t jit_pipeline_compile_attempts(const JitPipelineState* state) {
    return (state != NULL) ? state->compile_attempts : 0u;
}

uint64_t jit_pipeline_compile_success(const JitPipelineState* state) {
    return (state != NULL) ? state->compile_success : 0u;
}

uint64_t jit_pipeline_compile_fail(const JitPipelineState* state) {
    return (state != NULL) ? state->compile_fail : 0u;
}

uint64_t jit_pipeline_exec_attempts(const JitPipelineState* state) {
    return (state != NULL) ? state->exec_attempts : 0u;
}

uint64_t jit_pipeline_exec_hits(const JitPipelineState* state) {
    return (state != NULL) ? state->exec_hits : 0u;
}

uint64_t jit_pipeline_functions_built(const JitPipelineState* state) {
    return (state != NULL) ? state->functions_built : 0u;
}

uint64_t jit_pipeline_ir_ops_total(const JitPipelineState* state) {
    return (state != NULL) ? state->ir_ops_total : 0u;
}

uint64_t jit_pipeline_ir_unsupported_total(const JitPipelineState* state) {
    return (state != NULL) ? state->ir_unsupported_total : 0u;
}

uint64_t jit_pipeline_ir_call_sites_total(const JitPipelineState* state) {
    return (state != NULL) ? state->ir_call_sites_total : 0u;
}

uint64_t jit_pipeline_ir_invoke_sites_total(const JitPipelineState* state) {
    return (state != NULL) ? state->ir_invoke_sites_total : 0u;
}

uint64_t jit_pipeline_ir_loop_sites_total(const JitPipelineState* state) {
    return (state != NULL) ? state->ir_loop_sites_total : 0u;
}

uint64_t jit_pipeline_lowered_trace_count(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_trace_count : 0u;
}

uint64_t jit_pipeline_lowered_trace_ops_total(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_trace_ops_total : 0u;
}

uint64_t jit_pipeline_lowered_trace_calls_total(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_trace_calls_total : 0u;
}

uint64_t jit_pipeline_lowered_trace_invokes_total(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_trace_invokes_total : 0u;
}

uint64_t jit_pipeline_lowered_trace_unsupported_total(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_trace_unsupported_total : 0u;
}

uint64_t jit_pipeline_lowered_callsite_count(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_callsite_count : 0u;
}

uint64_t jit_pipeline_lowered_callsite_guard_hints(const JitPipelineState* state) {
    return (state != NULL) ? state->lowered_callsite_guard_hints : 0u;
}

uint64_t jit_pipeline_link_edge_count(const JitPipelineState* state) {
    return (state != NULL) ? state->link_edge_total : 0u;
}

uint64_t jit_pipeline_link_resolved_count(const JitPipelineState* state) {
    return (state != NULL) ? state->link_edge_resolved : 0u;
}

uint64_t jit_pipeline_link_unresolved_count(const JitPipelineState* state) {
    return (state != NULL) ? state->link_edge_unresolved : 0u;
}

int jit_pipeline_has_resolved_callee(const JitPipelineState* state, ObjFunction* function) {
    if (state == NULL || function == NULL) return 0;
    if (state->resolved_callee_table != NULL && state->resolved_callee_capacity > 0) {
        int cap = state->resolved_callee_capacity;
        uint32_t h = jit_pipeline_mix_u64((uint64_t)(uintptr_t)function);
        int idx = (int)(h & (uint32_t)(cap - 1));
        for (int probe = 0; probe < cap; probe++) {
            ObjFunction* cur = state->resolved_callee_table[idx];
            if (cur == NULL) return 0;
            if (cur == function) return 1;
            idx = (idx + 1) & (cap - 1);
        }
        return 0;
    }
    for (int i = 0; i < state->link_edge_count; i++) {
        const NextLinkEdge* e = &state->link_edges[i];
        if (e->callee_function == function) return 1;
    }
    return 0;
}

int jit_pipeline_has_resolved_callee_at_site(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function) {
    if (state == NULL || caller_function == NULL || callee_function == NULL) return 0;
    if (state->resolved_site_table != NULL && state->resolved_site_capacity > 0) {
        int cap = state->resolved_site_capacity;
        uint64_t key = (uint64_t)(uintptr_t)caller_function;
        key ^= ((uint64_t)(uintptr_t)callee_function >> 4);
        key ^= ((uint64_t)callsite_offset << 17);
        uint32_t h = jit_pipeline_mix_u64(key);
        int idx = (int)(h & (uint32_t)(cap - 1));
        for (int probe = 0; probe < cap; probe++) {
            const NextResolvedSiteEntry* e = &state->resolved_site_table[idx];
            if (e->caller_function == NULL) return 0;
            if (e->caller_function == caller_function &&
                e->callee_function == callee_function &&
                e->callsite_offset == callsite_offset) {
                return 1;
            }
            idx = (idx + 1) & (cap - 1);
        }
        return 0;
    }
    for (int i = 0; i < state->link_edge_count; i++) {
        const NextLinkEdge* e = &state->link_edges[i];
        if (e->caller_function != caller_function) continue;
        if (e->callsite_offset != callsite_offset) continue;
        if (e->callee_function == callee_function) return 1;
    }
    return 0;
}

int jit_pipeline_can_execute_linked(const JitPipelineState* state, ObjFunction* callee_function) {
    if (state == NULL || callee_function == NULL) return 0;
    return jit_pipeline_has_resolved_callee(state, callee_function);
}

int jit_pipeline_can_execute_linked_at_site(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function) {
    if (state == NULL || caller_function == NULL || callee_function == NULL) return 0;
    return jit_pipeline_has_resolved_callee_at_site(
        state, caller_function, callsite_offset, callee_function);
}

int jit_pipeline_link_exec_is_enabled(const JitPipelineState* state) {
    return (state != NULL) ? (int)state->link_exec_enabled : 0;
}

int jit_pipeline_link_patch_is_enabled(const JitPipelineState* state) {
    return (state != NULL) ? (int)state->link_patch_enabled : 0;
}

void jit_pipeline_note_link_exec_attempt(JitPipelineState* state) {
    if (state == NULL) return;
    state->link_exec_attempts++;
}

void jit_pipeline_note_link_exec_hit(JitPipelineState* state) {
    if (state == NULL) return;
    state->link_exec_hits++;
}

void jit_pipeline_note_link_patch_attempt(JitPipelineState* state) {
    if (state == NULL) return;
    state->link_patch_attempts++;
}

void jit_pipeline_note_link_patch_hit(JitPipelineState* state) {
    if (state == NULL) return;
    state->link_patch_hits++;
}

uint32_t jit_pipeline_link_site_cache_lookup(
    const JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function) {
    if (state == NULL || caller_function == NULL || callee_function == NULL) return 0u;
    uint32_t h = jit_pipeline_link_site_cache_hash(caller_function, callsite_offset, callee_function);
    const NextLinkSiteCacheEntry* e = &state->link_site_cache[h];
    if (e->caller_function != caller_function ||
        e->callee_function != callee_function ||
        e->callsite_offset != callsite_offset) {
        return 0u;
    }
    return e->payload_plus1;
}

void jit_pipeline_link_site_cache_store(
    JitPipelineState* state,
    ObjFunction* caller_function,
    uint32_t callsite_offset,
    ObjFunction* callee_function,
    uint32_t payload_plus1) {
    if (state == NULL || caller_function == NULL || callee_function == NULL) return;
    uint32_t h = jit_pipeline_link_site_cache_hash(caller_function, callsite_offset, callee_function);
    NextLinkSiteCacheEntry* e = &state->link_site_cache[h];
    e->caller_function = caller_function;
    e->callee_function = callee_function;
    e->callsite_offset = callsite_offset;
    e->payload_plus1 = payload_plus1;
}

int jit_pipeline_pipeline_enabled(void) {
    const char* v = getenv("TOI_JIT_NEXT");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

int jit_pipeline_link_exec_enabled(void) {
    const char* v = getenv("TOI_JIT_NEXT_LINK_EXEC");
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

int jit_pipeline_link_patch_enabled(void) {
    const char* v = getenv("TOI_JIT_NEXT_LINK_PATCH");
    if (v == NULL || v[0] == '\0') return 1;
    return strcmp(v, "0") != 0;
}

int jit_pipeline_native_trace_compiled_exec_enabled(void) {
    const char* v = getenv("TOI_JIT_NATIVE_TRACE_COMPILED");
    if (v == NULL || v[0] == '\0') return 1;
    return strcmp(v, "0") != 0;
}

void jit_pipeline_collect_stats(const JitPipelineState* state, JitPipelineStats* out_stats) {
    if (out_stats == NULL) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (state == NULL) return;
    out_stats->hot_functions = state->hot_functions;
    out_stats->compile_attempts = state->compile_attempts;
    out_stats->compile_success = state->compile_success;
    out_stats->compile_fail = state->compile_fail;
    out_stats->exec_attempts = state->exec_attempts;
    out_stats->exec_hits = state->exec_hits;
    out_stats->functions_built = state->functions_built;
    out_stats->ir_ops_total = state->ir_ops_total;
    out_stats->ir_unsupported_total = state->ir_unsupported_total;
    out_stats->ir_call_sites_total = state->ir_call_sites_total;
    out_stats->ir_invoke_sites_total = state->ir_invoke_sites_total;
    out_stats->ir_loop_sites_total = state->ir_loop_sites_total;
    out_stats->lowered_trace_count = state->lowered_trace_count;
    out_stats->lowered_trace_ops_total = state->lowered_trace_ops_total;
    out_stats->lowered_trace_calls_total = state->lowered_trace_calls_total;
    out_stats->lowered_trace_invokes_total = state->lowered_trace_invokes_total;
    out_stats->lowered_trace_unsupported_total = state->lowered_trace_unsupported_total;
    out_stats->lowered_callsite_count = state->lowered_callsite_count;
    out_stats->lowered_callsite_guard_hints = state->lowered_callsite_guard_hints;
    out_stats->link_edge_total = state->link_edge_total;
    out_stats->link_edge_resolved = state->link_edge_resolved;
    out_stats->link_edge_unresolved = state->link_edge_unresolved;
    out_stats->link_exec_attempts = state->link_exec_attempts;
    out_stats->link_exec_hits = state->link_exec_hits;
    out_stats->link_patch_attempts = state->link_patch_attempts;
    out_stats->link_patch_hits = state->link_patch_hits;
    out_stats->link_exec_enabled = state->link_exec_enabled;
    out_stats->link_patch_enabled = state->link_patch_enabled;
}
