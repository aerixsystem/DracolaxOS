/* lxscript/vm/vm.h — LXScript bytecode VM */
#include "../stdint.h"   /* kernel shim */
#ifndef LXS_VM_H
#define LXS_VM_H
#include "../lxscript.h"

/* Bytecode opcodes */
typedef enum {
    OP_NOP=0,
    OP_PUSH_INT, OP_PUSH_FLOAT, OP_PUSH_STR, OP_PUSH_NIL, OP_PUSH_BOOL,
    OP_POP, OP_DUP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LEQ, OP_GEQ,
    OP_AND, OP_OR, OP_NOT,
    OP_LOAD, OP_STORE,
    OP_JUMP, OP_JUMP_IF_FALSE,
    OP_CALL, OP_CALL_NATIVE, OP_RETURN,
    OP_HALT,
} opcode_t;

#define VM_STACK_SIZE 1024
#define VM_VARS_SIZE  256
#define VM_PROG_SIZE  65536
#define VM_NATIVE_MAX 128
#define VM_STR_MAX    128   /* max unique string literals per program */
#define VM_STR_LEN    256   /* max bytes per interned string          */

typedef struct {
    const char    *name;
    lxs_native_fn  fn;
} native_entry_t;

struct lxs_vm {
    lxs_val_t   stack[VM_STACK_SIZE];
    int         sp;
    lxs_val_t   vars[VM_VARS_SIZE];
    uint8_t     prog[VM_PROG_SIZE];
    int         pc;
    native_entry_t natives[VM_NATIVE_MAX];
    int         native_count;

    /* String table — interned at codegen time; pointers stay valid for the
     * lifetime of the VM.  OP_PUSH_STR reads an index into this table.
     * Fixes audit bug 2.9: old code stored raw 64-bit pointers cast to
     * int32_t, which truncated and caused UB above 4 GB. */
    char        str_table[VM_STR_MAX][VM_STR_LEN];
    int         str_count;

    char        error[256];
};

int vm_run(lxs_vm_t *vm, int entry_pc);
#endif
