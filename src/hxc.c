#include "hx/hxc.h"

#include <stdlib.h>
#include <string.h>

/* Formato:
     magic "HXCU" | formato u32 | abi u32 | flags u32 | hash del fuente u64
     longitud del cuerpo u64 | hash del cuerpo u64
     nombre del módulo (u32 + bytes)
     tipos:    u32 n, por tipo: nombre, u32 n_campos, (nombre, tipo)
     constantes: u32 n, por constante: nombre, tipo, valor (etiqueta + datos)
     funciones: u32 n, por función: nombre, u32 n_params,
                 (nombre, tipo, u8 flags), tipo de retorno
   Todos los enteros en little-endian, todos los desplazados dentro del búfer. */

#define HXCU_MAGIC "HXCU"
#define HXCU_FORMAT 1u
#define HXCU_ABI 1u
#define HXCU_HEADER 40

enum {
    T_VOID = 0,
    T_BOOL,
    T_INT,
    T_I64,
    T_FLOAT,
    T_STRING,
    T_DURATION,
    T_VEC2,
    T_VEC3,
    T_VEC4,
    T_MAT4,
    T_QUAT,
    T_REF,
    T_PTR,
    T_ARRAY,
    T_RESULT,
    T_NAMED,
    T_MAX
};

typedef struct {
    HxBuf b;
    int err;
} W;

static void w_u8(W *w, unsigned v) {
    char c = (char)v;
    hx_buf_put(&w->b, &c, 1);
}

static void w_u32(W *w, uint32_t v) {
    char t[4];
    t[0] = (char)(v & 0xff);
    t[1] = (char)((v >> 8) & 0xff);
    t[2] = (char)((v >> 16) & 0xff);
    t[3] = (char)((v >> 24) & 0xff);
    hx_buf_put(&w->b, t, 4);
}

static void w_u64(W *w, uint64_t v) {
    char t[8];
    for (int i = 0; i < 8; i++) t[i] = (char)((v >> (i * 8)) & 0xff);
    hx_buf_put(&w->b, t, 8);
}

static void w_str(W *w, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    w_u32(w, n);
    if (n) hx_buf_put(&w->b, s, n);
}

static void w_sym(W *w, HxSym s) { w_str(w, s ? hx_sym_str(s) : ""); }

static void w_ty(W *w, HxTy *t) {
    if (!t) {
        w_u8(w, T_VOID);
        return;
    }
    switch (t->kind) {
        case TY_VOID: w_u8(w, T_VOID); return;
        case TY_BOOL: w_u8(w, T_BOOL); return;
        case TY_INT: w_u8(w, T_INT); return;
        case TY_I64: w_u8(w, T_I64); return;
        case TY_FLOAT: w_u8(w, T_FLOAT); return;
        case TY_STRING: w_u8(w, T_STRING); return;
        case TY_DURATION: w_u8(w, T_DURATION); return;
        case TY_VEC2: w_u8(w, T_VEC2); return;
        case TY_VEC3: w_u8(w, T_VEC3); return;
        case TY_VEC4: w_u8(w, T_VEC4); return;
        case TY_MAT4: w_u8(w, T_MAT4); return;
        case TY_QUAT: w_u8(w, T_QUAT); return;
        case TY_REF:
            w_u8(w, T_REF);
            w_ty(w, t->inner);
            return;
        case TY_PTR:
            w_u8(w, T_PTR);
            w_ty(w, t->inner);
            return;
        case TY_ARRAY:
            w_u8(w, T_ARRAY);
            w_ty(w, t->elem);
            w_u64(w, (uint64_t)t->size);
            return;
        case TY_NAMED:
            if (hx_ty_is_result(t)) {
                w_u8(w, T_RESULT);
                w_ty(w, t->elem);
                w_ty(w, t->inner);
                return;
            }
            w_u8(w, T_NAMED);
            w_sym(w, t->name);
            return;
        default: w_u8(w, T_VOID); return;
    }
}

int hx_hxc_write(HxArena *arena, HxUnit *unit, HxModule *m, const char *path) {
    W w;
    memset(&w, 0, sizeof(w));
    w.b.arena = arena;
    hx_buf_put(&w.b, HXCU_MAGIC, 4);
    w_u32(&w, HXCU_FORMAT);
    w_u32(&w, HXCU_ABI);
    w_u32(&w, 0);
    HxHash h;
    hx_fnv_init(&h);
    if (m->src) hx_fnv_str(&h, m->src);
    w_u64(&w, h.h);
    w_u64(&w, 0); /* longitud del cuerpo, se rellena al final */
    w_u64(&w, 0); /* hash del cuerpo, se rellena al final */
    w_sym(&w, m->name);

    int ntypes = 0;
    for (int i = 0; i < m->types.len; i++)
        if (m->types.data[i].is_export) ntypes++;
    w_u32(&w, (uint32_t)ntypes);
    for (int i = 0; i < m->types.len; i++) {
        HxTypeDecl *t = &m->types.data[i];
        if (!t->is_export) continue;
        w_sym(&w, t->name);
        w_u32(&w, (uint32_t)t->fields.len);
        for (int j = 0; j < t->fields.len; j++) {
            w_sym(&w, t->fields.data[j].name);
            w_ty(&w, t->fields.data[j].ty);
        }
    }

    int nconsts = 0;
    for (int i = 0; i < m->consts.len; i++)
        if (m->consts.data[i].is_export) nconsts++;
    w_u32(&w, (uint32_t)nconsts);
    for (int i = 0; i < m->consts.len; i++) {
        HxConst *c = &m->consts.data[i];
        if (!c->is_export) continue;
        w_sym(&w, c->name);
        w_ty(&w, c->ty);
        w_u8(&w, 1);
        if (!c->value) {
            w_u8(&w, 0);
            continue;
        }
        switch (c->value->kind) {
            case EX_INT:
            case EX_DURATION:
            case EX_BOOL:
                w_u8(&w, 1);
                w_u64(&w, (uint64_t)c->value->ival);
                break;
            case EX_FLOAT: {
                w_u8(&w, 2);
                double d = c->value->fval;
                uint64_t bits;
                memcpy(&bits, &d, 8);
                w_u64(&w, bits);
                break;
            }
            case EX_STR:
                w_u8(&w, 3);
                w_u32(&w, (uint32_t)c->value->str.len);
                hx_buf_put(&w.b, c->value->str.raw, (size_t)c->value->str.len);
                break;
            default:
                w_u8(&w, 0);
                break;
        }
    }

    int nfuncs = 0;
    for (int i = 0; i < m->funcs.len; i++)
        if (m->funcs.data[i].is_export) nfuncs++;
    w_u32(&w, (uint32_t)nfuncs);
    for (int i = 0; i < m->funcs.len; i++) {
        HxFunc *f = &m->funcs.data[i];
        if (!f->is_export) continue;
        w_sym(&w, f->name);
        w_u32(&w, (uint32_t)f->params.len);
        for (int j = 0; j < f->params.len; j++) {
            w_sym(&w, f->params.data[j].name);
            w_ty(&w, f->params.data[j].ty);
            w_u8(&w, (unsigned)(f->params.data[j].is_ref ? 1 : 0));
        }
        w_ty(&w, f->ret);
    }

    HxHash body;
    hx_fnv_init(&body);
    hx_fnv_bytes(&body, w.b.data + HXCU_HEADER, w.b.len - HXCU_HEADER);
    uint64_t payload_len = (uint64_t)(w.b.len - HXCU_HEADER);
    for (int i = 0; i < 8; i++) {
        w.b.data[24 + i] = (char)((payload_len >> (i * 8)) & 0xff);
        w.b.data[32 + i] = (char)((body.h >> (i * 8)) & 0xff);
    }
    int rc = hx_write_file(path, w.b.data, w.b.len);
    free(w.b.data);
    return rc;
}

typedef struct {
    const char *p;
    const char *end;
    HxDiagBag *diags;
    const char *path;
    int depth;
    int trunc;
} R;

static uint32_t r_u32(R *r) {
    if (r->p + 4 > r->end) {
        r->p = r->end;
        r->trunc = 1;
        return 0;
    }
    const unsigned char *q = (const unsigned char *)r->p;
    r->p += 4;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) |
           ((uint32_t)q[3] << 24);
}

static uint64_t r_u64(R *r) {
    uint64_t v = 0;
    if (r->p + 8 > r->end) {
        r->p = r->end;
        r->trunc = 1;
        return 0;
    }
    const unsigned char *q = (const unsigned char *)r->p;
    r->p += 8;
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)q[i] << (i * 8);
    return v;
}

static unsigned r_u8(R *r) {
    if (r->p >= r->end) {
        r->trunc = 1;
        return 0;
    }
    return (unsigned)*r->p++;
}

static const char *r_str(R *r, uint32_t *out_len) {
    uint32_t n = r_u32(r);
    if ((size_t)(r->end - r->p) < n) {
        r->p = r->end;
        r->trunc = 1;
        *out_len = 0;
        return NULL;
    }
    const char *s = r->p;
    r->p += n;
    *out_len = n;
    return s;
}

static HxSym r_sym(R *r, HxIntern *intern) {
    uint32_t n = 0;
    const char *s = r_str(r, &n);
    if (!s || !n) return NULL;
    return hx_intern(intern, s, n);
}

static HxTy *r_ty(R *r, HxArena *arena, HxIntern *intern);

static HxTy *r_ty_body(R *r, HxArena *arena, HxIntern *intern, unsigned tag) {
    HxTy *t = (HxTy *)hx_arena_calloc(arena, sizeof(HxTy));
    switch (tag) {
        case T_VOID: t->kind = TY_VOID; return t;
        case T_BOOL: t->kind = TY_BOOL; return t;
        case T_INT: t->kind = TY_INT; return t;
        case T_I64: t->kind = TY_I64; return t;
        case T_FLOAT: t->kind = TY_FLOAT; return t;
        case T_STRING: t->kind = TY_STRING; return t;
        case T_DURATION: t->kind = TY_DURATION; return t;
        case T_VEC2: t->kind = TY_VEC2; return t;
        case T_VEC3: t->kind = TY_VEC3; return t;
        case T_VEC4: t->kind = TY_VEC4; return t;
        case T_MAT4: t->kind = TY_MAT4; return t;
        case T_QUAT: t->kind = TY_QUAT; return t;
        case T_REF:
            t->kind = TY_REF;
            t->inner = r_ty(r, arena, intern);
            return t;
        case T_PTR:
            t->kind = TY_PTR;
            t->inner = r_ty(r, arena, intern);
            return t;
        case T_ARRAY: {
            t->kind = TY_ARRAY;
            t->elem = r_ty(r, arena, intern);
            uint64_t sz = r_u64(r);
            if (sz > (uint64_t)1 << 32) {
                hx_error(r->diags, (HxSpan){0, 0}, "E0602", "%s: tamaño de arreglo fuera de rango",
                         r->path);
                sz = 0;
            }
            t->size = (int64_t)sz;
            return t;
        }
        case T_RESULT:
            t->kind = TY_NAMED;
            t->name = hx_intern_cstr(intern, "Result");
            t->elem = r_ty(r, arena, intern);
            t->inner = r_ty(r, arena, intern);
            return t;
        case T_NAMED:
            t->kind = TY_NAMED;
            t->name = r_sym(r, intern);
            return t;
        default:
            t->kind = TY_UNKNOWN;
            return t;
    }
}

static HxTy *r_ty(R *r, HxArena *arena, HxIntern *intern) {
    if (r->depth > 32) {
        hx_error(r->diags, (HxSpan){0, 0}, "E0602", "tipo demasiado anidado en %s", r->path);
        r->depth--;
        return hx_arena_calloc(arena, sizeof(HxTy));
    }
    r->depth++;
    unsigned tag = r_u8(r);
    HxTy *t = r_ty_body(r, arena, intern, tag);
    r->depth--;
    return t;
}

HxModule *hx_hxc_read(HxArena *arena, HxIntern *intern, HxDiagBag *diags, const char *path) {
    size_t len = 0;
    char *data = hx_read_file(arena, path, &len);
    if (!data) return NULL;
    if (len < (size_t)HXCU_HEADER || memcmp(data, HXCU_MAGIC, 4) != 0) {
        hx_error(diags, (HxSpan){0, 0}, "E0601", "%s no es una unidad .hxc", path);
        return NULL;
    }
    R r;
    memset(&r, 0, sizeof(r));
    r.p = data;
    r.end = data + len;
    r.diags = diags;
    r.path = path;
    r.p += 4;
    uint32_t format = r_u32(&r);
    uint32_t abi = r_u32(&r);
    r_u32(&r);
    r_u64(&r);
    uint64_t src_hash = 0;
    {
        const unsigned char *q = (const unsigned char *)r.p;
        for (int i = 0; i < 8; i++) src_hash |= (uint64_t)q[i] << (i * 8);
    }
    uint64_t payload_len = 0, payload_hash = 0;
    for (int i = 0; i < 8; i++) {
        payload_len |= (uint64_t)(unsigned char)r.p[i] << (i * 8);
        payload_hash |= (uint64_t)(unsigned char)r.p[8 + i] << (i * 8);
    }
    r.p += 16;
    (void)src_hash;
    if (format != HXCU_FORMAT || abi != HXCU_ABI) {
        hx_error(diags, (HxSpan){0, 0}, "E0603",
                 "%s usa formato %u/abi %u y esta versión entiende %u/%u", path, format, abi,
                 (unsigned)HXCU_FORMAT, (unsigned)HXCU_ABI);
        return NULL;
    }
    if (payload_len != (uint64_t)(r.end - r.p)) {
        hx_error(diags, (HxSpan){0, 0}, "E0602",
                 "%s está truncada: el cuerpo declara %llu bytes y el archivo tiene %zu", path,
                 (unsigned long long)payload_len, (size_t)(r.end - r.p));
        return NULL;
    }
    HxHash got;
    hx_fnv_init(&got);
    hx_fnv_bytes(&got, r.p, (size_t)payload_len);
    if (got.h != payload_hash) {
        hx_error(diags, (HxSpan){0, 0}, "E0602", "%s está dañada: el hash del cuerpo no coincide",
                 path);
        return NULL;
    }

    HxModule *m = (HxModule *)hx_arena_calloc(arena, sizeof(HxModule));
    m->file = path;
    m->src = "";
    m->name = r_sym(&r, intern);

    uint32_t ntypes = r_u32(&r);
    if (ntypes > 65536u) {
        hx_error(diags, (HxSpan){0, 0}, "E0602", "%s declara demasiados tipos", path);
        return NULL;
    }
    for (uint32_t i = 0; i < ntypes && r.p < r.end; i++) {
        HxTypeDecl t;
        memset(&t, 0, sizeof(t));
        t.is_export = 1;
        t.name = r_sym(&r, intern);
        uint32_t nf = r_u32(&r);
        if (nf > 65536u) return NULL;
        for (uint32_t j = 0; j < nf && r.p < r.end; j++) {
            HxField f;
            memset(&f, 0, sizeof(f));
            f.name = r_sym(&r, intern);
            f.ty = r_ty(&r, arena, intern);
            HX_VEC_PUSH(t.fields, f);
        }
        HX_VEC_PUSH(m->types, t);
    }

    uint32_t nconsts = r_u32(&r);
    for (uint32_t i = 0; i < nconsts && r.p < r.end; i++) {
        HxConst c;
        memset(&c, 0, sizeof(c));
        c.is_export = 1;
        c.name = r_sym(&r, intern);
        c.ty = r_ty(&r, arena, intern);
        r_u8(&r);
        unsigned tag = r_u8(&r);
        HxExpr *v = (HxExpr *)hx_arena_calloc(arena, sizeof(HxExpr));
        v->ty = c.ty;
        if (tag == 1) {
            v->kind = EX_INT;
            v->ival = (int64_t)r_u64(&r);
        } else if (tag == 2) {
            uint64_t bits = r_u64(&r);
            double d;
            memcpy(&d, &bits, 8);
            v->kind = EX_FLOAT;
            v->fval = d;
        } else if (tag == 3) {
            uint32_t n = 0;
            const char *s = r_str(&r, &n);
            v->kind = EX_STR;
            v->str.raw = s ? hx_arena_strndup(arena, s, n) : "";
            v->str.len = (int)n;
        } else {
            v->kind = EX_NIL;
        }
        c.value = v;
        HX_VEC_PUSH(m->consts, c);
    }

    uint32_t nfuncs = r_u32(&r);
    for (uint32_t i = 0; i < nfuncs && r.p < r.end; i++) {
        HxFunc f;
        memset(&f, 0, sizeof(f));
        f.is_export = 1;
        f.name = r_sym(&r, intern);
        uint32_t np = r_u32(&r);
        if (np > 65536u) return NULL;
        for (uint32_t j = 0; j < np && r.p < r.end; j++) {
            HxParam prm;
            memset(&prm, 0, sizeof(prm));
            prm.name = r_sym(&r, intern);
            prm.ty = r_ty(&r, arena, intern);
            prm.is_ref = (int)r_u8(&r);
            HX_VEC_PUSH(f.params, prm);
        }
        f.ret = r_ty(&r, arena, intern);
        HX_VEC_PUSH(m->funcs, f);
    }

    /* Una unidad truncada o manipulada puede dejar nombres a NULL: en ese caso
       se rechaza entera en lugar de propagar un modulo incoherente. */
    int bad = !m->name;
    for (int i = 0; i < m->types.len && !bad; i++) {
        bad = !m->types.data[i].name;
        for (int j = 0; j < m->types.data[i].fields.len; j++)
            bad = !m->types.data[i].fields.data[j].name;
    }
    for (int i = 0; i < m->consts.len && !bad; i++) bad = !m->consts.data[i].name;
    for (int i = 0; i < m->funcs.len && !bad; i++) {
        bad = !m->funcs.data[i].name;
        for (int j = 0; j < m->funcs.data[i].params.len; j++)
            bad = !m->funcs.data[i].params.data[j].name;
    }
    if (r.trunc || r.p != r.end || bad) {
        hx_error(diags, (HxSpan){0, 0}, "E0602", "%s está dañada o truncada", path);
        return NULL;
    }
    return m;
}