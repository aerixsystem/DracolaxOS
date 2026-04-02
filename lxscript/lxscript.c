/* lxscript/lxscript.c — LXScript VM public API implementation */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lxscript.h"
#include "vm/vm.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "codegen/codegen.h"

/* Arena for AST nodes — 256 KB, static so it survives until exec is done */
#define PARSE_ARENA_SIZE (256 * 1024)
static unsigned char s_parse_arena[PARSE_ARENA_SIZE];

lxs_vm_t *lxs_vm_new(void) {
    lxs_vm_t *vm = (lxs_vm_t *)calloc(1, sizeof(lxs_vm_t));
    return vm;
}

void lxs_vm_free(lxs_vm_t *vm) { free(vm); }

void lxs_register_native(lxs_vm_t *vm, const char *name, lxs_native_fn fn) {
    if (vm->native_count >= VM_NATIVE_MAX) return;
    vm->natives[vm->native_count].name = name;
    vm->natives[vm->native_count].fn   = fn;
    vm->native_count++;
}

const char *lxs_last_error(lxs_vm_t *vm) { return vm->error; }

/* Full pipeline: source → lex → parse → codegen → vm_run.
 * Replaced stub (which only handled bare print() calls) with the complete
 * parser+codegen that handles all LXScript constructs. */
int lxs_exec_string(lxs_vm_t *vm, const char *source) {
    /* 1. Parse source into AST */
    parser_t parser;
    parser_init(&parser, source, s_parse_arena, PARSE_ARENA_SIZE);
    ast_node_t *tree = parse_program(&parser);
    if (!tree || parser.error[0]) {
        snprintf(vm->error, sizeof(vm->error),
                 "parse error: %s", parser.error[0] ? parser.error : "null tree");
        return -1;
    }

    /* 2. Compile AST to bytecode */
    cg_t cg;
    cg_init(&cg, vm);
    /* Reset program buffer for this compilation */
    memset(vm->prog, 0, sizeof(vm->prog));
    vm->sp = 0;

    int r = cg_emit(&cg, tree);
    if (r != 0) {
        snprintf(vm->error, sizeof(vm->error),
                 "codegen error: %s", cg.error);
        return -1;
    }
    cg_finalise(&cg);

    /* 3. Execute */
    return vm_run(vm, 0);
}

int lxs_exec_file(lxs_vm_t *vm, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(vm->error, sizeof(vm->error), "cannot open '%s'", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char *)malloc((size_t)(sz + 1));
    if (!src) { fclose(f); return -1; }
    size_t _nr = fread(src, 1, (size_t)sz, f); (void)_nr;
    src[sz] = '\0';
    fclose(f);
    int r = lxs_exec_string(vm, src);
    free(src);
    return r;
}
