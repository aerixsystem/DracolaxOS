/* lxscript/lxscript.h — LXScript (.lxs) interpreter/compiler API
 *
 * LXScript is a minimal scripting language for DracolaxOS.
 * File extension: .lxs   Compiler binary: draco-script
 *
 * Language traits:
 *   - C-like syntax; optional static types
 *   - GC for heap objects; explicit alloc via alloc()/free()
 *   - Fibers via fiber_spawn() / fiber_yield()
 *   - FFI to C via extern declarations
 *
 * This header exposes the embedding API used by the kernel/userland.
 */
#ifndef LXSCRIPT_H
#define LXSCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque VM handle */
typedef struct lxs_vm lxs_vm_t;

/* Value type tag */
typedef enum {
    LXS_NIL = 0, LXS_BOOL, LXS_INT, LXS_FLOAT, LXS_STRING, LXS_OBJECT
} lxs_type_t;

typedef struct {
    lxs_type_t type;
    union { int i; long long f; const char *s; void *obj; } v;  /* no SSE/double */
} lxs_val_t;

/* ---- VM lifecycle ------------------------------------------------------- */
lxs_vm_t *lxs_vm_new(void);
void      lxs_vm_free(lxs_vm_t *vm);

/* ---- Execution ---------------------------------------------------------- */
/* Run source code string; returns 0 on success, -1 on error */
int  lxs_exec_string(lxs_vm_t *vm, const char *source);
/* Run compiled bytecode file */
int  lxs_exec_file(lxs_vm_t *vm, const char *path);

/* ---- Native bindings ---------------------------------------------------- */
typedef lxs_val_t (*lxs_native_fn)(lxs_vm_t *vm, int argc, lxs_val_t *argv);
void lxs_register_native(lxs_vm_t *vm, const char *name, lxs_native_fn fn);

/* ---- Standard library -------------------------------------------------- */
void lxs_stdlib_load(lxs_vm_t *vm);  /* load all standard library modules */

/* ---- Diagnostics ------------------------------------------------------- */
const char *lxs_last_error(lxs_vm_t *vm);

#ifdef __cplusplus
}
#endif
#endif /* LXSCRIPT_H */
