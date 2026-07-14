/* lxscript/stdlib/stdlib.c — LXScript standard library */
#include <stdio.h>
#include <string.h>
#include "../lxscript.h"
#include "../vm/vm.h"

static lxs_val_t lxs_print(lxs_vm_t *vm, int argc, lxs_val_t *args) {
    (void)vm;
    for (int i = 0; i < argc; i++) {
        if (args[i].type == LXS_STRING) printf("%s", args[i].v.s ? args[i].v.s : "");
        else if (args[i].type == LXS_INT)  printf("%d", args[i].v.i);
        else if (args[i].type == LXS_BOOL) printf("%s", args[i].v.i ? "true" : "false");
        else printf("nil");
    }
    printf("\n");
    lxs_val_t r; r.type = LXS_NIL; r.v.i = 0;
    return r;
}

void lxs_stdlib_load(lxs_vm_t *vm) {
    lxs_register_native(vm, "print", lxs_print);
}
