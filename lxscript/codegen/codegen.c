/* lxscript/codegen/codegen.c — AST → bytecode code generator
 *
 * Walks the AST produced by parser/parser.c and emits bytecode into
 * vm->prog[].  After cg_emit() + cg_finalise() succeed, call
 * vm_run(vm, 0) to execute.
 *
 * Variable model:
 *   Each named variable gets a unique slot index (0 .. CG_MAX_VARS-1).
 *   vm->vars[slot] holds the runtime value.
 *   Native functions are looked up by name in vm->natives[] at codegen time.
 *
 * Jump backpatching:
 *   Forward jumps emit a placeholder 0x00000000 address.  After the target
 *   is known, cg_patch() overwrites it with the real address.
 */

#include "../string.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../stdio.h"   /* shim: uses kernel klibc when -nostdinc */
#include "codegen.h"

/* =========================================================================
 * Emit helpers
 * ========================================================================= */

static int cg_ok(cg_t *cg) { return cg->error[0] == '\0'; }

static void cg_error(cg_t *cg, const char *msg) {
    if (!cg->error[0])
        snprintf(cg->error, sizeof(cg->error), "%s", msg);
}

/* Emit a single byte */
static void emit8(cg_t *cg, uint8_t b) {
    if (!cg_ok(cg)) return;
    if (cg->emit_pos >= VM_PROG_SIZE - 1) {
        cg_error(cg, "codegen: program too large");
        return;
    }
    cg->vm->prog[cg->emit_pos++] = b;
}

/* Emit a 4-byte little-endian integer */
static void emit32(cg_t *cg, int32_t v) {
    emit8(cg, (uint8_t)(v & 0xFF));
    emit8(cg, (uint8_t)((v >> 8) & 0xFF));
    emit8(cg, (uint8_t)((v >> 16) & 0xFF));
    emit8(cg, (uint8_t)((v >> 24) & 0xFF));
}

/* Emit opcode */
static void emit_op(cg_t *cg, opcode_t op) { emit8(cg, (uint8_t)op); }

/* Emit a jump instruction; return the offset of the 4-byte address field
 * so the caller can backpatch it once the target is known. */
static int emit_jump(cg_t *cg, opcode_t jump_op) {
    emit_op(cg, jump_op);
    int patch_off = cg->emit_pos;
    emit32(cg, 0);   /* placeholder */
    return patch_off;
}

/* Overwrite the 4-byte placeholder at patch_off with the current emit_pos */
static void cg_patch(cg_t *cg, int patch_off) {
    if (!cg_ok(cg)) return;
    int32_t target = cg->emit_pos;
    cg->vm->prog[patch_off]     = (uint8_t)(target & 0xFF);
    cg->vm->prog[patch_off + 1] = (uint8_t)((target >> 8)  & 0xFF);
    cg->vm->prog[patch_off + 2] = (uint8_t)((target >> 16) & 0xFF);
    cg->vm->prog[patch_off + 3] = (uint8_t)((target >> 24) & 0xFF);
}

/* =========================================================================
 * Symbol table
 * ========================================================================= */

/* Find or allocate a variable slot for name.
 * Returns slot index, or -1 on overflow. */
static int cg_var_slot(cg_t *cg, const char *name, int namelen) {
    /* Search existing */
    for (int i = 0; i < cg->var_count; i++) {
        if (strncmp(cg->vars[i].name, name, (size_t)namelen) == 0 &&
            cg->vars[i].name[namelen] == '\0')
            return cg->vars[i].slot;
    }
    /* Allocate new */
    if (cg->var_count >= CG_MAX_VARS) {
        cg_error(cg, "codegen: too many variables");
        return -1;
    }
    int idx = cg->var_count++;
    snprintf(cg->vars[idx].name, sizeof(cg->vars[idx].name),
             "%.*s", namelen, name);
    cg->vars[idx].slot = idx;
    return idx;
}

/* Look up a native function by name; return index or -1 */
static int cg_native_idx(cg_t *cg, const char *name, int namelen) {
    for (int i = 0; i < cg->vm->native_count; i++) {
        if (strncmp(cg->vm->natives[i].name, name, (size_t)namelen) == 0 &&
            cg->vm->natives[i].name[namelen] == '\0')
            return i;
    }
    return -1;
}

/* FIX 2.9: Intern string into vm->str_table; return index (not pointer).
 * Old code cast 64-bit char* to int32_t, truncating above 4 GB. */
static int cg_intern_str(cg_t *cg, const char *start, int len) {
    if (len >= 2 && (start[0] == '"' || start[0] == '\'')) {
        start++;
        len -= 2;
    }
    if (len < 0) len = 0;
    if (cg->vm->str_count >= VM_STR_MAX) {
        cg_error(cg, "codegen: too many string literals");
        return 0;
    }
    int idx = cg->vm->str_count++;
    int copy = len < VM_STR_LEN - 1 ? len : VM_STR_LEN - 1;
    memcpy(cg->vm->str_table[idx], start, (size_t)copy);
    cg->vm->str_table[idx][copy] = '\0';
    return idx;
}

/* =========================================================================
 * Forward declaration
 * ========================================================================= */
static void emit_node(cg_t *cg, ast_node_t *node);

/* =========================================================================
 * Expression emitters
 * ========================================================================= */

static void emit_binop(cg_t *cg, ast_node_t *node) {
    emit_node(cg, node->binop.left);
    emit_node(cg, node->binop.right);
    switch (node->binop.op) {
    case TK_PLUS:    emit_op(cg, OP_ADD); break;
    case TK_MINUS:   emit_op(cg, OP_SUB); break;
    case TK_STAR:    emit_op(cg, OP_MUL); break;
    case TK_SLASH:   emit_op(cg, OP_DIV); break;
    case TK_PERCENT: emit_op(cg, OP_MOD); break;
    case TK_EQEQ:   emit_op(cg, OP_EQ);  break;
    case TK_NEQ:     emit_op(cg, OP_NEQ); break;
    case TK_LT:      emit_op(cg, OP_LT);  break;
    case TK_GT:      emit_op(cg, OP_GT);  break;
    case TK_LEQ:     emit_op(cg, OP_LEQ); break;
    case TK_GEQ:     emit_op(cg, OP_GEQ); break;
    case TK_AND:     emit_op(cg, OP_AND); break;
    case TK_OR:      emit_op(cg, OP_OR);  break;
    default:
        cg_error(cg, "codegen: unknown binary operator");
    }
}

static void emit_unop(cg_t *cg, ast_node_t *node) {
    emit_node(cg, node->unop.operand);
    switch (node->unop.op) {
    case TK_NOT:   emit_op(cg, OP_NOT); break;
    case TK_MINUS: emit_op(cg, OP_PUSH_INT); emit32(cg, -1);
                   emit_op(cg, OP_MUL); break;
    default:
        cg_error(cg, "codegen: unknown unary operator");
    }
}

static void emit_call(cg_t *cg, ast_node_t *node) {
    /* Count and emit arguments */
    int argc = 0;
    for (ast_list_t *a = node->call.args; a; a = a->next) {
        emit_node(cg, a->node);
        argc++;
    }

    /* Check if it's a native function */
    const char *name = node->call.name;
    int namelen = (int)strlen(name);
    int ni = cg_native_idx(cg, name, namelen);
    if (ni >= 0) {
        emit_op(cg, OP_CALL_NATIVE);
        emit32(cg, (int32_t)ni);
        emit32(cg, (int32_t)argc);
        return;
    }

    /* User-defined function: look up entry point stored in vars[] as an int */
    int slot = cg_var_slot(cg, name, namelen);
    if (slot < 0) return;   /* error already set */
    emit_op(cg, OP_LOAD);
    emit32(cg, (int32_t)slot);
    emit_op(cg, OP_CALL);
    emit32(cg, (int32_t)argc);
}

/* =========================================================================
 * Statement / node emitter — the main recursive walk
 * ========================================================================= */

static void emit_node(cg_t *cg, ast_node_t *node) {
    if (!node || !cg_ok(cg)) return;

    switch (node->kind) {

    /* ---- Literals ---- */
    case AST_INT:
        emit_op(cg, OP_PUSH_INT);
        emit32(cg, (int32_t)node->ival);
        break;

    case AST_FLOAT:
        /* Store as integer truncation for now (float ops are V2) */
        emit_op(cg, OP_PUSH_INT);
        emit32(cg, (int32_t)(long long)node->fval);
        break;

    case AST_BOOL:
        emit_op(cg, OP_PUSH_BOOL);
        emit32(cg, node->bval ? 1 : 0);
        break;

    case AST_NIL:
        emit_op(cg, OP_PUSH_NIL);
        break;

    case AST_STRING: {
        /* String pointer: we encode the pointer address as an int32 —
         * only works in the 32-bit address space of the kernel heap.
         * For userland (lxs_cli) where full 64-bit pointers are needed,
         * a dedicated OP_PUSH_STR with a string-table index would be used.
         * For V1 purposes this is sufficient. */
        int str_idx = cg_intern_str(cg, node->tok.start, node->tok.len);
        emit_op(cg, OP_PUSH_STR);
        emit32(cg, (int32_t)str_idx);
        break;
    }

    /* ---- Variable load ---- */
    case AST_IDENT: {
        int slot = cg_var_slot(cg, node->tok.start, node->tok.len);
        if (slot < 0) return;
        emit_op(cg, OP_LOAD);
        emit32(cg, (int32_t)slot);
        break;
    }

    /* ---- Assignment ---- */
    case AST_ASSIGN: {
        emit_node(cg, node->assign.value);
        int slot = cg_var_slot(cg, node->assign.name,
                                (int)strlen(node->assign.name));
        if (slot < 0) return;
        emit_op(cg, OP_STORE);
        emit32(cg, (int32_t)slot);
        break;
    }

    /* ---- Let declaration ---- */
    case AST_LET: {
        if (node->let.init)
            emit_node(cg, node->let.init);
        else {
            emit_op(cg, OP_PUSH_NIL);   /* default-initialise to nil */
        }
        int slot = cg_var_slot(cg, node->let.name,
                                (int)strlen(node->let.name));
        if (slot < 0) return;
        emit_op(cg, OP_STORE);
        emit32(cg, (int32_t)slot);
        break;
    }

    /* ---- Binary / unary ops ---- */
    case AST_BINOP: emit_binop(cg, node); break;
    case AST_UNOP:  emit_unop(cg, node);  break;

    /* ---- Function call ---- */
    case AST_CALL:  emit_call(cg, node);  break;

    /* ---- Block: emit each statement in order ---- */
    case AST_BLOCK:
        for (ast_list_t *s = node->block.stmts; s && cg_ok(cg); s = s->next)
            emit_node(cg, s->node);
        break;

    /* ---- if (<cond>) <then> [else <else>] ----
     *
     *   emit(cond)
     *   JUMP_IF_FALSE → else_target
     *   emit(then_block)
     *   JUMP → end_target
     * else_target:
     *   emit(else_block)   [if present]
     * end_target:
     */
    case AST_IF: {
        emit_node(cg, node->if_s.cond);
        int else_patch = emit_jump(cg, OP_JUMP_IF_FALSE);
        emit_node(cg, node->if_s.then_b);
        int end_patch = emit_jump(cg, OP_JUMP);
        cg_patch(cg, else_patch);
        if (node->if_s.else_b)
            emit_node(cg, node->if_s.else_b);
        cg_patch(cg, end_patch);
        break;
    }

    /* ---- while (<cond>) <body> ----
     *
     * loop_top:
     *   emit(cond)
     *   JUMP_IF_FALSE → loop_end
     *   emit(body)
     *   JUMP → loop_top
     * loop_end:
     */
    case AST_WHILE: {
        int loop_top = cg->emit_pos;
        emit_node(cg, node->while_s.cond);
        int end_patch = emit_jump(cg, OP_JUMP_IF_FALSE);
        emit_node(cg, node->while_s.body);
        emit_op(cg, OP_JUMP);
        emit32(cg, (int32_t)loop_top);
        cg_patch(cg, end_patch);
        break;
    }

    /* ---- fn <name>(<params>) <body> ----
     *
     * Jump over the function body at declaration time so it doesn't execute
     * inline.  Store the entry PC as an integer in the function's var slot.
     *
     *   JUMP → after_body
     * fn_entry:
     *   [store each param from stack into its var slot, rightmost first]
     *   emit(body)
     *   OP_PUSH_NIL / OP_RETURN
     * after_body:
     *   PUSH_INT fn_entry
     *   STORE fn_slot
     */
    case AST_FN: {
        int skip_patch = emit_jump(cg, OP_JUMP);
        int fn_entry   = cg->emit_pos;

        /* Bind parameters: they arrive on stack left-to-right, so pop
         * in reverse order */
        int param_count = 0;
        for (ast_list_t *p = node->fn.params; p; p = p->next) param_count++;

        for (int pi = param_count - 1; pi >= 0; pi--) {
            ast_list_t *p = node->fn.params;
            for (int i = 0; i < pi; i++) p = p->next;
            /* p->node is an AST_IDENT carrying the param name */
            int slot = cg_var_slot(cg, p->node->tok.start, p->node->tok.len);
            if (slot < 0) return;
            emit_op(cg, OP_STORE);
            emit32(cg, (int32_t)slot);
        }

        emit_node(cg, node->fn.body);
        emit_op(cg, OP_PUSH_NIL);
        emit_op(cg, OP_RETURN);

        cg_patch(cg, skip_patch);   /* after body — resume here */

        /* Store entry PC as an integer into the function's name slot */
        int fn_slot = cg_var_slot(cg, node->fn.name,
                                   (int)strlen(node->fn.name));
        if (fn_slot < 0) return;
        emit_op(cg, OP_PUSH_INT);
        emit32(cg, (int32_t)fn_entry);
        emit_op(cg, OP_STORE);
        emit32(cg, (int32_t)fn_slot);
        break;
    }

    /* ---- return [<expr>] ---- */
    case AST_RETURN:
        if (node->ret.value)
            emit_node(cg, node->ret.value);
        else
            emit_op(cg, OP_PUSH_NIL);
        emit_op(cg, OP_RETURN);
        break;

    default:
        cg_error(cg, "codegen: unhandled AST node kind");
        break;
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void cg_init(cg_t *cg, lxs_vm_t *vm) {
    memset(cg, 0, sizeof(*cg));
    cg->vm       = vm;
    cg->emit_pos = 0;
}

int cg_emit(cg_t *cg, ast_node_t *node) {
    emit_node(cg, node);
    return cg_ok(cg) ? 0 : -1;
}

void cg_finalise(cg_t *cg) {
    emit_op(cg, OP_HALT);
    /* Point vm->pc to start */
    cg->vm->pc = 0;
}
