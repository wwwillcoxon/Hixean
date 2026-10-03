#include "hx/check.h"
#include "hx/mono.h"
#include "hx/parse.h"

#include <string.h>

typedef struct {
    HxSym name;
    HxTy *ty;
    int kind;
    HxSpan span;
    int arena_depth;
    int borrowed;
} HxSymEntry;

enum { SK_VAR, SK_FUNC, SK_TYPE, SK_CONST, SK_MODULE, SK_TRAIT };

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
    int uses_enum;
    struct HxFunc *cur_func;
    HxTy *ret_ty;
    int loop_depth;
    /* parametros de tipo de la funcion generica que se esta comprobando: un
       tipo como Caja<T> se deja sin resolver hasta la instancia */
    HxSym cur_tparams[HX_MAX_TPARAMS];
    int n_cur_tparams;
    int body_hoisted; /* el cuerpo actual ya fue declarado por hx_decl_locals */
    const char *caps[16]; /* capacidades activadas con --capability */
    int n_caps;
    int uses_net;
    HxTy *cur_self; /* tipo que implementa el TRAIT que se esta comprobando */
    int defer_depth;
    int loop_defer_depth;
    int arena_depth;
    int match_defer;
    int in_forin; /* dentro del iterable de un FOR: ahi si caben los iteradores */
} HxChecker;

static HxExpr *hx_expr_check(HxChecker *c, HxExpr *e);
static HxExpr *hx_generic_call_check(HxChecker *c, HxExpr *e, struct HxFunc *g);
static void hx_define_module_scope(HxChecker *c, HxModule *mod);
static void hx_collect_names(HxChecker *c, HxModule *mod);
static void hx_resolve_signature(HxChecker *c, struct HxFunc *fn);
static void hx_check_func(HxChecker *c, struct HxFunc *f);
static int hx_iter_ctor_check(HxChecker *c, HxExpr *e, const char *name, HxExpr *recv);
static int hx_capability(const HxChecker *c, const char *name);
static int hx_net_check(HxChecker *c, HxExpr *e, const char *name);

/* --- capacidades ---------------------------------------------------------
   `net` es la primera capacidad del lenguaje: sin ella, IMPORT net falla con
   un mensaje que dice cómo activarla. Las primitivas viven en el runtime y el
   módulo de la biblioteca estándar se genera en build/gen. */

/* --- std.net -------------------------------------------------------------
   Funciones de la capacidad `net`. Todas hablan con 127.0.0.1 salvo donde se
   pasa la direccion. Devuelven ENTERO: 0 o mas si todo va bien, o un codigo de
   error del sistema (negativo) si no. */

static int hx_net_check(HxChecker *c, HxExpr *e, const char *name) {
    if (!hx_capability(c, "net")) return 0;
    struct {
        const char *name;
        int nargs;
        int ret_string;
    } tabla[] = {{"NET_UDP", 0, 0},   {"NET_TCP", 0, 0},        {"NET_BIND", 2, 0},
                 {"NET_SEND", 4, 0},   {"NET_RECV", 1, 1},        {"NET_RECV_DE", 3, 1},
                 {"NET_LISTEN", 2, 0}, {"NET_ACCEPT", 1, 0},      {"NET_CONNECT", 2, 0},
                 {"NET_CLOSE", 1, 0},  {"NET_ERROR", 0, 0},       {NULL, 0, 0}};
    int idx = -1;
    for (int i = 0; tabla[i].name; i++)
        if (!hx_ascii_casecmp(name, tabla[i].name)) idx = i;
    if (idx < 0) return 0;
    if (e->call.args.len != tabla[idx].nargs) {
        hx_error(c->diags, e->span, "E0306",
                 hx_arena_sprintf(c->arena, "%s espera %d argumento(s), recibió %d", name,
                                  tabla[idx].nargs, e->call.args.len));
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return 1;
    }
    for (int i = 0; i < e->call.args.len; i++) {
        e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
        HxExpr *a = e->call.args.data[i].value;
        HxTy *ty = a->ty;
        if (!ty) continue;
        int es_ref = ty->kind == TY_REF;
        HxTy *inner = es_ref ? ty->inner : ty;
        int es_str = inner->kind == TY_STRING;
        int es_i64 = inner->kind == TY_I64 || inner->kind == TY_INT;
        if (!es_i64 && !es_str)
            hx_error(c->diags, a->span, "E0902",
                     hx_arena_sprintf(c->arena, "%s: el argumento %d debe ser INT, I64 o STRING",
                                      name, i + 1));
    }
    c->uses_net = 1;
    if (c->unit->n_caps_used < 8) {
        const char **slot = &c->unit->caps_used[c->unit->n_caps_used];
        *slot = hx_intern_cstr(c->intern, "net");
        c->unit->n_caps_used++;
    }
    e->is_intrin = 6;
    e->method = hx_intern_cstr(c->intern, tabla[idx].name);
    e->ty = hx_ty_builtin(c->arena, tabla[idx].ret_string ? TY_STRING : TY_INT);
    return 1;
}

static int hx_capability(const HxChecker *c, const char *name) {
    for (int i = 0; i < c->n_caps; i++)
        if (!hx_ascii_casecmp(c->caps[i], name)) return 1;
    return 0;
}

static int hx_iter_ctor_check(HxChecker *c, HxExpr *e, const char *name, HxExpr *recv);
static int hx_capability(const HxChecker *c, const char *name);
static int hx_net_check(HxChecker *c, HxExpr *e, const char *name);
static struct HxFunc *hx_lambda_func(HxChecker *c, HxExpr *e);
static int hx_ty_enum_like(const HxTy *t);
static int hx_enum_member(HxChecker *c, HxExpr *e, const char *member, HxExpr *recv);

static void hx_scope_push(HxChecker *c) {
    HxScope *s = (HxScope *)hx_arena_calloc(c->arena, sizeof(HxScope));
    s->parent = c->scope;
    c->scope = s;
}

static void hx_scope_pop(HxChecker *c) { c->scope = c->scope->parent; }

/* Doblar un nombre nunca debe caer: el parser ya pone un nombreAnonimo cuando
   no hay identificador, pero una unidad .hxc manipulada tambien puede llegar
   hasta aqui. */
static HxSym hx_fold(HxChecker *c, HxSym sym) {
    const char *s = hx_sym_str(sym);
    if (!s) s = "_anonimo";
    return hx_intern_fold_ascii(c->intern, s, strlen(s));
}

/* MAYBE<T> tiene tres metodos y ningun operador nuevo: el lenguaje ya hace
   metodos sobre valores (STRING) y un `??` mas seria la excepcion. */
static const char *hx_maybe_metodos[] = {"IsNil", "Or", "Map", NULL};

static int hx_es_maybe(HxTy *t) { return t && t->kind == TY_MAYBE; }

/* Los operadores que un programa puede recargar. La palabra es la que aparece en
   el nombre de C, porque un "+" no puede estar en un identificador. */
static const char *hx_op_palabra(const char *op) {
    static const struct {
        const char *signo;
        const char *palabra;
    } ops[] = {{"+", "add"},   {"-", "sub"},  {"*", "mul"}, {"/", "div"}, {"MOD", "mod"},
               {"++", "cat"},  {"==", "eq"},  {"<>", "ne"}, {"<", "lt"},  {"<=", "le"},
               {">", "gt"},    {">=", "ge"},  {NULL, NULL}};
    for (int i = 0; ops[i].signo; i++)
        if (!strcmp(ops[i].signo, op)) return ops[i].palabra;
    return NULL;
}

/* El nombre con el que se busca la sobrecarga: op_<palabra>__<Tipo>. */
static const char *hx_op_key(HxArena *a, HxBinOp op, HxTy *ty) {
    const char *signo = hx_binop_spelling(op);
    const char *palabra = hx_op_palabra(signo);
    if (!palabra || !ty || ty->kind != TY_NAMED) return NULL;
    return hx_arena_sprintf(a, "op_%s__%s", palabra, hx_ty_name(ty));
}

/* una variable se llama como operador si su nombre no es un identificador */
static int hx_es_operador(HxSym name) {
    const char *texto = hx_sym_str(name);
    if (!texto) return 0;
    for (const char *p = texto; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return 1;
    return 0;
}

/* un nombre ya declarado en ESTE ambito: sombra legitima en uno mas hondo */
static int hx_declared_here(HxChecker *c, HxSym name) {
    for (int i = 0; i < c->scope->syms.len; i++)
        if (c->scope->syms.data[i].name == name &&
            (c->scope->syms.data[i].kind == SK_VAR || c->scope->syms.data[i].kind == SK_CONST))
            return 1;
    return 0;
}

static void hx_define(HxChecker *c, HxSym name, HxTy *ty, int kind, HxSpan span) {
    HxSymEntry e = {name, ty, kind, span, 0, 0};
    HX_VEC_PUSH(c->scope->syms, e);
}

static HxSymEntry *hx_lookup_in_scope(HxChecker *c, HxSym name) {
    for (int i = c->scope->syms.len - 1; i >= 0; i--)
        if (c->scope->syms.data[i].name == name) return &c->scope->syms.data[i];
    for (HxScope *s = c->scope->parent; s; s = s->parent)
        for (int i = s->syms.len - 1; i >= 0; i--)
            if (s->syms.data[i].name == name) return &s->syms.data[i];
    return NULL;
}

static void hx_mark_borrow(HxChecker *c, HxExpr *arg, HxSpan sp) {
    if (!arg || arg->kind != EX_PATH || arg->path.parts.len != 1) {
        hx_diag_note(c->diags, sp, "E0408",
                     "un REF sólo puede tomar prestada una variable con nombre",
                     "si necesitas arithmetic cruda usa PTR", NULL);
        return;
    }
    HxSym folded = hx_fold(c, arg->path.parts.data[0].name);
    HxSymEntry *se = hx_lookup_in_scope(c, folded);
    if (!se) return;
    if (se->borrowed) {
        hx_diag_note(c->diags, sp, "E0408",
                     hx_arena_sprintf(c->arena, "'%s' ya está prestado",
                                      hx_sym_str(arg->path.parts.data[0].name)),
                     "una variable se presta a lo sumo una vez por ambito (unicidad)",
                     NULL);
        return;
    }
    se->borrowed = 1;
}

static HxSymEntry *hx_lookup(HxChecker *c, HxSym name) {
    for (HxScope *s = c->scope; s; s = s->parent) {
        for (int i = s->syms.len - 1; i >= 0; i--)
            if (s->syms.data[i].name == name) return &s->syms.data[i];
    }
    return NULL;
}

static HxTypeDecl *hx_find_type(HxChecker *c, HxSym name) {
    for (int i = 0; i < c->unit->n_type_instances; i++)
        if (!hx_ascii_casecmp(hx_sym_str(c->unit->type_instances[i]->name), hx_sym_str(name)))
            return c->unit->type_instances[i];
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

static HxTraitDecl *hx_find_trait(HxChecker *c, HxSym name) {
    for (int m = 0; m < c->unit->modules.len; m++)
        for (int i = 0; i < c->unit->modules.data[m].traits.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(c->unit->modules.data[m].traits.data[i].name),
                                  hx_sym_str(name)))
                return &c->unit->modules.data[m].traits.data[i];
    return NULL;
}

/* Busca METODO para el tipo concreto `ty` en una implementacion del trait. */
static struct HxFunc *hx_find_impl_method(HxChecker *c, HxTraitDecl *tr, HxTy *ty,
                                          HxSym method) {
    if (!ty) return NULL;
    char tname[128];
    if (ty->kind == TY_NAMED) snprintf(tname, sizeof(tname), "%s", hx_sym_str(ty->name));
    else snprintf(tname, sizeof(tname), "%s", hx_ty_name(ty));
    for (int m = 0; m < c->unit->modules.len; m++) {
        HxModule *mod = &c->unit->modules.data[m];
        for (int i = 0; i < mod->impls.len; i++) {
            HxImplDecl *im = &mod->impls.data[i];
            if (hx_ascii_casecmp(hx_sym_str(im->trait_name), hx_sym_str(tr->name))) continue;
            if (hx_ascii_casecmp(hx_sym_str(im->type_name), tname)) continue;
            for (int j = 0; j < im->methods.len; j++) {
                HxSym on = im->methods.data[j].orig_name
                               ? im->methods.data[j].orig_name
                               : im->methods.data[j].name;
                if (!hx_ascii_casecmp(hx_sym_str(on), hx_sym_str(method)))
                    return &im->methods.data[j];
            }
        }
    }
    return NULL;
}

static struct HxFunc *hx_find_func(HxChecker *c, HxSym name) {
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
    /* MAYBE<T> acepta un T: envolver es siempre correcto */
    if (to->kind == TY_MAYBE && !hx_ty_equal(from, to)) return 1;
    /* un registro con mas campos sirve donde se pide uno con menos */
    if (hx_ty_subtype(from, to)) return 1;
    /* el literal 0 es el puntero nulo */
    if (to->kind == TY_PTR) {
        if (from->kind == TY_PTR) return 1;
        if (from->kind == TY_INT || from->kind == TY_I64) return 1;
        hx_diag_note(c->diags, sp, "E0301",
                     hx_arena_sprintf(c->arena, "se esperaba %s, se encontró %s",
                                      hx_ty_name(to), hx_ty_name(from)),
                     what, NULL);
        return 0;
    }
    if (hx_ty_equal(from, to)) return 1;
    int fr = hx_ty_rank(from), tr = hx_ty_rank(to);
    if (fr && tr && from->kind == to->kind) return 1;
    if (fr && tr && fr < tr) return 1;
    if (fr == 2 && tr == 4) return 1;
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

typedef struct {
    const char *name;
    int nargs;
    HxTyKind ret;
    int vec_arg;
    int vec_len; /* componentes exigidos; 0 significa cualquiera */
} HxVecIntrin;

static const HxVecIntrin hx_vec_intrins[] = {
    {"DOT", 2, TY_FLOAT, 1, 3},       {"CROSS", 2, TY_VEC3, 1, 3},
    {"NORMALIZED", 1, 0, 1, 3},        {"LEN", 1, TY_FLOAT, 1, 0},
    {"NORMALIZE", 1, TY_VEC3, 1, 3},   {"SWIZZLE", 1, TY_VEC4, 1, 0},
    {NULL, 0, TY_UNKNOWN, 0, 0},
};

static const HxVecIntrin *hx_find_vec_intrin(const char *name, int *nvec) {
    for (int i = 0; hx_vec_intrins[i].name; i++)
        if (!hx_ascii_casecmp(hx_vec_intrins[i].name, name)) {
            if (nvec) *nvec = hx_vec_intrins[i].vec_arg;
            return &hx_vec_intrins[i];
        }
    return NULL;
}

/* Metodos de ARRAY[T]. El tamaño del arreglo vive en el tipo, asi que Len es
   una constante y At es un indice con la comprobacion puesta: en el perfil
   freestanding no hay nadie que lo mire mas que el programa. */
typedef struct {
    const char *name;
    int nargs;
    HxTyKind ret;
    int elem_ret; /* 1: devuelve el tipo del elemento */
} HxArrIntrin;

/* First y Last se quedan con los iteradores: `Rango(1,9).First()` ya existe y
   no tiene sentido que el mismo nombre signifique dos cosas. */
static const HxArrIntrin hx_array_intrins[] = {
    {"Len", 0, TY_I64, 0},
    {"At", 1, TY_UNKNOWN, 1},
    {NULL, 0, TY_UNKNOWN, 0},
};

static const HxArrIntrin *hx_find_array_intrin(const char *name) {
    for (int i = 0; hx_array_intrins[i].name; i++)
        if (!hx_ascii_casecmp(hx_array_intrins[i].name, name)) return &hx_array_intrins[i];
    return NULL;
}

/* El receptor de At/First/Last tiene que ser una variable o un campo: se emite
   dos veces (puntero y tamaño) y un valor temporal se evaluaria dos veces. */
static int hx_es_direccionable(HxExpr *x) {
    if (!x) return 0;
    if (x->kind == EX_PATH) return 1;
    if (x->kind == EX_MEMB) return hx_es_direccionable(x->member.base);
    return 0;
}

static const HxIntrin *hx_find_intrin(HxTy *recv, const char *name) {
    if (!recv || recv->kind != TY_STRING) return NULL;
    for (int i = 0; hx_string_intrins[i].name; i++)
        if (!hx_ascii_casecmp(hx_string_intrins[i].name, name)) return &hx_string_intrins[i];
    return NULL;
}

static HxExpr *hx_expr_check(HxChecker *c, HxExpr *e);

/* Coerciona una expresion a un tipo y, si hace falta, deja anotado que hay que
   convertirla de forma estructural al emitir. */
static void hx_coerce_to(HxChecker *c, HxExpr **slot, HxTy *to, HxSpan sp,
                         const char *what) {
    if (!*slot || !to) return;
    /* NIL no tiene tipo propio: lo toma del sitio donde aparece, y solo vale
       para un MAYBE. Fuera de ahi es un error, no un 0 disfrazado */
    if ((*slot)->is_nil) {
        if (to->kind != TY_MAYBE)
            hx_error(c->diags, (*slot)->span, "E0211",
                     "NIL solo vale para un MAYBE, y aqui se esperaba '%s'",
                     hx_ty_name(to));
        (*slot)->ty = to;
        return;
    }
    hx_coerce(c, (*slot)->ty, to, sp, what);
    if ((*slot)->ty && to && !hx_ty_equal((*slot)->ty, to) && hx_ty_subtype((*slot)->ty, to))
        (*slot)->conv_ty = to;
    /* envolver un T en un MAYBE<T> tambien es una conversion pendiente: el
       emisor la materializa con hx_maybe_some_T() */
    if ((*slot)->ty && to && to->kind == TY_MAYBE && (*slot)->ty->kind != TY_MAYBE &&
        (*slot)->kind != EX_NIL)
        (*slot)->conv_ty = to;
}

/* Or y Map de MAYBE<T>. Sin operadores nuevos: el lenguaje ya resuelve metodos
   sobre valores, y `??` seria justo la excepcion que el lenguaje evita. */
static int hx_maybe_call_check(HxChecker *c, HxExpr *e, HxExpr *recv, const char *mi) {
    int es_map = !hx_ascii_casecmp(mi, "Map");
    if (e->call.args.len != 1)
        hx_error(c->diags, e->span, "E0306", "'%s' espera 1 argumento, recibio %d", mi,
                 e->call.args.len);
    for (int q = 0; q < e->call.args.len; q++) {
        e->call.args.data[q].value = hx_expr_check(c, e->call.args.data[q].value);
        if (!es_map && recv->ty->elem)
            hx_coerce_to(c, &e->call.args.data[q].value, recv->ty->elem,
                         e->call.args.data[q].span, NULL);
    }
    /* Map devuelve el MAYBE que devuelve f, que puede ser de otro tipo */
    HxTy *out = recv->ty->elem ? recv->ty->elem : hx_ty_builtin(c->arena, TY_UNKNOWN);
    if (es_map && e->call.args.len == 1 && e->call.args.data[0].value->ty)
        out = e->call.args.data[0].value->ty;
    e->is_intrin = es_map ? 10 : 9;
    e->method = hx_intern_cstr(c->intern, mi);
    e->recv = recv;
    e->ty = out;
    return 1;
}

static int hx_vec_len(HxTy *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_VEC2: return 2;
        case TY_VEC3: return 3;
        case TY_VEC4:
        case TY_QUAT: return 4;
        case TY_UNKNOWN:
        case TY_VOID:
        case TY_BOOL:
        case TY_INT:
        case TY_I64:
        case TY_FLOAT:
        case TY_STRING:
        case TY_DURATION:
        case TY_NAMED:
        case TY_ARRAY:
        case TY_REF:
        case TY_PTR:
        case TY_MAT4:
        case TY_ITER:
        case TY_MAYBE: break;
    }
    return 0;
}

/* Len/At/First/Last de un arreglo de tamaño fijo. El tamaño vive en el tipo, de
   modo que Len es una constante y At solo necesita la comparacion. */
static int hx_array_method_check(HxChecker *c, HxExpr *e, HxExpr *recv, const char *member) {
    const HxArrIntrin *ai = hx_find_array_intrin(member);
    if (!ai || !recv) return 0;
    int dado = e->call.args.len;
    if (dado != ai->nargs)
        hx_error(c->diags, e->span, "E0306",
                 hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s), recibió %d", member,
                                  ai->nargs, dado));
    for (int i = 0; i < dado; i++) {
        e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
        HxTy *at = e->call.args.data[i].value->ty;
        if (at && at->kind != TY_INT && at->kind != TY_I64)
            hx_error(c->diags, e->call.args.data[i].span, "E0306",
                     hx_arena_sprintf(c->arena, "'%s' espera un índice INT o I64", member));
    }
    if (!hx_es_direccionable(recv))
        hx_error(c->diags, e->span, "E0402",
                 hx_arena_sprintf(c->arena,
                                  "'%s' necesita una variable o un campo, no un valor temporal",
                                  member));
    e->is_intrin = 7;
    e->method = hx_intern_cstr(c->intern, member);
    e->recv = recv;
    e->ty = ai->elem_ret
                ? (recv->ty && recv->ty->elem ? recv->ty->elem : hx_ty_builtin(c->arena, TY_UNKNOWN))
                : hx_ty_builtin(c->arena, ai->ret);
    return 1;
}

static int hx_vec_index_of(char c) {
    if (c == 'x' || c == 'r') return 0;
    if (c == 'y' || c == 'g') return 1;
    if (c == 'z' || c == 'b') return 2;
    if (c == 'w' || c == 'a') return 3;
    return -1;
}

static int hx_vec_component(HxChecker *c, HxTy *t, const char *member, HxSpan *sp) {
    int len = hx_vec_len(t);
    if (!len) return -1;
    size_t n = strlen(member);
    if (n == 0 || n > 4) return -1;
    int idx[4];
    for (size_t i = 0; i < n; i++) {
        int k = hx_vec_index_of(member[i]);
        if (k < 0 || k >= len) {
            hx_error(c->diags, *sp, "E0402",
                     hx_arena_sprintf(c->arena, "'%s' no es un componente o swizzle válido aquí",
                                      member));
            return -1;
        }
        idx[i] = k;
    }
    if (n == 1) return idx[0] + 1;
    return 100 + idx[0] * 10 + idx[1] + (n == 3 ? 1000 + idx[2] * 10 : 0) +
           (n == 4 ? 10000 + idx[2] * 100 + idx[3] * 10 : 0);
}

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
            if (se->arena_depth > c->arena_depth) {
                hx_diag_note(c->diags, parts[0].span, "E0407",
                             hx_arena_sprintf(c->arena,
                                              "'%s' pertenece a una arena que ya terminó",
                                              hx_sym_str(parts[0].name)),
                             "la memoria de una ARENA se libera al salir del bloque",
                             "copia el valor fuera del bloque o devuelve un Result");
                e->ty = se->ty;
                return e;
            }
            HxTy *t = se->ty;
            if (se->kind == SK_VAR && t && t->kind == TY_REF) {
                e->deref = 1;
                t = t->inner;
            }
            if (se->kind == SK_MODULE) {
                t = hx_ty_builtin(c->arena, TY_VOID);
                for (int i = split; i < n; i++) {
                    const char *mn = hx_sym_str(parts[i].name);
                    HxSym member = hx_intern_fold_ascii(c->intern, mn, strlen(mn));
                    struct HxFunc *f = hx_find_func(c, member);
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
                                     "sólo los elementos EXPORT (funciones, constantes, tipos) son visibles al importar",
                                     NULL);
                        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                        return e;
                    }
                    t = f->ret;
                }
            } else if (split < n) {
                const char *member = hx_sym_str(parts[split].name);
                int vcomp = hx_vec_component(c, t, member, &parts[split].span);
                if (vcomp > 0) {
                    int mlen = (int)strlen(member);
                    e->prefix_len = split;
                    e->method = parts[split].name;
                    e->vec_component = vcomp;
                    e->ty = hx_ty_builtin(c->arena,
                                          mlen == 1   ? TY_FLOAT
                                          : mlen == 2 ? TY_VEC2
                                          : mlen == 3 ? TY_VEC3
                                                      : TY_VEC4);
                    return e;
                }
                /* `Color.AZUL`: el simbolo de la izquierda es el ENUM */
                if (se->kind == SK_TYPE && split < n) {
                    HxTypeDecl *ed = hx_find_type(c, parts[0].name);
                    if (ed && ed->is_enum) {
                        for (int fi = 0; fi < ed->fields.len; fi++) {
                            if (hx_ascii_casecmp(hx_sym_str(ed->fields.data[fi].name),
                                                 member))
                                continue;
                            HxSym cn = hx_intern_cstr(
                                c->intern, hx_arena_sprintf(c->arena, "%s_%s",
                                                            hx_sym_str(ed->name),
                                                            hx_sym_str(ed->fields.data[fi].name)));
                            HxConst *kc = hx_find_const(c, cn);
                            if (!kc) continue;
                            e->path.parts.len = 0;
                            HX_VEC_PUSH(e->path.parts, ((HxPathPart){kc->name, parts[split].span}));
                            e->prefix_len = 0;
                            e->ty = kc->ty;
                            return e;
                        }
                        hx_error(c->diags, parts[split].span, "E0303",
                                 hx_arena_sprintf(c->arena,
                                                  "%s no tiene una variante llamada '%s'",
                                                  hx_sym_str(ed->name), member));
                        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                        return e;
                    }
                }
                int es_metodo_enum = !hx_ascii_casecmp(member, "Ordinal") ||
                                     !hx_ascii_casecmp(member, "ENUM_A_INT") ||
                                     !hx_ascii_casecmp(member, "Nombre");
                if (t && t->decl && t->decl->is_enum && split < n && es_metodo_enum) {
                    HxExpr *recvexpr = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
                    recvexpr->kind = EX_PATH;
                    recvexpr->span = e->span;
                    for (int k = 0; k < split; k++) HX_VEC_PUSH(recvexpr->path.parts, parts[k]);
                    recvexpr->ty = t;
                    if (hx_enum_member(c, e, member, recvexpr)) return e;
                }
                if (t && t->decl && t->decl->is_enum && split < n) {
                    /* Color.AZUL: la variante es una constante con el nombre del
                       enum delante, para que en C no choque con otra AZUL */
                    HxTypeDecl *ed = t->decl;
                    for (int fi = 0; fi < ed->fields.len; fi++) {
                        if (hx_ascii_casecmp(hx_sym_str(ed->fields.data[fi].name), member)) continue;
                        HxConst *kc = hx_find_const(
                            c, hx_intern_cstr(c->intern,
                                              hx_arena_sprintf(c->arena, "%s_%s",
                                                                hx_sym_str(ed->name),
                                                                hx_sym_str(ed->fields.data[fi].name))));
                        if (!kc) continue;
                        e->path.parts.len = 0;
                        HX_VEC_PUSH(e->path.parts,
                                    ((HxPathPart){kc->name, parts[split].span}));
                        e->prefix_len = 0;
                        e->ty = kc->ty;
                        return e;
                    }
                    hx_error(c->diags, parts[split].span, "E0303",
                             hx_arena_sprintf(c->arena, "%s no tiene una variante llamada '%s'",
                                              hx_sym_str(ed->name), member));
                    e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                    return e;
                }
                if (t && t->decl) {
                    for (int fi = 0; fi < t->decl->fields.len; fi++) {
                        HxField *fld = &t->decl->fields.data[fi];
                        if (!fld->ty) continue;
                        if (hx_ascii_casecmp(hx_sym_str(fld->name), member)) continue;
                        e->prefix_len = split + 1;
                        e->ty = fld->ty;
                        return e;
                    }
                }
                /* MAYBE<T>: `m.IsNil` es un miembro como los de ENUM */
                if (hx_es_maybe(t) && !hx_ascii_casecmp(member, "IsNil")) {
                    HxExpr *recvexpr = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
                    recvexpr->kind = EX_MEMB;
                    recvexpr->span = e->span;
                    HxExpr *sub = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
                    sub->kind = EX_PATH;
                    sub->span = e->span;
                    for (int k = 0; k < split; k++)
                        HX_VEC_PUSH(sub->path.parts, parts[k]);
                    recvexpr->member.base = hx_path_check(c, sub);
                    recvexpr->member.name = parts[split].name;
                    recvexpr->is_intrin = 8;
                    recvexpr->method = parts[split].name;
                    recvexpr->ty = hx_ty_builtin(c->arena, TY_BOOL);
                    *e = *recvexpr;
                    return e;
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
                             t && t->kind == TY_STRING
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
    /* `Rango(1,3).MAP(F)` llega como EX_MEMB con la llamada anterior en base */
    if (raw_callee->kind == EX_MEMB) {
        const char *mn = hx_sym_str(raw_callee->member.name);
        int es_iter = !hx_ascii_casecmp(mn, "Map") || !hx_ascii_casecmp(mn, "Filter") ||
                      !hx_ascii_casecmp(mn, "Take") || !hx_ascii_casecmp(mn, "First");
        int es_maybe = !hx_ascii_casecmp(mn, "Or") || !hx_ascii_casecmp(mn, "Map");
        if (es_iter || es_maybe) {
            HxExpr *irecv = hx_expr_check(c, raw_callee->member.base);
            if (es_maybe && hx_es_maybe(irecv->ty)) {
                hx_maybe_call_check(c, e, irecv, mn);
                return e;
            }
            if (es_iter && hx_iter_ctor_check(c, e, mn, irecv)) return e;
        }
    }
    if (raw_callee->kind == EX_PATH && raw_callee->path.parts.len == 1) {
        const char *solo = hx_sym_str(raw_callee->path.parts.data[0].name);
        if (!hx_lookup(c, raw_callee->path.parts.data[0].name) &&
            !strncmp(solo, "NET_", 4) && hx_net_check(c, e, solo))
            return e;
    }
    if (raw_callee->kind == EX_PATH && raw_callee->path.parts.len == 1 &&
        !hx_lookup(c, raw_callee->path.parts.data[0].name)) {
        const char *fname = hx_sym_str(raw_callee->path.parts.data[0].name);
        int needs_vec = 0;
        const HxVecIntrin *vi = hx_find_vec_intrin(fname, &needs_vec);
        if (vi) {
            if (e->call.args.len != vi->nargs)
                hx_error(c->diags, e->span, "E0306",
                         hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s), recibió %d",
                                          fname, vi->nargs, e->call.args.len));
            for (int i = 0; i < e->call.args.len; i++) {
                e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
                HxTy *at = e->call.args.data[i].value->ty;
                if (i < vi->vec_arg) {
                    int largo = hx_vec_len(at);
                    if (at && !largo)
                        hx_error(c->diags, e->call.args.data[i].span, "E0402",
                                 hx_arena_sprintf(c->arena, "'%s' espera un vector", fname));
                    else if (largo && vi->vec_len && largo != vi->vec_len)
                        hx_error(c->diags, e->call.args.data[i].span, "E0402",
                                 hx_arena_sprintf(c->arena,
                                                  "'%s' espera un vector de %d componentes",
                                                  fname, vi->vec_len));
                }
            }
            e->method = raw_callee->path.parts.data[0].name;
            e->is_intrin = 3;
            e->ty = hx_ty_builtin(c->arena, vi->ret == 0 ? TY_VEC3 : vi->ret);
            return e;
        }
    }
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
            if (is_err && arg && arg->ty && arg->ty->kind != TY_STRING)
                hx_error(c->diags, e->span, "E0309",
                         "el error de un Result debe ser STRING en esta versión");
            e->payload_ty = arg ? arg->ty : NULL;
            e->ty = (HxTy *)hx_arena_calloc(c->arena, sizeof(HxTy));
            e->ty->kind = TY_NAMED;
            e->ty->name = hx_intern_cstr(c->intern, "Result");
            e->ty->elem = arg ? arg->ty : NULL;
            e->ty->inner = hx_ty_builtin(c->arena, TY_STRING);
            e->ty->n_targs = 2;
            if (raw_callee->prefix_len) raw_callee->prefix_len = 0;
            return e;
        }
    }
    /* Suma.Mas(a, b): el primer nombre es un TRAIT y el despacho es estatico,
       segun el tipo del primer argumento */
    if (raw_callee->kind == EX_PATH && raw_callee->path.parts.len == 2) {
        HxTraitDecl *tr = hx_find_trait(c, raw_callee->path.parts.data[0].name);
        if (tr) {
            HxSym mname = raw_callee->path.parts.data[1].name;
            for (int i = 0; i < e->call.args.len; i++)
                e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
            if (!e->call.args.len) {
                hx_error(c->diags, e->span, "E0710",
                         hx_arena_sprintf(c->arena, "el metodo %s del TRAIT %s espera un receptor",
                                          hx_sym_str(mname), hx_sym_str(tr->name)));
                e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                return e;
            }
            HxTy *recv = e->call.args.data[0].value->ty;
            struct HxFunc *m = hx_find_impl_method(c, tr, recv, mname);
            if (!m) {
                hx_error(c->diags, raw_callee->path.parts.data[1].span, "E0707",
                         hx_arena_sprintf(c->arena, "%s no implementa %s.%s", hx_ty_name(recv),
                                          hx_sym_str(tr->name), hx_sym_str(mname)));
                e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
                return e;
            }
            int want = m->params.len;
            if (e->call.args.len != want)
                hx_error(c->diags, e->span, "E0306",
                         hx_arena_sprintf(c->arena, "%s.%s espera %d argumento(s), recibio %d",
                                          hx_sym_str(tr->name), hx_sym_str(mname), want,
                                          e->call.args.len));
            for (int i = 0; i < e->call.args.len && i < want; i++)
                hx_coerce_to(c, &e->call.args.data[i].value, m->params.data[i].ty,
                             e->call.args.data[i].span, hx_sym_str(m->name));
            e->fn = m;
            e->ty = m->ret;
            return e;
        }
    }
    /* Rango(...) y los adaptadores MAP/FILTER/TAKE/FIRST */
    if (raw_callee->kind == EX_MEMB) {
        const char *mn = hx_sym_str(raw_callee->member.name);
        int es_iter = !hx_ascii_casecmp(mn, "Map") || !hx_ascii_casecmp(mn, "Filter") ||
                      !hx_ascii_casecmp(mn, "Take") || !hx_ascii_casecmp(mn, "First");
        if (es_iter) {
            HxExpr *recv = hx_expr_check(c, raw_callee->member.base);
            if (hx_iter_ctor_check(c, e, mn, recv)) return e;
        }
    }
    if (raw_callee->kind == EX_PATH) {
        HxPathPart *pp = raw_callee->path.parts.data;
        int np = raw_callee->path.parts.len;
        if (np == 1 && hx_iter_ctor_check(c, e, hx_sym_str(pp[0].name), NULL)) return e;
        if (np >= 2) {
            HxExpr *recv = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
            recv->kind = EX_PATH;
            recv->span = e->span;
            for (int k = 0; k < np - 1; k++) HX_VEC_PUSH(recv->path.parts, pp[k]);
            recv = hx_expr_check(c, recv);
            if (hx_iter_ctor_check(c, e, hx_sym_str(pp[np - 1].name), recv)) return e;
        }
    }
    /* `a.At(3)` y `t.celdas.At(2)`: se comprueba el receptor antes que el camino
       entero, porque comprobar `a.At` dispara el error de miembro inexistente
       antes de que nadie llegue a mirar que ARRAY si tiene ese metodo. El
       prefijo no incluye el nombre del metodo, asi que hx_path_check no falla. */
    if (raw_callee->kind == EX_PATH && raw_callee->path.parts.len >= 2) {
        int np = raw_callee->path.parts.len;
        const char *mn = hx_sym_str(raw_callee->path.parts.data[np - 1].name);
        if (hx_find_array_intrin(mn) || (!hx_ascii_casecmp(mn, "Or") ||
                                         !hx_ascii_casecmp(mn, "Map"))) {
            HxExpr *recv_e = (HxExpr *)hx_arena_calloc(c->arena, sizeof(HxExpr));
            recv_e->kind = EX_PATH;
            recv_e->span = raw_callee->span;
            for (int k = 0; k < np - 1; k++)
                HX_VEC_PUSH(recv_e->path.parts, raw_callee->path.parts.data[k]);
            HxExpr *recv = hx_path_check(c, recv_e);
            if (recv->ty && recv->ty->kind == TY_ARRAY &&
                hx_array_method_check(c, e, recv, mn))
                return e;
            if (hx_es_maybe(recv->ty) && hx_maybe_call_check(c, e, recv, mn)) return e;
        }
    }
    HxExpr *callee = hx_expr_check(c, raw_callee);
    struct HxFunc *f = NULL;
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
                                 "sólo los elementos EXPORT (funciones, constantes, tipos) son visibles al importar", NULL);
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
    if (!f && callee->kind == EX_MEMB) {
        HxExpr *recv = hx_expr_check(c, callee->member.base);
        if (hx_enum_member(c, e, hx_sym_str(callee->member.name), recv)) return e;
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
    if (f->is_generic) return hx_generic_call_check(c, e, f);
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
    int *refs = (int *)hx_arena_calloc(c->arena, sizeof(int) * (e->call.args.len + 1));
    for (int i = 0; i < e->call.args.len; i++) {
        HxArg *arg = &e->call.args.data[i];
        arg->value = hx_expr_check(c, arg->value);
        if (i < f->params.len && f->params.data[i].ty && f->params.data[i].ty->kind == TY_REF)
            refs[i] = 1;
        HxParam *p = (i < f->params.len) ? &f->params.data[i] : NULL;
        if (p && p->ty) {
            if (p->ty->kind == TY_REF) {
                hx_mark_borrow(c, arg->value, arg->span);
                if (arg->value->ty && !hx_ty_equal(arg->value->ty, p->ty->inner))
                    hx_error(c->diags, arg->span, "E0301",
                             hx_arena_sprintf(c->arena, "REF %s espera un %s",
                                              hx_sym_str(f->name), hx_ty_name(p->ty->inner)));
            } else {
                hx_coerce_to(c, &arg->value, p->ty, arg->span, hx_sym_str(f->name));
            }
        }
    }
    e->ty = f->ret ? f->ret : hx_ty_builtin(c->arena, TY_VOID);
    e->ret_arg_refs = refs;
    e->n_arg_refs = e->call.args.len;
    return e;
}

/* --- funciones genericas -------------------------------------------------
   Una llamada a `FUNCTION F<T>(...)` se monomorfiza: se unifican los tipos
   de los argumentos con los parametros, se busca una instancia con esa misma
   combinacion y, si no existe, se crea una copia del cuerpo con T sustituido
   por el tipo concreto. El punto de llamada apunta a la instancia. */

static int hx_unify(HxChecker *c, HxTy *param, HxTy *arg, HxSym *tps, HxTy **subs, int n) {
    if (!param) return 1;
    if (param->kind == TY_NAMED) {
        for (int i = 0; i < n; i++)
            if (tps[i] == param->name) {
                if (!subs[i]) {
                    subs[i] = arg;
                } else if (arg && !hx_ty_equal(subs[i], arg)) {
                    hx_error(c->diags, (HxSpan){0, 0}, "E0703",
                             "'%s' deduce %s y %s en la misma llamada",
                             hx_sym_str(tps[i]), hx_ty_name(subs[i]), hx_ty_name(arg));
                }
                return 1;
            }
    }
    /* un tipo parametrizado se unifica estructuralmente: Caja<T> contra
       Caja<Int> liga T con Int, no con el tipo entero */
    if (param->n_targs && arg && arg->kind == TY_NAMED &&
        param->n_targs == arg->n_targs && param->name == arg->name) {
        if (!hx_unify(c, param->elem, arg->elem, tps, subs, n)) return 0;
        if (param->n_targs > 1 && !hx_unify(c, param->inner, arg->inner, tps, subs, n)) return 0;
        return 1;
    }
    if (param->elem && !hx_unify(c, param->elem, arg, tps, subs, n)) return 0;
    if (param->inner && !hx_unify(c, param->inner, arg, tps, subs, n)) return 0;
    return 1;
}

static struct HxFunc *hx_instance_for(HxChecker *c, struct HxFunc *g, HxTy **targs, int n, HxSpan sp,
                               HxModule *mod) {
    char key[512];
    const char *mname = g->module >= 0 && g->module < c->unit->modules.len
                            ? hx_sym_str(c->unit->modules.data[g->module].name)
                            : "m";
    int k = snprintf(key, sizeof(key), "%s__%s__", mname, hx_sym_str(g->name));
    for (int i = 0; i < n && k > 0 && k < (int)sizeof(key); i++) {
        char one[128];
        hx_ty_mangle(targs[i], one, sizeof(one));
        k += snprintf(key + k, sizeof(key) - (size_t)k, "%s_", one);
    }
    for (int i = 0; i < c->unit->n_instances; i++) {
        struct HxFunc *inst = c->unit->instances[i];
        if (inst->name && !strcmp(hx_sym_str(inst->name), key)) return inst;
    }
    /* DONDE T: Trait se comprueba contra el tipo concreto antes de crear la
       instancia: si falta la implementación, el programa no tiene sentido. */
    for (int ci = 0; ci < g->n_constraints; ci++) {
        int ti = -1;
        for (int tp = 0; tp < n; tp++)
            if (g->tparams[tp] == g->constrained[ci]) ti = tp;
        if (ti < 0) {
            hx_error(c->diags, sp, "E0716",
                     hx_arena_sprintf(c->arena, "'%s' no es un parametro de tipo de '%s'",
                                      hx_sym_str(g->constrained[ci]), hx_sym_str(g->name)));
            return NULL;
        }
        HxTraitDecl *tr = hx_find_trait(c, g->ctraits[ci]);
        if (!tr) {
            hx_error(c->diags, sp, "E0711",
                     hx_arena_sprintf(c->arena, "no existe el TRAIT '%s'",
                                      hx_sym_str(g->ctraits[ci])));
            return NULL;
        }
        char tn[128];
        if (targs[ti]->kind == TY_NAMED)
            snprintf(tn, sizeof(tn), "%s", hx_sym_str(targs[ti]->name));
        else
            snprintf(tn, sizeof(tn), "%s", hx_ty_name(targs[ti]));
        int hay = 0;
        for (int m = 0; m < c->unit->modules.len && !hay; m++)
            for (int im_i = 0; im_i < c->unit->modules.data[m].impls.len && !hay; im_i++) {
                HxImplDecl *im = &c->unit->modules.data[m].impls.data[im_i];
                if (hx_ascii_casecmp(hx_sym_str(im->trait_name), hx_sym_str(tr->name))) continue;
                if (!hx_ascii_casecmp(hx_sym_str(im->type_name), tn)) hay = 1;
            }
        if (!hay) {
            hx_error(c->diags, sp, "E0716",
                     hx_arena_sprintf(c->arena, "%s no implementa el TRAIT %s que exige '%s'",
                                      tn, hx_sym_str(tr->name), hx_sym_str(g->name)));
            return NULL;
        }
    }
    struct HxFunc *inst = hx_func_instantiate(c->arena, c->intern, g, targs, n, g->module,
                                       hx_sym_str(c->unit->modules.data[g->module].name), sp,
                                       c->diags);
    if (!inst) return NULL;
    if (c->unit->n_instances == c->unit->cap_instances) {
        c->unit->cap_instances = c->unit->cap_instances ? c->unit->cap_instances * 2 : 8;
        c->unit->instances = (struct HxFunc **)hx_arena_realloc_tmp(
            c->unit->instances, sizeof(struct HxFunc *) * (size_t)c->unit->cap_instances);
    }
    c->unit->instances[c->unit->n_instances++] = inst;
    inst->name = hx_intern_cstr(c->intern, key);

    HxModule *saved_mod = c->mod;
    c->mod = &c->unit->modules.data[inst->module];
    hx_scope_push(c);
    hx_define_module_scope(c, c->mod);
    hx_collect_names(c, c->mod);
    hx_resolve_signature(c, inst);
    int errs_before = c->diags->errors;
    hx_check_func(c, inst);
    if (c->diags->errors > errs_before)
        hx_diag_note(c->diags, sp, "E0704",
                     hx_arena_sprintf(c->arena, "al instanciar %s para %s",
                                      hx_sym_str(g->name), key),
                     "una funcion generica se comprueba una vez por cada combinacion de "
                     "argumentos de tipo que aparece en el programa",
                     NULL);
    hx_scope_pop(c);
    c->mod = saved_mod;
    (void)mod;
    return inst;
}

static HxExpr *hx_generic_call_check(HxChecker *c, HxExpr *e, struct HxFunc *g) {
    HxTy *subs[HX_MAX_TPARAMS];
    memset(subs, 0, sizeof(subs));
    int n = g->n_tparams;
    if (e->call.args.len != g->params.len) {
        hx_error(c->diags, e->span, "E0306",
                 hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s), recibió %d",
                                  hx_sym_str(g->name), g->params.len, e->call.args.len));
        for (int i = 0; i < e->call.args.len; i++)
            e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return e;
    }
    for (int i = 0; i < e->call.args.len; i++) {
        HxArg *arg = &e->call.args.data[i];
        arg->value = hx_expr_check(c, arg->value);
        hx_unify(c, g->params.data[i].ty, arg->value->ty, g->tparams, subs, n);
    }
    struct HxFunc *inst = hx_instance_for(c, g, subs, n, e->span, c->mod);
    if (!inst) {
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return e;
    }
    int *refs = (int *)hx_arena_calloc(c->arena, sizeof(int) * (e->call.args.len + 1));
    for (int i = 0; i < e->call.args.len; i++) {
        HxArg *arg = &e->call.args.data[i];
        HxParam *p = &inst->params.data[i];
        if (p->ty && p->ty->kind == TY_REF) {
            refs[i] = 1;
            hx_mark_borrow(c, arg->value, arg->span);
            if (arg->value->ty && !hx_ty_equal(arg->value->ty, p->ty->inner))
                hx_error(c->diags, arg->span, "E0301",
                         hx_arena_sprintf(c->arena, "REF %s espera un %s", hx_sym_str(inst->name),
                                          hx_ty_name(p->ty->inner)));
        } else if (p->ty) {
            hx_coerce_to(c, &arg->value, p->ty, arg->span, hx_sym_str(inst->name));
        }
    }
    e->fn = inst;
    e->ty = inst->ret;
    e->ret_arg_refs = refs;
    e->n_arg_refs = e->call.args.len;
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
    /* OPERATOR + en un TYPE convierte la expresion binaria en una llamada: el
       resto del compilador no necesita saber nada de esto */
    if (op != OP_AND && op != OP_OR && op != OP_XOR && l && r) {
        const char *key = hx_op_key(c->arena, op, l);
        if (key) {
            HxSym folded = hx_intern_fold_ascii(c->intern, key, strlen(key));
            struct HxFunc *ov = hx_find_func(c, folded);
            if (ov) {
                /* los operandos se copian antes de tocar kind: el resto de la
                   estructura se lee todavia como expresion binaria */
                HxExpr *lhs = e->bin.lhs, *rhs = e->bin.rhs;
                e->kind = EX_CALL;
                e->call.callee = NULL;
                e->call.args.len = 0;
                e->call.args.cap = 0;
                e->call.args.data = NULL;
                HxArg a1, a2;
                memset(&a1, 0, sizeof(a1));
                memset(&a2, 0, sizeof(a2));
                a1.value = lhs;
                a2.value = rhs;
                a1.span = lhs->span;
                a2.span = rhs->span;
                HX_VEC_PUSH(e->call.args, a1);
                HX_VEC_PUSH(e->call.args, a2);
                e->fn = ov;
                if (ov->params.len != 2) {
                    hx_error(c->diags, e->span, "E0214",
                             "OPERATOR %s necesita exactamente 2 parametros",
                             hx_binop_spelling(op));
                } else {
                    hx_coerce_to(c, &a1.value, ov->params.data[0].ty, a1.span,
                                 hx_sym_str(ov->name));
                    hx_coerce_to(c, &a2.value, ov->params.data[1].ty, a2.span,
                                 hx_sym_str(ov->name));
                }
                e->ty = ov->ret ? ov->ret : hx_ty_builtin(c->arena, TY_UNKNOWN);
                return e;
            }
        }
    }
    if (op == OP_CONCAT) {
        if ((l && l->kind != TY_STRING) || (r && r->kind != TY_STRING))
            hx_error(c->diags, e->span, "E0307", "'++' requiere dos operandos STRING");
        e->ty = hx_ty_builtin(c->arena, TY_STRING);
        return e;
    }
    if (is_cmp) {
        if (l && r && (l->kind == TY_PTR || r->kind == TY_PTR)) {
            /* los punteros se comparan por identidad o con 0; ya se ha comprobado */
            e->ty = hx_ty_builtin(c->arena, TY_BOOL);
            return e;
        }
        if (l && r && (hx_ty_rank(l) || hx_ty_enum_like(l)) &&
            (hx_ty_rank(r) || hx_ty_enum_like(r)) && (hx_ty_rank(l) || hx_ty_rank(r)))
            hx_coerce(c, r, l, e->span, NULL);
        int eq_only = op == OP_EQ || op == OP_NE;
        int ok = 1;
        if (l && r) {
            if (op == OP_EQ || op == OP_NE) {
                HxExpr *le = e->bin.lhs, *re = e->bin.rhs;
                int ptr_nulo = (l && l->kind == TY_PTR && re && re->kind == EX_INT &&
                                re->ival == 0) ||
                               (r && r->kind == TY_PTR && le && le->kind == EX_INT &&
                                le->ival == 0);
                if (ptr_nulo) {
                    e->ty = hx_ty_builtin(c->arena, TY_BOOL);
                    return e;
                }
                if (l && l->kind == TY_PTR && r && r->kind == TY_PTR) {
                    e->ty = hx_ty_builtin(c->arena, TY_BOOL);
                    return e;
                }
            }
            int comparable = hx_ty_is_numeric(l) && hx_ty_is_numeric(r);
            /* igualdad entre cadenas y entre booleanos si tiene sentido */
            int same = hx_ty_equal(l, r);
            if (!comparable && !(eq_only && same)) ok = 0;
        }
        if (!ok) {
            hx_error(c->diags, e->span, "E0307",
                     hx_arena_sprintf(c->arena, "'%s' no está definido entre %s y %s",
                                      hx_binop_symbol(op), hx_ty_name(l), hx_ty_name(r)));
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            return e;
        }
        e->ty = hx_ty_builtin(c->arena, TY_BOOL);
        return e;
    }
    if (is_arith && ((l && hx_vec_len(l)) || (r && hx_vec_len(r)))) {
        if (op == OP_MUL && l && hx_vec_len(l) && r && hx_ty_is_numeric(r)) {
            e->ty = l;
            return e;
        }
        if (op == OP_MUL && r && hx_vec_len(r) && l && hx_ty_is_numeric(l)) {
            e->ty = r;
            return e;
        }
        if ((op == OP_ADD || op == OP_SUB) && l && hx_vec_len(l) && r && hx_vec_len(r)) {
            e->ty = l;
            return e;
        }
        hx_error(c->diags, e->span, "E0402",
                 hx_arena_sprintf(c->arena,
                                  "'%s' no está definido entre %s y %s",
                                  hx_binop_symbol(op), hx_ty_name(l), hx_ty_name(r)));
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return e;
    }
    if (op == OP_DIV || op == OP_MOD) {
        HxExpr *div = e->bin.rhs;
        int cero = 0;
        if (div->kind == EX_INT) cero = div->ival == 0;
        else if (div->kind == EX_FLOAT) cero = div->fval == 0.0;
        if (cero)
            hx_error(c->diags, div->span, "E0305",
                     "el divisor de '%s' es cero y el programa no puede dividirse",
                     hx_binop_symbol(op));
    }
    if (is_arith) {
        if (hx_ty_enum_like(l) || hx_ty_enum_like(r)) {
            hx_error(c->diags, e->span, "E0308",
                     "un ENUM no admite aritmética; compara variantes o conviértelo con "
                     "ENUM_A_INT");
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            return e;
        }
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

/* --- iteradores perezosos ------------------------------------------------
   Un iterador es ITER<T>. El compilador reconoce los constructores Rango y
   RangoF y los adaptadores MAP, FILTER, TAKE y FIRST; todos se resuelven a
   tiempo de compilacion y no dejan tabla virtual. */
static HxTy *hx_iter_ty(HxChecker *c, HxTy *elem) {
    HxTy *t = (HxTy *)hx_arena_calloc(c->arena, sizeof(HxTy));
    t->kind = TY_ITER;
    t->elem = elem;
    t->n_targs = 1;
    return t;
}

static struct HxFunc *hx_find_func_named(HxChecker *c, HxSym name) {
    for (int m = 0; m < c->unit->modules.len; m++)
        for (int i = 0; i < c->unit->modules.data[m].funcs.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(c->unit->modules.data[m].funcs.data[i].name),
                                  hx_sym_str(name)))
                return &c->unit->modules.data[m].funcs.data[i];
    return NULL;
}

/* Devuelve 1 si el nombre es un constructor o adaptador de iteradores. */
/* Una FUNC(...) ... END se comprueba como cualquier otra funcion pero con un
   nombre generado, porque en C no hay valores de funcion. */
static struct HxFunc *hx_lambda_func(HxChecker *c, HxExpr *e) {
    if (!e->lit) return NULL;
    struct HxFunc *f = e->lit;
    if (!f->name) {
        char *nm = hx_arena_sprintf(c->arena, "hx_anon_%s_%d", hx_sym_str(c->mod->name),
                                     c->unit->n_lambdas);
        f->name = hx_intern_cstr(c->intern, nm);
        f->module = c->mod->index;
        f->is_export = 0;
        if (c->unit->n_lambdas == c->unit->cap_lambdas) {
            c->unit->cap_lambdas = c->unit->cap_lambdas ? c->unit->cap_lambdas * 2 : 8;
            c->unit->lambdas = (struct HxFunc **)hx_arena_realloc_tmp(
                c->unit->lambdas, sizeof(struct HxFunc *) * (size_t)c->unit->cap_lambdas);
        }
        c->unit->lambdas[c->unit->n_lambdas++] = f;
    }
    hx_resolve_signature(c, f);
    hx_check_func(c, f);
    return f;
}

static int hx_iter_ctor_check(HxChecker *c, HxExpr *e, const char *name, HxExpr *recv) {
    /* un nombre declarado por el programa tiene prioridad sobre el constructor */
    HxSymEntry *declarado = NULL;
    {
        char folded[64];
        size_t k = 0;
        for (const char *p = name; *p && k + 1 < sizeof(folded); p++)
            folded[k++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        folded[k] = 0;
        declarado = hx_lookup(c, hx_intern_cstr(c->intern, folded));
    }
    if (declarado && declarado->kind != SK_MODULE) return 0;
    int is_rango = !hx_ascii_casecmp(name, "Rango");
    int is_rangof = !hx_ascii_casecmp(name, "RangoF");
    int is_map = !hx_ascii_casecmp(name, "Map");
    int is_filter = !hx_ascii_casecmp(name, "Filter");
    int is_take = !hx_ascii_casecmp(name, "Take");
    int is_first = !hx_ascii_casecmp(name, "First");
    if (!is_rango && !is_rangof && !is_map && !is_filter && !is_take && !is_first) return 0;
    e->is_intrin = 4;
    e->method = hx_intern_cstr(c->intern, name);

    if (is_rango || is_rangof) {
        /* El emisor construye el iterador como una declaracion antes del
           bucle. Fuera del FOR no hay donde dejarla, y antes de avisar esto
           reventaba el compilador con `DIM it AS ITER<INT> = Rango(1, 3)`. */
        if (!c->in_forin) {
            hx_error(c->diags, e->span, "E0717",
                     hx_arena_sprintf(c->arena,
                                      "%s sólo se puede usar en el iterable de un FOR", name),
                     "un iterador necesita una variable y una arena, y el emisor las crea "
                     "al abrir el bucle",
                     NULL);
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            return 1;
        }
        if (e->call.args.len != 2) {
            hx_error(c->diags, e->span, "E0306",
                     hx_arena_sprintf(c->arena, "%s espera 2 argumento(s)", name));
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            return 1;
        }
        for (int i = 0; i < 2; i++) {
            e->call.args.data[i].value = hx_expr_check(c, e->call.args.data[i].value);
            hx_coerce(c, e->call.args.data[i].value->ty,
                      hx_ty_builtin(c->arena, is_rangof ? TY_FLOAT : TY_I64),
                      e->call.args.data[i].span, name);
        }
        e->ty = hx_iter_ty(c, hx_ty_builtin(c->arena, is_rangof ? TY_FLOAT : TY_INT));
        return 1;
    }
    if (recv && hx_es_maybe(recv->ty) && !hx_ascii_casecmp(hx_sym_str(e->method), "Map"))
        return hx_maybe_call_check(c, e, recv, "Map");
    if (!recv || !recv->ty || recv->ty->kind != TY_ITER) {
        hx_error(c->diags, e->span, "E0713",
                 hx_arena_sprintf(c->arena, "%s sólo se puede aplicar a un iterador", name));
        e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
        return 1;
    }
    if (is_first) {
        e->recv = recv;
        e->ty = recv->ty->elem ? recv->ty->elem : hx_ty_builtin(c->arena, TY_UNKNOWN);
        return 1;
    }
    if (is_take) {
        if (e->call.args.len != 1) {
            hx_error(c->diags, e->span, "E0306", "TAKE espera 1 argumento");
            e->ty = recv->ty;
            return 1;
        }
        e->call.args.data[0].value = hx_expr_check(c, e->call.args.data[0].value);
        hx_coerce(c, e->call.args.data[0].value->ty, hx_ty_builtin(c->arena, TY_I64),
                  e->call.args.data[0].span, "TAKE");
        e->recv = recv;
        e->ty = recv->ty;
        return 1;
    }
    /* MAP y FILTER reciben una funcion de primer orden */
    if (e->call.args.len != 1) {
        hx_error(c->diags, e->span, "E0306", "%s espera 1 argumento", name);
        e->ty = recv->ty;
        return 1;
    }
    HxExpr *arg = e->call.args.data[0].value;
    struct HxFunc *f = NULL;
    if (arg->kind == EX_FUNC) f = hx_lambda_func(c, arg);
    if (arg->kind == EX_PATH && arg->path.parts.len) {
        HxSym fname = arg->path.parts.data[arg->path.parts.len - 1].name;
        f = hx_find_func_named(c, fname);
        if (f && f->is_generic)
            f = NULL; /* una funcion generica no cabe como valor todavia */
    }
    if (!f) {
        hx_error(c->diags, arg->span, "E0714",
                 hx_arena_sprintf(c->arena, "%s espera el nombre de una funcion", name));
        e->recv = recv;
        e->ty = recv->ty;
        return 1;
    }
    e->call.args.data[0].value = arg;
    e->recv = recv;
    if (is_filter) {
        if (f->params.len != 1 || (f->ret && f->ret->kind != TY_BOOL))
            hx_error(c->diags, arg->span, "E0715",
                     hx_arena_sprintf(c->arena,
                                      "FILTER espera una funcion de %s a BOOL",
                                      hx_ty_name(recv->ty->elem)));
        e->ty = recv->ty;
        return 1;
    }
    if (f->params.len != 1)
        hx_error(c->diags, arg->span, "E0715",
                 hx_arena_sprintf(c->arena, "MAP espera una funcion de un argumento"));
    HxTy *out = f->ret ? f->ret : (recv->ty->elem ? recv->ty->elem : hx_ty_builtin(c->arena, TY_UNKNOWN));
    if (recv->ty->elem && f->params.len == 1 && f->params.data[0].ty &&
        !hx_ty_equal(f->params.data[0].ty, recv->ty->elem))
        hx_error(c->diags, arg->span, "E0715",
                 hx_arena_sprintf(c->arena, "MAP espera una funcion de %s, %s toma %s",
                                  hx_ty_name(recv->ty->elem), hx_sym_str(f->name),
                                  hx_ty_name(f->params.data[0].ty)));
    e->ty = hx_iter_ty(c, out);
    return 1;
}

static int hx_ty_enum_like(const HxTy *t) {
    return t && t->kind == TY_NAMED && t->decl && t->decl->is_enum;
}

/* La conversion de un ENUM a INT y a STRING la resuelve el verificador: hace
   falta el tipo declarado, no solo el valor. */
static int hx_enum_member(HxChecker *c, HxExpr *e, const char *member, HxExpr *recv) {
    if (!hx_ty_enum_like(recv->ty)) return 0;
    int es_ordinal = !hx_ascii_casecmp(member, "Ordinal");
    int es_int = !hx_ascii_casecmp(member, "ENUM_A_INT");
    int es_nombre = !hx_ascii_casecmp(member, "Nombre");
    if (!es_ordinal && !es_int && !es_nombre) return 0;
    e->is_intrin = 5;
    e->method = hx_intern_cstr(c->intern, es_ordinal ? "Ordinal" : es_int ? "ENUM_A_INT" : "Nombre");
    e->recv = recv;
    e->payload_ty = recv->ty;
    e->ty = es_nombre ? hx_ty_builtin(c->arena, TY_STRING) : hx_ty_builtin(c->arena, TY_INT);
    return 1;
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
            /* NIL no tiene tipo propio: lo toma del sitio donde aparece. El
               contexto se comprueba despues, cuando ya se sabe que se espera. */
            e->is_nil = 1;
            e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
        case EX_FUNC: {
            struct HxFunc *f = hx_lambda_func(c, e);
            e->ty = f && f->ret ? f->ret : hx_ty_builtin(c->arena, TY_UNKNOWN);
            return e;
        }
        case EX_DEREF: {
            e->try.inner = hx_expr_check(c, e->try.inner);
            HxTy *pt = e->try.inner->ty;
            if (!pt || pt->kind != TY_PTR)
                hx_error(c->diags, e->span, "E0722",
                         "'^' sólo se puede aplicar a un PTR");
            e->ty = pt && pt->kind == TY_PTR ? pt->inner : hx_ty_builtin(c->arena, TY_UNKNOWN);
            return e;
        }
        case EX_PATH:
            return hx_path_check(c, e);
        case EX_CALL:
            return hx_call_check(c, e);
        case EX_BIN:
            return hx_bin_check(c, e);
        case EX_UN: {
            e->un.operand = hx_expr_check(c, e->un.operand);
            if (e->un.op == UOP_ADDR) {
                HxExpr *o = e->un.operand;
                if (!o || o->kind != EX_PATH || o->path.parts.len != 1)
                    hx_error(c->diags, e->span, "E0720",
                             "'&' sólo se puede aplicar a una variable con nombre");
                else {
                    HxSym folded = hx_intern_fold_ascii(c->intern,
                                                         hx_sym_str(o->path.parts.data[0].name),
                                                         strlen(hx_sym_str(
                                                             o->path.parts.data[0].name)));
                    HxSymEntry *se = hx_lookup(c, folded);
                    if (se && se->arena_depth > c->arena_depth)
                        hx_error(c->diags, e->span, "E0721",
                                 "'&' sobre una variable de ARENA: el puntexto quedaria "
                                 "colgado al salir del bloque");
                }
                HxTy *inner = o && o->ty ? o->ty : hx_ty_builtin(c->arena, TY_UNKNOWN);
                if (inner->kind == TY_REF || inner->kind == TY_PTR) inner = inner->inner;
                HxTy *pt = (HxTy *)hx_arena_calloc(c->arena, sizeof(HxTy));
                pt->kind = TY_PTR;
                pt->inner = inner;
                e->ty = pt;
                return e;
            }
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
            if (e->index.end) {
                /* `a[i..j]` se parseaba y se comprobaba, pero el emisor se
                   comia el final y leia un solo elemento: peor que un error */
                hx_error(c->diags, e->span, "E0210",
                         "un rango en un índice (a[i..j]) todavía no se puede usar: "
                         "aquí solo se leería el primer elemento");
            }
            e->ty = e->index.base->ty && e->index.base->ty->kind == TY_ARRAY
                        ? e->index.base->ty->elem
                        : hx_ty_builtin(c->arena, TY_UNKNOWN);
            break;
        case EX_MEMB: {
            e->member.base = hx_expr_check(c, e->member.base);
            HxTy *bt = e->member.base->ty;
            const char *member = hx_sym_str(e->member.name);
            int vcomp = hx_vec_component(c, bt, member, &e->member.name_span);
            if (vcomp > 0) {
                int mlen = (int)strlen(member);
                e->method = e->member.name;
                e->vec_component = vcomp;
                e->ty = hx_ty_builtin(c->arena,
                                      mlen == 1   ? TY_FLOAT
                                      : mlen == 2 ? TY_VEC2
                                      : mlen == 3 ? TY_VEC3
                                                  : TY_VEC4);
                break;
            }
            if (bt && bt->decl && bt->decl->is_enum && hx_enum_member(c, e, member, e->member.base))
                break;
            if (bt && bt->decl && bt->decl->is_enum &&
                hx_enum_member(c, e, member, e->member.base))
                break;
            if (bt && bt->kind == TY_ARRAY &&
                hx_array_method_check(c, e, e->member.base, member))
                break;
            if (hx_es_maybe(bt)) {
                for (int mi = 0; hx_maybe_metodos[mi]; mi++)
                    if (!hx_ascii_casecmp(hx_maybe_metodos[mi], member)) {
                        if (mi == 0)
                            hx_error(c->diags, e->member.name_span, "E0306",
                                     "IsNil no lleva parentesis: se escribe como un "
                                     "miembro");
                        e->is_intrin = 8;
                        e->method = e->member.name;
                        e->ty = hx_ty_builtin(c->arena, TY_BOOL);
                        break;
                    }
                if (e->is_intrin == 8) break;
            }
            if (bt && bt->decl) {
                for (int fi = 0; fi < bt->decl->fields.len; fi++) {
                    HxField *fld = &bt->decl->fields.data[fi];
                    if (!fld->ty) continue;
                    if (hx_ascii_casecmp(hx_sym_str(fld->name), member)) continue;
                    e->is_intrin = 4;
                    e->method = e->member.name;
                    e->ty = fld->ty;
                    break;
                }
            }
            if (!e->ty || e->ty->kind == TY_UNKNOWN) {
                hx_error(c->diags, e->member.name_span, "E0303",
                         hx_arena_sprintf(c->arena, "'%s' no tiene un miembro llamado '%s'",
                                          hx_ty_name(bt), member));
                e->ty = hx_ty_builtin(c->arena, TY_UNKNOWN);
            }
            break;
        }
        case EX_TRY:
            e->try.inner = hx_expr_check(c, e->try.inner);
            e->ty = e->try.inner->ty;
            break;
        case EX_VEC:
            for (int i = 0; i < e->vec.len; i++)
                e->vec.items[i] = hx_expr_check(c, e->vec.items[i]);
            e->ty = hx_ty_builtin(c->arena,
                                  e->vec.len == 2   ? TY_VEC2
                                  : e->vec.len == 3 ? TY_VEC3
                                  : e->vec.len == 4 ? TY_VEC4
                                                   : TY_UNKNOWN);
            if (e->vec.len == 4 && e->vec.items[3]->ty &&
                e->vec.items[3]->ty->kind == TY_FLOAT)
                e->ty = hx_ty_builtin(c->arena, TY_QUAT);
            for (int i = 0; i < e->vec.len; i++) {
                HxTy *ct = e->vec.items[i]->ty;
                if (ct && ct->kind != TY_FLOAT && ct->kind != TY_INT &&
                    ct->kind != TY_UNKNOWN)
                    hx_error(c->diags, e->vec.items[i]->span, "E0402",
                             "un literal de vector sólo admite FLOAT o INT");
            }
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
            if (t->decl) return t; /* ya resuelto (puede ser una instancia) */
            if (t->name && !hx_ascii_casecmp(hx_sym_str(t->name), "SELF")) {
                if (!c->cur_self) {
                    if (report)
                        hx_error(c->diags, sp, "E0706",
                                 "SELF sólo se puede usar dentro de una implementación de TRAIT");
                    t->kind = TY_UNKNOWN;
                    return t;
                }
                return c->cur_self;
            }
            if (!hx_ascii_casecmp(hx_sym_str(t->name), "Result")) {
                t->elem = hx_resolve_type(c, t->elem, sp, report);
                t->inner = hx_resolve_type(c, t->inner, sp, report);
                return t;
            }
            if (!hx_ascii_casecmp(hx_sym_str(t->name), "MAYBE")) {
                /* MAYBE T: un valor o nada. T -> MAYBE T es una conversion
                   implicita, al reves que deshacerla no. */
                t->elem = hx_resolve_type(c, t->elem, sp, report);
                if (!t->elem) t->elem = hx_ty_builtin(c->arena, TY_UNKNOWN);
                t->kind = TY_MAYBE;
                return t;
            }
            HxTy *b = hx_ty_lookup_builtin(c->arena, t->name);
            if (b) {
                t->kind = b->kind;
                return t;
            }
            HxTypeDecl *d = hx_find_type(c, t->name);
            if (d && d->n_tparams) {
                int deferred = 0;
                for (int i = 0; i < d->n_tparams && !deferred; i++) {
                    HxTy *ta = i == 0 ? t->elem : t->inner;
                    if (ta && ta->kind == TY_NAMED && ta->name)
                        for (int k = 0; k < c->n_cur_tparams; k++)
                            if (c->cur_tparams[k] == ta->name) deferred = 1;
                }
                if (deferred) return t; /* se resolvera en cada instancia */
                if (t->n_targs != d->n_tparams) {
                    if (report)
                        hx_error(c->diags, sp, "E0705",
                                 hx_arena_sprintf(c->arena, "'%s' espera %d argumento(s) de tipo",
                                                  hx_sym_str(d->name), d->n_tparams));
                    t->kind = TY_UNKNOWN;
                    return t;
                }
                HxTy *targs[HX_MAX_TPARAMS];
                for (int i = 0; i < d->n_tparams; i++)
                    targs[i] = hx_resolve_type(c, i == 0 ? t->elem : t->inner, sp, report);
                HxTypeDecl *inst = hx_type_instantiate(c->arena, c->intern, c->unit, d, targs,
                                                      d->n_tparams, c->diags, sp);
                for (int i = 0; i < inst->fields.len; i++)
                    inst->fields.data[i].ty =
                        hx_resolve_type(c, inst->fields.data[i].ty, sp, 0);
                /* el nodo conserva el nombre escrito por quien programa: la
                   instancia vive en decl, y asi una firma generica clonada
                   vuelve a instanciar igual que la original */
                t->decl = inst;
                return t;
            }
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
        case TY_MAYBE:
            t->elem = hx_resolve_type(c, t->elem, sp, report);
            return t;
        case TY_REF:
        case TY_PTR:
            t->inner = hx_resolve_type(c, t->inner, sp, report);
            return t;
        case TY_ITER:
            t->elem = hx_resolve_type(c, t->elem, sp, report);
            return t;
        /* estos no llevan nada que resolver: se listan uno a uno para que un
           kind nuevo avise al compilar en vez de colarse por aqui */
        case TY_UNKNOWN:
        case TY_VOID:
        case TY_BOOL:
        case TY_INT:
        case TY_I64:
        case TY_FLOAT:
        case TY_STRING:
        case TY_DURATION:
        case TY_VEC2:
        case TY_VEC3:
        case TY_VEC4:
        case TY_MAT4:
        case TY_QUAT: return t;
    }
    return t;
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
            HxSym folded = hx_fold(c, s->dim.name);
            if (hx_declared_here(c, folded))
                hx_error(c->diags, s->dim.name_span, "E0315",
                         "'%s' ya está declarado en este ámbito", hx_sym_str(s->dim.name));
            hx_define(c, folded, t ? t : hx_ty_builtin(c->arena, TY_INT), SK_VAR,
                      s->dim.name_span);
        }
    }
}

static void hx_check_body(HxChecker *c, HxStmtVec *body);

static void hx_define_pattern_binding(HxChecker *c, HxSym name, HxTy *ty, HxSpan sp) {
    HxSym folded = hx_fold(c, name);
    hx_define(c, folded, ty, SK_VAR, sp);
}

static int hx_body_defer(HxStmtVec *body) {
    for (int i = 0; i < body->len; i++)
        if (body->data[i].kind == ST_DEFER) return 1;
    return 0;
}

static void hx_check_pattern(HxChecker *c, HxPattern *pat, HxTy *subj) {
    if (!pat) return;
    /* En un MATCH sobre un ENUM, un nombre desnudo es una variante si existe;
       si no, sigue siendo un binding. */
    if (subj && hx_ty_enum_like(subj) && subj->decl && pat->kind == PAT_BIND && pat->name) {
        for (int i = 0; i < subj->decl->fields.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(subj->decl->fields.data[i].name),
                                  hx_sym_str(pat->name))) {
                pat->kind = PAT_CONSTRUCTOR;
                pat->ctor = subj->decl->fields.data[i].name;
                break;
            }
    }
    switch (pat->kind) {
        case PAT_NIL:
            /* CASE NIL solo tiene sentido sobre un MAYBE: es la forma de
               preguntar por el Maybe sin operadores nuevos */
            if (!hx_es_maybe(subj))
                hx_error(c->diags, pat->span, "E0211",
                         "CASE NIL necesita un MATCH sobre un MAYBE (aqui el sujeto es '%s')",
                         hx_ty_name(subj));
            break;
        case PAT_BIND:
            if (hx_es_maybe(subj))
                hx_define_pattern_binding(c, pat->name, subj->elem, pat->span);
            else
                hx_define_pattern_binding(c, pat->name, subj, pat->span);
            break;
        case PAT_LITERAL:
            pat->lit = hx_expr_check(c, pat->lit);
            /* sobre un MAYBE, el literal se compara con el valor de dentro */
            {
                HxTy *cmp = hx_es_maybe(subj) ? subj->elem : subj;
                if (pat->lit->ty && cmp && !hx_ty_equal(pat->lit->ty, cmp))
                    hx_error(c->diags, pat->lit->span, "E0406",
                             "el patrón no puede ser %s aquí", hx_ty_name(pat->lit->ty));
            }
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
            /* no se puede escribir en un miembro de un temporal: `f(a).v = 1`
               no tiene a donde escribir */
            if (s->assign.target && s->assign.target->kind == EX_MEMB) {
                HxExpr *base = s->assign.target->member.base;
                if (base && base->kind != EX_PATH && base->kind != EX_INDEX &&
                    base->kind != EX_DEREF)
                    hx_error(c->diags, s->assign.target->span, "E0723",
                             "no se puede asignar a un miembro de un valor temporal");
            }
        } {
            s->assign.target = hx_expr_check(c, s->assign.target);
            s->assign.value = hx_expr_propagate(c, s->assign.value, s->assign.value->span);
            if (s->assign.compound && s->assign.op == OP_ADD && s->assign.target->ty &&
                s->assign.target->ty->kind == TY_STRING)
                hx_error(c->diags, s->span, "E0309",
                         "'+' no concatena cadenas; usa '++' para STRING");
            if (s->assign.target->ty && s->assign.value->ty)
                hx_coerce_to(c, &s->assign.value, s->assign.target->ty, s->assign.value->span,
                          NULL);
            break;
        }
        case ST_DIM: {
            HxTy *t = hx_resolve_type(c, s->dim.ty, s->dim.name_span, 1);
            if (s->dim.init) {
                s->dim.init = hx_expr_propagate(c, s->dim.init, s->dim.init->span);
                if (s->dim.init->ty && s->dim.init->ty->kind == TY_ARRAY && (!t || t->kind == TY_UNKNOWN))
                    t = s->dim.init->ty;
                if (t && t != s->dim.init->ty)
                    hx_coerce_to(c, &s->dim.init, t, s->dim.init->span, NULL);
                if (!t) t = s->dim.init->ty;
            }
            if (!t) t = hx_ty_builtin(c->arena, TY_INT);
            s->dim.ty = t;
            HxSym folded = hx_fold(c, s->dim.name);
            if (!c->body_hoisted && hx_declared_here(c, folded)) {
                hx_error(c->diags, s->dim.name_span, "E0315",
                         "'%s' ya está declarado en este ámbito", hx_sym_str(s->dim.name));
                break;
            }
            hx_define(c, folded, t, SK_VAR, s->dim.name_span);
            if (c->arena_depth && t && t->kind == TY_ARRAY) c->scope->syms.data[c->scope->syms.len - 1].arena_depth = c->arena_depth;
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
            HxSym folded = hx_fold(c, s->konst.name);
            if (!c->body_hoisted && hx_declared_here(c, folded)) {
                hx_error(c->diags, s->konst.name_span, "E0315",
                         "'%s' ya está declarado en este ámbito", hx_sym_str(s->konst.name));
                break;
            }
            hx_define(c, folded, s->konst.ty, SK_CONST, s->konst.name_span);
            break;
        }
        case ST_PRINT:
            for (int i = 0; i < s->print.items.len; i++) {
                HxPrintItem *it = &s->print.items.data[i];
                if (it->expr) {
                    it->expr = hx_expr_propagate(c, it->expr, it->expr->span);
                    if (it->expr->is_nil && (!it->expr->ty || it->expr->ty->kind != TY_MAYBE))
                        hx_error(c->diags, it->expr->span, "E0211",
                                 "PRINT no puede imprimir NIL: no hay ningun valor");
                    HxTy *t = it->expr->ty;
                    if (t && t->kind != TY_STRING && t->kind != TY_INT && t->kind != TY_I64 &&
                        t->kind != TY_FLOAT && t->kind != TY_BOOL && t->kind != TY_DURATION &&
                        t->kind != TY_UNKNOWN && !hx_vec_len(t) && t->kind != TY_MAT4 &&
                        t->kind != TY_QUAT && !hx_ty_enum_like(t))
                        hx_error(c->diags, it->expr->span, "E0311",
                                 "PRINT no admite valores de ese tipo");
                }
            }
            break;
        case ST_IF: {
            int had_defer = hx_body_defer(&s->if_.then) || s->if_.has_else;
            if (had_defer) c->defer_depth++;
            {
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
            }
            if (had_defer) c->defer_depth--;
            break;
        }
        case ST_WHILE:
            if (hx_body_defer(&s->while_.body)) c->defer_depth++;
            {
            hx_scope_push(c);
            s->while_.cond = hx_expr_check(c, s->while_.cond);
            if (s->while_.cond->ty && s->while_.cond->ty->kind != TY_BOOL)
                hx_error(c->diags, s->while_.cond->span, "E0312",
                         "la condición de WHILE debe ser BOOL");
            c->loop_depth++;
            c->loop_defer_depth = c->defer_depth;
            hx_check_body(c, &s->while_.body);
            c->loop_depth--;
            hx_scope_pop(c);
            }
            if (hx_body_defer(&s->while_.body)) c->defer_depth--;
            break;
        case ST_FOR:
            if (hx_body_defer(&s->for_.body)) c->defer_depth++;
            {
            hx_scope_push(c);
            s->for_.start = hx_expr_check(c, s->for_.start);
            s->for_.end = hx_expr_check(c, s->for_.end);
            if (s->for_.step) s->for_.step = hx_expr_check(c, s->for_.step);
            HxTy *vt = hx_ty_builtin(c->arena, TY_INT);
            if (s->for_.start && s->for_.start->ty) vt = s->for_.start->ty;
            HxSym folded = hx_fold(c, s->for_.var);
            hx_define(c, folded, vt, SK_VAR, s->for_.var_span);
            c->loop_depth++;
            c->loop_defer_depth = c->defer_depth;
            hx_check_body(c, &s->for_.body);
            c->loop_depth--;
            hx_scope_pop(c);
            }
            if (hx_body_defer(&s->for_.body)) c->defer_depth--;
            break;
        case ST_RETURN:
            if (s->ret.value) {
                s->ret.value = hx_expr_propagate(c, s->ret.value, s->ret.value->span);
                /* Err(e) toma el tipo del valor de exito de la firma: el
                   Ok concreto lo decide el tipo de retorno de la funcion */
                if (c->ret_ty && hx_ty_is_result(c->ret_ty) && s->ret.value->is_err_ctor &&
                    hx_ty_is_result(s->ret.value->ty)) {
                    HxTy *want = c->ret_ty->elem;
                    hx_coerce(c, s->ret.value->ty->inner, c->ret_ty->inner,
                              s->ret.value->span, NULL);
                    s->ret.value->payload_ty = want;
                    s->ret.value->ty = c->ret_ty;
                } else if (c->ret_ty) {
                    hx_coerce_to(c, &s->ret.value, c->ret_ty, s->ret.value->span, NULL);
                }
            }
            break;
        case ST_FORIN: {
            c->in_forin++;
            s->forin_.iter = hx_expr_check(c, s->forin_.iter);
            c->in_forin--;
            HxTy *elem = NULL;
            if (s->forin_.iter->ty && s->forin_.iter->ty->kind == TY_ITER)
                elem = s->forin_.iter->ty->elem;
            else
                hx_error(c->diags, s->span, "E0712",
                         "FOR ... IN espera un iterador construido con Rango");
            if (!elem) elem = hx_ty_builtin(c->arena, TY_UNKNOWN);
            hx_define(c, hx_fold(c, s->forin_.var),
                      elem, SK_VAR, s->forin_.var_span);
            int saved_loop = c->loop_depth;
            int saved_defer = c->defer_depth;
            int saved_loop_defer = c->loop_defer_depth;
            c->loop_depth++;
            c->loop_defer_depth = c->defer_depth;
            hx_check_body(c, &s->forin_.body);
            c->loop_depth = saved_loop;
            c->defer_depth = saved_defer;
            c->loop_defer_depth = saved_loop_defer;
            break;
        }
        case ST_BREAK:
        case ST_CONTINUE:
            if (!c->loop_depth)
                hx_error(c->diags, s->span, "E0313",
                         "BREAK y CONTINUE sólo pueden aparecer dentro de un bucle");
            else if (c->defer_depth > c->loop_defer_depth)
                hx_error(c->diags, s->span, "E0407",
                         "BREAK o CONTINUE no puede saltar por encima de un DEFER");
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
            if (!s->inner.len) {
                hx_error(c->diags, s->span, "E0401", "DEFER requiere una sentencia");
                break;
            }
            hx_scope_push(c);
            hx_check_body(c, &s->inner);
            hx_scope_pop(c);
            break;
        case ST_MATCH: {
            {
                int md = 0;
                for (int k = 0; k < s->match.cases.len; k++)
                    if (hx_body_defer(&s->match.cases.data[k].body)) md = 1;
                if (md) c->defer_depth++;
                c->match_defer = md;
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
            } else if (hx_es_maybe(subj)) {
                /* un MAYBE esta cubierto con CASE NIL y cualquier caso que
                   enganche el valor (binding, literal o comodin) */
                int nil = 0, otro = 0;
                for (int i = 0; i < s->match.cases.len; i++) {
                    HxPatKind k = s->match.cases.data[i].pattern->kind;
                    if (k == PAT_NIL) nil = 1;
                    else if (k == PAT_WILDCARD || k == PAT_BIND || k == PAT_LITERAL) otro = 1;
                }
                if (!nil || !otro)
                    hx_error(c->diags, s->span, "E0405",
                             "un MATCH sobre MAYBE necesita CASE NIL y un caso para el "
                             "valor (por ejemplo un binding)");
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
                /* un ENUM se considera cubierto cuando aparecen todas sus
                   variantes, igual que un Result con Ok y Err */
                int enum_cubierto = 0;
                if (subj && hx_ty_enum_like(subj) && subj->decl) {
                    enum_cubierto = 1;
                    for (int f = 0; f < subj->decl->fields.len && enum_cubierto; f++) {
                        int visto = 0;
                        for (int i = 0; i < s->match.cases.len; i++) {
                            HxPattern *pt = s->match.cases.data[i].pattern;
                            if (pt->kind == PAT_WILDCARD || pt->kind == PAT_BIND) {
                                visto = 1;
                                break;
                            }
                            HxSym nm = pt->kind == PAT_CONSTRUCTOR ? pt->ctor
                                                                   : (pt->name ? pt->name : NULL);
                            if (nm && !hx_ascii_casecmp(hx_sym_str(nm),
                                                        hx_sym_str(subj->decl->fields.data[f].name)))
                                visto = 1;
                        }
                        if (!visto) enum_cubierto = 0;
                    }
                }
                if (!((hx_ty_is_result(subj) && covered_ok && covered_err) || enum_cubierto))
                    hx_error(c->diags, s->span, "E0405",
                             "MATCH exige CASE ELSE o cubrir todos los casos de Ok y Err");
            }
            hx_scope_pop(c);
                if (c->match_defer) c->defer_depth--;
            }
            break;
        }
        case ST_NOP: break;
    }
}

static void hx_check_body(HxChecker *c, HxStmtVec *body) {
    for (int i = 0; i < body->len; i++) {
        for (int k = 0; k < c->scope->syms.len; k++) c->scope->syms.data[k].borrowed = 0;
        hx_check_stmt(c, &body->data[i]);
        if (c->diags->errors > c->diags->max_errors) break;
    }
}

static void hx_analyze_tail(struct HxFunc *f) {
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

static void hx_check_func(HxChecker *c, struct HxFunc *f) {
    hx_scope_push(c);
    struct HxFunc *prev = c->cur_func;
    HxTy *prev_ret = c->ret_ty;
    int prev_loop = c->loop_depth;
    HxSym prev_tps[HX_MAX_TPARAMS];
    memcpy(prev_tps, c->cur_tparams, sizeof(prev_tps));
    int prev_ntps = c->n_cur_tparams;
    memcpy(c->cur_tparams, f->tparams, sizeof(c->cur_tparams));
    c->n_cur_tparams = f->n_tparams;
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
        HxSym folded = hx_fold(c, prm->name);
        hx_define(c, folded, prm->ty, SK_VAR, prm->span);
    }
    c->ret_ty = f->ret;
    if (c->ret_ty && c->ret_ty->kind == TY_VOID) c->ret_ty = NULL;
    hx_check_body(c, &f->body);
    hx_analyze_tail(f);
    c->cur_func = prev;
    c->ret_ty = prev_ret;
    c->loop_depth = prev_loop;
    memcpy(c->cur_tparams, prev_tps, sizeof(prev_tps));
    c->n_cur_tparams = prev_ntps;
    hx_scope_pop(c);
}

static void hx_define_module_scope(HxChecker *c, HxModule *mod) {
    for (int mi = 0; mi < mod->imports.len; mi++) {
        HxImport *im = &mod->imports.data[mi];
        const char *ip = hx_sym_str(im->path);
        const char *dot = strrchr(ip, '.');
        const char *base = dot ? dot + 1 : ip;
        HxSym as = im->alias ? im->alias : hx_intern_cstr(c->intern, base);
        hx_define(c, hx_fold(c, as),
                  hx_ty_builtin(c->arena, TY_VOID), SK_MODULE, im->span);
    }
}

static void hx_collect_names(HxChecker *c, HxModule *mod) {
    HxArena *a = c->arena;
    for (int i = 0; i < mod->types.len; i++) {
        HxSym folded = hx_fold(c, mod->types.data[i].name);
        hx_define(c, folded, NULL, SK_TYPE, mod->types.data[i].span);
    }
    for (int i = 0; i < mod->funcs.len; i++) {
        HxSym folded = hx_fold(c, mod->funcs.data[i].name);
        hx_define(c, folded, mod->funcs.data[i].ret, SK_FUNC, mod->funcs.data[i].span);
    }
    for (int i = 0; i < mod->consts.len; i++) {
        HxSym folded = hx_fold(c, mod->consts.data[i].name);
        hx_define(c, folded, mod->consts.data[i].ty, SK_CONST, mod->consts.data[i].span);
    }
    (void)a;
}

static void hx_resolve_signature(HxChecker *c, struct HxFunc *fn) {
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
        for (int k = 0; k < mod->caps.len; k++) {
            const char *cap = hx_sym_str(mod->caps.data[k].name);
            int ya = 0;
            for (int q = 0; q < c.n_caps; q++)
                if (!hx_ascii_casecmp(c.caps[q], cap)) ya = 1;
            if (!ya && c.n_caps < 16) c.caps[c.n_caps++] = cap;
        }
        for (int t = 0; t < mod->types.len; t++) {
            if (mod->types.data[t].n_tparams) {
                mod->types.data[t].is_generic = 1;
                continue;
            }
            if (mod->types.data[t].is_enum) {
                /* ENUM Color / ROJO / VERDE: cada variante es una constante
                   tipada que vale su posición. Se definen como
                   Color_ROJO para poderles dar nombre propio en C. */
                HxTypeDecl *ed = &mod->types.data[t];
                for (int i = 0; i < ed->fields.len; i++) {
                    HxConst kc;
                    memset(&kc, 0, sizeof(kc));
                    kc.is_export = ed->is_export;
                    kc.name = hx_intern_cstr(
                        c.intern, hx_arena_sprintf(c.arena, "%s_%s", hx_sym_str(ed->name),
                                                  hx_sym_str(ed->fields.data[i].name)));
                    HxExpr *v = (HxExpr *)hx_arena_calloc(c.arena, sizeof(HxExpr));
                    v->kind = EX_INT;
                    v->ival = i;
                    v->span = ed->fields.data[i].span;
                    kc.value = v;
                    HxTy *et = (HxTy *)hx_arena_calloc(c.arena, sizeof(HxTy));
                    et->kind = TY_NAMED;
                    et->name = ed->name;
                    et->decl = ed;
                    kc.ty = et;
                    v->ty = et;
                    kc.span = ed->fields.data[i].span;
                    HX_VEC_PUSH(mod->consts, kc);
                }
                continue;
            }
            for (int i = 0; i < mod->types.data[t].fields.len; i++) {
                HxField *fld = &mod->types.data[t].fields.data[i];
                fld->ty = hx_resolve_type(&c, fld->ty, fld->span, 1);
            }
        }
        for (int i = 0; i < mod->impls.len; i++) {
            HxImplDecl *im = &mod->impls.data[i];
            HxTraitDecl *tr = hx_find_trait(&c, im->trait_name);
            if (!tr) {
                hx_error(unit->diags, im->span, "E0711",
                         hx_arena_sprintf(c.arena, "no existe el TRAIT '%s'",
                                          hx_sym_str(im->trait_name)));
                continue;
            }
            for (int j = 0; j < tr->methods.len; j++) {
                HxSym want = tr->methods.data[j];
                int found = 0;
                for (int k = 0; k < im->methods.len; k++)
                    if (!hx_ascii_casecmp(hx_sym_str(im->methods.data[k].name), hx_sym_str(want)))
                        found = 1;
                if (!found)
                    hx_error(unit->diags, im->span, "E0707",
                             hx_arena_sprintf(c.arena,
                                              "%s no implementa el METODO %s.%s",
                                              hx_sym_str(im->type_name), hx_sym_str(tr->name),
                                              hx_sym_str(want)));
            }
            HxTy *self_ty = hx_ty_builtin(c.arena, TY_UNKNOWN);
            {
                HxTy *probe = (HxTy *)hx_arena_calloc(c.arena, sizeof(HxTy));
                probe->kind = TY_NAMED;
                probe->name = im->type_name;
                self_ty = hx_resolve_type(&c, probe, im->span, 1);
            }
            for (int j = 0; j < im->methods.len; j++) {
                struct HxFunc *f = &im->methods.data[j];
                c.cur_self = self_ty;
                hx_resolve_signature(&c, f);
                c.cur_self = NULL;
                f->is_export = 1; /* las implementaciones se emiten con nombre propio */
                char *mangled = hx_arena_sprintf(
                    c.arena, "%s__%s__%s__%s", hx_sym_str(mod->name), hx_sym_str(im->type_name),
                    hx_sym_str(im->trait_name), hx_sym_str(f->name));
                f->name = hx_intern_cstr(c.intern, mangled);
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
        for (int f = 0; f < mod->funcs.len; f++) {
            struct HxFunc *fn = &mod->funcs.data[f];
            if (fn->n_tparams) {
                fn->is_generic = 1;
                fn->module = m;
                continue; /* la firma se resuelve por instancia */
            }
            hx_resolve_signature(&c, fn);
            /* `OPERATOR +` no puede llamarse "+" en C: se renombra con la
               palabra del operador y el tipo del primer parametro */
            if (hx_es_operador(fn->name)) {
                const char *signo = hx_sym_str(fn->name);
                const char *palabra = hx_op_palabra(signo);
                HxTy *pt = fn->params.len ? fn->params.data[0].ty : NULL;
                if (!palabra) {
                    /* "%s" explicito: un nombre de operador puede ser "%" y
                       pasarlo como formato hace que vsprintf se coma */
                    hx_error(c.diags, fn->span, "E0213", "'%s' no se puede sobrecargar", signo);
                } else if (!pt || pt->kind != TY_NAMED) {
                    hx_error(c.diags, fn->span, "E0215",
                             "OPERATOR %s necesita un primer parametro de un TYPE declarado "
                             "(no %s)",
                             signo, hx_ty_name(pt));
                } else {
                    char *nombre = hx_arena_sprintf(c.arena, "op_%s__%s", palabra,
                                                    hx_ty_name(pt));
                    fn->name = hx_intern_cstr(c.intern, nombre);
                    hx_define(&c, hx_fold(&c, fn->name), fn->ret, SK_FUNC, fn->span);
                }
            }
        }
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
        for (int f = 0; f < mod->funcs.len; f++) {
            struct HxFunc *ovf = &mod->funcs.data[f];
            if (!ovf->is_operator || !ovf->is_generic) continue;
            hx_define(&c, hx_fold(&c, ovf->name), ovf->ret, SK_FUNC, ovf->span);
        }
        for (int f = 0; f < mod->funcs.len; f++)
            if (!mod->funcs.data[f].n_tparams) hx_check_func(&c, &mod->funcs.data[f]);
        for (int i = 0; i < mod->impls.len; i++) {
            HxImplDecl *im = &mod->impls.data[i];
            HxTy *self_ty = (HxTy *)hx_arena_calloc(c.arena, sizeof(HxTy));
            self_ty->kind = TY_NAMED;
            self_ty->name = im->type_name;
            self_ty = hx_resolve_type(&c, self_ty, im->span, 0);
            for (int j = 0; j < im->methods.len; j++) {
                c.cur_self = self_ty;
                hx_check_func(&c, &im->methods.data[j]);
                c.cur_self = NULL;
            }
        }
        hx_scope_push(&c);
        hx_decl_locals(&c, &mod->top);
        c.body_hoisted = 1;
        hx_check_body(&c, &mod->top);
        c.body_hoisted = 0;
        hx_scope_pop(&c);
        hx_scope_pop(&c);
    }
    return unit->diags->errors == before;
}
