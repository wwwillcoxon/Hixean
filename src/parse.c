#include "hx/ast.h"
#include "hx/lex.h"
#include "hx/parse.h"

#include <string.h>

typedef struct {
    HxArena *arena;
    HxIntern *intern;
    HxDiagBag *diags;
    HX_VEC_ANON(HxToken) toks;
    int pos;
    int panicking;
} HxParser;

static HxToken *hx_peek(HxParser *p, int ahead) {
    int i = p->pos;
    while (i < p->toks.len && ahead >= 0) {
        if (p->toks.data[i].kind == TK_COMMENT) {
            i++;
            continue;
        }
        if (ahead == 0) break;
        ahead--;
        i++;
    }
    while (i < p->toks.len && p->toks.data[i].kind == TK_COMMENT) i++;
    if (i >= p->toks.len) return &p->toks.data[p->toks.len - 1];
    return &p->toks.data[i];
}

static HxToken *hx_cur(HxParser *p) { return hx_peek(p, 0); }

static HxToken *hx_at(HxParser *p, int ahead) { return hx_peek(p, ahead); }

static HxSpan hx_tok_span(HxParser *p, int rel) {
    int i = p->pos;
    if (rel >= 0) {
        int n = 0;
        while (i < p->toks.len) {
            if (p->toks.data[i].kind == TK_COMMENT) {
                i++;
                continue;
            }
            if (n == rel) return p->toks.data[i].span;
            n++;
            i++;
        }
        return p->toks.data[p->toks.len - 1].span;
    }
    int j = i - 1;
    while (j >= 0) {
        if (p->toks.data[j].kind != TK_COMMENT) return p->toks.data[j].span;
        j--;
    }
    return p->toks.data[0].span;
}

static void hx_bump(HxParser *p) {
    if (hx_cur(p)->kind != TK_EOF) p->pos++;
}

static int hx_tok_str_eq(HxToken *t, const char *s) {
    size_t n = strlen(s);
    return t->kind == TK_PUNCT && t->str_raw && (size_t)t->str_len == n &&
           memcmp(t->str_raw, s, n) == 0;
}

static int hx_is_punct(HxParser *p, const char *s) { return hx_tok_str_eq(hx_cur(p), s); }

static int hx_is_punct_at(HxParser *p, int i, const char *s) {
    return hx_tok_str_eq(hx_at(p, i), s);
}

static int hx_is_kw(HxParser *p, HxTokKind k) { return hx_cur(p)->kind == k; }

static int hx_eat_punct(HxParser *p, const char *s) {
    if (hx_is_punct(p, s)) {
        hx_bump(p);
        return 1;
    }
    return 0;
}

static int hx_eat_kw(HxParser *p, HxTokKind k) {
    if (hx_is_kw(p, k)) {
        hx_bump(p);
        return 1;
    }
    return 0;
}



static void hx_expect_punct(HxParser *p, const char *s) {
    if (hx_eat_punct(p, s)) return;
    HxToken *t = hx_cur(p);
    hx_diag_note(p->diags, t->span, "E0201",
                 hx_arena_sprintf(p->arena, "se esperaba '%s'", s),
                 hx_arena_sprintf(p->arena, "se encontró %s aquí", hx_tok_kind_name(t->kind)),
                 NULL);
    p->panicking = 1;
}

static void hx_expect_kw(HxParser *p, HxTokKind k, const char *what) {
    if (hx_eat_kw(p, k)) return;
    HxToken *t = hx_cur(p);
    hx_diag_note(p->diags, t->span, "E0201",
                 hx_arena_sprintf(p->arena, "se esperaba %s", what),
                 hx_arena_sprintf(p->arena, "se encontró %s aquí", hx_tok_kind_name(t->kind)),
                 NULL);
    p->panicking = 1;
}

static void hx_skip_nl(HxParser *p) {
    while (hx_is_kw(p, TK_NL)) hx_bump(p);
}

static void hx_sync_stmt(HxParser *p) {
    p->panicking = 0;
    int depth = 0;
    while (!hx_is_kw(p, TK_EOF)) {
        HxTokKind k = hx_cur(p)->kind;
        if (hx_tok_is_kw(k)) {
            const char *text = hx_tok_text(k);
            if (text && (!strcmp(text, "if") || !strcmp(text, "while") ||
                         !strcmp(text, "for") || !strcmp(text, "arena") ||
                         !strcmp(text, "match")))
                depth++;
            if (text && !strcmp(text, "end")) {
                if (depth <= 1) return;
                depth--;
            }
        }
        if (k == TK_NL && depth == 0) {
            hx_bump(p);
            return;
        }
        hx_bump(p);
    }
}

static HxExpr *hx_expr(HxParser *p);
static HxTy *hx_type(HxParser *p);
static void hx_stmt_into(HxParser *p, HxStmtVec *out);
static HxPattern *hx_pattern(HxParser *p);
static HxExpr *hx_expr_new(HxParser *p, HxExprKind k, HxSpan sp);
static HxExpr *hx_primary(HxParser *p);

static HxPattern *hx_pat_new(HxParser *p, HxPatKind k, HxSpan sp) {
    HxPattern *pat = (HxPattern *)hx_arena_calloc(p->arena, sizeof(HxPattern));
    pat->kind = k;
    pat->span = sp;
    return pat;
}

static HxPattern *hx_pattern_primary(HxParser *p) {
    HxToken *t = hx_cur(p);
    HxSpan sp = t->span;
    if (hx_tok_str_eq(t, "_")) {
        hx_bump(p);
        return hx_pat_new(p, PAT_WILDCARD, sp);
    }
    if (t->kind == TK_KW_TRUE || t->kind == TK_KW_FALSE) {
        hx_bump(p);
        HxPattern *pat = hx_pat_new(p, PAT_LITERAL, sp);
        pat->lit = hx_expr_new(p, EX_BOOL, sp);
        pat->lit->ival = t->kind == TK_KW_TRUE;
        return pat;
    }
    if (t->kind == TK_INT || t->kind == TK_FLOAT || t->kind == TK_STRING ||
        t->kind == TK_DURATION || (t->kind == TK_PUNCT && t->str_len == 1 &&
                                   (t->str_raw[0] == '-' || t->str_raw[0] == '+'))) {
        HxPattern *pat = hx_pat_new(p, PAT_LITERAL, sp);
        pat->lit = hx_primary(p);
        return pat;
    }
    if (t->kind == TK_IDENT) {
        HxSym name = t->sym;
        HxSpan nsp = t->span;
        hx_bump(p);
        if (hx_is_punct(p, ".")) {
            hx_bump(p);
            if (hx_is_kw(p, TK_IDENT)) hx_bump(p);
        }
        if (hx_is_punct(p, "(")) {
            hx_bump(p);
            hx_skip_nl(p);
            HxPattern *pat = hx_pat_new(p, PAT_CONSTRUCTOR, nsp);
            pat->ctor = name;
            if (!hx_is_punct(p, ")")) {
                for (;;) {
                    hx_skip_nl(p);
                    HX_VEC_PUSH(pat->args, *hx_pattern(p));
                    hx_skip_nl(p);
                    if (!hx_eat_punct(p, ",")) break;
                }
            }
            hx_expect_punct(p, ")");
            return pat;
        }
        HxPattern *pat = hx_pat_new(p, PAT_BIND, nsp);
        pat->name = name;
        return pat;
    }
    hx_error(p->diags, sp, "E0206", "se esperaba un patrón");
    hx_bump(p);
    return hx_pat_new(p, PAT_WILDCARD, sp);
}

static HxPattern *hx_pattern(HxParser *p) {
    HxPattern *lhs = hx_pattern_primary(p);
    if (hx_is_kw(p, TK_KW_TO) || hx_is_punct(p, "..")) {
        hx_bump(p);
        HxPattern *pat = hx_pat_new(p, PAT_RANGE, lhs->span);
        pat->lo = lhs->lit ? lhs->lit : NULL;
        if (!pat->lo) {
            hx_error(p->diags, lhs->span, "E0206", "un rango debe empezar con un literal");
        }
        pat->hi = hx_primary(p);
        return pat;
    }
    while (hx_is_punct(p, "|")) {
        hx_bump(p);
        HxPattern *rhs = hx_pattern_primary(p);
        HxPattern *alt = hx_pat_new(p, PAT_CONSTRUCTOR, lhs->span);
        alt->ctor = hx_intern_cstr(p->intern, "__or__");
        HX_VEC_PUSH(alt->args, *lhs);
        HX_VEC_PUSH(alt->args, *rhs);
        lhs = alt;
    }
    return lhs;
}

static HxExpr *hx_expr_new(HxParser *p, HxExprKind k, HxSpan sp) {
    HxExpr *e = (HxExpr *)hx_arena_calloc(p->arena, sizeof(HxExpr));
    e->kind = k;
    e->span = sp;
    return e;
}

static HxStmt *hx_stmt_new(HxParser *p, HxStmtKind k, HxSpan sp) {
    HxStmt *s = (HxStmt *)hx_arena_calloc(p->arena, sizeof(HxStmt));
    s->kind = k;
    s->span = sp;
    return s;
}

static int hx_eat_type_marker(HxParser *p) {
    if (hx_eat_punct(p, ":")) return 1;
    if (hx_eat_kw(p, TK_KW_AS)) return 1;
    return 0;
}

static HxSpan hx_join(HxSpan a, HxSpan b) {
    HxSpan r = a;
    uint32_t bend = b.start + b.len;
    if (bend > r.start + r.len) r.len = bend - r.start;
    return r;
}

static HxTy *hx_ty_mk(HxArena *a, HxTyKind kind) {
    HxTy *t = (HxTy *)hx_arena_calloc(a, sizeof(HxTy));
    t->kind = kind;
    return t;
}

/* ---------------- types ---------------- */

static HxTy *hx_type(HxParser *p) {
    HxSpan sp = hx_cur(p)->span;
    HxTy *base = NULL;

    if (hx_eat_kw(p, TK_KW_MAYBE)) {
        HxTy *inner = hx_type(p);
        HxTy *t = hx_ty_mk(p->arena, TY_NAMED);
        t->name = hx_intern_cstr(p->intern, "MAYBE");
        t->elem = inner;
        return t;
    }
    if (hx_is_kw(p, TK_KW_REF) || hx_is_kw(p, TK_KW_PTR)) {
        int is_ref = hx_is_kw(p, TK_KW_REF);
        hx_bump(p);
        HxTy *inner = hx_type(p);
        HxTy *t = hx_ty_mk(p->arena, is_ref ? TY_REF : TY_PTR);
        t->inner = inner;
        return t;
    }

    if (hx_is_kw(p, TK_IDENT)) {
        HxSym name = hx_cur(p)->sym;
        HxSpan nsp = hx_cur(p)->span;
        hx_bump(p);
        base = hx_ty_mk(p->arena, TY_NAMED);
        base->name = name;
        if (!hx_ascii_casecmp(hx_sym_str(name), "Result") && hx_is_punct(p, "<")) {
            hx_bump(p);
            base = hx_ty_mk(p->arena, TY_NAMED);
            base->name = hx_intern_cstr(p->intern, "Result");
            if (!hx_is_punct(p, ">")) {
                base->elem = hx_type(p);
                if (hx_eat_punct(p, ",")) base->inner = hx_type(p);
            }
            if (!hx_eat_punct(p, ">")) hx_expect_punct(p, ">");
            return base;
        }
        while (hx_is_punct(p, ".")) {
            hx_bump(p);
            if (!hx_is_kw(p, TK_IDENT)) {
                hx_error(p->diags, hx_cur(p)->span, "E0202", "se esperaba un nombre de tipo");
                break;
            }
            HxSym part = hx_cur(p)->sym;
            hx_bump(p);
            HxTy *t = hx_ty_mk(p->arena, TY_NAMED);
            t->name = part;
            t->elem = base;
            base = t;
        }
        (void)nsp;
    } else {
        hx_error(p->diags, sp, "E0203", "se esperaba un tipo");
        p->panicking = 1;
        return hx_ty_mk(p->arena, TY_UNKNOWN);
    }

    while (hx_is_punct(p, "[")) {
        hx_bump(p);
        HxTy *t = hx_ty_mk(p->arena, TY_ARRAY);
        t->elem = base;
        t->size = -1;
        if (!hx_is_punct(p, "]")) {
            HxExpr *n = hx_expr(p);
            if (n && n->kind == EX_INT) t->size = n->ival;
        }
        hx_expect_punct(p, "]");
        base = t;
    }
    return base;
}

/* ---------------- expressions ---------------- */

static HxExpr *hx_binary(HxParser *p, int prec);

static int hx_binop_for(HxParser *p, int prec, HxBinOp *out) {
    HxToken *t = hx_cur(p);
    if (t->kind != TK_PUNCT && t->kind < TK_KW_FIRST) return 0;
    const char *s = t->kind == TK_PUNCT ? t->str_raw : hx_tok_text(t->kind);
    if (!s) return 0;
    struct {
        const char *text;
        int prec;
        HxBinOp op;
    } table[] = {
        {"or", 1, OP_OR},   {"xor", 1, OP_XOR},  {"and", 2, OP_AND},
        {"==", 3, OP_EQ},   {"=", 3, OP_EQ},     {"<>", 3, OP_NE},
        {"!=", 3, OP_NE},   {"<=", 3, OP_LE},    {">=", 3, OP_GE},
        {"<", 3, OP_LT},    {">", 3, OP_GT},     {"+", 4, OP_ADD},
        {"-", 4, OP_SUB},   {"++", 4, OP_CONCAT},{"+%", 4, OP_ADDW},
        {"-%", 4, OP_SUBW}, {"+|", 4, OP_ADDS},  {"-|", 4, OP_SUBS},
        {"*", 5, OP_MUL},   {"*%", 5, OP_MUL},   {"*|", 5, OP_MULS},
        {"/", 5, OP_DIV},   {"mod", 5, OP_MOD},  {NULL, 0, OP_ADD},
    };
    size_t sl = (t->kind == TK_PUNCT) ? (size_t)t->str_len : strlen(s);
    for (int i = 0; table[i].text; i++) {
        if (prec != table[i].prec) continue;
        if (strlen(table[i].text) == sl && memcmp(s, table[i].text, sl) == 0) {
            *out = table[i].op;
            return 1;
        }
    }
    return 0;
}

static HxExpr *hx_primary(HxParser *p) {
    HxToken *t = hx_cur(p);
    HxSpan sp = t->span;

    if (t->kind == TK_PUNCT) {
        const char *s = t->str_raw;
        size_t sn = (size_t)t->str_len;
        if (sn == 1 && s[0] == '(') {
            hx_bump(p);
            hx_skip_nl(p);
            HxExpr *first = hx_expr(p);
            hx_skip_nl(p);
            if (hx_is_punct(p, ",")) {
                HxExpr *e = hx_expr_new(p, EX_VEC, sp);
                e->vec.items = (HxExpr **)hx_arena_alloc(p->arena, sizeof(HxExpr *) * 4);
                e->vec.len = 0;
                e->vec.items[e->vec.len++] = first;
                while (hx_eat_punct(p, ",")) {
                    hx_skip_nl(p);
                    if (e->vec.len >= 4) {
                        hx_error(p->diags, hx_cur(p)->span, "E0204",
                                 "un literal de vector tiene como máximo 4 componentes");
                        break;
                    }
                    e->vec.items[e->vec.len++] = hx_expr(p);
                    hx_skip_nl(p);
                }
                hx_expect_punct(p, ")");
                e->span = hx_join(sp, hx_tok_span(p, -1));
                return e;
            }
            hx_expect_punct(p, ")");
            return first;
        }
        if (sn == 1 && (s[0] == '-' || s[0] == '+')) {
            int neg = s[0] == '-';
            hx_bump(p);
            HxExpr *operand = hx_primary(p);
            if (!neg) return operand;
            HxExpr *e = hx_expr_new(p, EX_UN, hx_join(sp, operand->span));
            e->un.op = UOP_NEG;
            e->un.operand = operand;
            return e;
        }
        if (sn == 1 && s[0] == '[') {
            hx_bump(p);
            hx_skip_nl(p);
            HxExpr *e = hx_expr_new(p, EX_VEC, sp);
            e->vec.items = (HxExpr **)hx_arena_alloc(p->arena, sizeof(HxExpr *) * 4);
            e->vec.len = 0;
            e->vec.items[e->vec.len++] = hx_expr(p);
            hx_skip_nl(p);
            while (hx_eat_punct(p, ",")) {
                hx_skip_nl(p);
                if (e->vec.len >= 4) {
                    hx_error(p->diags, hx_cur(p)->span, "E0204",
                             "un literal de vector tiene como máximo 4 componentes");
                    break;
                }
                e->vec.items[e->vec.len++] = hx_expr(p);
                hx_skip_nl(p);
            }
            hx_expect_punct(p, "]");
            e->span = hx_join(sp, hx_tok_span(p, -1));
            return e;
        }
    }

    if (t->kind == TK_INT) {
        hx_bump(p);
        HxExpr *e = hx_expr_new(p, EX_INT, sp);
        e->ival = t->ival;
        return e;
    }
    if (t->kind == TK_FLOAT) {
        hx_bump(p);
        HxExpr *e = hx_expr_new(p, EX_FLOAT, sp);
        e->fval = t->fval;
        return e;
    }
    if (t->kind == TK_DURATION) {
        hx_bump(p);
        HxExpr *e = hx_expr_new(p, EX_DURATION, sp);
        e->ival = (int64_t)t->fval;
        return e;
    }
    if (t->kind == TK_KW_TRUE || t->kind == TK_KW_FALSE) {
        hx_bump(p);
        HxExpr *e = hx_expr_new(p, EX_BOOL, sp);
        e->ival = t->ival;
        return e;
    }
    if (t->kind == TK_KW_NIL) {
        hx_bump(p);
        return hx_expr_new(p, EX_NIL, sp);
    }
    if (t->kind == TK_STRING) {
        hx_bump(p);
        HxExpr *e = hx_expr_new(p, EX_STR, sp);
        e->str.raw = t->str_raw;
        e->str.len = t->str_len;
        for (int i = 0; i < t->str_len; i++) {
            if (t->str_raw[i] == '{' && (i + 1 >= t->str_len || t->str_raw[i + 1] != '{')) {
                e->has_holes = 1;
                break;
            }
        }
        if (hx_is_punct(p, ".") && hx_at(p, 1)->kind == TK_IDENT &&
            hx_is_punct_at(p, 2, "(")) {
            e->method = hx_at(p, 1)->sym;
            hx_bump(p);
            hx_bump(p);
        }
        return e;
    }
    if (t->kind == TK_KW_NOT) {
        hx_bump(p);
        HxExpr *operand = hx_primary(p);
        HxExpr *e = hx_expr_new(p, EX_UN, hx_join(sp, operand->span));
        e->un.op = UOP_NOT;
        e->un.operand = operand;
        return e;
    }
    if (t->kind == TK_IDENT || hx_tok_is_kw(t->kind)) {
        HxExpr *e = hx_expr_new(p, EX_PATH, sp);
        HX_VEC_PUSH(e->path.parts, ((HxPathPart){t->sym, sp}));
        hx_bump(p);
        while (hx_is_punct(p, ".") &&
               (hx_at(p, 1)->kind == TK_IDENT || hx_tok_is_kw(hx_at(p, 1)->kind))) {
            hx_bump(p);
            HxSym nm = hx_cur(p)->sym;
            HxSpan nsp = hx_cur(p)->span;
            hx_bump(p);
            HX_VEC_PUSH(e->path.parts, ((HxPathPart){nm, nsp}));
        }
        return e;
    }

    hx_error(p->diags, sp, "E0205", "se esperaba una expresión");
    hx_bump(p);
    return hx_expr_new(p, EX_NIL, sp);
}

static HxExpr *hx_postfix(HxParser *p) {
    HxExpr *e = hx_primary(p);
    for (;;) {
        if (hx_is_punct(p, "(")) {
            HxSpan sp = e->span;
            hx_bump(p);
            hx_skip_nl(p);
            HxExpr *call = hx_expr_new(p, EX_CALL, sp);
            call->call.callee = e;
            if (!hx_is_punct(p, ")")) {
                for (;;) {
                    hx_skip_nl(p);
                    HxArg arg;
                    memset(&arg, 0, sizeof(arg));
                    if ((hx_is_kw(p, TK_IDENT) || hx_tok_is_kw(hx_cur(p)->kind)) &&
                        hx_is_punct_at(p, 1, ":")) {
                        arg.name = hx_cur(p)->sym;
                        arg.name_span = hx_cur(p)->span;
                        hx_bump(p);
                        hx_bump(p);
                        hx_skip_nl(p);
                    }
                    arg.value = hx_expr(p);
                    arg.span = arg.value->span;
                    HX_VEC_PUSH(call->call.args, arg);
                    hx_skip_nl(p);
                    if (!hx_eat_punct(p, ",")) break;
                }
            }
            hx_expect_punct(p, ")");
            call->span = hx_join(sp, hx_tok_span(p, -1));
            e = call;
            continue;
        }
        if (hx_is_punct(p, "[")) {
            HxSpan sp = e->span;
            hx_bump(p);
            HxExpr *idx = hx_expr_new(p, EX_INDEX, sp);
            idx->index.base = e;
            idx->index.start = NULL;
            idx->index.end = NULL;
            if (!hx_is_punct(p, "]") && !hx_is_punct(p, ".."))
                idx->index.start = hx_expr(p);
            if (hx_eat_punct(p, "..")) {
                if (!hx_is_punct(p, "]")) idx->index.end = hx_expr(p);
            }
            hx_expect_punct(p, "]");
            idx->span = hx_join(sp, hx_tok_span(p, -1));
            e = idx;
            continue;
        }
        if (hx_is_punct(p, ".") &&
            (hx_at(p, 1)->kind == TK_IDENT || hx_tok_is_kw(hx_at(p, 1)->kind))) {
            hx_bump(p);
            HxSym nm = hx_cur(p)->sym;
            HxSpan nsp = hx_cur(p)->span;
            hx_bump(p);
            HxExpr *m = hx_expr_new(p, EX_MEMB, hx_join(e->span, nsp));
            m->member.base = e;
            m->member.name = nm;
            m->member.name_span = nsp;
            e = m;
            continue;
        }
        if (hx_is_punct(p, "?")) {
            HxSpan qsp = hx_cur(p)->span;
            hx_bump(p);
            HxExpr *t = hx_expr_new(p, EX_TRY, hx_join(e->span, qsp));
            t->try.inner = e;
            e = t;
            continue;
        }
        break;
    }
    return e;
}

static HxExpr *hx_binary(HxParser *p, int prec) {
    HxExpr *lhs = prec >= 6 ? hx_postfix(p) : hx_binary(p, prec + 1);
    if (prec > 5) return lhs;
    for (;;) {
        HxBinOp op;
        if (!hx_binop_for(p, prec, &op)) break;
        hx_bump(p);
        hx_skip_nl(p);
        HxExpr *rhs = hx_binary(p, prec + 1);
        HxExpr *e = hx_expr_new(p, EX_BIN, hx_join(lhs->span, rhs->span));
        e->bin.op = op;
        e->bin.lhs = lhs;
        e->bin.rhs = rhs;
        lhs = e;
    }
    return lhs;
}

static HxExpr *hx_expr(HxParser *p) { return hx_binary(p, 1); }

/* ---------------- statements ---------------- */

static int hx_at_block_end(HxParser *p) {
    if (hx_is_kw(p, TK_KW_NEXT)) return 1;
    if (hx_is_kw(p, TK_KW_ELSE) || hx_is_kw(p, TK_KW_ELSEIF)) return 1;
    if (hx_is_kw(p, TK_KW_CASE)) return 1;
    if (hx_is_kw(p, TK_KW_WEND)) return 1;
    if (!hx_is_kw(p, TK_KW_END)) return 0;
    const char *text = hx_tok_text(hx_at(p, 1)->kind);
    if (!text) return 0;
    return !hx_ascii_casecmp(text, "if") || !hx_ascii_casecmp(text, "while") ||
           !hx_ascii_casecmp(text, "for") || !hx_ascii_casecmp(text, "arena") ||
           !hx_ascii_casecmp(text, "function") || !hx_ascii_casecmp(text, "match");
}

static void hx_block_body(HxParser *p, HxStmtVec *out) {
    for (;;) {
        hx_skip_nl(p);
        if (hx_is_kw(p, TK_EOF) || hx_at_block_end(p)) break;
        if (p->panicking) {
            hx_sync_stmt(p);
            continue;
        }
        int before = p->pos;
        hx_stmt_into(p, out);
        if (p->pos == before) {
            hx_bump(p);
            continue;
        }
        if (!hx_at_block_end(p) && !hx_is_kw(p, TK_EOF) && !hx_is_kw(p, TK_NL) &&
            !hx_is_kw(p, TK_KW_ELSE) && !hx_is_kw(p, TK_KW_ELSEIF))
            hx_expect_kw(p, TK_NL, "fin de línea");
    }
}

static void hx_parse_bindings(HxParser *p, HxBindVec *out) {
    for (;;) {
        HxBind b;
        memset(&b, 0, sizeof(b));
        b.name_span = hx_cur(p)->span;
        if (!hx_is_kw(p, TK_IDENT)) {
            hx_error(p->diags, hx_cur(p)->span, "E0206", "se esperaba un nombre de variable");
            p->panicking = 1;
            return;
        }
        b.name = hx_cur(p)->sym;
        hx_bump(p);
        if (hx_eat_type_marker(p)) b.ty = hx_type(p);
        if (hx_eat_punct(p, "=")) {
            hx_skip_nl(p);
            b.value = hx_expr(p);
        }
        b.span = b.name_span;
        HX_VEC_PUSH(*out, b);
        if (!hx_eat_punct(p, ",")) return;
        hx_skip_nl(p);
    }
}

static void hx_if_tail(HxParser *p, HxStmt *s) {
    for (;;) {
        if (hx_is_kw(p, TK_KW_ELSEIF)) {
            hx_bump(p);
            HxExpr *c = hx_expr(p);
            hx_expect_kw(p, TK_KW_THEN, "THEN");
            if (s->if_.n_elifs >= 16) {
                hx_error(p->diags, s->span, "E0207", "demasiadas ramas ELSEIF");
                return;
            }
            int i = s->if_.n_elifs++;
            s->if_.elifs[i].cond = c;
            if (hx_is_kw(p, TK_NL) || hx_is_kw(p, TK_EOF)) {
                hx_skip_nl(p);
                hx_block_body(p, &s->if_.elifs[i].then);
                hx_skip_nl(p);
                if (!hx_is_kw(p, TK_KW_ELSEIF) && !hx_is_kw(p, TK_KW_ELSE)) {
                    hx_expect_kw(p, TK_KW_END, "END");
                    hx_expect_kw(p, TK_KW_IF, "IF");
                }
            } else {
                hx_stmt_into(p, &s->if_.elifs[i].then);
            }
            continue;
        }
        if (hx_is_kw(p, TK_KW_ELSE)) {
            hx_bump(p);
            s->if_.has_else = 1;
            if (hx_is_kw(p, TK_NL) || hx_is_kw(p, TK_EOF)) {
                hx_skip_nl(p);
                hx_block_body(p, &s->if_.else_);
                hx_expect_kw(p, TK_KW_END, "END");
                hx_expect_kw(p, TK_KW_IF, "IF");
            } else {
                hx_stmt_into(p, &s->if_.else_);
            }
        }
        return;
    }
}

static void hx_stmt_into(HxParser *p, HxStmtVec *out) {
    HxToken *t = hx_cur(p);
    HxSpan sp = t->span;

    if (hx_tok_str_eq(t, ".")) {
        hx_bump(p);
        HX_VEC_PUSH(*out, *hx_stmt_new(p, ST_NOP, sp));
        return;
    }

    switch (t->kind) {
        case TK_KW_IF: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_IF, sp);
            s->if_.cond = hx_expr(p);
            hx_expect_kw(p, TK_KW_THEN, "THEN");
            if (hx_is_kw(p, TK_NL) || hx_is_kw(p, TK_EOF)) {
                hx_skip_nl(p);
                hx_block_body(p, &s->if_.then);
                if (!hx_is_kw(p, TK_KW_ELSEIF) && !hx_is_kw(p, TK_KW_ELSE)) {
                    hx_expect_kw(p, TK_KW_END, "END");
                    hx_expect_kw(p, TK_KW_IF, "IF");
                }
            } else {
                hx_stmt_into(p, &s->if_.then);
            }
            hx_if_tail(p, s);
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_WHILE: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_WHILE, sp);
            s->while_.cond = hx_expr(p);
            hx_skip_nl(p);
            hx_block_body(p, &s->while_.body);
            if (hx_eat_kw(p, TK_KW_END)) hx_expect_kw(p, TK_KW_WHILE, "WHILE");
            else if (!hx_eat_kw(p, TK_KW_WEND)) hx_expect_kw(p, TK_KW_END, "END");
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_FOR: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_FOR, sp);
            s->for_.var_span = hx_cur(p)->span;
            if (!hx_is_kw(p, TK_IDENT)) {
                hx_error(p->diags, s->for_.var_span, "E0206",
                         "se esperaba el nombre de la variable de bucle");
                p->panicking = 1;
                HX_VEC_PUSH(*out, *s);
                return;
            }
            s->for_.var = hx_cur(p)->sym;
            hx_bump(p);
            if (hx_eat_type_marker(p)) hx_type(p);
            if (hx_eat_punct(p, "=")) {
                hx_skip_nl(p);
                s->for_.start = hx_expr(p);
                hx_expect_kw(p, TK_KW_TO, "TO");
                s->for_.end = hx_expr(p);
                if (hx_eat_kw(p, TK_KW_STEP)) {
                    hx_skip_nl(p);
                    s->for_.step = hx_expr(p);
                }
            } else {
                hx_expect_punct(p, "=");
            }
            hx_skip_nl(p);
            hx_block_body(p, &s->for_.body);
            if (hx_eat_kw(p, TK_KW_END)) hx_expect_kw(p, TK_KW_NEXT, "NEXT");
            else hx_expect_kw(p, TK_KW_NEXT, "NEXT");
            if (hx_is_kw(p, TK_IDENT)) hx_bump(p);
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_ARENA: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_ARENA, sp);
            if (hx_is_kw(p, TK_IDENT)) {
                s->arena.name = hx_cur(p)->sym;
                hx_bump(p);
            }
            hx_skip_nl(p);
            hx_block_body(p, &s->arena.body);
            hx_expect_kw(p, TK_KW_END, "END");
            hx_expect_kw(p, TK_KW_ARENA, "ARENA");
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_MATCH: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_MATCH, sp);
            s->match.subject = hx_expr(p);
            if (hx_eat_kw(p, TK_KW_AS)) {
                if (hx_is_kw(p, TK_IDENT)) {
                    s->match.subject_name = hx_cur(p)->sym;
                    hx_bump(p);
                }
            }
            hx_skip_nl(p);
            while (hx_is_kw(p, TK_KW_CASE)) {
                hx_bump(p);
                if (hx_is_kw(p, TK_KW_ELSE)) {
                    hx_bump(p);
                    hx_expect_kw(p, TK_KW_THEN, "THEN");
                    s->match.has_else = 1;
                    hx_skip_nl(p);
                    HxStmtVec body = {0, 0, 0};
                    hx_block_body(p, &body);
                    s->match.else_body = body;
                    continue;
                }
                HxMatchCase mc;
                memset(&mc, 0, sizeof(mc));
                mc.pattern = hx_pattern(p);
                if (hx_eat_kw(p, TK_KW_WHEN)) {
                    hx_skip_nl(p);
                    mc.guard = hx_expr(p);
                }
                hx_expect_kw(p, TK_KW_THEN, "THEN");
                hx_skip_nl(p);
                hx_block_body(p, &mc.body);
                HX_VEC_PUSH(s->match.cases, mc);
            }
            hx_expect_kw(p, TK_KW_END, "END");
            hx_expect_kw(p, TK_KW_MATCH, "MATCH");
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_DEFER: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_DEFER, sp);
            hx_stmt_into(p, &s->inner);
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_DIM:
        case TK_KW_CONST: {
            int is_const = t->kind == TK_KW_CONST;
            hx_bump(p);
            HxBindVec binds;
            memset(&binds, 0, sizeof(binds));
            hx_parse_bindings(p, &binds);
            for (int i = 0; i < binds.len; i++) {
                HxStmt *s =
                    hx_stmt_new(p, is_const ? ST_CONST : ST_DIM, binds.data[i].name_span);
                s->span = binds.data[i].name_span;
                if (is_const) {
                    s->konst.name = binds.data[i].name;
                    s->konst.name_span = binds.data[i].name_span;
                    s->konst.value = binds.data[i].value;
                    s->konst.ty = binds.data[i].ty;
                } else {
                    s->dim.name = binds.data[i].name;
                    s->dim.name_span = binds.data[i].name_span;
                    s->dim.ty = binds.data[i].ty;
                    s->dim.init = binds.data[i].value;
                    s->dim.is_const = 0;
                }
                HX_VEC_PUSH(*out, *s);
            }
            return;
        }
        case TK_KW_RETURN: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_RETURN, sp);
            if (!hx_is_kw(p, TK_NL) && !hx_is_kw(p, TK_EOF) && !hx_is_punct(p, "}"))
                s->ret.value = hx_expr(p);
            HX_VEC_PUSH(*out, *s);
            return;
        }
        case TK_KW_BREAK:
            hx_bump(p);
            HX_VEC_PUSH(*out, *hx_stmt_new(p, ST_BREAK, sp));
            return;
        case TK_KW_CONTINUE:
            hx_bump(p);
            HX_VEC_PUSH(*out, *hx_stmt_new(p, ST_CONTINUE, sp));
            return;
        case TK_KW_EXIT: {
            hx_bump(p);
            HxStmt *s = hx_stmt_new(p, ST_EXIT, sp);
            if (hx_eat_punct(p, "(")) {
                s->exit_.code = hx_expr(p);
                hx_expect_punct(p, ")");
            } else if (hx_is_kw(p, TK_KW_FUNCTION)) {
                hx_bump(p);
            } else if (hx_is_kw(p, TK_KW_FOR) || hx_is_kw(p, TK_KW_WHILE)) {
                hx_bump(p);
                s->kind = ST_BREAK;
            }
            HX_VEC_PUSH(*out, *s);
            return;
        }
        default: break;
    }

    if (t->kind == TK_KW_PRINT) {
        hx_bump(p);
        HxStmt *s = hx_stmt_new(p, ST_PRINT, sp);
        while (!hx_is_kw(p, TK_NL) && !hx_is_kw(p, TK_EOF)) {
            if (hx_is_punct(p, ";")) {
                hx_bump(p);
                HX_VEC_PUSH(s->print.items, ((HxPrintItem){NULL, PS_SEP_SEMI}));
                continue;
            }
            if (hx_is_punct(p, ",")) {
                hx_bump(p);
                HX_VEC_PUSH(s->print.items, ((HxPrintItem){NULL, PS_SEP_COMMA}));
                continue;
            }
            HxExpr *e = hx_expr(p);
            HX_VEC_PUSH(s->print.items, ((HxPrintItem){e, PS_EXPR}));
        }
        HX_VEC_PUSH(*out, *s);
        return;
    }

    HxExpr *e = hx_binary(p, 4);
    if ((e->kind == EX_PATH || e->kind == EX_INDEX) && hx_is_punct(p, "=")) {
        hx_bump(p);
        hx_skip_nl(p);
        HxStmt *s = hx_stmt_new(p, ST_ASSIGN, sp);
        s->assign.target = e;
        s->assign.op = OP_ADD;
        s->assign.value = hx_expr(p);
        s->span = hx_join(e->span, s->assign.value->span);
        HX_VEC_PUSH(*out, *s);
        return;
    }
    static const char *ops[] = {"+=", "-=", "*=", "/="};
    for (int i = 0; i < 4; i++) {
        if (!hx_is_punct(p, ops[i])) continue;
        hx_bump(p);
        hx_skip_nl(p);
        HxStmt *s = hx_stmt_new(p, ST_ASSIGN, sp);
        s->assign.target = e;
        s->assign.op = i == 0 ? OP_ADD : i == 1 ? OP_SUB : i == 2 ? OP_MUL : OP_DIV;
        s->assign.compound = 1;
        s->assign.value = hx_expr(p);
        s->span = hx_join(e->span, s->assign.value->span);
        HX_VEC_PUSH(*out, *s);
        return;
    }
    HxStmt *s = hx_stmt_new(p, ST_EXPR, e->span);
    s->expr = e;
    HX_VEC_PUSH(*out, *s);
}

/* ---------------- declarations ---------------- */

static void hx_skip_generic(HxParser *p) {
    if (!hx_is_punct(p, "<")) return;
    int depth = 0;
    do {
        HxToken *t = hx_cur(p);
        if (t->kind == TK_PUNCT && !strcmp(t->str_raw, "<")) depth++;
        if (t->kind == TK_PUNCT && !strcmp(t->str_raw, ">")) depth--;
        hx_bump(p);
    } while (depth > 0 && !hx_is_kw(p, TK_EOF));
}

static void hx_parse_func(HxParser *p, HxFunc *f, int is_export) {
    f->is_export = is_export;
    f->span = hx_cur(p)->span;
    hx_bump(p);
    if (hx_eat_kw(p, TK_KW_OPERATOR)) {
        HxToken *op = hx_cur(p);
        f->name_span = op->span;
        f->name = hx_intern(p->intern, op->str_raw ? op->str_raw : "operator",
                            op->str_raw ? (size_t)op->str_len : 8);
        hx_bump(p);
    } else {
        if (!hx_is_kw(p, TK_IDENT)) {
            hx_error(p->diags, hx_cur(p)->span, "E0208", "se esperaba el nombre de la función");
            p->panicking = 1;
            return;
        }
        f->name_span = hx_cur(p)->span;
        f->name = hx_cur(p)->sym;
        hx_bump(p);
    }
    hx_skip_generic(p);
    hx_expect_punct(p, "(");
    if (!hx_is_punct(p, ")")) {
        for (;;) {
            HxParam prm;
            memset(&prm, 0, sizeof(prm));
            if (hx_is_kw(p, TK_KW_REF)) {
                prm.is_ref = 1;
                hx_bump(p);
            }
            if (!hx_is_kw(p, TK_IDENT)) {
                hx_error(p->diags, hx_cur(p)->span, "E0209",
                         "se esperaba el nombre de un parámetro");
                p->panicking = 1;
                break;
            }
            prm.name = hx_cur(p)->sym;
            prm.span = hx_cur(p)->span;
            hx_bump(p);
            if (hx_eat_type_marker(p)) prm.ty = hx_type(p);
            if (hx_eat_punct(p, "=")) prm.default_value = hx_expr(p);
            HX_VEC_PUSH(f->params, prm);
            if (!hx_eat_punct(p, ",")) break;
        }
    }
    hx_expect_punct(p, ")");
    f->ret = hx_eat_kw(p, TK_KW_AS) ? hx_type(p) : hx_ty_mk(p->arena, TY_VOID);
    if (hx_eat_kw(p, TK_KW_TAIL)) f->is_comptime_only = 1;
    hx_skip_nl(p);
    hx_block_body(p, &f->body);
    hx_expect_kw(p, TK_KW_END, "END");
    hx_expect_kw(p, TK_KW_FUNCTION, "FUNCTION");
    if (hx_is_kw(p, TK_IDENT)) hx_bump(p);
}

static void hx_parse_type(HxParser *p, HxTypeDecl *t, int is_export) {
    t->is_export = is_export;
    t->span = hx_cur(p)->span;
    hx_bump(p);
    t->name = hx_cur(p)->sym;
    hx_bump(p);
    hx_skip_generic(p);
    hx_skip_nl(p);
    while (!hx_is_kw(p, TK_KW_END) && !hx_is_kw(p, TK_EOF)) {
        if (p->panicking) {
            hx_sync_stmt(p);
            continue;
        }
        HxField f;
        memset(&f, 0, sizeof(f));
        if (hx_eat_kw(p, TK_KW_UNIQUE)) f.is_unique = 1;
        f.span = hx_cur(p)->span;
        f.name = hx_cur(p)->sym;
        hx_bump(p);
        if (!hx_eat_type_marker(p)) hx_expect_punct(p, ":");
        f.ty = hx_type(p);
        HX_VEC_PUSH(t->fields, f);
        if (!hx_eat_kw(p, TK_NL)) hx_expect_kw(p, TK_NL, "fin de línea");
    }
    hx_expect_kw(p, TK_KW_END, "END");
    hx_expect_kw(p, TK_KW_TYPE, "TYPE");
}

static void hx_skip_rest_of_line(HxParser *p) {
    while (!hx_is_kw(p, TK_NL) && !hx_is_kw(p, TK_EOF)) hx_bump(p);
    hx_eat_kw(p, TK_NL);
}

void hx_parse_module(HxUnit *unit, HxModule *m, const char *src, const char *file) {
    HxParser p;
    memset(&p, 0, sizeof(p));
    p.arena = unit->arena;
    p.intern = unit->intern;
    p.diags = unit->diags;
    p.diags->ctx_file = file;
    p.diags->ctx_src = src;

    HxLexer lx;
    hx_lex_init(&lx, unit->arena, unit->intern, unit->diags, file, src);
    hx_lex(&lx);
    for (int i = 0; i < lx.tokens.len; i++) HX_VEC_PUSH(p.toks, lx.tokens.data[i]);

    HxSym declared_name = NULL;
    if (hx_is_kw(&p, TK_KW_MODULE)) {
        hx_bump(&p);
        if (hx_is_kw(&p, TK_IDENT)) {
            declared_name = hx_cur(&p)->sym;
            hx_bump(&p);
        }
        hx_skip_nl(&p);
    }
    m->name = declared_name ? declared_name
                             : hx_intern_cstr(unit->intern, hx_path_stem(unit->arena, file));
    m->file = file;
    m->src = src;

    while (!hx_is_kw(&p, TK_EOF)) {
        if (p.panicking) {
            hx_sync_stmt(&p);
            continue;
        }
        hx_skip_nl(&p);
        if (hx_is_kw(&p, TK_EOF)) break;
        int is_export = 0;
        if (hx_is_kw(&p, TK_KW_EXPORT)) {
            is_export = 1;
            hx_bump(&p);
        }
        if (hx_is_kw(&p, TK_KW_MODULE)) {
            hx_bump(&p);
            if (hx_is_kw(&p, TK_IDENT)) hx_bump(&p);
            hx_skip_nl(&p);
            continue;
        }
        if (hx_is_kw(&p, TK_KW_IMPORT)) {
            HxImport im;
            memset(&im, 0, sizeof(im));
            im.span = hx_cur(&p)->span;
            hx_bump(&p);
            char *path = hx_arena_strdup(unit->arena, "");
            int first = 1;
            while (hx_is_kw(&p, TK_IDENT)) {
                if (first)
                    path = hx_arena_strdup(unit->arena, hx_sym_str(hx_cur(&p)->sym));
                else
                    path = hx_arena_sprintf(unit->arena, "%s.%s", path, hx_sym_str(hx_cur(&p)->sym));
                first = 0;
                hx_bump(&p);
                if (!hx_eat_punct(&p, ".")) break;
            }
            im.path = hx_intern_cstr(unit->intern, path);
            im.path_span = im.span;
            if (hx_eat_kw(&p, TK_KW_AS)) {
                if (hx_is_kw(&p, TK_IDENT)) {
                    im.alias = hx_cur(&p)->sym;
                    hx_bump(&p);
                }
            }
            HX_VEC_PUSH(m->imports, im);
            hx_skip_rest_of_line(&p);
            continue;
        }
        if (hx_is_kw(&p, TK_KW_FUNCTION)) {
            HxFunc f;
            memset(&f, 0, sizeof(f));
            hx_parse_func(&p, &f, is_export);
            if (f.name) {
                f.index = m->funcs.len;
                HX_VEC_PUSH(m->funcs, f);
            }
            continue;
        }
        if (hx_is_kw(&p, TK_KW_TYPE)) {
            HxTypeDecl t;
            memset(&t, 0, sizeof(t));
            hx_parse_type(&p, &t, is_export);
            t.index = m->types.len;
            HX_VEC_PUSH(m->types, t);
            continue;
        }
        if (hx_is_kw(&p, TK_KW_END)) {
            hx_bump(&p);
            hx_eat_kw(&p, TK_KW_MODULE);
            hx_eat_kw(&p, TK_NL);
            continue;
        }
        if (hx_is_kw(&p, TK_KW_TEST)) {
            hx_bump(&p);
            if (hx_is_kw(&p, TK_IDENT)) hx_bump(&p);
            hx_expect_punct(&p, "(");
            int depth = 1;
            while (depth > 0 && !hx_is_kw(&p, TK_EOF)) {
                if (hx_is_punct(&p, "(")) depth++;
                if (hx_is_punct(&p, ")")) depth--;
                hx_bump(&p);
            }
            if (hx_eat_kw(&p, TK_KW_AS)) hx_type(&p);
            hx_skip_nl(&p);
            HxStmtVec body;
            hx_block_body(&p, &body);
            hx_expect_kw(&p, TK_KW_END, "END");
            hx_expect_kw(&p, TK_KW_TEST, "TEST");
            continue;
        }
        int before = p.pos;
        hx_stmt_into(&p, &m->top);
        if (p.pos == before) hx_bump(&p);
    }

    for (int i = m->top.len - 1; i >= 0; i--) {
        HxStmt *st = &m->top.data[i];
        if (st->kind != ST_CONST) continue;
        HxConst cst;
        memset(&cst, 0, sizeof(cst));
        cst.name = st->konst.name;
        cst.value = st->konst.value;
        cst.ty = st->konst.ty;
        cst.span = st->konst.name_span;
        HX_VEC_PUSH(m->consts, cst);
        memmove(&m->top.data[i], &m->top.data[i + 1],
                (size_t)(m->top.len - i - 1) * sizeof(HxStmt));
        m->top.len--;
    }
}
HxExpr *hx_parse_subexpr(HxUnit *unit, const char *src, const char *file) {
    HxParser p;
    memset(&p, 0, sizeof(p));
    p.arena = unit->arena;
    p.intern = unit->intern;
    p.diags = unit->diags;
    HxLexer lx;
    hx_lex_init(&lx, unit->arena, unit->intern, unit->diags, file, src);
    hx_lex(&lx);
    for (int i = 0; i < lx.tokens.len; i++) HX_VEC_PUSH(p.toks, lx.tokens.data[i]);
    if (hx_is_kw(&p, TK_EOF)) return hx_expr_new(&p, EX_NIL, (HxSpan){0, 0});
    return hx_expr(&p);
}
