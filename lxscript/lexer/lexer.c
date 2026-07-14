/* lxscript/lexer/lexer.c — LXScript lexer */
#include "../string.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../stdlib.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../ctype.h"   /* shim: uses kernel klibc when -nostdinc */
#include "lexer.h"

void lexer_init(lexer_t *l, const char *src) {
    l->src = src; l->pos = 0; l->line = 1; l->col = 1;
}

static char peek_c(lexer_t *l) { return l->src[l->pos]; }
static char adv(lexer_t *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else l->col++;
    return c;
}

static token_t make_tok(token_type_t t, const char *s, int len, int line, int col) {
    token_t tok; memset(&tok, 0, sizeof(tok));
    tok.type = t; tok.start = s; tok.len = len; tok.line = line; tok.col = col;
    return tok;
}

static void skip_ws(lexer_t *l) {
    for (;;) {
        while (isspace((unsigned char)peek_c(l))) adv(l);
        if (peek_c(l) == '/' && l->src[l->pos+1] == '/') {
            while (peek_c(l) && peek_c(l) != '\n') adv(l);
        } else break;
    }
}

static const struct { const char *kw; token_type_t t; } keywords[] = {
    {"if",TK_IF},{"else",TK_ELSE},{"while",TK_WHILE},{"for",TK_FOR},
    {"return",TK_RETURN},{"let",TK_LET},{"fn",TK_FN},
    {"true",TK_TRUE},{"false",TK_FALSE},{"nil",TK_NIL},
    {"import",TK_IMPORT},{"extern",TK_EXTERN},
    {"int",TK_INT_TYPE},{"float",TK_FLOAT_TYPE},
    {"str",TK_STR_TYPE},{"bool",TK_BOOL_TYPE},
    {NULL,TK_EOF}
};

token_t lexer_next(lexer_t *l) {
    skip_ws(l);
    int sl = l->line, sc = l->col;
    const char *s = l->src + l->pos;
    char c = peek_c(l);
    if (!c) return make_tok(TK_EOF, s, 0, sl, sc);

    /* Identifiers and keywords */
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)peek_c(l)) || peek_c(l) == '_') adv(l);
        int len = (int)(l->src + l->pos - s);
        for (int i = 0; keywords[i].kw; i++)
            if ((int)strlen(keywords[i].kw) == len && memcmp(keywords[i].kw, s, (size_t)len) == 0)
                return make_tok(keywords[i].t, s, len, sl, sc);
        return make_tok(TK_IDENT, s, len, sl, sc);
    }

    /* Numbers */
    if (isdigit((unsigned char)c)) {
        int is_f = 0;
        while (isdigit((unsigned char)peek_c(l))) adv(l);
        if (peek_c(l) == '.') { is_f = 1; adv(l); while(isdigit((unsigned char)peek_c(l))) adv(l); }
        int len = (int)(l->src + l->pos - s);
        token_t tok = make_tok(is_f ? TK_FLOAT : TK_INT, s, len, sl, sc);
        /* Float literals stored as truncated integer — no SSE/double in kernel */
        tok.lit.ival = strtoll(s, NULL, 10);
        (void)is_f;  /* float flag recorded in TK_FLOAT token type only */
        return tok;
    }

    /* Strings */
    if (c == '"') {
        adv(l);
        while (peek_c(l) && peek_c(l) != '"') { if (peek_c(l) == '\\') adv(l); adv(l); }
        if (peek_c(l) == '"') adv(l);
        return make_tok(TK_STRING, s, (int)(l->src + l->pos - s), sl, sc);
    }

    adv(l);
    switch (c) {
        case '+': return make_tok(TK_PLUS,     s,1,sl,sc);
        case '-': if(peek_c(l)=='>'){adv(l);return make_tok(TK_ARROW,s,2,sl,sc);}
                  return make_tok(TK_MINUS,    s,1,sl,sc);
        case '*': return make_tok(TK_STAR,     s,1,sl,sc);
        case '/': return make_tok(TK_SLASH,    s,1,sl,sc);
        case '%': return make_tok(TK_PERCENT,  s,1,sl,sc);
        case '=': if(peek_c(l)=='='){adv(l);return make_tok(TK_EQEQ,s,2,sl,sc);}
                  return make_tok(TK_EQ,       s,1,sl,sc);
        case '!': if(peek_c(l)=='='){adv(l);return make_tok(TK_NEQ, s,2,sl,sc);}
                  return make_tok(TK_NOT,      s,1,sl,sc);
        case '<': if(peek_c(l)=='='){adv(l);return make_tok(TK_LEQ, s,2,sl,sc);}
                  return make_tok(TK_LT,       s,1,sl,sc);
        case '>': if(peek_c(l)=='='){adv(l);return make_tok(TK_GEQ, s,2,sl,sc);}
                  return make_tok(TK_GT,       s,1,sl,sc);
        case '&': if(peek_c(l)=='&'){adv(l);return make_tok(TK_AND, s,2,sl,sc);}break;
        case '|': if(peek_c(l)=='|'){adv(l);return make_tok(TK_OR,  s,2,sl,sc);}break;
        case '(': return make_tok(TK_LPAREN,   s,1,sl,sc);
        case ')': return make_tok(TK_RPAREN,   s,1,sl,sc);
        case '{': return make_tok(TK_LBRACE,   s,1,sl,sc);
        case '}': return make_tok(TK_RBRACE,   s,1,sl,sc);
        case '[': return make_tok(TK_LBRACKET, s,1,sl,sc);
        case ']': return make_tok(TK_RBRACKET, s,1,sl,sc);
        case ',': return make_tok(TK_COMMA,    s,1,sl,sc);
        case ';': return make_tok(TK_SEMICOLON,s,1,sl,sc);
        case ':': return make_tok(TK_COLON,    s,1,sl,sc);
        case '.': return make_tok(TK_DOT,      s,1,sl,sc);
    }
    return make_tok(TK_ERROR, s, 1, sl, sc);
}

token_t lexer_peek(lexer_t *l) {
    lexer_t saved = *l;
    token_t t = lexer_next(l);
    *l = saved;
    return t;
}
