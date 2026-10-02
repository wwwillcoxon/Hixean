#ifndef HX_LEX_H
#define HX_LEX_H

#include "hx/common.h"
#include "hx/diag.h"

typedef enum {
    TK_EOF = 0,
    TK_NL,
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_DURATION,
    TK_STRING,
    TK_COMMENT,
    TK_PUNCT,

    TK_KW_FIRST = 256,
#define HX_KEYWORD(id, text) TK_KW_##id,
#include "hx/keywords.def"
#undef HX_KEYWORD
    TK_KW_LAST
} HxTokKind;

typedef struct {
    HxTokKind kind;
    HxSpan span;
    HxSym sym;
    int64_t ival;
    double fval;
    const char *str_raw;
    int str_len;
} HxToken;

typedef struct {
    const char *src;
    const char *file;
    const char *p;
    HxArena *arena;
    HxIntern *intern;
    HxDiagBag *diags;
    HX_VEC_ANON(HxToken) tokens;
    int line;
} HxLexer;

void hx_lex_init(HxLexer *lx, HxArena *a, HxIntern *intern, HxDiagBag *diags,
                 const char *file, const char *src);
void hx_lex(HxLexer *lx);
const char *hx_tok_kind_name(HxTokKind k);
const char *hx_tok_text(HxTokKind k);
int hx_tok_is_kw(HxTokKind k);
int hx_tok_is_eof(HxTokKind k);

#endif