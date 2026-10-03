#include "hx/ast.h"

#include <string.h>

static const struct {
    const char *name;
    HxTyKind kind;
} hx_builtin_types[] = {
    {"VOID", TY_VOID},       {"BOOL", TY_BOOL},       {"INT", TY_INT},
    {"I64", TY_I64},         {"FLOAT", TY_FLOAT},     {"STRING", TY_STRING},
    {"DURATION", TY_DURATION}, {"VEC2", TY_VEC2},     {"VEC3", TY_VEC3},
    {"VEC4", TY_VEC4},       {"MAT4", TY_MAT4},       {"QUAT", TY_QUAT},
    {"UNKNOWN", TY_UNKNOWN},
    {"ITER", TY_ITER},
    {NULL, TY_UNKNOWN},
};

const char *hx_ty_name(const HxTy *t) {
    if (!t) return "?";
    switch (t->kind) {
        case TY_VOID: return "VOID";
        case TY_BOOL: return "BOOL";
        case TY_INT: return "INT";
        case TY_I64: return "I64";
        case TY_FLOAT: return "FLOAT";
        case TY_STRING: return "STRING";
        case TY_DURATION: return "DURATION";
        case TY_ARRAY: return "ARRAY";
        case TY_REF: return "REF";
        case TY_PTR: return "PTR";
        case TY_VEC2: return "vec2";
        case TY_VEC3: return "vec3";
        case TY_VEC4: return "vec4";
        case TY_MAT4: return "mat4";
        case TY_QUAT: return "quat";
        case TY_ITER: return "ITER";
        case TY_NAMED: return t->name ? hx_sym_str(t->name) : "?";
        default: return "?";
    }
}

HxTy *hx_ty_builtin(HxArena *a, HxTyKind kind) {
    HxTy *t = (HxTy *)hx_arena_calloc(a, sizeof(HxTy));
    t->kind = kind;
    return t;
}

/* Subtipado estructural: un registro con mas campos sirve donde se pide uno con
   menos, siempre que los campos comunes sean del mismo tipo. Los campos son
   invariantes porque se puede escribir a traves de REF. */
static int hx_field(const HxTypeDecl *d, const char *name) {
    if (!d) return -1;
    for (int i = 0; i < d->fields.len; i++)
        if (!hx_ascii_casecmp(hx_sym_str(d->fields.data[i].name), name)) return i;
    return -1;
}

int hx_ty_subtype(const HxTy *from, const HxTy *to) {
    if (!from || !to) return 0;
    if (hx_ty_equal(from, to)) return 1;
    if (from->kind != TY_NAMED || to->kind != TY_NAMED) return 0;
    if (hx_ty_is_result(from) || hx_ty_is_result(to)) return 0;
    if (!from->decl || !to->decl) return 0;
    if (from->decl->is_enum || to->decl->is_enum) return 0;
    for (int i = 0; i < to->decl->fields.len; i++) {
        const char *fname = hx_sym_str(to->decl->fields.data[i].name);
        int j = hx_field(from->decl, fname);
        if (j < 0) return 0;
        if (!hx_ty_equal(from->decl->fields.data[j].ty, to->decl->fields.data[i].ty)) return 0;
    }
    return 1;
}

int hx_ty_equal(const HxTy *a, const HxTy *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TY_NAMED: {
            int same = a->name == b->name || (a->name && b->name &&
                                               !strcmp(hx_sym_str(a->name), hx_sym_str(b->name)));
            if (!same) return 0;
            int ra = a->name && !hx_ascii_casecmp(hx_sym_str(a->name), "Result");
            int rb = b->name && !hx_ascii_casecmp(hx_sym_str(b->name), "Result");
            if (ra || rb) {
                if (!hx_ty_equal(a->elem, b->elem)) return 0;
                return hx_ty_equal(a->inner, b->inner);
            }
            if (!a->n_targs && !b->n_targs) return 1;
            if (a->n_targs != b->n_targs) return 0;
            if (!hx_ty_equal(a->elem, b->elem)) return 0;
            if (a->n_targs > 1 && !hx_ty_equal(a->inner, b->inner)) return 0;
            return 1;
        }
        case TY_ARRAY: return a->size == b->size && hx_ty_equal(a->elem, b->elem);
        case TY_VEC2:
        case TY_VEC3:
        case TY_VEC4:
        case TY_MAT4:
        case TY_QUAT: return 1;
        case TY_REF:
        case TY_PTR: return hx_ty_equal(a->inner, b->inner);
        default: return 1;
    }
}

HxTy *hx_ty_lookup_builtin(HxArena *a, HxSym name) {
    if (!name) return NULL;
    const char *s = hx_sym_str(name);
    for (int i = 0; hx_builtin_types[i].name; i++) {
        if (!hx_ascii_casecmp(s, hx_builtin_types[i].name))
            return hx_ty_builtin(a, hx_builtin_types[i].kind);
    }
    return NULL;
}

int hx_ty_is_result(const HxTy *t) {
    return t && t->kind == TY_NAMED && !hx_ascii_casecmp(hx_sym_str(t->name), "Result");
}

int hx_ty_is_numeric(const HxTy *t) {
    return t && (t->kind == TY_INT || t->kind == TY_I64 || t->kind == TY_FLOAT ||
                 t->kind == TY_DURATION);
}

int hx_ty_is_scalar(const HxTy *t) { return t && t->kind != TY_VOID; }

int hx_ty_rank(const HxTy *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_BOOL: return 1;
        case TY_INT: return 2;
        case TY_I64: return 3;
        case TY_FLOAT: return 4;
        case TY_DURATION: return 3;
        default: return 0;
    }
}

const char *hx_binop_symbol(HxBinOp op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_MOD: return "MOD";
        case OP_CONCAT: return "++";
        case OP_ADDW: return "+%";
        case OP_SUBW: return "-%";
        case OP_ADDS: return "+|";
        case OP_SUBS: return "-|";
        case OP_MULS: return "*|";
        case OP_EQ: return "==";
        case OP_NE: return "<>";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_AND: return "AND";
        case OP_OR: return "OR";
        case OP_XOR: return "XOR";
    }
    return "?";
}

const char *hx_binop_spelling(HxBinOp op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_CONCAT: return "++";
        case OP_ADDW: return "+%";
        case OP_SUBW: return "-%";
        case OP_ADDS: return "+|";
        case OP_SUBS: return "-|";
        case OP_MULS: return "*|";
        default: return hx_binop_symbol(op);
    }
}