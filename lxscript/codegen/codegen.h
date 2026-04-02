/* lxscript/codegen/codegen.h — AST → bytecode code generator
 *
 * Walks the AST produced by parser.c and emits bytecode into vm->prog[].
 * After a successful codegen_emit() call, vm_run(vm, 0) executes the code.
 *
 * Supports:
 *   - Integer and boolean literals
 *   - String literals (stored in a per-codegen string pool, pointers valid
 *     for the lifetime of the cg_t context)
 *   - let <name> = <expr>;          (variable declaration + assignment)
 *   - <name> = <expr>;              (variable assignment)
 *   - <name>                        (variable load)
 *   - Binary ops: + - * / % == != < > <= >=  and  or
 *   - Unary op:   not / -
 *   - if (<cond>) <block> [else <block>]
 *   - while (<cond>) <block>
 *   - fn <name>(<params>) <block>   (first-class functions, single frame)
 *   - <name>(<args>)                (native call or user-defined call)
 *   - return [<expr>];
 */
#ifndef LXS_CODEGEN_H
#define LXS_CODEGEN_H

#include "../parser/parser.h"
#include "../vm/vm.h"

/* Maximum number of local variables / string literals per compilation unit */
#define CG_MAX_VARS    64
#define CG_MAX_STRS    128
#define CG_STR_MAX     256
#define CG_MAX_PATCHES 64    /* backpatch slots for forward jumps */

typedef struct {
    char name[64];
    int  slot;          /* index into vm->vars[] */
} cg_var_t;

typedef struct {
    int  instr_off;     /* offset of the 4-byte address field to patch */
} cg_patch_t;

typedef struct {
    lxs_vm_t   *vm;
    int         emit_pos;           /* next byte to write in vm->prog[]  */

    cg_var_t    vars[CG_MAX_VARS];
    int         var_count;

    /* Strings are interned directly into vm->str_table at codegen time.
     * (str_pool removed in fix 2.9 — no longer needed here.) */

    char        error[256];
} cg_t;

/* Initialise codegen context targeting vm */
void cg_init(cg_t *cg, lxs_vm_t *vm);

/* Emit bytecode for an AST tree rooted at node.
 * Returns 0 on success, -1 on error (cg->error set). */
int  cg_emit(cg_t *cg, ast_node_t *node);

/* Emit OP_HALT at end; call after cg_emit() succeeds */
void cg_finalise(cg_t *cg);

#endif /* LXS_CODEGEN_H */
