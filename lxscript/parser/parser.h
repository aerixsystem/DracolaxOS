/* lxscript/parser/parser.h — Recursive descent parser producing AST */
#ifndef LXS_PARSER_H
#define LXS_PARSER_H
#include "../lexer/lexer.h"
#include "../stddef.h"   /* kernel shim */

typedef enum {
    AST_INT, AST_FLOAT, AST_STRING, AST_BOOL, AST_NIL,
    AST_IDENT, AST_BINOP, AST_UNOP, AST_ASSIGN,
    AST_CALL, AST_IF, AST_WHILE, AST_BLOCK, AST_RETURN, AST_LET, AST_FN,
} ast_kind_t;

struct ast_node;
typedef struct ast_list { struct ast_node *node; struct ast_list *next; } ast_list_t;

typedef struct ast_node {
    ast_kind_t kind;
    token_t    tok;
    union {
        long long  ival;
        long long  fval;  /* stored as int64 — no SSE/double in kernel */
        const char *sval;
        int        bval;
        struct { struct ast_node *left, *right; token_type_t op; } binop;
        struct { token_type_t op; struct ast_node *operand; } unop;
        struct { const char *name; struct ast_node *value; } assign;
        struct { const char *name; ast_list_t *args; } call;
        struct { struct ast_node *cond, *then_b, *else_b; } if_s;
        struct { struct ast_node *cond, *body; } while_s;
        struct { ast_list_t *stmts; } block;
        struct { struct ast_node *value; } ret;
        struct { const char *name; struct ast_node *init; } let;
        struct { const char *name; ast_list_t *params; struct ast_node *body; } fn;
    };
} ast_node_t;

typedef struct {
    lexer_t lex; token_t cur; char error[256];
    unsigned char *arena; size_t arena_used, arena_sz;
} parser_t;

void       parser_init(parser_t *p, const char *src,
                        unsigned char *arena, size_t arena_sz);
ast_node_t *parse_program(parser_t *p);
#endif
