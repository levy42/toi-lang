#ifndef VM_H
#define VM_H

#include "object.h"
#include "value.h"
#include "table.h"

typedef struct {
    ObjString* a;
    ObjString* b;
    ObjString* c;
    ObjString* out;
} BuildString3CacheEntry;

typedef struct VM {
   ObjThread* current_thread;
   ObjThread* gc_parked_threads;
   Table globals;
   Table modules;  // Cache of loaded modules (native and .toi)
   ObjString* build_string_cache[65536];
   BuildString3CacheEntry build_string3_cache[4096];
    int use_thread_tls;
   int cli_argc;
   char** cli_argv;
    int disable_gc;
    uint32_t gc_check_interval;
    uint32_t gc_check_counter;
    int is_repl;
    ObjString* mm_index;
    ObjString* mm_newindex;
    ObjString* mm_str;
    ObjString* mm_call;
    ObjString* mm_new;
    ObjString* mm_append;
    ObjString* mm_next;
    ObjString* mm_slice;
    ObjString* str_module_name;
    ObjString* str_upper_name;
    ObjString* str_lower_name;
    ObjString* str_find_name;
    ObjString* str_replace_name;
    ObjString* slice_name;
    ObjString* module_name_key;
    ObjString* module_file_key;
    ObjString* module_main_key;
    Value str_upper_fn;
    Value str_lower_fn;
    Value str_find_fn;
    Value str_replace_fn;
    int tiered_jit_enabled;
    int tiered_jit_profile;
    uint32_t tier1_hot_threshold;
    uint64_t tier1_promote_count;
    uint64_t tier1_call2_patch_count;
    uint32_t tier2_hot_threshold;
    uint32_t tier2_inline_hot_threshold;
    uint64_t tier2_promote_count;
    uint64_t tier2_exec_count;
    int native_jit_enabled;
    uint64_t native_jit_hot_candidates;
    uint64_t native_jit_compile_attempts;
    uint64_t native_jit_compiled_count;
    uint64_t native_jit_exec_attempts;
    uint64_t native_jit_exec_hits;
    void* native_jit;
    int opcode_prof_enabled;
    uint64_t opcode_prof_count[256];
    uint64_t opcode_prof_ns[256];
    int gc_prof_enabled;
    uint64_t gc_prof_start_ns;
    uint64_t gc_prof_collect_count;
    uint64_t gc_prof_auto_count;
    uint64_t gc_prof_manual_count;
    uint64_t gc_prof_final_count;
    uint64_t gc_prof_total_ns;
    uint64_t gc_prof_mark_ns;
    uint64_t gc_prof_sweep_ns;
    uint64_t gc_prof_max_ns;
    uint64_t gc_prof_auto_trigger_checks;
    uint64_t gc_prof_auto_trigger_hits;
    uint64_t gc_prof_bytes_before_sum;
    uint64_t gc_prof_bytes_after_sum;
    uint64_t gc_prof_bytes_freed_sum;
    int call_prof_enabled;
    uint64_t call_prof_op_call_count;
    uint64_t call_prof_op_call0_count;
    uint64_t call_prof_op_call1_count;
    uint64_t call_prof_op_call2_count;
    uint64_t call_prof_op_qcall2_count;
    uint64_t call_prof_op_call_named_count;
    uint64_t call_prof_op_call_expand_count;
    uint64_t call_prof_op_call_ns;
    uint64_t call_prof_op_call0_ns;
    uint64_t call_prof_op_call1_ns;
    uint64_t call_prof_op_call2_ns;
    uint64_t call_prof_op_call_named_ns;
    uint64_t call_prof_op_call_expand_ns;
    uint64_t call_prof_call_ic_hit_count;
    uint64_t call_prof_call_ic_miss_count;
    uint64_t call_prof_call_direct_fast_count;
    uint64_t call_prof_call_fallback_count;
    uint64_t call_prof_call_expand_named_count;
    uint64_t call_prof_call_expand_positional_count;
    uint64_t call_prof_call_expand_spread_values;
    uint64_t call_prof_call_value_native_count;
    uint64_t call_prof_call_value_closure_count;
    uint64_t call_prof_call_value_fast_count;
    uint64_t call_prof_call_value_noncallable_count;
    uint64_t call_prof_call_value_native_ns;
    uint64_t call_prof_call_value_closure_ns;
    uint64_t call_prof_call_value_native_fast_ns;
    uint64_t call_prof_call_value_native_slow_ns;
    uint64_t call_prof_native_fast_try_count;
    uint64_t call_prof_native_fast_hit_count;
    uint64_t call_prof_native_fast_miss_count;
    uint64_t call_prof_native_fast_error_count;
    uint64_t call_prof_native_slow_count;
    uint64_t call_prof_native_thread_switch_count;
    uint64_t call_prof_native_fast_str_upper_hit_count;
    uint64_t call_prof_native_fast_str_lower_hit_count;
    uint64_t call_prof_native_fast_str_replace_hit_count;
    uint64_t call_prof_native_fast_str_join_hit_count;
    uint64_t call_prof_native_fast_str_format_hit_count;
    uint64_t call_prof_native_fast_core_next_hit_count;
    uint64_t call_prof_native_fast_core_min_hit_count;
    uint64_t call_prof_native_fast_core_max_hit_count;
    uint64_t call_prof_native_fast_core_sum_hit_count;
    uint64_t call_prof_call2_ic_hit_count;
    uint64_t call_prof_call2_ic_miss_count;
    uint64_t call_prof_call2_direct_fast_count;
    uint64_t call_prof_call2_typed_fast_count;
    uint64_t call_prof_call2_tiny_inline_fast_count;
    uint64_t call_prof_call2_fallback_count;
    uint64_t call_prof_call0_direct_fast_count;
    uint64_t call_prof_call0_bound_native_fast_count;
    uint64_t call_prof_call0_native_count;
    uint64_t call_prof_call0_fallback_count;
    uint64_t call_prof_call1_direct_fast_count;
    uint64_t call_prof_call1_typed_fast_count;
    uint64_t call_prof_call1_native_count;
    uint64_t call_prof_call1_fallback_count;
    ObjNative* call_prof_native_slow_keys[16];
    uint64_t call_prof_native_slow_call_counts[16];
    uint64_t call_prof_native_slow_ns_by_key[16];
    int table_prof_enabled;
    int table_site_prof_enabled;
    uint64_t table_prof_get_ic_exact_hit_count;
    uint64_t table_prof_get_ic_slot_hit_count;
    uint64_t table_prof_get_ic_miss_count;
    uint64_t table_prof_get_generic_lookup_count;
    uint64_t table_prof_get_meta_index_count;
    uint64_t table_prof_set_ic_exact_hit_count;
    uint64_t table_prof_set_ic_slot_hit_count;
    uint64_t table_prof_set_ic_miss_count;
    uint64_t table_prof_set_generic_store_count;
    uint64_t table_prof_set_meta_newindex_count;
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

// Exposed VM Core Functions
void init_vm(VM* vm);
void free_vm(VM* vm);
InterpretResult interpret(VM* vm, ObjFunction* function);
void push(VM* vm, Value value);
Value pop(VM* vm);
Value peek(VM* vm, int distance);
void define_native(VM* vm, const char* name, NativeFn function);
void vm_runtime_error(VM* vm, const char* format, ...);
ObjString* number_key_string(double num);
int call(VM* vm, ObjClosure* closure, int arg_count);
int call_value(VM* vm, Value callee, int arg_count, CallFrame** frame, uint8_t** ip);
void maybe_collect_garbage_slow(VM* vm);
static inline void maybe_collect_garbage(VM* vm) {
    if (vm->disable_gc) return;
    if (vm->gc_check_counter > 1) {
        vm->gc_check_counter--;
        return;
    }
    vm->gc_check_counter = vm->gc_check_interval;
    maybe_collect_garbage_slow(vm);
}
InterpretResult vm_run(VM* vm, int min_frame_count);
InterpretResult vm_run_until_thread(VM* vm, int min_frame_count, ObjThread* stop_thread);
Value get_metamethod(VM* vm, Value val, const char* name);
void vm_request_interrupt(void);
ObjThread* vm_current_thread(VM* vm);
void vm_set_current_thread(VM* vm, ObjThread* thread);
int vm_native_wait(VM* vm, const char* command, Value resource);
void vm_enable_thread_tls(VM* vm);

#endif
