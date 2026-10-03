#include "hx/mono.h"

#include <string.h>

/* Sustitucion de parametros de tipo y clon del AST.
   Cada instancia de una funcion generica recibe una copia profunda de su
   cuerpo: compartir los nodos haria que la comprobacion de una instancia
   escribiera sus tipos sobre los de la siguiente. */

static int hx_tparam_index(const HxSym *tps, int n, HxSym name) {
    for (int i = 0; i < n; i++)
        if (tps[i] == name) return i;
    return -1;
}

int hx_ty_has_tparam(const HxTy *t, const HxSym *tps, int n) {
    if (!t) return 0;
    if (t->kind == TY_NAMED && hx_tparam_index(tps, n, t->name) >= 0) return 1;
    return hx_ty_has_tparam(t->elem, tps, n) || hx_ty_has_tparam(t->inner, tps, n);
}

HxTy *hx_ty_subst(HxArena *a, const HxTy *t, const HxSym *tps, HxTy **subs, int n) {
    if (!t) return NULL;
    if (t->kind == TY_NAMED) {
        int i = hx_tparam_index(tps, n, t->name);
        if (i >= 0) return subs[i];
    }
    int has = hx_ty_has_tparam(t, tps, n);
    /* sin parametro que sustituir se devuelve una copia: el llamante puede
       mutarla y el tipo original es const */
    HxTy *nt = (HxTy *)hx_arena_calloc(a, sizeof(HxTy));
    if (!has) {
        *nt = *t;
        return nt;
    }
    nt->kind = t->kind;
    nt->name = t->name;
    nt->elem = hx_ty_subst(a, t->elem, tps, subs, n);
    nt->inner = hx_ty_subst(a, t->inner, tps, subs, n);
    nt->size = t->size;
    nt->n_targs = t->n_targs;
    return nt;
}

void hx_ty_mangle(const HxTy *t, char *out, int cap) {
    const char *simple = NULL;
    char tmp[128];
    int k = 0;
    if (!t) {
        snprintf(out, (size_t)cap, "v");
        return;
    }
    switch (t->kind) {
        case TY_VOID: simple = "v"; break;
        case TY_BOOL: simple = "b"; break;
        case TY_INT: simple = "i"; break;
        case TY_I64: simple = "l"; break;
        case TY_FLOAT: simple = "f"; break;
        case TY_STRING: simple = "s"; break;
        case TY_DURATION: simple = "d"; break;
        case TY_VEC2: simple = "v2"; break;
        case TY_VEC3: simple = "v3"; break;
        case TY_VEC4: simple = "v4"; break;
        case TY_MAT4: simple = "m4"; break;
        case TY_QUAT: simple = "q4"; break;
        case TY_UNKNOWN: simple = "u"; break;
        default: simple = NULL; break;
    }
    if (simple) {
        snprintf(out, (size_t)cap, "%s", simple);
        return;
    }
    if (t->kind == TY_NAMED && t->name && hx_sym_str(t->name) &&
        !strcmp(hx_sym_str(t->name), "Result")) {
        hx_ty_mangle(t->elem, tmp, sizeof(tmp));
        k = snprintf(out, (size_t)cap, "R%s_", tmp);
        if (k < 0 || k >= cap) return;
        hx_ty_mangle(t->inner, tmp, sizeof(tmp));
        snprintf(out + k, (size_t)(cap - k), "%s", tmp);
        return;
    }
    if (t->kind == TY_NAMED) {
        const char *nm = t->name ? hx_sym_str(t->name) : "?";
        k = 0;
        for (const char *p = nm; *p && k < cap - 1; p++)
            out[k++] = (char)((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                                 (*p >= '0' && *p <= '9') ? *p : '_');
        out[k] = 0;
        for (int i = 0; i < t->n_targs && i < 2; i++) {
            char arg[128];
            if (k >= cap - 2) break;
            hx_ty_mangle(i == 0 ? t->elem : t->inner, arg, sizeof(arg));
            int w = snprintf(out + k, (size_t)(cap - k), "%s%s", i == 0 ? "__" : "_", arg);
            if (w < 0 || k + w >= cap) {
                out[cap - 1] = 0;
                break;
            }
            k += w;
        }
        return;
    }
    switch (t->kind) {
        case TY_ARRAY: {
            hx_ty_mangle(t->elem, tmp, sizeof(tmp));
            snprintf(out, (size_t)cap, "A%lld%s", (long long)t->size, tmp);
            return;
        }
        case TY_REF: {
            hx_ty_mangle(t->inner, tmp, sizeof(tmp));
            snprintf(out, (size_t)cap, "P%s", tmp);
            return;
        }
        case TY_PTR: {
            hx_ty_mangle(t->inner, tmp, sizeof(tmp));
            snprintf(out, (size_t)cap, "Q%s", tmp);
            return;
        }
        default:
            snprintf(out, (size_t)cap, "u");
            return;
    }
}

static HxExpr *hx_clone_expr(HxArena *a, const HxExpr *x, const HxSym *tps, HxTy **subs,
                             int n);

static HxStmt *hx_clone_stmt(HxArena *a, const HxStmt *s, const HxSym *tps, HxTy **subs,
                             int n);

static HxStmtVec hx_clone_stmts(HxArena *a, const HxStmtVec *v, const HxSym *tps, HxTy **subs,
                               int n) {
    HxStmtVec out;
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < v->len; i++)
        HX_VEC_PUSH(out, *hx_clone_stmt(a, &v->data[i], tps, subs, n));
    return out;
}

static HxExpr *hx_clone_expr(HxArena *a, const HxExpr *x, const HxSym *tps, HxTy **subs,
                             int n) {
    if (!x) return NULL;
    HxExpr *c = (HxExpr *)hx_arena_calloc(a, sizeof(HxExpr));
    *c = *x;
    c->ty = hx_ty_subst(a, x->ty, tps, subs, n);
    c->payload_ty = hx_ty_subst(a, x->payload_ty, tps, subs, n);
    c->ret_arg_refs = NULL;
    c->n_arg_refs = 0;
    switch (x->kind) {
        case EX_PATH: {
            memset(&c->path, 0, sizeof(c->path));
            for (int i = 0; i < x->path.parts.len; i++)
                HX_VEC_PUSH(c->path.parts, x->path.parts.data[i]);
            break;
        }
        case EX_CALL: {
            memset(&c->call, 0, sizeof(c->call));
            c->call.callee = hx_clone_expr(a, x->call.callee, tps, subs, n);
            for (int i = 0; i < x->call.args.len; i++) {
                HxArg g = x->call.args.data[i];
                g.value = hx_clone_expr(a, g.value, tps, subs, n);
                HX_VEC_PUSH(c->call.args, g);
            }
            break;
        }
        case EX_BIN:
            c->bin.lhs = hx_clone_expr(a, x->bin.lhs, tps, subs, n);
            c->bin.rhs = hx_clone_expr(a, x->bin.rhs, tps, subs, n);
            break;
        case EX_UN:
            c->un.operand = hx_clone_expr(a, x->un.operand, tps, subs, n);
            break;
        case EX_INDEX:
            c->index.base = hx_clone_expr(a, x->index.base, tps, subs, n);
            c->index.start = hx_clone_expr(a, x->index.start, tps, subs, n);
            c->index.end = hx_clone_expr(a, x->index.end, tps, subs, n);
            break;
        case EX_TRY:
            c->try.inner = hx_clone_expr(a, x->try.inner, tps, subs, n);
            break;
        case EX_VEC: {
            memset(&c->vec, 0, sizeof(c->vec));
            for (int i = 0; i < x->vec.len; i++) {
                c->vec.items = (HxExpr **)hx_arena_realloc_tmp(
                    c->vec.items, sizeof(HxExpr *) * (size_t)(c->vec.len + 1));
                c->vec.items[c->vec.len++] = hx_clone_expr(a, x->vec.items[i], tps, subs, n);
            }
            break;
        }
        case EX_STR: {
            /* los tramos de una interpolacion los construye el verificador, que
               vuelve a hacerlo sobre la copia porque conservamos has_holes */
            c->segs = NULL;
            c->n_segs = 0;
            break;
        }
        default:
            break;
    }
    if (x->recv) c->recv = hx_clone_expr(a, x->recv, tps, subs, n);
    if (x->fn) c->fn = x->fn;
    return c;
}

static HxPattern *hx_clone_pat(HxArena *a, const HxPattern *p, const HxSym *tps, HxTy **subs,
                               int n) {
    if (!p) return NULL;
    HxPattern *c = (HxPattern *)hx_arena_calloc(a, sizeof(HxPattern));
    *c = *p;
    c->lit = hx_clone_expr(a, p->lit, tps, subs, n);
    c->lo = hx_clone_expr(a, p->lo, tps, subs, n);
    c->hi = hx_clone_expr(a, p->hi, tps, subs, n);
    memset(&c->args, 0, sizeof(c->args));
    for (int i = 0; i < p->args.len; i++)
        HX_VEC_PUSH(c->args, *hx_clone_pat(a, &p->args.data[i], tps, subs, n));
    return c;
}

static HxStmt *hx_clone_stmt(HxArena *a, const HxStmt *s, const HxSym *tps, HxTy **subs,
                             int n) {
    HxStmt *c = (HxStmt *)hx_arena_calloc(a, sizeof(HxStmt));
    *c = *s;
    switch (s->kind) {
        case ST_EXPR: c->expr = hx_clone_expr(a, s->expr, tps, subs, n); break;
        case ST_ASSIGN:
            c->assign.target = hx_clone_expr(a, s->assign.target, tps, subs, n);
            c->assign.value = hx_clone_expr(a, s->assign.value, tps, subs, n);
            break;
        case ST_DIM:
            c->dim.ty = hx_ty_subst(a, s->dim.ty, tps, subs, n);
            c->dim.init = hx_clone_expr(a, s->dim.init, tps, subs, n);
            break;
        case ST_PRINT: {
            memset(&c->print, 0, sizeof(c->print));
            for (int i = 0; i < s->print.items.len; i++) {
                HxPrintItem it = s->print.items.data[i];
                it.expr = hx_clone_expr(a, it.expr, tps, subs, n);
                HX_VEC_PUSH(c->print.items, it);
            }
            break;
        }
        case ST_IF:
            c->if_.cond = hx_clone_expr(a, s->if_.cond, tps, subs, n);
            c->if_.then = hx_clone_stmts(a, &s->if_.then, tps, subs, n);
            c->if_.else_ = hx_clone_stmts(a, &s->if_.else_, tps, subs, n);
            for (int i = 0; i < s->if_.n_elifs; i++) {
                c->if_.elifs[i].cond = hx_clone_expr(a, s->if_.elifs[i].cond, tps, subs, n);
                c->if_.elifs[i].then = hx_clone_stmts(a, &s->if_.elifs[i].then, tps, subs, n);
            }
            break;
        case ST_WHILE:
            c->while_.cond = hx_clone_expr(a, s->while_.cond, tps, subs, n);
            c->while_.body = hx_clone_stmts(a, &s->while_.body, tps, subs, n);
            break;
        case ST_FOR:
            c->for_.start = hx_clone_expr(a, s->for_.start, tps, subs, n);
            c->for_.end = hx_clone_expr(a, s->for_.end, tps, subs, n);
            c->for_.step = hx_clone_expr(a, s->for_.step, tps, subs, n);
            c->for_.body = hx_clone_stmts(a, &s->for_.body, tps, subs, n);
            break;
        case ST_RETURN: c->ret.value = hx_clone_expr(a, s->ret.value, tps, subs, n); break;
        case ST_EXIT: c->exit_.code = hx_clone_expr(a, s->exit_.code, tps, subs, n); break;
        case ST_BLOCK: c->block.stmts = hx_clone_stmts(a, &s->block.stmts, tps, subs, n); break;
        case ST_ARENA:
            c->arena.body = hx_clone_stmts(a, &s->arena.body, tps, subs, n);
            break;
        case ST_DEFER: c->inner = hx_clone_stmts(a, &s->inner, tps, subs, n); break;
        case ST_CONST:
            c->konst.ty = hx_ty_subst(a, s->konst.ty, tps, subs, n);
            c->konst.value = hx_clone_expr(a, s->konst.value, tps, subs, n);
            break;
        case ST_MATCH: {
            c->match.subject = hx_clone_expr(a, s->match.subject, tps, subs, n);
            memset(&c->match.cases, 0, sizeof(c->match.cases));
            for (int i = 0; i < s->match.cases.len; i++) {
                HxMatchCase mc = s->match.cases.data[i];
                mc.pattern = hx_clone_pat(a, mc.pattern, tps, subs, n);
                mc.guard = hx_clone_expr(a, mc.guard, tps, subs, n);
                mc.body = hx_clone_stmts(a, &mc.body, tps, subs, n);
                HX_VEC_PUSH(c->match.cases, mc);
            }
            c->match.else_body = hx_clone_stmts(a, &s->match.else_body, tps, subs, n);
            break;
        }
        default: break;
    }
    return c;
}

struct HxFunc *hx_func_instantiate(HxArena *a, HxIntern *intern, struct HxFunc *g, HxTy **targs, int n,
                            int module, const char *module_name, HxSpan sp, HxDiagBag *diags) {
    if (n != g->n_tparams) {
        if (diags)
            hx_error(diags, sp, "E0701",
                     "'%s' espera %d parametro(s) de tipo, recibió %d", hx_sym_str(g->name),
                     g->n_tparams, n);
        return NULL;
    }
    for (int i = 0; i < n; i++)
        if (!targs[i]) {
            if (diags)
                hx_error(diags, sp, "E0702",
                         "no se pudo inferir el tipo de '%s' en '%s'",
                         hx_sym_str(g->tparams[i]), hx_sym_str(g->name));
            return NULL;
        }

    struct HxFunc *inst = (struct HxFunc *)hx_arena_calloc(a, sizeof(struct HxFunc));
    *inst = *g;
    inst->is_instance = 1;
    inst->is_generic = 0;
    inst->n_tparams = 0;
    inst->is_export = 0;
    inst->module = module;
    memset(&inst->params, 0, sizeof(inst->params));
    memset(&inst->body, 0, sizeof(inst->body));
    inst->ret = hx_ty_subst(a, g->ret, g->tparams, targs, n);
    for (int i = 0; i < g->params.len; i++) {
        HxParam p = g->params.data[i];
        p.ty = hx_ty_subst(a, p.ty, g->tparams, targs, n);
        p.default_value = hx_clone_expr(a, p.default_value, g->tparams, targs, n);
        HX_VEC_PUSH(inst->params, p);
    }
    inst->body = hx_clone_stmts(a, &g->body, g->tparams, targs, n);

    char mang[512];
    int k = snprintf(mang, sizeof(mang), "%s__%s", module_name ? module_name : "m",
                     hx_sym_str(g->name));
    for (int i = 0; i < n && k > 0 && k < (int)sizeof(mang); i++) {
        char one[128];
        hx_ty_mangle(targs[i], one, sizeof(one));
        k += snprintf(mang + k, sizeof(mang) - (size_t)k, "__%s", one);
    }
    inst->name = hx_intern_cstr(intern, mang);
    inst->name_span = sp;
    inst->span = g->span;
    return inst;
}
/* --- tipos genericos ---------------------------------------------------- */

HxTypeDecl *hx_type_instantiate(HxArena *a, HxIntern *intern, HxUnit *unit, HxTypeDecl *g,
                                 HxTy **targs, int n, HxDiagBag *diags, HxSpan sp) {
    char key[512];
    const char *mname = g->module >= 0 && g->module < unit->modules.len
                            ? hx_sym_str(unit->modules.data[g->module].name)
                            : "m";
    int k = snprintf(key, sizeof(key), "%s__%s__", mname, hx_sym_str(g->name));
    for (int i = 0; i < n; i++) {
        if (k <= 0 || k >= (int)sizeof(key) - 2) break;
        char one[128];
        hx_ty_mangle(targs[i], one, sizeof(one));
        int w = snprintf(key + k, sizeof(key) - (size_t)k, "%s_", one);
        if (w < 0 || k + w >= (int)sizeof(key)) break;
        k += w;
    }
    for (int i = 0; i < unit->n_type_instances; i++)
        if (!strcmp(hx_sym_str(unit->type_instances[i]->name), key)) return unit->type_instances[i];

    HxTypeDecl *inst = (HxTypeDecl *)hx_arena_calloc(a, sizeof(HxTypeDecl));
    *inst = *g;
    inst->name = hx_intern_cstr(intern, key);
    inst->n_tparams = 0;
    inst->is_generic = 0;
    inst->is_instance = 1;
    inst->is_export = 0;
    memset(&inst->fields, 0, sizeof(inst->fields));
    for (int i = 0; i < g->fields.len; i++) {
        HxField f = g->fields.data[i];
        f.ty = hx_ty_subst(a, f.ty, g->tparams, targs, n);
        HX_VEC_PUSH(inst->fields, f);
    }
    if (unit->n_type_instances == unit->cap_type_instances) {
        unit->cap_type_instances = unit->cap_type_instances ? unit->cap_type_instances * 2 : 8;
        unit->type_instances = (HxTypeDecl **)hx_arena_realloc_tmp(
            unit->type_instances, sizeof(HxTypeDecl *) * (size_t)unit->cap_type_instances);
    }
    unit->type_instances[unit->n_type_instances++] = inst;
    (void)diags;
    (void)sp;
    return inst;
}
