#include "hx/check.h"
#include "hx/parse.h"

#include <string.h>

typedef struct {
    HxSym name;
    HxTy *ty;
    int kind;
    HxSpan span;
} HxSymEntry;

enum { SK_VAR, SK_FUNC, SK_TYPE, SK_CONST, SK_MODULE };

typedef struct HxScope {
    HX_VEC_ANON(HxSymEntry) syms;
    struct HxScope *parent;
} HxScope;

typedef struct {
    HxUnit *unit;
    HxModule *mod;
    HxScope *scope;
    HxArena *arena;
    HxIntern *intern;
    HxDiagBag *diags;
    HxFunc *cur_func;
    HxTy *ret_ty;
    int loop_depth;
} HxChecker;

static void hx_scope_push(HxChecker *c) {
    HxScope *s = (HxScope *)hx_arena_calloc(c->arena, sizeof(HxScope));
    s->parent = c->scope;
    c->scope = s;
}

static void hx_scope_pop(HxChecker *c) { c->scope = c->scope->parent; }

static void hx_define(HxChecker *c, HxSym name, HxTy *ty, int kind, HxSpan span) {
    HxSymEntry e = {name, ty, kind, span};
    HX_VEC_PUSH(c->scope->syms, e);
}

static HxSymEntry *hx_lookup(HxChecker *c, HxSym name) {
    for (HxScope *s = c->scope; s; s = s->parent) {
        for (int i = s->syms.len - 1; i >= 0; i--)
            if (s->syms.data[i].name == name) return &s->syms.data[i];
    }
    return NULL;
}

static HxTypeDecl *hx_find_type(HxChecker *c, HxSym name) {
    for (int m = 0; m < c->unit->modules.len; m++) {
        HxModule *mod = &c->unit->modules.data[m];
        for (int i = 0; i < mod->types.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(mod->types.data[i].name), hx_sym_str(name)))
                return &mod->types.data[i];
    }
    return NULL;
}

static HxConst *hx_find_const(HxChecker *c, HxSym name) {
    for (int m = 0; m < c->unit->modules.len; m++) {
        HxModule *mod = &c->unit->modules.data[m];
        for (int i = 0; i < mod->consts.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(mod->consts.data[i].name), hx_sym_str(name)))
                return &mod->consts.data[i];
    }
    return NULL;
}

static HxFunc *hx_find_func(HxChecker *c, HxSym name) {
    for (int m = 0; m < c->unit->modules.len; m++) {
        HxModule *mod = &c->unit->modules.data[m];
        for (int i = 0; i < mod->funcs.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(mod->funcs.data[i].name), hx_sym_str(name)))
                return &mod->funcs.data[i];
    }
    return NULL;
}

static HxTy *hx_resolve_type(HxChecker *c, HxTy *t, HxSpan sp, int report);

static int hx_coerce(HxChecker *c, HxTy *from, HxTy *to, HxSpan sp, const char *what) {
    if (!from || !to) return 1;
    if (from->kind == TY_UNKNOWN || to->kind == TY_UNKNOWN) return 1;
    if (hx_ty_equal(from, to)) return 1;
    int fr = hx_ty_rank(from), tr = hx_ty_rank(to);
    if (fr && tr && from->kind == to->kind) return 1;
    if (fr && tr && fr < tr) return 1;
    hx_diag_note(c->diags, sp, "E0301",
                 hx_arena_sprintf(c->arena, "se esperaba %s, se encontró %s", hx_ty_name(to),
                                  hx_ty_name(from)),
                 what, NULL);
    return 0;
}

static HxTy *hx_int_ty(HxChecker *c) { return hx_ty_builtin(c->arena, TY_INT); }

typedef struct {
    const char *name;
    const char *cname;
    int nargs;
    HxTyKind ret;
} HxIntrin;

static const HxIntrin hx_string_intrins[] = {
    {"Len", "len_str", 0, TY_I64},
    {"IsEmpty", "is_empty_str", 0, TY_BOOL},
    {"Upper", "upper_str", 0, TY_STRING},
    {"Lower", "lower_str", 0, TY_STRING},
    {"Trim", "trim_str", 0, TY_STRING},
    {"Slice", "str_slice", 2, TY_STRING},
    {"At", "str_at", 1, TY_STRING},
    {"Repeat", "repeat_str", 1, TY_STRING},
    {NULL, NULL, 0, TY_UNKNOWN},
};

static const HxIntrin *hx_find_intrin(HxTy *recv, const char *name) {
    if (!recv || recv->kind != TY_STRING) return NULL;
    for (int i = 0; hx_string_intrins[i].name; i++)
        if (!hx_ascii_casecmp(hx_string_intrins[i].name, name)) return &hx_string_intrins[i];
    return NULL;
}

static HxExpr *hx_expr_check(HxChecker *c, HxExpr *e);

static void hx_str_check(HxChecker *c, HxExpr *e) {
    const char *raw = e->str.raw;
    int len = e->str.len;
    e->ty = hx_ty_builtin(c->arena, TY_STRING);
    if (!e->has_holes) return;
    HX_VEC(segs, HxStrSeg);
    int lit_start = 0;
    for (int i = 0; i < len; i++) {
        if (raw[i] == '{' && i + 1 < len && raw[i + 1] == '{') {
            i++;
            continue;
        }
        if (raw[i] != '{') continue;
        int depth = 1, j = i + 1;
        while (j < len && depth) {
            if (raw[j] == '{') depth++;
            if (raw[j] == '}') depth--;
            j++;
        }
        if (j >= len && depth) {
            hx_error(c->diags, e->span, "E0305", "interpolación sin cerrar en la cadena");
            return;
        }
        if (i > lit_start)
            HX_VEC_PUSH(segs, ((HxStrSeg){raw + lit_start, i - lit_start, NULL}));
        char *sub = hx_arena_strndup(c->arena, raw + i + 1, (size_t)(j - i - 2));
        HxExpr *hole = hx_parse_subexpr(c->unit, sub, "interpolación");
        HxChecker sub_c = *c;
        hole = hx_expr_check(&sub_c, hole);
        if (!hole->ty || hole->ty->kind == TY_UNKNOWN ||
            (hole->ty->kind == TY_NAMED)) {
            hx_error(c->diags, e->span, "E0305",
                     "una interpolación debe ser un escalar imprimible");
        }
        HX_VEC_PUSH(segs, ((HxStrSeg){NULL, 0, hole}));
        e->has_holes = 1;
        i = j - 1;
        lit_start = j;
    }
    if (lit_start < len) HX_VEC_PUSH(segs, ((HxStrSeg){raw + lit_start, len - lit_start, NULL}));
    if (segs.len == 0) HX_VEC_PUSH(segs, ((HxStrSeg){"", 0, NULL}));
    e->segs = segs.data;
    e->n_segs = segs.len;
}

static HxExpr *hx_path_check(HxChecker *c, HxExpr *e) {
    HxPathPart *parts = e->path.parts.data;
    int n = e->path.parts.len;
    for (int split = n; split >= 1; split--) {
        char *name = hx_arena_strdup(c->arena, "");
        for (int i = 0; i < split; i++) {
            if (i) name = hx_arena_sprintf(c->arena, "%s.%s", name, hx_sym_str(parts[i].name));
            else name = hx_arena_strdup(c->arena, hx_sym_str(parts[i].name));
        }
        HxSym sym = hx_intern_fold_ascii(c->intern, name, strlen(name));
        HxSymEntry *se = hx_lookup(c, sym);
        if (se) {
            HxTy *t = se->ty;
            if (se->kind == SK_MODULE) {
                t = hx_ty_builtin(c->arena, TY_VOID);
                for (int i = split; i < n; i++) {
                    const char *mn = hx_sym_str(parts[i].name);
                    HxSym member = hx_intern_fold_ascii(c->intern, mn, strlen(mn));
                    HxFunc *f = hx_find_func(c, member);
                    if (!f) {
                        HxConst *kc = hx_find_const(c, member);
                        if (kc) {
                            e->path.parts.len = 0;
                            HX_VEC_PUSH(e->path.parts,
                                        ((HxPathPart){hx_intern_cstr(c->intern, hx_sym_str(
                                                              kc->name)),
                                                      parts[i].span}));
                            e->prefix_len = 0;
                            t = kc->ty;
                            break;
                        }
                    }
                    if (!f) {
                        hx_diag_note(c->diags, parts[i].span, "E0302",
                                     hx_arena_sprintf(c->arena,
                                                      "'%s' no está exportado por el módulo '%s'",
                                                      hx_sym_str(parts[i].name),
                                                      hx_sym_str(parts[0].name)),
                                     "sólo las funciones EXPORT son visibles al importar",
                                     NULL);
                        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                        return e;
                    }
                    t = f->ret;
                }
            } else if (split < n) {
                const char *member = hx_sym_str(parts[split].name);
                if (t->decl) {
                    for (int fi = 0; fi < t->decl->fields.len; fi++) {
                        HxField *fld = &t->decl->fields.data[fi];
                        if (hx_ascii_casecmp(hx_sym_str(fld->name), member)) continue;
                        e->prefix_len = split + 1;
                        e->ty = fld->ty;
                        return e;
                    }
                }
                const HxIntrin *in = hx_find_intrin(t, member);
                if (in) {
                    e->prefix_len = split;
                    e->method = parts[split].name;
                    e->is_intrin = 1;
                    e->ty = hx_ty_builtin(c->arena, in->ret);
                    return e;
                }
                hx_diag_note(c->diags, parts[split].span, "E0303",
                             hx_arena_sprintf(c->arena, "'%s' no tiene miembros",
                                              hx_sym_str(parts[0].name)),
                             t->kind == TY_STRING
                                 ? hx_arena_sprintf(c->arena,
                                                    "STRING tiene: %s",
                                                    "Len IsEmpty Upper Lower Trim Slice At Repeat")
                                 : NULL,
                             NULL);
            }
            e->ty = t;
            return e;
        }
    }
    HxSpan sp = parts[0].span;
    hx_diag_note(c->diags, sp, "E0304",
                 hx_arena_sprintf(c->arena, "no se encontró '%s'", hx_sym_str(parts[0].name)),
                 "las variables deben declararse con DIM antes de usarse",
                 hx_arena_sprintf(c->arena, "DIM %s AS INT", hx_sym_str(parts[0].name)));
    e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
    return e;
}

static HxExpr *hx_call_check(HxChecker *c, HxExpr *e) {
    HxExpr *raw_callee = e->call.callee;
    if (raw_callee->kind == EX_PATH && raw_callee->path.parts.len == 1) {
        const char *cn = hx_sym_str(raw_callee->path.parts.data[0].name);
        int is_ok = !hx_ascii_casecmp(cn, "Ok");
        int is_err = !hx_ascii_casecmp(cn, "Err");
        if (is_ok || is_err) {
            if (e->call.args.len != 1)
                hx_error(c->diags, e->span, "E0306",
                         hx_arena_sprintf(c->arena, "'%s' espera exactamente 1 argumento",
                                          is_ok ? "Ok" : "Err"));
            HxExpr *arg = e->call.args.len ? e->call.args.data[0].value : NULL;
            if (arg) arg = hx_expr_check(c, arg);
            e->is_ok_ctor = is_ok;
            e->is_err_ctor = is_err;
            e->payload_ty = arg ? arg->ty : NULL;
            e->ty = (HxTy *)hx_arena_calloc(c->arena, sizeof(HxTy));
            e->ty->kind = TY_NAMED;
            e->ty->name = hx_intern_cstr(c->intern, "Result");
            e->ty->elem = arg ? arg->ty : NULL;
            e->ty->inner = hx_ty_builtin(c->arena, TY_STRING);
            if (raw_callee->prefix_len) raw_callee->prefix_len = 0;
            return e;
        }
    }
    HxExpr *callee = hx_expr_check(c, raw_callee);
    HxFunc *f = NULL;
    if (callee->kind == EX_PATH) {
        HxPathPart *parts = callee->path.parts.data;
        int n = callee->path.parts.len;
        if (n >= 2) {
            HxSymEntry *se = hx_lookup(c, parts[0].name);
            HxSym last = parts[n - 1].name;
            const char *lname = hx_sym_str(last);
            HxSym lfolded = hx_intern_fold_ascii(c->intern, lname, strlen(lname));
            if (se && se->kind == SK_MODULE) {
                f = hx_find_func(c, lfolded);
                if (!f || (!f->is_export && hx_ascii_casecmp(hx_sym_str(parts[0].name), hx_sym_str(c->mod->name)))) {
                    hx_diag_note(c->diags, parts[n - 1].span, "E0302",
                                 hx_arena_sprintf(c->arena,
                                                  "'%s' no está exportado por el módulo '%s'",
                                                  hx_sym_str(last), hx_sym_str(parts[0].name)),
                                 "sólo las funciones EXPORT son visibles al importar", NULL);
                }
            }
        }
        if (!f) {
            const char *fname = hx_sym_str(parts[n - 1].name);
            HxSym folded = hx_intern_fold_ascii(c->intern, fname, strlen(fname));
            HxSymEntry *se = hx_lookup(c, folded);
            if (se && se->kind == SK_FUNC) f = hx_find_func(c, folded);
        }
    }
    if (!f && callee->kind == EX_STR && callee->method) {
        HxExpr *recv = hx_expr_check(c, callee);
        const char *member = hx_sym_str(callee->method);
        const HxIntrin *in = hx_find_intrin(recv->ty, member);
        if (in) {
            e->is_intrin = 1;
            e->method = callee->method;
            e->recv = recv;
            if (e->call.args.len != in->nargs)
                hx_error(c->diags, e->span, "E0306",
                         hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s), recibió %d",
                                          member, in->nargs, e->call.args.len));
            for (int i = 0; i < e->call.args.len; i++) {
                e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
                if (in->nargs && (in->name[0] == 'S' || in->name[0] == 'A'))
                    hx_coerce(c, e->call.args.data[i].value->ty,
                              hx_ty_builtin(c->arena, TY_I64), e->call.args.data[i].span, NULL);
            }
            e->ty = hx_ty_builtin(c->arena, in->ret);
            return e;
        }
    }
    if (!f && callee->kind == EX_PATH) {
        HxPathPart *parts = callee->path.parts.data;
        int n = callee->path.parts.len;
        if (n >= 2) {
            HxExpr *recv_e = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
            recv_e->kind = EX_PATH;
            recv_e->span = callee->span;
            for (int k = 0; k < n - 1; k++) HX_VEC_PUSH(recv_e->path.parts, parts[k]);
            HxExpr *recv = hx_path_check(c, recv_e);
            const char *member = hx_sym_str(parts[n - 1].name);
            const HxIntrin *in = hx_find_intrin(recv->ty, member);
            if (in) {
                e->is_intrin = 1;
                e->method = parts[n - 1].name;
                e->recv = recv;
                if (in->nargs == 0) e->is_intrin = 2;
                int given = e->call.args.len;
                if (given != in->nargs)
                    hx_error(c->diags, e->span, "E0306",
                             hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s), recibió %d",
                                              member, in->nargs, given));
                for (int i = 0; i < e->call.args.len; i++) {
                    e->call.args.data[i].value =
                        hx_expr_check(c, e->call.args.data[i].value);
                    if (in->nargs && (in->name[0] == 'S' || in->name[0] == 'A'))
                        hx_coerce(c, e->call.args.data[i].value->ty,
                                  hx_ty_builtin(c->arena, TY_I64),
                                  e->call.args.data[i].span, NULL);
                }
                e->ty = hx_ty_builtin(c->arena, in->ret);
                return e;
            }
        }
    }
    if (!f) {
        hx_diag_note(c->diags, e->span, "E0305",
                     hx_arena_sprintf(c->arena, "no se encontró la función '%s'",
                                      hx_sym_str(callee->kind == EX_PATH &&
                                                     callee->path.parts.len
                                                 ? callee->path.parts.data[0].name
                                                 : hx_intern_cstr(c->intern, "?"))),
                     NULL, NULL);
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return e;
    }
    int required = 0;
    for (int i = 0; i < f->params.len; i++)
        if (!f->params.data[i].default_value) required++;
    int given = e->call.args.len;
    if (given < required || given > f->params.len) {
        hx_diag_note(c->diags, e->span, "E0306",
                     hx_arena_sprintf(c->arena, "'%s' espera %d..%d argumentos, recibió %d",
                                      hx_sym_str(f->name), required, f->params.len, given),
                     required == f->params.len
                         ? hx_arena_sprintf(c->arena, "firma: FUNCTION %s(", hx_sym_str(f->name))
                         : NULL,
                     NULL);
    }
    for (int i = 0; i < e->call.args.len; i++) {
        HxArg *arg = &e->call.args.data[i];
        arg->value = hx_expr_check(c, arg->value);
        HxParam *p = (i < f->params.len) ? &f->params.data[i] : NULL;
        if (p && p->ty) hx_coerce(c, arg->value->ty, p->ty, arg->span, hx_sym_str(f->name));
    }
    e->ty = f->ret ? f->ret : hx_ty_builtin(c->arena, TY_VOID);
    return e;
}

static HxExpr *hx_bin_check(HxChecker *c, HxExpr *e) {
    e->bin.lhs = hx_expr_check(c, e->bin.lhs);
    e->bin.rhs = hx_expr_check(c, e->bin.rhs);
    HxTy *l = e->bin.lhs->ty, *r = e->bin.rhs->ty;
    HxBinOp op = e->bin.op;
    int is_cmp = op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE || op == OP_GT ||
                 op == OP_GE;
    int is_logic = op == OP_AND || op == OP_OR || op == OP_XOR;
    int is_arith = op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_MOD ||
                   op == OP_ADDW || op == OP_SUBW || op == OP_ADDS || op == OP_SUBS ||
                   op == OP_MULS;
    if (is_logic) {
        if ((l && l->kind != TY_BOOL) || (r && r->kind != TY_BOOL))
            hx_error(c->diags, e->span, "E0307", "AND, OR y XOR requieren operandos BOOL");
        e->ty = hx_ty_builtin(c->arena, TY_BOOL);
        return e;
    }
    if (op == OP_CONCAT) {
        if ((l && l->kind != TY_STRING) || (r && r->kind != TY_STRING))
            hx_error(c->diags, e->span, "E0307", "'++' requiere dos operandos STRING");
        e->ty = hx_ty_builtin(c->arena, TY_STRING);
        return e;
    }
    if (is_cmp) {
        if (l && r && hx_ty_rank(l) && hx_ty_rank(r)) hx_coerce(c, r, l, e->span, NULL);
        e->ty = hx_ty_builtin(c->arena, TY_BOOL);
        return e;
    }
    if (is_arith) {
        if ((l && !hx_ty_is_numeric(l)) || (r && !hx_ty_is_numeric(r))) {
            hx_diag_note(c->diags, e->span, "E0307",
                         hx_arena_sprintf(c->arena, "'%s' no admite operandos %s y %s",
                                          hx_binop_symbol(op), hx_ty_name(l), hx_ty_name(r)),
                         NULL, NULL);
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            return e;
        }
        HxTy *common = hx_ty_rank(l) >= hx_ty_rank(r) ? l : r;
        e->ty = common ? common : hx_int_ty(c);
        return e;
    }
    e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
    return e;
}

static int hx_has_nested_try(HxExpr *e) {
    if (!e) return 0;
    if (e->kind == EX_TRY) return 1;
    switch (e->kind) {
        case EX_CALL: {
            if (hx_has_nested_try(e->call.callee)) return 1;
            for (int i = 0; i < e->call.args.len; i++)
                if (hx_has_nested_try(e->call.args.data[i].value)) return 1;
            break;
        }
        case EX_BIN:
            return hx_has_nested_try(e->bin.lhs) || hx_has_nested_try(e->bin.rhs);
        case EX_UN: return hx_has_nested_try(e->un.operand);
        case EX_INDEX:
            return hx_has_nested_try(e->index.base) || hx_has_nested_try(e->index.start);
        case EX_TRY: return 1;
        default: break;
    }
    return 0;
}

static HxExpr *hx_expr_propagate(HxChecker *c, HxExpr *e, HxSpan site) {
    if (!e) return e;
    if (e->kind == EX_TRY) {
        e->try.inner = hx_expr_check(c, e->try.inner);
        if (!hx_ty_is_result(e->try.inner->ty))
            hx_error(c->diags, e->span, "E0404",
                     "'?' sólo se aplica a un valor de tipo Result");
        e->propagate = 1;
        e->ty = e->try.inner->ty ? e->try.inner->ty->elem : NULL;
        return e;
    }
    HxExpr *r = hx_expr_check(c, e);
    if (hx_has_nested_try(r))
        hx_error(c->diags, site, "E0403",
                 "'?' sólo puede aparecer como valor completo de una asignación, "
                 "DIM o RETURN");
    return r;
}

static HxExpr *hx_expr_check(HxChecker *c, HxExpr *e) {
    if (!e) return e;
    switch (e->kind) {
        case EX_INT:
            e->ty = hx_int_ty(c);
            break;
        case EX_FLOAT:
            e->ty = hx_ty_builtin(c->arena, TY_FLOAT);
            break;
        case EX_DURATION:
            e->ty = hx_ty_builtin(c->arena, TY_DURATION);
            break;
        case EX_BOOL:
            e->ty = hx_ty_builtin(c->arena, TY_BOOL);
            break;
        case EX_STR:
            hx_str_check(c, e);
            break;
        case EX_NIL:
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
        case EX_PATH:
            return hx_path_check(c, e);
        case EX_CALL:
            return hx_call_check(c, e);
        case EX_BIN:
            return hx_bin_check(c, e);
        case EX_UN: {
            e->un.operand = hx_expr_check(c, e->un.operand);
            if (e->un.op == UOP_NOT) {
                if (e->un.operand->ty && e->un.operand->ty->kind != TY_BOOL)
                    hx_error(c->diags, e->un.operand->span, "E0307",
                             "NOT requiere un operando BOOL");
                e->ty = hx_ty_builtin(c->arena, TY_BOOL);
            } else {
                if (e->un.operand->ty && !hx_ty_is_numeric(e->un.operand->ty))
                    hx_error(c->diags, e->un.operand->span, "E0307",
                             "no se puede negar un valor de tipo no numérico");
                e->ty = e->un.operand->ty;
            }
            break;
        }
        case EX_INDEX:
            e->index.base = hx_expr_check(c, e->index.base);
            if (e->index.start) e->index.start = hx_expr_check(c, e->index.start);
            if (e->index.end) e->index.end = hx_expr_check(c, e->index.end);
            e->ty = e->index.base->ty && e->index.base->ty->kind == TY_ARRAY
                        ? e->index.base->ty->elem
                        : hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
        case EX_TRY:
            e->try.inner = hx_expr_check(c, e->try.inner);
            e->ty = e->try.inner->ty;
            break;
        case EX_VEC:
            hx_error(c->diags, e->span, "E0402",
                     "los tipos vectoriales llegan en el objetivo M8");
            for (int i = 0; i < e->vec.len; i++)
                e->vec.items[i] = hx_expr_check(c, e->vec.items[i]);
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
        default:
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
    }
    return e;
}

static HxTy *hx_resolve_type(HxChecker *c, HxTy *t, HxSpan sp, int report) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_NAMED: {
            if (!hx_ascii_casecmp(hx_sym_str(t->name), "Result")) {
                t->elem = hx_resolve_type(c, t->elem, sp, report);
                t->inner = hx_resolve_type(c, t->inner, sp, report);
                return t;
            }
            HxTy *b = hx_ty_lookup_builtin(c->arena, t->name);
            if (b) {
                t->kind = b->kind;
                return t;
            }
            HxTypeDecl *d = hx_find_type(c, t->name);
            if (d) {
                t->decl = d;
                return t;
            }
            if (report)
                hx_diag_note(c->diags, sp, "E0308",
                             hx_arena_sprintf(c->arena, "tipo desconocido '%s'",
                                              hx_sym_str(t->name)),
                             "los tipos incorporados son BOOL INT I64 FLOAT STRING DURATION",
                             NULL);
            t->kind = TY_UNKNOWN;
            return t;
        }
        case TY_ARRAY:
            t->elem = hx_resolve_type(c, t->elem, sp, report);
            return t;
        case TY_REF:
        case TY_PTR:
            t->inner = hx_resolve_type(c, t->inner, sp, report);
            return t;
        default: return t;
    }
}

static void hx_decl_locals(HxChecker *c, HxStmtVec *body) {
    for (int i = 0; i < body->len; i++) {
        HxStmt *s = &body->data[i];
        if (s->kind == ST_DIM) {
            HxTy *t = hx_resolve_type(c, s->dim.ty, s->dim.name_span, 1);
            if (!t && s->dim.init) {
                s->dim.init = hx_expr_check(c, s->dim.init);
                t = s->dim.init->ty;
            }
            s->dim.ty = t;
            HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(s->dim.name),
                                                 strlen(hx_sym_str(s->dim.name)));
            hx_define(c, folded, t ? t : hx_ty_builtin(c->arena, TY_INT), SK_VAR,
                      s->dim.name_span);
        }
    }
}

static void hx_check_body(HxChecker *c, HxStmtVec *body);

static void hx_define_pattern_binding(HxChecker *c, HxSym name, HxTy *ty, HxSpan sp) {
    HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(name), strlen(hx_sym_str(name)));
    hx_define(c, folded, ty, SK_VAR, sp);
}

static void hx_check_pattern(HxChecker *c, HxPattern *pat, HxTy *subj) {
    if (!pat) return;
    switch (pat->kind) {
        case PAT_BIND:
            hx_define_pattern_binding(c, pat->name, subj, pat->span);
            break;
        case PAT_LITERAL:
            pat->lit = hx_expr_check(c, pat->lit);
            if (pat->lit->ty && subj && !hx_ty_equal(pat->lit->ty, subj))
                hx_error(c->diags, pat->lit->span, "E0406",
                         hx_arena_sprintf(c->arena, "el patrón no puede ser %s aquí",
                                          hx_ty_name(subj)));
            break;
        case PAT_RANGE:
            pat->lo = hx_expr_check(c, pat->lo);
            pat->hi = hx_expr_check(c, pat->hi);
            break;
        case PAT_CONSTRUCTOR: {
            int is_or = !hx_ascii_casecmp(hx_sym_str(pat->ctor), "__or__");
            if (is_or) {
                for (int i = 0; i < pat->args.len; i++) hx_check_pattern(c, &pat->args.data[i], subj);
                return;
            }
            int is_ok = !hx_ascii_casecmp(hx_sym_str(pat->ctor), "Ok");
            int is_err = !hx_ascii_casecmp(hx_sym_str(pat->ctor), "Err");
            if ((is_ok || is_err) && hx_ty_is_result(subj)) {
                HxTy *pt = is_ok ? subj->elem : subj->inner;
                if (pat->args.len == 1)
                    hx_check_pattern(c, &pat->args.data[0], pt);
                return;
            }
            for (int i = 0; i < pat->args.len; i++) hx_check_pattern(c, &pat->args.data[i], subj);
            return;
        }
        case PAT_WILDCARD: break;
    }
}

static void hx_check_stmt(HxChecker *c, HxStmt *s) {
    switch (s->kind) {
        case ST_EXPR:
            s->expr = hx_expr_check(c, s->expr);
            if (s->expr->ty && s->expr->ty->kind == TY_UNKNOWN) {
                /* ya reportado */
            }
            break;
        case ST_ASSIGN: {
            s->assign.target = hx_expr_check(c, s->assign.target);
            s->assign.value = hx_expr_propagate(c, s->assign.value, s->assign.value->span);
            if (s->assign.compound && s->assign.op == OP_ADD && s->assign.target->ty &&
                s->assign.target->ty->kind == TY_STRING)
                hx_error(c->diags, s->span, "E0309",
                         "'+' no concatena cadenas; usa '++' para STRING");
            if (s->assign.target->ty && s->assign.value->ty)
                hx_coerce(c, s->assign.value->ty, s->assign.target->ty, s->assign.value->span,
                          NULL);
            break;
        }
        case ST_DIM: {
            HxTy *t = hx_resolve_type(c, s->dim.ty, s->dim.name_span, 1);
            if (s->dim.init) {
                s->dim.init = hx_expr_propagate(c, s->dim.init, s->dim.init->span);
                if (s->dim.init->ty && s->dim.init->ty->kind == TY_ARRAY && (!t || t->kind == TY_UNKNOWN))
                    t = s->dim.init->ty;
                if (!t) t = s->dim.init->ty;
                else hx_coerce(c, s->dim.init->ty, t, s->dim.init->span, NULL);
            }
            if (!t) t = hx_ty_builtin(c->arena, TY_INT);
            s->dim.ty = t;
            HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(s->dim.name),
                                                 strlen(hx_sym_str(s->dim.name)));
            hx_define(c, folded, t, SK_VAR, s->dim.name_span);
            break;
        }
        case ST_CONST: {
            if (!s->konst.value)
                hx_error(c->diags, s->span, "E0310", "CONST requiere un valor inicial");
            else {
                s->konst.value = hx_expr_check(c, s->konst.value);
                if (s->konst.value->kind != EX_INT && s->konst.value->kind != EX_FLOAT &&
                    s->konst.value->kind != EX_STR && s->konst.value->kind != EX_BOOL &&
                    s->konst.value->kind != EX_DURATION)
                    hx_error(c->diags, s->konst.value->span, "E0310",
                             "CONST sólo admite literales y operaciones constantes");
            }
            s->konst.ty = s->konst.value ? s->konst.value->ty : NULL;
            HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(s->konst.name),
                                                 strlen(hx_sym_str(s->konst.name)));
            hx_define(c, folded, s->konst.ty, SK_CONST, s->konst.name_span);
            break;
        }
        case ST_PRINT:
            for (int i = 0; i < s->print.items.len; i++) {
                HxPrintItem *it = &s->print.items.data[i];
                if (it->expr) {
                    it->expr = hx_expr_propagate(c, it->expr, it->expr->span);
                    HxTy *t = it->expr->ty;
                    if (t && t->kind != TY_STRING && t->kind != TY_INT && t->kind != TY_I64 &&
                        t->kind != TY_FLOAT && t->kind != TY_BOOL && t->kind != TY_DURATION &&
                        t->kind != TY_UNKNOWN)
                        hx_error(c->diags, it->expr->span, "E0311",
                                 "PRINT no admite valores de ese tipo");
                }
            }
            break;
        case ST_IF: {
            hx_scope_push(c);
            s->if_.cond = hx_expr_check(c, s->if_.cond);
            if (s->if_.cond->ty && s->if_.cond->ty->kind != TY_BOOL)
                hx_error(c->diags, s->if_.cond->span, "E0312",
                         "la condición de IF debe ser BOOL");
            hx_check_body(c, &s->if_.then);
            hx_scope_pop(c);
            for (int i = 0; i < s->if_.n_elifs; i++) {
                hx_scope_push(c);
                s->if_.elifs[i].cond = hx_expr_check(c, s->if_.elifs[i].cond);
                hx_check_body(c, &s->if_.elifs[i].then);
                hx_scope_pop(c);
            }
            if (s->if_.has_else) {
                hx_scope_push(c);
                hx_check_body(c, &s->if_.else_);
                hx_scope_pop(c);
            }
            break;
        }
        case ST_WHILE:
            hx_scope_push(c);
            s->while_.cond = hx_expr_check(c, s->while_.cond);
            if (s->while_.cond->ty && s->while_.cond->ty->kind != TY_BOOL)
                hx_error(c->diags, s->while_.cond->span, "E0312",
                         "la condición de WHILE debe ser BOOL");
            c->loop_depth++;
            hx_check_body(c, &s->while_.body);
            c->loop_depth--;
            hx_scope_pop(c);
            break;
        case ST_FOR:
            hx_scope_push(c);
            s->for_.start = hx_expr_check(c, s->for_.start);
            s->for_.end = hx_expr_check(c, s->for_.end);
            if (s->for_.step) s->for_.step = hx_expr_check(c, s->for_.step);
            HxTy *vt = hx_ty_builtin(c->arena, TY_INT);
            if (s->for_.start && s->for_.start->ty) vt = s->for_.start->ty;
            HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(s->for_.var),
                                                 strlen(hx_sym_str(s->for_.var)));
            hx_define(c, folded, vt, SK_VAR, s->for_.var_span);
            c->loop_depth++;
            hx_check_body(c, &s->for_.body);
            c->loop_depth--;
            hx_scope_pop(c);
            break;
        case ST_RETURN:
            if (s->ret.value) {
                s->ret.value = hx_expr_propagate(c, s->ret.value, s->ret.value->span);
                if (c->ret_ty) hx_coerce(c, s->ret.value->ty, c->ret_ty, s->ret.value->span, NULL);
            }
            break;
        case ST_BREAK:
        case ST_CONTINUE:
            if (!c->loop_depth)
                hx_error(c->diags, s->span, "E0313",
                         "BREAK y CONTINUE sólo pueden aparecer dentro de un bucle");
            break;
        case ST_EXIT:
            if (s->exit_.code) {
                s->exit_.code = hx_expr_check(c, s->exit_.code);
                if (s->exit_.code->ty && s->exit_.code->ty->kind != TY_INT)
                    hx_error(c->diags, s->exit_.code->span, "E0314",
                             "el código de salida debe ser INT");
            }
            break;
        case ST_BLOCK:
            hx_scope_push(c);
            hx_check_body(c, &s->block.stmts);
            hx_scope_pop(c);
            break;
        case ST_ARENA:
            hx_scope_push(c);
            hx_check_body(c, &s->arena.body);
            hx_scope_pop(c);
            break;
        case ST_DEFER:
            hx_scope_push(c);
            hx_check_body(c, &s->inner);
            hx_scope_pop(c);
            break;
        case ST_MATCH: {
            hx_scope_push(c);
            s->match.subject = hx_expr_check(c, s->match.subject);
            if (s->match.subject_name) {
                HxSym folded = hx_intern_fold_ascii(c->intern,
                                                     hx_sym_str(s->match.subject_name),
                                                     strlen(hx_sym_str(s->match.subject_name)));
                hx_define(c, folded, s->match.subject->ty, SK_VAR, s->span);
            }
            HxTy *subj = s->match.subject->ty;
            for (int i = 0; i < s->match.cases.len; i++) {
                hx_scope_push(c);
                hx_check_pattern(c, s->match.cases.data[i].pattern, subj);
                if (s->match.cases.data[i].guard)
                    s->match.cases.data[i].guard =
                        hx_expr_check(c, s->match.cases.data[i].guard);
                hx_check_body(c, &s->match.cases.data[i].body);
                hx_scope_pop(c);
            }
            if (s->match.has_else) {
                hx_scope_push(c);
                hx_check_body(c, &s->match.else_body);
                hx_scope_pop(c);
            } else {
                int covered_ok = 0, covered_err = 0;
                for (int i = 0; i < s->match.cases.len && hx_ty_is_result(subj); i++) {
                    HxPattern *pt = s->match.cases.data[i].pattern;
                    if (pt->kind != PAT_CONSTRUCTOR) {
                        if (pt->kind == PAT_WILDCARD || pt->kind == PAT_BIND) {
                            covered_ok = covered_err = 1;
                        }
                        continue;
                    }
                    if (!hx_ascii_casecmp(hx_sym_str(pt->ctor), "Ok")) covered_ok = 1;
                    if (!hx_ascii_casecmp(hx_sym_str(pt->ctor), "Err")) covered_err = 1;
                }
                if (!(hx_ty_is_result(subj) && covered_ok && covered_err))
                    hx_error(c->diags, s->span, "E0405",
                             "MATCH exige CASE ELSE o cubrir todos los casos de Ok y Err");
            }
            hx_scope_pop(c);
            break;
        }
        case ST_NOP: break;
    }
}

static void hx_check_body(HxChecker *c, HxStmtVec *body) {
    for (int i = 0; i < body->len; i++) {
        hx_check_stmt(c, &body->data[i]);
        if (c->diags->errors > c->diags->max_errors) break;
    }
}

static int hx_is_self_call(HxExpr *e, HxSym fname, int *argc) {
    if (!e || e->kind != EX_CALL) return 0;
    HxExpr *callee = e->call.callee;
    if (!callee || callee->kind != EX_PATH) return 0;
    int n = callee->path.parts.len;
    if (!hx_ascii_casecmp(hx_sym_str(callee->path.parts.data[n - 1].name), hx_sym_str(fname)))
        return 1;
    return 0;
}

static void hx_analyze_tail(HxFunc *f) {
    f->is_tail_loop = 0;
    if (f->body.len < 1) return;
    for (int i = 0; i < f->params.len; i++)
        if (f->params.data[i].is_ref) return;
    HxStmt *last = &f->body.data[f->body.len - 1];
    if (last->kind != ST_RETURN || !last->ret.value) return;
    HxExpr *call = last->ret.value;
    if (call->kind != EX_CALL || !call->call.callee || call->call.callee->kind != EX_PATH)
        return;
    HxExpr *callee = call->call.callee;
    int n = callee->path.parts.len;
    if (hx_ascii_casecmp(hx_sym_str(callee->path.parts.data[n - 1].name), hx_sym_str(f->name)))
        return;
    if (call->call.args.len != f->params.len) return;
    f->is_tail_loop = 1;
}

static void hx_check_func(HxChecker *c, HxFunc *f) {
    hx_scope_push(c);
    HxFunc *prev = c->cur_func;
    HxTy *prev_ret = c->ret_ty;
    int prev_loop = c->loop_depth;
    c->cur_func = f;
    c->loop_depth = 0;
    for (int i = 0; i < f->params.len; i++) {
        HxParam *prm = &f->params.data[i];
        HxTy *t = prm->ty;
        if (!t && prm->default_value) {
            prm->default_value = hx_expr_check(c, prm->default_value);
            t = prm->default_value->ty;
        }
        prm->ty = t ? t : hx_ty_builtin(c->arena, TY_INT);
        HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(prm->name),
                                             strlen(hx_sym_str(prm->name)));
        hx_define(c, folded, prm->ty, SK_VAR, prm->span);
    }
    c->ret_ty = f->ret;
    if (c->ret_ty && c->ret_ty->kind == TY_VOID) c->ret_ty = NULL;
    hx_check_body(c, &f->body);
    hx_analyze_tail(f);
    c->cur_func = prev;
    c->ret_ty = prev_ret;
    c->loop_depth = prev_loop;
    hx_scope_pop(c);
}

static void hx_define_module_scope(HxChecker *c, HxModule *mod) {
    for (int mi = 0; mi < mod->imports.len; mi++) {
        HxImport *im = &mod->imports.data[mi];
        const char *ip = hx_sym_str(im->path);
        const char *dot = strrchr(ip, '.');
        const char *base = dot ? dot + 1 : ip;
        HxSym as = im->alias ? im->alias : hx_intern_cstr(c->intern, base);
        hx_define(c, hx_intern_fold_ascii(c->intern, hx_sym_str(as), strlen(hx_sym_str(as))),
                  hx_ty_builtin(c->arena, TY_VOID), SK_MODULE, im->span);
    }
}

static void hx_collect_names(HxChecker *c, HxModule *mod) {
    HxArena *a = c->arena;
    for (int i = 0; i < mod->types.len; i++) {
        HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(mod->types.data[i].name),
                                             strlen(hx_sym_str(mod->types.data[i].name)));
        hx_define(c, folded, NULL, SK_TYPE, mod->types.data[i].span);
    }
    for (int i = 0; i < mod->funcs.len; i++) {
        HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(mod->funcs.data[i].name),
                                             strlen(hx_sym_str(mod->funcs.data[i].name)));
        hx_define(c, folded, mod->funcs.data[i].ret, SK_FUNC, mod->funcs.data[i].span);
    }
    for (int i = 0; i < mod->consts.len; i++) {
        HxSym folded = hx_intern_fold_ascii(c->intern, hx_sym_str(mod->consts.data[i].name),
                                             strlen(hx_sym_str(mod->consts.data[i].name)));
        hx_define(c, folded, mod->consts.data[i].ty, SK_CONST, mod->consts.data[i].span);
    }
    (void)a;
}

static void hx_resolve_signature(HxChecker *c, HxFunc *fn) {
    for (int i = 0; i < fn->params.len; i++) {
        HxParam *prm = &fn->params.data[i];
        prm->ty = hx_resolve_type(c, prm->ty, prm->span, 1);
        if (!prm->ty && prm->default_value) {
            prm->default_value = hx_expr_check(c, prm->default_value);
            prm->ty = prm->default_value->ty;
        }
        if (!prm->ty) prm->ty = hx_ty_builtin(c->arena, TY_INT);
    }
    fn->ret = hx_resolve_type(c, fn->ret, fn->span, 1);
    if (!fn->ret) fn->ret = hx_ty_builtin(c->arena, TY_VOID);
}

int hx_check_unit(HxUnit *unit) {
    int before = unit->diags->errors;
    HxChecker c;
    memset(&c, 0, sizeof(c));
    c.unit = unit;
    c.arena = unit->arena;
    c.intern = unit->intern;
    c.diags = unit->diags;

    for (int m = 0; m < unit->modules.len; m++) {
        HxModule *mod = &unit->modules.data[m];
        unit->diags->ctx_file = mod->file;
        unit->diags->ctx_src = mod->src;
        hx_scope_push(&c);
        hx_define_module_scope(&c, mod);
        hx_collect_names(&c, mod);
        for (int t = 0; t < mod->types.len; t++) {
            for (int i = 0; i < mod->types.data[t].fields.len; i++) {
                HxField *fld = &mod->types.data[t].fields.data[i];
                fld->ty = hx_resolve_type(&c, fld->ty, fld->span, 1);
            }
        }
        for (int i = 0; i < mod->consts.len; i++) {
            HxConst *kc = &mod->consts.data[i];
            kc->ty = hx_resolve_type(&c, kc->ty, kc->span, 1);
            if (kc->value) {
                kc->value = hx_expr_check(&c, kc->value);
                if (!kc->ty) kc->ty = kc->value->ty;
            }
        }
        for (int f = 0; f < mod->funcs.len; f++) hx_resolve_signature(&c, &mod->funcs.data[f]);
        hx_scope_pop(&c);
    }

    for (int m = 0; m < unit->modules.len; m++) {
        HxModule *mod = &unit->modules.data[m];
        unit->diags->ctx_file = mod->file;
        unit->diags->ctx_src = mod->src;
        c.mod = mod;
        hx_scope_push(&c);
        hx_define_module_scope(&c, mod);
        hx_collect_names(&c, mod);
        for (int f = 0; f < mod->funcs.len; f++) hx_check_func(&c, &mod->funcs.data[f]);
        hx_scope_push(&c);
        hx_decl_locals(&c, &mod->top);
        hx_check_body(&c, &mod->top);
        hx_scope_pop(&c);
        hx_scope_pop(&c);
    }
    return unit->diags->errors == before;
}
