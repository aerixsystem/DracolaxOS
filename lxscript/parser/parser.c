/* lxscript/parser/parser.c — Recursive-descent parser for LXScript */
#include "../stdio.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../string.h"   /* shim: uses kernel klibc when -nostdinc */
#include "../stdlib.h"   /* shim: uses kernel klibc when -nostdinc */
#include "parser.h"

static void *arena_alloc(parser_t *p, size_t sz) {
    sz = (sz + 7) & ~(size_t)7;
    if (p->arena_used + sz > p->arena_sz) return NULL;
    void *ptr = p->arena + p->arena_used;
    p->arena_used += sz;
    memset(ptr, 0, sz);
    return ptr;
}

static ast_node_t *new_node(parser_t *p, ast_kind_t k, token_t tok) {
    ast_node_t *n = (ast_node_t *)arena_alloc(p, sizeof(ast_node_t));
    if (n) { n->kind = k; n->tok = tok; }
    return n;
}

static ast_list_t *new_list(parser_t *p, ast_node_t *node) {
    ast_list_t *l = (ast_list_t *)arena_alloc(p, sizeof(ast_list_t));
    if (l) l->node = node;
    return l;
}

static void advance(parser_t *p) { p->cur = lexer_next(&p->lex); }
static int  check(parser_t *p, token_type_t t) { return p->cur.type == t; }
static int  match(parser_t *p, token_type_t t) {
    if (check(p,t)) { advance(p); return 1; } return 0;
}
static token_t expect(parser_t *p, token_type_t t, const char *msg) {
    token_t tok = p->cur;
    if (!match(p,t) && !p->error[0])
        snprintf(p->error, sizeof(p->error), "%s (line %d)", msg, p->cur.line);
    return tok;
}

static ast_node_t *parse_expr(parser_t *p);
static ast_node_t *parse_stmt(parser_t *p);
static ast_node_t *parse_block(parser_t *p);

static ast_node_t *parse_primary(parser_t *p) {
    token_t tok = p->cur;
    if (match(p,TK_INT))    { ast_node_t *n=new_node(p,AST_INT,tok);    if(n)n->ival=tok.lit.ival; return n; }
    if (match(p,TK_FLOAT))  { ast_node_t *n=new_node(p,AST_FLOAT,tok);  if(n)n->fval=tok.lit.fval; return n; }
    if (match(p,TK_STRING)) { ast_node_t *n=new_node(p,AST_STRING,tok); if(n)n->sval=tok.start;    return n; }
    if (match(p,TK_TRUE))   { ast_node_t *n=new_node(p,AST_BOOL,tok);   if(n)n->bval=1; return n; }
    if (match(p,TK_FALSE))  { ast_node_t *n=new_node(p,AST_BOOL,tok);   if(n)n->bval=0; return n; }
    if (match(p,TK_NIL))    { return new_node(p,AST_NIL,tok); }
    if (match(p,TK_IDENT)) {
        if (check(p,TK_LPAREN)) {
            advance(p);
            ast_node_t *call = new_node(p,AST_CALL,tok);
            if (!call) return NULL;
            char *name = (char *)arena_alloc(p,(size_t)(tok.len+1));
            if (name) { memcpy(name,tok.start,(size_t)tok.len); name[tok.len]='\0'; }
            call->call.name = name;
            ast_list_t *args=NULL,**tail=&args;
            while (!check(p,TK_RPAREN)&&!check(p,TK_EOF)) {
                ast_node_t *arg=parse_expr(p); if(!arg)break;
                ast_list_t *l=new_list(p,arg); *tail=l; tail=&l->next;
                if (!match(p,TK_COMMA)) break;
            }
            expect(p,TK_RPAREN,"expected ')'");
            call->call.args=args; return call;
        }
        ast_node_t *n=new_node(p,AST_IDENT,tok); if(n)n->sval=tok.start; return n;
    }
    if (match(p,TK_LPAREN)) { ast_node_t *e=parse_expr(p); expect(p,TK_RPAREN,"expected ')'"); return e; }
    if (!p->error[0]) snprintf(p->error,sizeof(p->error),"unexpected token at line %d",tok.line);
    advance(p); return NULL;
}

static ast_node_t *parse_unary(parser_t *p) {
    token_t tok=p->cur;
    if (match(p,TK_NOT)||match(p,TK_MINUS)) {
        ast_node_t *n=new_node(p,AST_UNOP,tok);
        if(n){n->unop.op=tok.type;n->unop.operand=parse_unary(p);}
        return n;
    }
    return parse_primary(p);
}

#define BINOP(name,next,...) \
static ast_node_t *name(parser_t *p){ \
    ast_node_t *l=next(p); \
    token_type_t ops[]={__VA_ARGS__,TK_EOF}; \
    for(;;){ int m=0; for(int i=0;ops[i]!=TK_EOF;i++){ if(check(p,ops[i])){ \
        token_t op=p->cur;advance(p); ast_node_t *r=next(p); \
        ast_node_t *n=new_node(p,AST_BINOP,op); \
        if(n){n->binop.left=l;n->binop.right=r;n->binop.op=op.type;} \
        l=n;m=1;break;}} if(!m)break;} return l;}

BINOP(parse_mul, parse_unary, TK_STAR,TK_SLASH,TK_PERCENT)
BINOP(parse_add, parse_mul,   TK_PLUS,TK_MINUS)
BINOP(parse_cmp, parse_add,   TK_LT,TK_GT,TK_LEQ,TK_GEQ)
BINOP(parse_eq,  parse_cmp,   TK_EQEQ,TK_NEQ)
BINOP(parse_and, parse_eq,    TK_AND)
BINOP(parse_or,  parse_and,   TK_OR)

static ast_node_t *parse_assign(parser_t *p) {
    ast_node_t *l=parse_or(p);
    if (l&&l->kind==AST_IDENT&&check(p,TK_EQ)) {
        token_t op=p->cur; advance(p);
        ast_node_t *v=parse_assign(p);
        ast_node_t *n=new_node(p,AST_ASSIGN,op);
        if(n){n->assign.name=l->tok.start;n->assign.value=v;} return n;
    }
    return l;
}

static ast_node_t *parse_expr(parser_t *p) { return parse_assign(p); }

static ast_node_t *parse_block(parser_t *p) {
    token_t tok=p->cur; expect(p,TK_LBRACE,"expected '{'");
    ast_node_t *blk=new_node(p,AST_BLOCK,tok); if(!blk)return NULL;
    ast_list_t *stmts=NULL,**tail=&stmts;
    while(!check(p,TK_RBRACE)&&!check(p,TK_EOF)) {
        ast_node_t *s=parse_stmt(p); if(!s||p->error[0])break;
        ast_list_t *l=new_list(p,s); *tail=l; tail=&l->next;
    }
    expect(p,TK_RBRACE,"expected '}'");
    blk->block.stmts=stmts; return blk;
}

static ast_node_t *parse_stmt(parser_t *p) {
    token_t tok=p->cur;
    if (match(p,TK_LET)) {
        ast_node_t *n=new_node(p,AST_LET,tok); if(!n)return NULL;
        token_t nm=expect(p,TK_IDENT,"expected identifier");
        if (match(p,TK_COLON)) advance(p); /* skip type */
        expect(p,TK_EQ,"expected '='");
        n->let.name=nm.start; n->let.init=parse_expr(p);
        expect(p,TK_SEMICOLON,"expected ';'"); return n;
    }
    if (match(p,TK_FN)) {
        ast_node_t *n=new_node(p,AST_FN,tok); if(!n)return NULL;
        token_t nm=expect(p,TK_IDENT,"expected function name");
        n->fn.name=nm.start;
        expect(p,TK_LPAREN,"expected '('");
        ast_list_t *params=NULL,**tail=&params;
        while(!check(p,TK_RPAREN)&&!check(p,TK_EOF)) {
            token_t pn=p->cur; if(!match(p,TK_IDENT))break;
            if(match(p,TK_COLON))advance(p);
            ast_node_t *pnode=new_node(p,AST_IDENT,pn); if(pnode)pnode->sval=pn.start;
            ast_list_t *l=new_list(p,pnode); *tail=l; tail=&l->next;
            if(!match(p,TK_COMMA))break;
        }
        expect(p,TK_RPAREN,"expected ')'");
        if(match(p,TK_ARROW))advance(p);
        n->fn.params=params; n->fn.body=parse_block(p); return n;
    }
    if (match(p,TK_IF)) {
        ast_node_t *n=new_node(p,AST_IF,tok); if(!n)return NULL;
        n->if_s.cond=parse_expr(p); n->if_s.then_b=parse_block(p);
        if(match(p,TK_ELSE)){n->if_s.else_b=parse_block(p);} return n;
    }
    if (match(p,TK_WHILE)) {
        ast_node_t *n=new_node(p,AST_WHILE,tok); if(!n)return NULL;
        n->while_s.cond=parse_expr(p); n->while_s.body=parse_block(p); return n;
    }
    if (match(p,TK_RETURN)) {
        ast_node_t *n=new_node(p,AST_RETURN,tok); if(!n)return NULL;
        if(!check(p,TK_SEMICOLON))n->ret.value=parse_expr(p);
        expect(p,TK_SEMICOLON,"expected ';'"); return n;
    }
    ast_node_t *e=parse_expr(p); match(p,TK_SEMICOLON); return e;
}

void parser_init(parser_t *p, const char *src,
                 unsigned char *arena, size_t arena_sz) {
    lexer_init(&p->lex, src);
    p->error[0]='\0'; p->arena=arena; p->arena_sz=arena_sz; p->arena_used=0;
    advance(p);
}

ast_node_t *parse_program(parser_t *p) {
    token_t tok=p->cur;
    ast_node_t *blk=new_node(p,AST_BLOCK,tok); if(!blk)return NULL;
    ast_list_t *stmts=NULL,**tail=&stmts;
    while(!check(p,TK_EOF)) {
        ast_node_t *s=parse_stmt(p); if(!s||p->error[0])break;
        ast_list_t *l=new_list(p,s); *tail=l; tail=&l->next;
    }
    blk->block.stmts=stmts; return blk;
}
