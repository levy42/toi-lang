/* Entry trampolines have the same VM/closure/args/out ABI as the ARM64 chains. */
static int xe_callee_thunk(XeCalleeEntry* entry) {
    X64Emitter e = {0};
    xe_mov(&e, 9, 1); // out -> r9
    xe_mov(&e, 8, 2); // args -> r8
    xe_imm(&e, 1, entry->argc);
    xe_imm(&e, 2, (uintptr_t)entry->program);
    xe_imm(&e, 0, (uintptr_t)&toi_jit_execute_program_direct_with_args);
    xe_byte(&e, 0xff); xe_byte(&e, 0xe0);
    void* fn;
    if (!xe_publish(&e, NULL, &fn, &entry->entry_memory, &entry->entry_size)) return 0;
    entry->entry_fn = (XeCalleeFn)fn; return 1;
}
static int xe_native_callee_thunk(XeCalleeEntry* entry) {
    X64Emitter e = {0};
    xe_push(&e, 12); xe_push(&e, 13); xe_push(&e, 14); xe_push(&e, 15);
    int state_offset = entry->slot_count * 16;
    int frame = ((state_offset + (int)sizeof(ToiJitNativeExecState) + 15) & ~15) + 8;
    xe_stack(&e, frame, 0);
    xe_mov(&e, 12, 7); xe_mov(&e, 13, 6); xe_mov(&e, 14, 2); xe_mov(&e, 15, 1);
    for (int i = 0; i < entry->slot_count; i++) {
        xe_imm(&e, 0, VAL_NIL); xe_store(&e, 0, 4, i * 16);
        xe_imm(&e, 0, 0); xe_store(&e, 0, 4, i * 16 + 8);
    }
    xe_imm(&e, 0, VAL_OBJ); xe_store(&e, 0, 4, 0); xe_store(&e, 13, 4, 8);
    for (int i = 0; i < entry->argc; i++) xe_value_copy(&e, 4, (i + 1) * 16, 14, i * 16);
    xe_imm(&e, 0, VAL_NUMBER);
    xe_store(&e, 0, 4, entry->var_slot * 16); xe_store(&e, 0, 4, entry->end_slot * 16);
    xe_double(&e, 0, -1); xe_sse_mem(&e, 1, 0, 4, entry->var_slot * 16 + 8);
    xe_mov(&e, 7, 12); xe_mov(&e, 6, 13); xe_lea(&e, 2, 4, 0);
    xe_imm(&e, 1, entry->slot_count); xe_lea(&e, 8, 4, state_offset);
    xe_call(&e, (void*)entry->fn);
    xe_byte(&e, 0x83); xe_byte(&e, 0xf8); xe_byte(&e, TOI_JIT_NATIVE_EXEC_EXIT);
    size_t failed = xe_jump(&e, 5);
    xe_value_copy(&e, 15, 0, 4, entry->ret_slot * 16); xe_imm(&e, 0, 1);
    size_t done = xe_jump(&e, -1);
    xe_patch(&e, failed, e.count); xe_imm(&e, 0, 0);
    xe_patch(&e, done, e.count);
    xe_stack(&e, frame, 1); xe_pop(&e, 15); xe_pop(&e, 14); xe_pop(&e, 13); xe_pop(&e, 12); xe_byte(&e, 0xc3);
    void* fn;
    if (!xe_publish(&e, NULL, &fn, &entry->entry_memory, &entry->entry_size)) return 0;
    entry->entry_fn = (XeCalleeFn)fn; return 1;
}
static XeCalleeEntry* xe_make_callee(XeTraceContext* ctx, ObjClosure* closure, JitProgram* program, int argc) {
    (void)ctx;
    XeCalleeEntry* entry = calloc(1, sizeof(*entry)); if (!entry) return NULL;
    entry->closure = closure; entry->program = program; entry->argc = (uint8_t)argc;
    ToiJitNativeTraceInstr lowered[256]; int count = 0;
    uint8_t ret, var, end, slots;
    int acyclic = program->op_count > 0 && program->op_count <= 1024;
    for (int j = 0; acyclic && j < program->op_count; j++) {
        JitOp* op = &program->ops[j];
        if ((op->op == JOP_JUMP || op->op == JOP_JUMP_IF_FALSE || op->op == JOP_JUMP_IF_TRUE) && op->target <= j) acyclic = 0;
    }
    if (acyclic && program->max_slot >= argc && program->max_slot <= 251 &&
        jit_native_lower_chain_safe_program(program, argc, lowered, &count, &ret, &var, &end, &slots)) {
        int pure = 1;
        for (int j = 0; j < count; j++) {
            switch (lowered[j].op) {
                case TOI_JIT_NATIVE_OP_CALL: case TOI_JIT_NATIVE_OP_INVOKE: case TOI_JIT_NATIVE_OP_APPEND:
                case TOI_JIT_NATIVE_OP_SLICE:
                case TOI_JIT_NATIVE_OP_SET_TABLE: case TOI_JIT_NATIVE_OP_SET_TABLE_NUM:
                case TOI_JIT_NATIVE_OP_BUILD_STRING2: case TOI_JIT_NATIVE_OP_BUILD_STRING3:
                case TOI_JIT_NATIVE_OP_BUILD_STRING4: case TOI_JIT_NATIVE_OP_BUILD_STRING5: pure = 0; break;
                default: break;
            }
        }
        void* fn;
        if (pure && xe_compile_trace(lowered, count, var, end, &fn, &entry->memory, &entry->size)) {
            entry->fn = (ToiJitNativeTraceFn)fn; entry->ret_slot = ret;
            entry->var_slot = var; entry->end_slot = end; entry->slot_count = slots;
            if (xe_native_callee_thunk(entry)) return entry;
            toi_jit_x64_free_code(entry->memory, entry->size); entry->memory = NULL; entry->fn = NULL;
        }
    }
    if (xe_callee_thunk(entry)) return entry;
    free(entry); return NULL;
}
static void xe_trace_link_callees(XeTraceContext* ctx) {
    for (int i = 0; i < ctx->count; i++) {
        ToiJitNativeTraceInstr* in = &ctx->ops[i]; uint64_t hint = in->aux_data; in->aux_data = 0;
        if (in->op != TOI_JIT_NATIVE_OP_CALL && in->op != TOI_JIT_NATIVE_OP_INVOKE &&
            in->op != TOI_JIT_NATIVE_OP_GET_TABLE && in->op != TOI_JIT_NATIVE_OP_GET_TABLE_NUM &&
            in->op != TOI_JIT_NATIVE_OP_SET_TABLE && in->op != TOI_JIT_NATIVE_OP_SET_TABLE_NUM &&
            (in->op < TOI_JIT_NATIVE_OP_BUILD_STRING2 || in->op > TOI_JIT_NATIVE_OP_BUILD_STRING5)) continue;
        XeSite* site = calloc(1, sizeof(*site)); if (!site) continue;
        site->owner = ctx; site->fused_slot = -1;
        ctx->sites[i] = site; in->aux_data = (uint64_t)(uintptr_t)site;
        if (in->op >= TOI_JIT_NATIVE_OP_BUILD_STRING2 && in->op <= TOI_JIT_NATIVE_OP_BUILD_STRING5 &&
            i + 2 < ctx->count && ctx->ops[i + 1].op == TOI_JIT_NATIVE_OP_STORE_LOCAL &&
            ctx->ops[i + 2].op == TOI_JIT_NATIVE_OP_POP) {
            int incoming = 0;
            for (int j = 0; j < ctx->count; j++) {
                const ToiJitNativeTraceInstr* branch = &ctx->ops[j];
                if ((branch->op == TOI_JIT_NATIVE_OP_JUMP || branch->op == TOI_JIT_NATIVE_OP_JUMP_IF_FALSE ||
                     branch->op == TOI_JIT_NATIVE_OP_JUMP_IF_TRUE) &&
                    (branch->target == i + 1 || branch->target == i + 2)) incoming = 1;
            }
            if (!incoming) {
                site->fused_slot = ctx->ops[i + 1].a; ctx->skip_store[i + 1] = 1;
                ctx->stats.fused_stores++;
            }
        }
        if (in->op == TOI_JIT_NATIVE_OP_CALL && hint && IS_CLOSURE(in->constant)) {
            XeCalleeEntry* e = xe_cached_callee(ctx, NULL, AS_CLOSURE(in->constant), (JitProgram*)(uintptr_t)hint, in->a);
            if (e) site->calls[0] = e;
        }
    }
}
