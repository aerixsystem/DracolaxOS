/* kernel/lxs_kernel.c — Kernel-side LXScript execution engine
 *
 * Replaces the userland lxscript/lxscript.c for in-kernel use.
 * Uses kmalloc/kfree instead of malloc/free, and VFS reads instead of fopen.
 * Exposes lxs_kernel_exec(source) — the entry point for the shell `lxs` command.
 *
 * The lxscript lexer/parser/codegen/vm source files are compiled freestanding
 * using shim headers in lxscript/ that redirect <string.h> etc. to klibc.
 */
#include "types.h"
#include "klibc.h"
#include "log.h"
#include "mm/vmm.h"
#include "fs/vfs.h"
#include "drivers/vga/fb.h"
#include "lxs_kernel.h"

/* Include lxscript internals directly */
#include "../lxscript/lxscript.h"
#include "../lxscript/vm/vm.h"
#include "../lxscript/lexer/lexer.h"
#include "../lxscript/parser/parser.h"
#include "../lxscript/codegen/codegen.h"

/* Arena for the AST — allocated once per exec, freed after run */
#define LXS_ARENA_SIZE (128 * 1024)

/* ---- Native print() ---------------------------------------------------- */

static lxs_val_t native_print(lxs_vm_t *vm, int argc, lxs_val_t *argv) {
    (void)vm;
    for (int i = 0; i < argc; i++) {
        char buf[256];
        lxs_val_t *v = &argv[i];
        if (v->type == LXS_STRING && v->v.s) {
            fb_print(0, 0, v->v.s, 0xFFFFFFFF, 0);   /* to fb console */
            kinfo("[lxs] %s\n", v->v.s);
        } else if (v->type == LXS_INT) {
            snprintf(buf, sizeof(buf), "%d", v->v.i);
            kinfo("[lxs] %s\n", buf);
        } else if (v->type == LXS_BOOL) {
            kinfo("[lxs] %s\n", v->v.i ? "true" : "false");
        } else if (v->type == LXS_NIL) {
            kinfo("[lxs] nil\n");
        }
    }
    lxs_val_t r; r.type = LXS_NIL; r.v.i = 0;
    return r;
}

static lxs_val_t native_println(lxs_vm_t *vm, int argc, lxs_val_t *argv) {
    lxs_val_t r = native_print(vm, argc, argv);
    kinfo("[lxs] \n");
    return r;
}

static lxs_val_t native_tostring(lxs_vm_t *vm, int argc, lxs_val_t *argv) {
    (void)vm;
    if (argc < 1) { lxs_val_t r; r.type=LXS_STRING; r.v.s=""; return r; }
    /* For now return type name — full stringify needs a static buffer in VM */
    lxs_val_t r; r.type = LXS_STRING;
    r.v.s = (argv[0].type == LXS_INT)    ? "(int)"    :
            (argv[0].type == LXS_BOOL)   ? "(bool)"   :
            (argv[0].type == LXS_STRING) ? argv[0].v.s :
                                            "nil";
    return r;
}

/* ---- Public API --------------------------------------------------------- */

int lxs_kernel_exec(const char *source) {
    if (!source || !*source) return 0;

    /* Allocate arena for AST nodes */
    uint8_t *arena = (uint8_t *)kmalloc(LXS_ARENA_SIZE);
    if (!arena) { kerror("lxs: OOM for parse arena\n"); return -1; }

    /* Allocate VM */
    lxs_vm_t *vm = (lxs_vm_t *)kmalloc(sizeof(lxs_vm_t));
    if (!vm) { kfree(arena); kerror("lxs: OOM for VM\n"); return -1; }
    memset(vm, 0, sizeof(lxs_vm_t));

    /* Register standard natives */
    lxs_register_native(vm, "print",    native_print);
    lxs_register_native(vm, "println",  native_println);
    lxs_register_native(vm, "tostring", native_tostring);

    /* Parse */
    parser_t parser;
    parser_init(&parser, source, arena, LXS_ARENA_SIZE);
    ast_node_t *tree = parse_program(&parser);
    if (!tree || parser.error[0]) {
        kerror("lxs parse error: %s\n", parser.error[0] ? parser.error : "null");
        kfree(vm); kfree(arena);
        return -1;
    }

    /* Codegen */
    cg_t cg;
    cg_init(&cg, vm);
    memset(vm->prog, 0, sizeof(vm->prog));
    vm->sp = 0;

    int r = cg_emit(&cg, tree);
    if (r != 0) {
        kerror("lxs codegen error: %s\n", cg.error);
        kfree(vm); kfree(arena);
        return -1;
    }
    cg_finalise(&cg);

    /* Execute */
    r = vm_run(vm, 0);
    if (r != 0)
        kerror("lxs runtime error: %s\n", vm->error);

    kfree(vm);
    kfree(arena);
    return r;
}

int lxs_kernel_exec_file(vfs_node_t *node) {
    if (!node) return -1;
    /* Read file content (up to 64 KB) */
    enum { SRC_MAX = 65536 };
    char *src = (char *)kmalloc(SRC_MAX);
    if (!src) { kerror("lxs: OOM reading file\n"); return -1; }
    int len = vfs_read(node, 0, SRC_MAX - 1, (uint8_t *)src);
    if (len <= 0) { kfree(src); kerror("lxs: empty file\n"); return -1; }
    src[len] = '\0';
    int r = lxs_kernel_exec(src);
    kfree(src);
    return r;
}
