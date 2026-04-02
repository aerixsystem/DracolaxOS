/* lxscript/lexer/lexer.h */
#ifndef LXS_LEXER_H
#define LXS_LEXER_H
#include "../stddef.h"   /* kernel shim */
typedef enum {
    TK_EOF=0, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LEQ, TK_GEQ,
    TK_AND, TK_OR, TK_NOT,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_SEMICOLON, TK_COLON, TK_DOT, TK_ARROW,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_RETURN, TK_LET, TK_FN,
    TK_TRUE, TK_FALSE, TK_NIL, TK_IMPORT, TK_EXTERN,
    TK_INT_TYPE, TK_FLOAT_TYPE, TK_STR_TYPE, TK_BOOL_TYPE,
    TK_ERROR,
} token_type_t;

typedef struct {
    token_type_t type;
    const char  *start;
    int          len;
    int          line, col;
    union { long long ival; long long fval; } lit;  /* fval stored as int64 — no SSE */
} token_t;

typedef struct {
    const char *src;
    int         pos, line, col;
} lexer_t;

void    lexer_init(lexer_t *l, const char *src);
token_t lexer_next(lexer_t *l);
token_t lexer_peek(lexer_t *l);
#endif
