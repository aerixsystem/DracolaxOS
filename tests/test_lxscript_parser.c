/* tests/test_lxscript_parser.c — Unit tests for LXScript lexer + parser
 * Build: gcc -O2 -Wall -std=c11 -o test_lxs \
 *         tests/test_lxscript_parser.c \
 *         lxscript/lexer/lexer.c lxscript/parser/parser.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../lxscript/lexer/lexer.h"
#include "../lxscript/parser/parser.h"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) \
    do { if(cond){printf("[PASS] %s\n",msg);pass++;}else{printf("[FAIL] %s\n",msg);fail++;} }while(0)

static unsigned char arena[1024*64];

static ast_node_t *quick_parse(const char *src) {
    static parser_t p;
    parser_init(&p, src, arena, sizeof(arena));
    return parse_program(&p);
}

static parser_t last_parser;
static ast_node_t *quick_parse2(const char *src) {
    parser_init(&last_parser, src, arena, sizeof(arena));
    return parse_program(&last_parser);
}

int main(void) {
    /* ---- Lexer tests ---- */
    {
        lexer_t l; lexer_init(&l, "let x = 42;");
        token_t t = lexer_next(&l);
        CHECK(t.type == TK_LET, "lex: 'let' keyword");
        t = lexer_next(&l);
        CHECK(t.type == TK_IDENT, "lex: identifier 'x'");
        t = lexer_next(&l);
        CHECK(t.type == TK_EQ, "lex: '=' operator");
        t = lexer_next(&l);
        CHECK(t.type == TK_INT && t.lit.ival == 42, "lex: integer literal 42");
        t = lexer_next(&l);
        CHECK(t.type == TK_SEMICOLON, "lex: semicolon");
    }

    /* ---- String literal ---- */
    {
        lexer_t l; lexer_init(&l, "\"hello world\"");
        token_t t = lexer_next(&l);
        CHECK(t.type == TK_STRING, "lex: string literal");
        CHECK(t.len == 13, "lex: string literal length (with quotes)");
    }

    /* ---- Float literal ---- */
    {
        lexer_t l; lexer_init(&l, "3.14");
        token_t t = lexer_next(&l);
        CHECK(t.type == TK_FLOAT, "lex: float literal");
    }

    /* ---- Keywords ---- */
    {
        lexer_t l; lexer_init(&l, "fn if else while return true false nil");
        token_type_t expected[] = {
            TK_FN, TK_IF, TK_ELSE, TK_WHILE, TK_RETURN,
            TK_TRUE, TK_FALSE, TK_NIL, TK_EOF
        };
        int ok = 1;
        for (int i = 0; expected[i] != TK_EOF; i++) {
            token_t t = lexer_next(&l);
            if (t.type != expected[i]) { ok = 0; break; }
        }
        CHECK(ok, "lex: all keyword types recognised");
    }

    /* ---- Parser: let statement ---- */
    {
        ast_node_t *prog = quick_parse("let x = 10;");
        CHECK(prog != NULL, "parse: non-null result");
        CHECK(prog->kind == AST_BLOCK, "parse: program is block");
        ast_list_t *s = prog->block.stmts;
        CHECK(s != NULL, "parse: block has statements");
        CHECK(s->node->kind == AST_LET, "parse: let statement");
    }

    /* ---- Parser: function declaration ---- */
    {
        ast_node_t *prog = quick_parse("fn add(a, b) { return a; }");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_FN, "parse: fn declaration");
    }

    /* ---- Parser: if statement ---- */
    {
        ast_node_t *prog = quick_parse("if x > 0 { let y = 1; }");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_IF, "parse: if statement");
    }

    /* ---- Parser: while loop ---- */
    {
        ast_node_t *prog = quick_parse("while x > 0 { x = x - 1; }");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_WHILE, "parse: while loop");
    }

    /* ---- Parser: binary expression ---- */
    {
        ast_node_t *prog = quick_parse("1 + 2 * 3;");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_BINOP, "parse: binary expression");
        /* Precedence: 1 + (2*3) → top node is '+' */
        CHECK(s->node->binop.op == TK_PLUS, "parse: '+' is root (precedence correct)");
    }

    /* ---- Parser: function call ---- */
    {
        ast_node_t *prog = quick_parse2("print(42);");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_CALL, "parse: function call");
        CHECK(s->node->call.name &&
              strncmp(s->node->call.name, "print", 5) == 0,
              "parse: call name is 'print'");
        CHECK(last_parser.error[0] == '\0', "parse: no parse errors");
    }

    /* ---- Parser: nested blocks ---- */
    {
        ast_node_t *prog = quick_parse(
            "fn outer(x) { if x > 0 { let y = x - 1; } }");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        CHECK(s && s->node->kind == AST_FN, "parse: nested fn+if+let");
    }

    /* ---- Parser: boolean literals ---- */
    {
        ast_node_t *prog = quick_parse("let a = true; let b = false;");
        ast_list_t *s = prog ? prog->block.stmts : NULL;
        int ok = (s && s->node->kind == AST_LET &&
                  s->next && s->next->node->kind == AST_LET);
        CHECK(ok, "parse: two let statements");
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
