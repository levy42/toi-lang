/* Owned per-trace inline caches. Published entries are immutable until the
 * parent trace is freed; PIC replacement never frees executable code in use. */
#define XE_COUNT(ctx, name) ((void)__atomic_fetch_add(&(ctx)->stats.name, 1, __ATOMIC_RELAXED))
static XeCalleeEntry* xe_make_callee(XeTraceContext*, ObjClosure*, JitProgram*, int);

static int xe_key_equal(ObjString* a, ObjString* b) {
    return a == b || (a && b && a->length == b->length &&
                     memcmp(a->chars, b->chars, (size_t)a->length) == 0);
}
static int xe_table_slot(XeSite* site, ObjTable* table, ObjString* key) {
    for (int i = 0; i < 4; i++) {
        XeTableLane* lane = &site->tables[i];
        if (__atomic_load_n(&lane->table, __ATOMIC_ACQUIRE) != table ||
            __atomic_load_n(&lane->key, __ATOMIC_RELAXED) != key ||
            __atomic_load_n(&lane->shape, __ATOMIC_RELAXED) != table->table.shape_version) continue;
        int slot = __atomic_load_n(&lane->slot, __ATOMIC_RELAXED);
        // Validate the actual entry as well as the cache. Pointer reuse, shape
        // changes and replacement of a method value must never return stale data.
        if (slot >= 0 && slot < table->table.capacity && xe_key_equal(table->table.entries[slot].key, key)) return slot;
    }
    return -1;
}
static void xe_table_remember(XeSite* site, ObjTable* table, ObjString* key, int slot) {
    unsigned i = __atomic_fetch_add(&site->table_next, 1, __ATOMIC_RELAXED) & 3u;
    XeTableLane* lane = &site->tables[i];
    __atomic_store_n(&lane->slot, slot, __ATOMIC_RELAXED);
    __atomic_store_n(&lane->key, key, __ATOMIC_RELAXED);
    __atomic_store_n(&lane->shape, table->table.shape_version, __ATOMIC_RELAXED);
    __atomic_store_n(&lane->table, table, __ATOMIC_RELEASE);
}
static int xe_table_lookup(XeSite* site, ObjTable* table, ObjString* key, Value* out, int method) {
    int slot = xe_table_slot(site, table, key);
    if (slot >= 0) {
        *out = table->table.entries[slot].value;
        if (method) XE_COUNT(site->owner, method_ic_hits);
        else XE_COUNT(site->owner, table_ic_hits);
        return 1;
    }
    XE_COUNT(site->owner, table_ic_misses);
    if (!table_get_with_slot(&table->table, key, out, &slot)) return 0;
    xe_table_remember(site, table, key, slot); return 1;
}
static void xe_table_store(XeSite* site, ObjTable* table, ObjString* key, Value value) {
    int slot = xe_table_slot(site, table, key);
    if (slot >= 0 && table_set_at_slot(&table->table, slot, key, value)) {
        XE_COUNT(site->owner, table_ic_hits); return;
    }
    XE_COUNT(site->owner, table_ic_misses);
    table_set_with_slot(&table->table, key, value, &slot);
    xe_table_remember(site, table, key, slot);
}
static XeCalleeEntry* xe_cached_callee(XeTraceContext* ctx, VM* vm, ObjClosure* closure,
                                     JitProgram* hint, int argc) {
    ObjFunction* f = closure->function;
    if (!f || f->is_variadic || f->is_generator || f->defaults_count || f->arity != argc) return NULL;
    XeCalleeEntry* e = __atomic_load_n(&ctx->callees, __ATOMIC_ACQUIRE);
    for (; e; e = e->next) if (e->closure == closure && e->argc == argc) return e;
    pthread_mutex_lock(&ctx->compile_lock);
    for (e = ctx->callees; e; e = e->next) {
        if (e->closure == closure && e->argc == argc) break;
    }
    if (!e && ctx->entry_count < 64) {
        JitProgram* program = hint;
        if (!program && vm) (void)jit_get_compiled_closure_program(vm, closure, &program);
        if (program && program->function == f) {
            e = xe_make_callee(ctx, closure, program, argc);
            if (e) {
                e->next = ctx->callees;
                __atomic_store_n(&ctx->callees, e, __ATOMIC_RELEASE);
                ctx->entry_count++; XE_COUNT(ctx, callee_compiles);
            }
        }
    }
    pthread_mutex_unlock(&ctx->compile_lock);
    return e;
}
static int xe_site_call(XeSite* site, VM* vm, Value callee, Value* args, int argc, Value* out) {
    if (IS_BOUND_METHOD(callee)) {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        if (!bound->method || argc >= 255) return 0;
        Value shifted[argc + 1]; shifted[0] = bound->receiver;
        for (int i = 0; i < argc; i++) shifted[i + 1] = args[i];
        return xe_site_call(site, vm, OBJ_VAL(bound->method), shifted, argc + 1, out);
    }
    if (!IS_CLOSURE(callee)) return jit_call_primitive(vm, callee, args, argc, out);
    ObjClosure* closure = AS_CLOSURE(callee);
    // The tiny evaluator is already a specialized path and also handles
    // synthetic tiny functions without a general compiled program.
    if (argc == 2 && closure->function->tiny_inline_kind != TINY_INLINE_NONE)
        return jit_call_primitive(vm, callee, args, argc, out);
    XeCalleeEntry* entry = NULL;
    for (int i = 0; i < 4; i++) {
        XeCalleeEntry* candidate = __atomic_load_n(&site->calls[i], __ATOMIC_ACQUIRE);
        if (candidate && candidate->closure == closure && candidate->argc == argc) { entry = candidate; break; }
    }
    if (entry) XE_COUNT(site->owner, call_pic_hits);
    else {
        XE_COUNT(site->owner, call_pic_misses);
        entry = xe_cached_callee(site->owner, vm, closure, NULL, argc);
        if (entry) {
            unsigned lane = __atomic_fetch_add(&site->call_next, 1, __ATOMIC_RELAXED) & 3u;
            __atomic_store_n(&site->calls[lane], entry, __ATOMIC_RELEASE);
        }
    }
    if (entry) {
        int ok = xe_callee_execute(entry, vm, callee, args, argc, out);
        if (ok && entry->fn) XE_COUNT(site->owner, native_chain_hits);
        // A failed call has its own continuation; do not speculatively execute
        // it a second time here. The outer trace materializes the CALL operands.
        return ok;
    }
    return jit_call_primitive(vm, callee, args, argc, out);
}
static int xe_site_invoke(XeSite* site, VM* vm, Value receiver, Value method, Value* args, int argc, Value* out) {
    if (IS_TABLE(receiver) && IS_STRING(method) && AS_TABLE(receiver)->no_metatable_fast) {
        ObjTable* table = AS_TABLE(receiver); Value target;
        if (!xe_table_lookup(site, table, AS_STRING(method), &target, 1)) return 0;
        if (!table->is_module && jit_value_is_self_callable(target)) {
            if (argc >= 255) return 0;
            Value shifted[argc + 1]; shifted[0] = receiver;
            for (int i = 0; i < argc; i++) shifted[i + 1] = args[i];
            return xe_site_call(site, vm, target, shifted, argc + 1, out);
        }
        return xe_site_call(site, vm, target, args, argc, out);
    }
    return jit_invoke_primitive(vm, receiver, method, args, argc, out);
}
