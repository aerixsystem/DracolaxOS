/* lxscript/tools/lxs_cli.c — draco-script CLI
 * Usage:
 *   lxs run   <file.lxs>    — run a script
 *   lxs repl               — interactive REPL
 *   lxs compile <in> <out> — compile to bytecode (stub)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../lxscript.h"
#include "../vm/vm.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../codegen/codegen.h"

/* Minimal single-expression evaluator for REPL */
static void repl(lxs_vm_t *vm) {
    char line[512];
    printf("LXScript REPL — DracolaxOS v1\nType 'exit' to quit.\n");
    for (;;) {
        printf(">> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (strncmp(line, "exit", 4) == 0) break;
        lxs_exec_string(vm, line);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: lxs <run|repl|compile> [args]\n"); return 1; }
    lxs_vm_t *vm = lxs_vm_new();
    lxs_stdlib_load(vm);
    int rc = 0;
    if (strcmp(argv[1], "repl") == 0) {
        repl(vm);
    } else if (strcmp(argv[1], "run") == 0 && argc >= 3) {
        rc = lxs_exec_file(vm, argv[2]);
        if (rc != 0) fprintf(stderr, "error: %s\n", lxs_last_error(vm));
    } else if (strcmp(argv[1], "compile") == 0 && argc >= 4) {
        /* lxs compile <input.lxs> <output.lxbc>
         * Parse the source, codegen bytecode, write binary .lxbc file.
         *
         * .lxbc format:
         *   Bytes 0-3  : magic "LXBC"
         *   Bytes 4-7  : version (uint32 LE) = 1
         *   Bytes 8-11 : bytecode length (uint32 LE)
         *   Bytes 12.. : raw bytecode (vm->prog[0..len-1])
         */
        FILE *fin = fopen(argv[2], "r");
        if (!fin) {
            fprintf(stderr, "compile: cannot open '%s'\n", argv[2]);
            rc = 1;
        } else {
            /* Read source */
            fseek(fin, 0, SEEK_END);
            long fsz = ftell(fin);
            rewind(fin);
            char *src = (char *)malloc((size_t)(fsz + 1));
            if (!src) { fclose(fin); fprintf(stderr, "compile: OOM\n"); rc = 1; goto compile_done; }
            size_t _nr = fread(src, 1, (size_t)fsz, fin); (void)_nr;
            src[fsz] = '\0';
            fclose(fin);

            /* Parse */
            static unsigned char arena[256 * 1024];
            parser_t parser;
            parser_init(&parser, src, arena, sizeof(arena));
            ast_node_t *tree = parse_program(&parser);
            if (!tree || parser.error[0]) {
                fprintf(stderr, "compile: parse error: %s\n", parser.error);
                free(src); rc = 1; goto compile_done;
            }

            /* Codegen into a fresh VM */
            lxs_vm_t *cvm = lxs_vm_new();
            lxs_stdlib_load(cvm);
            cg_t cg;
            cg_init(&cg, cvm);
            int cer = cg_emit(&cg, tree);
            free(src);
            if (cer != 0) {
                fprintf(stderr, "compile: codegen error: %s\n", cg.error);
                lxs_vm_free(cvm); rc = 1; goto compile_done;
            }
            cg_finalise(&cg);

            uint32_t code_len = (uint32_t)cg.emit_pos + 1; /* +1 for HALT */

            /* Write .lxbc */
            FILE *fout = fopen(argv[3], "wb");
            if (!fout) {
                fprintf(stderr, "compile: cannot write '%s'\n", argv[3]);
                lxs_vm_free(cvm); rc = 1; goto compile_done;
            }
            /* Magic */
            fwrite("LXBC", 1, 4, fout);
            /* Version = 1 (LE) */
            uint8_t ver[4] = {1, 0, 0, 0};
            fwrite(ver, 1, 4, fout);
            /* Length (LE) */
            uint8_t len_b[4];
            len_b[0] = (uint8_t)(code_len & 0xFF);
            len_b[1] = (uint8_t)((code_len >> 8) & 0xFF);
            len_b[2] = (uint8_t)((code_len >> 16) & 0xFF);
            len_b[3] = (uint8_t)((code_len >> 24) & 0xFF);
            fwrite(len_b, 1, 4, fout);
            /* Bytecode */
            fwrite(cvm->prog, 1, code_len, fout);
            fclose(fout);

            fprintf(stdout, "compiled '%s' → '%s' (%u bytes)\n",
                    argv[2], argv[3], (unsigned)code_len);
            lxs_vm_free(cvm);
        }
        compile_done:;
    } else if (strcmp(argv[1], "compile") == 0) {
        fprintf(stderr, "usage: lxs compile <input.lxs> <output.lxbc>\n");
        rc = 1;
    } else {
        fprintf(stderr, "unknown command '%s'\n", argv[1]);
        rc = 1;
    }
    lxs_vm_free(vm);
    return rc;
}
