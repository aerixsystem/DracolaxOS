/* lxscript/vm/vm.c — LXScript bytecode VM interpreter */
#include "../stdint.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../string.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../stdio.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../stdlib.h"   /* shim: uses kernel klibc when -nostdinc */
#include "vm.h"

#define PUSH(v)  do{ if(vm->sp>=VM_STACK_SIZE-1){snprintf(vm->error,sizeof(vm->error),"stack overflow");return -1;} vm->stack[vm->sp++]=(v); }while(0)
#define POP()    (vm->sp>0 ? vm->stack[--vm->sp] : (lxs_val_t){LXS_NIL,{0}})
#define READ8()  (vm->prog[vm->pc++])
#define READ32() (vm->pc+=4, (int32_t)( vm->prog[vm->pc-4]|(vm->prog[vm->pc-3]<<8)|(vm->prog[vm->pc-2]<<16)|(vm->prog[vm->pc-1]<<24) ))

static lxs_val_t ival(long long i){ lxs_val_t v; v.type=LXS_INT;   v.v.i=(int)i; return v; }
static lxs_val_t bval(int b)      { lxs_val_t v; v.type=LXS_BOOL;  v.v.i=b;      return v; }
static lxs_val_t nilv(void)       { lxs_val_t v; v.type=LXS_NIL;   v.v.i=0;      return v; }
static lxs_val_t sval(const char *s){ lxs_val_t v; v.type=LXS_STRING; v.v.s=s;      return v; }

int vm_run(lxs_vm_t *vm, int entry_pc) {
    vm->pc = entry_pc;
    for (;;) {
        opcode_t op = (opcode_t)READ8();
        switch (op) {
        case OP_NOP: break;
        case OP_PUSH_INT: { int32_t n=READ32(); PUSH(ival(n)); } break;
        case OP_PUSH_BOOL:{ int32_t n=READ32(); PUSH(bval(n)); } break;
        case OP_PUSH_STR: {
            /* FIX 2.9: reads a string-table index (int32), not a raw pointer.
             * The string was interned into vm->str_table at codegen time. */
            int32_t idx = READ32();
            if (idx < 0 || idx >= vm->str_count) {
                snprintf(vm->error, sizeof(vm->error),
                         "OP_PUSH_STR: bad string index %d", idx);
                return -1;
            }
            PUSH(sval(vm->str_table[idx]));
        } break;
        case OP_PUSH_NIL: PUSH(nilv()); break;
        case OP_POP:  POP(); break;
        case OP_DUP:  { lxs_val_t t=POP(); PUSH(t); PUSH(t); } break;
        case OP_ADD:  { lxs_val_t b=POP(),a=POP(); PUSH(ival(a.v.i+b.v.i)); } break;
        case OP_SUB:  { lxs_val_t b=POP(),a=POP(); PUSH(ival(a.v.i-b.v.i)); } break;
        case OP_MUL:  { lxs_val_t b=POP(),a=POP(); PUSH(ival(a.v.i*b.v.i)); } break;
        case OP_DIV:  { lxs_val_t b=POP(),a=POP();
                        if(!b.v.i){snprintf(vm->error,sizeof(vm->error),"div by zero");return -1;}
                        PUSH(ival(a.v.i/b.v.i)); } break;
        case OP_EQ:   { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i==b.v.i)); } break;
        case OP_NEQ:  { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i!=b.v.i)); } break;
        case OP_LT:   { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i< b.v.i)); } break;
        case OP_GT:   { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i> b.v.i)); } break;
        case OP_AND:  { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i&&b.v.i)); } break;
        case OP_OR:   { lxs_val_t b=POP(),a=POP(); PUSH(bval(a.v.i||b.v.i)); } break;
        case OP_NOT:  { lxs_val_t a=POP(); PUSH(bval(!a.v.i)); } break;
        case OP_LOAD: { int32_t idx=READ32();
                        PUSH(idx<VM_VARS_SIZE?vm->vars[idx]:nilv()); } break;
        case OP_STORE:{ int32_t idx=READ32(); lxs_val_t v=POP();
                        if(idx<VM_VARS_SIZE) vm->vars[idx]=v; } break;
        case OP_JUMP: { int32_t addr=READ32(); vm->pc=addr; } break;
        case OP_JUMP_IF_FALSE:{ int32_t addr=READ32(); lxs_val_t c=POP();
                        if(!c.v.i) vm->pc=addr; } break;
        case OP_CALL_NATIVE: {
            int32_t idx=READ32(), argc=READ32();
            if(idx<0||idx>=vm->native_count){snprintf(vm->error,sizeof(vm->error),"bad native %d",idx);return -1;}
            lxs_val_t args[16]; int n=argc<16?argc:16;
            for(int i=n-1;i>=0;i--) args[i]=POP();
            lxs_val_t r=vm->natives[idx].fn(vm,n,args);
            PUSH(r);
        } break;
        case OP_RETURN: return 0;
        case OP_HALT:   return 0;
        default:
            snprintf(vm->error,sizeof(vm->error),"unknown opcode %d at pc=%d",op,vm->pc-1);
            return -1;
        }
    }
}

/* ---- lxs_register_native ------------------------------------------------
 * Moved here from lxscript/lxscript.c so it is available in the kernel build
 * without pulling in malloc/free/fopen from lxscript.c.             */
void lxs_register_native(lxs_vm_t *vm, const char *name, lxs_native_fn fn) {
    if (!vm || vm->native_count >= VM_NATIVE_MAX) return;
    vm->natives[vm->native_count].name = name;
    vm->natives[vm->native_count].fn   = fn;
    vm->native_count++;
}

const char *lxs_last_error(lxs_vm_t *vm) {
    return vm ? vm->error : "";
}
